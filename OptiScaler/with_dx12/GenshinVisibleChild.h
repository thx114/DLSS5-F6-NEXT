#pragma once

#include <Windows.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <tuple>
#include <CommCtrl.h>
#include <atomic>
#include <string>
#include <memory>
#include <new>
#include <cstdint>
#include <mutex>
#include <array>
#include <wrl/client.h>
#pragma comment(lib, "comctl32.lib")

// R564 bounded owner: only same-process, top-level windowed parent and
// window mutation/destruction run on its owner; Present may run on the render thread.
// No worker, thread attach, synchronous cross-thread SendMessage or HWND reuse.
// GPU resource lifetime belongs to the wrapper; Hide never releases it.
class GenshinVisibleChild final
{
public:
    // Copied from the actual ReturnedPicture ticket, not inferred from a HWND,
    // provider clock, latest CPU Render or monotonically increasing wrapper ID.
    struct ContentSource {
        uint64_t device11=0,context11=0,device12=0,native=0,backbuffer=0,selected=0;
        uint64_t deviceEpoch=0,ownerEpoch=0,configEpoch=0,historyEpoch=0,instance=0;
        uint64_t generation=0,sourceFrame=0,picture=0,present=0,submit=0,copy=0;
        uint32_t thread=0,subresource=0;
        bool Known() const noexcept {
            return device11 && context11 && device12 && native && backbuffer && selected &&
                deviceEpoch && ownerEpoch && configEpoch && instance && generation && picture &&
                submit && copy && thread;
        }
        bool operator==(const ContentSource& b) const noexcept {
            return std::tie(device11,context11,device12,native,backbuffer,selected,deviceEpoch,ownerEpoch,
                configEpoch,historyEpoch,instance,generation,sourceFrame,picture,present,submit,copy,thread,subresource)==
                std::tie(b.device11,b.context11,b.device12,b.native,b.backbuffer,b.selected,b.deviceEpoch,b.ownerEpoch,
                b.configEpoch,b.historyEpoch,b.instance,b.generation,b.sourceFrame,b.picture,b.present,b.submit,b.copy,b.thread,b.subresource);
        }
    };
private:
    struct PhysicalContent {
        ContentSource source{};
        uint64_t epoch=0,stream=0,outputGeneration=0,sequence=0,value=0;
        UINT index=0,rawFlags=0;
        bool primed=false; // hidden RESTART requested, not a DWM/pixel witness
        Microsoft::WRL::ComPtr<IDXGISwapChain3> chain;
        uintptr_t color=0; // identity only: no observer backbuffer ref may block ResizeBuffers
        std::shared_ptr<std::atomic<bool>> outputLive; // invalidation only, NEVER GPU completion
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        Microsoft::WRL::ComPtr<ID3D12Device> device;
        bool OutputLive() const noexcept {return outputLive && outputLive->load(std::memory_order_acquire);}
        bool Ready(uint64_t& completed) const noexcept {
            if(!OutputLive() || !chain || !color || !queue || !fence || !device || !value || value==UINT64_MAX)return false;
            completed=fence->GetCompletedValue();
            return completed!=UINT64_MAX && completed>=value && SUCCEEDED(device->GetDeviceRemovedReason());
        }
    };
    // Serialize revocation / source publication / owner reveal / BEFORE-parent.
    // Native Present, SDK/raw Present and GPU waits stay OUTSIDE this lock.
    // Owner reveal is try-only; there is no render->owner synchronous message.
    // The shared gate outlives the owner pointer during a captured SDK worker
    // call. It owns only bounded observations; endpoint custody/retirement stays
    // unchanged. A CPU raw call ending never completes a GPU tail.
    struct DisplayGate {
        std::recursive_mutex mutex;
        HWND child=nullptr;
        bool live=false,hiddenAcknowledged=false;
        std::weak_ptr<std::atomic<bool>> outputLive;
        struct HiddenTail {
            Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
            Microsoft::WRL::ComPtr<ID3D12Fence> fence;
            Microsoft::WRL::ComPtr<ID3D12Device> device;
            uint64_t value=0;
            bool inFlight=false,uncertain=false;
            bool Complete() const noexcept {
                if(inFlight || uncertain)return false;
                if(!value)return true; // reserved, but no raw work was submitted
                if(!queue || !fence || !device || value==UINT64_MAX)return false;
                const auto completed=fence->GetCompletedValue();
                return completed!=UINT64_MAX && completed>=value && SUCCEEDED(device->GetDeviceRemovedReason());
            }
        };
        // Same bound as retained physical chains. Reuse a slot only on the same
        // real queue/fence, or after its prior work has positively completed.
        std::array<HiddenTail,4> hiddenTails;
        bool HiddenReady() noexcept {
            bool ready=true;
            for(auto& tail:hiddenTails) {
                if(!tail.Complete())ready=false;
                else tail={}; // release observations only after actual completion (or no raw)
            }
            return ready;
        }
        bool RawInFlight() const noexcept {
            for(const auto& tail:hiddenTails)if(tail.inFlight)return true;
            return false;
        }
    };
    const std::shared_ptr<DisplayGate> display_=std::make_shared<DisplayGate>();
    inline static std::atomic<std::shared_ptr<DisplayGate>> retainedDisplay_;
    ContentSource parentContent_{}; // protected by snapshotMutex_
    PhysicalContent pendingContent_; // bounded, observer refs; endpoint owns GPU retirement
    static constexpr wchar_t Property[] = L"OptiScaler.R564.NativeParent.Owner";
    static constexpr UINT HideMessage = WM_APP + 0x564;
    static constexpr UINT RetireMessage = WM_APP + 0x625;
    static constexpr UINT CommandMessage = WM_APP + 0x718;
    static constexpr UINT_PTR LeaseTimer = 0x718;
    static constexpr ULONGLONG LeaseMs = 250, SnapshotMs = 500;
    inline static std::atomic<uint64_t> nextCookie_{1};
    static uint64_t NewCookie() noexcept {
        auto value=nextCookie_.load();
        while(value!=UINT64_MAX)if(nextCookie_.compare_exchange_weak(value,value+1))return value;
        return 0; // saturate: queued messages never regain an old lifetime cookie
    }
    const uint64_t cookie_ = NewCookie();
    unsigned retireAttempts_=0; // owner only, at most eight callback attempts
    using RetireCallback=void (*)(void*);
    std::atomic<RetireCallback> retire_{nullptr};
    std::atomic<void*> retireContext_{nullptr};
    // There is one FG provider/output owner in this route. Keep its HWND key
    // even after WM_NCDESTROY until the wrapper's real drain completes. This
    // prevents legacy release hooks from treating an invalid child HWND as
    // permission to release references belonging to other GPU owners.
    inline static std::atomic<HWND> retainedChild_{nullptr};
    inline static std::atomic<void*> retainedOwner_{nullptr};
    inline static thread_local GenshinVisibleChild* creatingOwner_ = nullptr;
    struct OutputIdentities {
        std::mutex mutex;
        std::array<Microsoft::WRL::ComPtr<IUnknown>,4> owners;
    };
    static OutputIdentities* Outputs() noexcept {static auto* p=new(std::nothrow) OutputIdentities;return p;}
    HWND ownedChild_ = nullptr;
    HMODULE module_ = nullptr;
    std::wstring className_;
    bool classRegistered_ = false;
    HWND parent_ = nullptr, child_ = nullptr;
    std::atomic<uint64_t> hideRequested_{0};
    std::atomic<HWND> messageParent_{nullptr};
    const DWORD thread_ = GetCurrentThreadId();
    std::atomic<uint64_t> epoch_{1};
    std::atomic<bool> commandPosted_{false};
    std::atomic<uint64_t> showEpoch_{0};
    std::atomic<ULONGLONG> showUntil_{0};
    struct WindowSnapshot {
        HWND parent=nullptr, child=nullptr;
        uint64_t epoch=0; UINT width=0,height=0,dpi=0;
        ULONGLONG published=0; bool ready=false,visible=false;
    };
    mutable std::mutex snapshotMutex_;
    WindowSnapshot snapshot_{};
    // R723: a fallback revokes one visibility lease. Repeated Presents must not
    // replace a pending owner acknowledgement or invalidate a fresh hidden snapshot.
    // Protected by snapshotMutex_; only a valid Show starts a new visibility lease.
    bool hideLatched_ = false;
    void ArmHideLocked() noexcept {
        if(!hideLatched_) {
            BumpEpoch();hideRequested_.store(epoch_.load());hideLatched_=true;
        }
        showEpoch_.store(0);showUntil_.store(0);parentContent_={};
    }
    // Bounded scalar observation only. Calls are outside snapshotMutex_.
    // Separate owner/render budgets keep queue acknowledgements observable.
    mutable std::atomic<unsigned> ownerReports_{0}, renderReports_{0};
    void Observe(const char* event,uint64_t subject=0,int value=-1) const noexcept {
        const auto actual=GetCurrentThreadId();auto& budget=actual==thread_?ownerReports_:renderReports_;
        auto n=budget.load(std::memory_order_relaxed);
        do {if(n>=256)return;}while(!budget.compare_exchange_weak(n,n+1,std::memory_order_relaxed));
        try{LOG_INFO("R827 phase38 child lifecycle: pid={} cookie={} lane={} report={} event={} subject={} value={} now={} owner_thread={} actual_thread={} epoch={} hide={} command={} show_epoch={} show_until={} observation_only=true",
            GetCurrentProcessId(),cookie_,actual==thread_?"owner":"render",n+1,event,subject,value,GetTickCount64(),
            thread_,actual,epoch_.load(),hideRequested_.load(),commandPosted_.load(),showEpoch_.load(),showUntil_.load());}catch(...){}
    }
    void BumpEpoch() noexcept {
        auto e=epoch_.load();
        while(e!=UINT64_MAX && !epoch_.compare_exchange_weak(e,e+1)) {}
    }
    bool QueueCommand() noexcept {
        if(commandPosted_.exchange(true))return true;
        if(ownedChild_ && PostMessageW(ownedChild_,CommandMessage,
            reinterpret_cast<WPARAM>(this),static_cast<LPARAM>(cookie_)))return true;
        commandPosted_.store(false);return false;
    }
    void Publish(bool ready) noexcept {
        std::lock_guard display(display_->mutex);
        // Only the child's OWN visibility bit acknowledges hidden bootstrap.
        // An invisible/minimized parent does not make a visible child safe.
        display_->child=child_;
        display_->live=child_ && !closed_ && GetPropW(child_,Property)==this;
        display_->hiddenAcknowledged=display_->live && !(GetWindowLongPtrW(child_,GWL_STYLE)&WS_VISIBLE);
        const bool visible=child_ && IsWindowVisible(child_)!=FALSE;
        std::lock_guard lock(snapshotMutex_);
        snapshot_={parent_,child_,epoch_.load(),width_,height_,dpi_,GetTickCount64(),
            ready && !closed_ && !hideRequested_.load(),visible};
    }
    bool RefusePreparation() noexcept {
        // A failed owner check must revoke readiness even when Hide
        // already acknowledged this lease and is therefore a no-op.
        Hide();Publish(false);return false;
    }
    WindowSnapshot ReadSnapshot() const noexcept {
        std::lock_guard lock(snapshotMutex_);return snapshot_;
    }
    bool Current(const WindowSnapshot& s) const noexcept {
        return s.ready && s.epoch && s.epoch!=UINT64_MAX && s.epoch==epoch_.load() &&
            !hideRequested_.load() && GetTickCount64()-s.published<=SnapshotMs;
    }
    void OwnerCommand() noexcept {
        commandPosted_.store(false);if(showEpoch_.load() || hideRequested_.load())Observe("owner-command");
        if(hideRequested_.load())Hide();
        if(!Prepare()){Observe("owner-prepare-refused");return;}
        const auto e=showEpoch_.load();
        if(e && e==epoch_.load() && GetTickCount64()<showUntil_.load())ApplyShow(e);
    }
    UINT width_ = 0, height_ = 0, dpi_ = 0;
    bool subclassed_ = false, closed_ = false;
    explicit GenshinVisibleChild(HWND parent) : parent_(parent), messageParent_(parent) {}

    static LRESULT CALLBACK ChildProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        auto* self = reinterpret_cast<GenshinVisibleChild*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE)
        {
            // Only our active CreateWindow call may enroll a child. Never
            // interpret another window's ordinary GWLP_USERDATA as this owner.
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lp);
            if (!creatingOwner_ || !create || create->lpCreateParams != creatingOwner_) return FALSE;
            self = creatingOwner_;
            self->child_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        const bool creating = self && self == creatingOwner_;
        const bool retained = self && self == retainedOwner_.load(std::memory_order_acquire) &&
            hwnd == retainedChild_.load(std::memory_order_acquire) && GetPropW(hwnd, Property) == self;
        // Pointer equality precedes every owner dereference. A foreign or stale
        // callback is an ordinary DefWindowProc call, including WM_NCDESTROY.
        if ((!creating && !retained) || self->child_ != hwnd)
            return DefWindowProcW(hwnd, msg, wp, lp);
        if ((msg == RetireMessage || msg == CommandMessage) && self &&
            wp==reinterpret_cast<WPARAM>(self) && static_cast<uint64_t>(lp)==self->cookie_ &&
            GetPropW(hwnd,Property)==self && self->child_==hwnd)
        {
            if(msg==CommandMessage){self->OwnerCommand();return 0;}
            // Callback may delete the wrapper and this child after actual drain.
            auto fn=self->retire_.load(std::memory_order_acquire);
            auto context=self->retireContext_.load(std::memory_order_relaxed);
            if(fn && self->retireAttempts_<8){++self->retireAttempts_;fn(context);}
            return 0;
        }
        if (msg == WM_NCHITTEST) return HTTRANSPARENT;
        if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
        if (msg == WM_ERASEBKGND) return 1;
        if(msg==WM_TIMER && wp==LeaseTimer && self && GetPropW(hwnd,Property)==self) {
            if(auto fn=self->retire_.load(std::memory_order_acquire)) {
                if(self->retireAttempts_<8){
                    auto context=self->retireContext_.load(std::memory_order_relaxed);
                    ++self->retireAttempts_;fn(context); // may delete self: return immediately
                }
                return 0; // exhausted retries retain this bounded owner
            }
            if(GetTickCount64()>=self->showUntil_.load() && IsWindowVisible(hwnd)){self->Observe("timer-hide");self->Hide();}
            self->OwnerCommand();return 0;
        }
        if (msg == WM_NCDESTROY && self)
        {
            std::lock_guard display(self->display_->mutex);
            self->display_->live=false;self->display_->hiddenAcknowledged=false;
            if (GetPropW(hwnd, Property) == self) RemovePropW(hwnd, Property);
            KillTimer(hwnd,LeaseTimer);
            self->child_ = nullptr; self->closed_ = true; self->BumpEpoch();self->Publish(false);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    static LRESULT CALLBACK ParentProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                       UINT_PTR id, DWORD_PTR data)
    {
        auto* self = reinterpret_cast<GenshinVisibleChild*>(data);
        if (!self || self != retainedOwner_.load(std::memory_order_acquire) ||
            id != reinterpret_cast<UINT_PTR>(self) || GetPropW(hwnd, Property) != self || self->parent_ != hwnd)
            return DefSubclassProc(hwnd, msg, wp, lp);
        if (msg == HideMessage && wp == reinterpret_cast<WPARAM>(self) && static_cast<uint64_t>(lp)==self->cookie_) { self->Invalidate(); return 0; }
        if (msg == WM_SIZE || msg == WM_WINDOWPOSCHANGED || msg == WM_DPICHANGED ||
            msg == WM_SHOWWINDOW || msg == WM_ACTIVATEAPP || msg == WM_CLOSE || msg == WM_DESTROY)
        {self->Observe("parent-invalidate",msg);self->Invalidate();}
        if (msg == WM_NCDESTROY)
        {
            self->Invalidate(); self->closed_ = true; self->parent_ = nullptr; self->subclassed_ = false;
            self->messageParent_.store(nullptr);self->Publish(false);
            if (GetPropW(hwnd, Property) == self) RemovePropW(hwnd, Property);
            RemoveWindowSubclass(hwnd, ParentProc, id);
        }
        return DefSubclassProc(hwnd, msg, wp, lp);
    }

public:
    GenshinVisibleChild(const GenshinVisibleChild&) = delete;
    GenshinVisibleChild& operator=(const GenshinVisibleChild&) = delete;
    bool HideRequested() const noexcept { return hideRequested_.load(); }
    bool OnOwnerThread() const noexcept { return GetCurrentThreadId() == thread_; }
    HWND Parent() const noexcept { return ReadSnapshot().parent; }
    HWND Child() const noexcept { return ReadSnapshot().child; }
    uint64_t Epoch() const noexcept { return epoch_.load(); }
    UINT Width() const noexcept { return ReadSnapshot().width; }
    UINT Height() const noexcept { return ReadSnapshot().height; }
    UINT Dpi() const noexcept { return ReadSnapshot().dpi; }
    // A whole receipt is copied under one lock. No HWND API executes there.
    bool Snapshot(HWND& parent,HWND& child,uint64_t& epoch,UINT& width,UINT& height,UINT& dpi) const noexcept {
        const auto s=ReadSnapshot();if(!Current(s))return false;
        parent=s.parent;child=s.child;epoch=s.epoch;width=s.width;height=s.height;dpi=s.dpi;return true;
    }
    bool Visible(uint64_t epoch,const char** observedStage=nullptr) const noexcept {
        WindowSnapshot s;
        bool outputLive=false;
        {std::lock_guard lock(snapshotMutex_);s=snapshot_;
            outputLive=pendingContent_.epoch==epoch && pendingContent_.OutputLive();}
        const char* reached="visible-current";
        const auto stage=[&](const char* value){reached=value;if(observedStage)*observedStage=value;return true;};
        const bool result=stage("visible-current") && Current(s) && stage("visible-epoch") && s.epoch==epoch &&
            stage("visible-ack") && s.visible && stage("visible-physical-lifetime") && outputLive &&
            stage("visible-deadline") && GetTickCount64()<showUntil_.load();
        if(!result)Observe(reached,epoch,s.visible?1:0);
        return result;
    }

    static std::unique_ptr<GenshinVisibleChild> Create(HWND parent)
    {
        DWORD pid = 0;
        if (retainedChild_.load() || !IsWindow(parent) || GetWindowThreadProcessId(parent, &pid) != GetCurrentThreadId() ||
            pid != GetCurrentProcessId() || (GetWindowLongPtrW(parent, GWL_STYLE) & WS_CHILD) ||
            GetPropW(parent, Property) || IsIconic(parent)) return {};
        // Coordinates must be physical and share the parent's awareness context.
        if (!AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(), GetWindowDpiAwarenessContext(parent))) return {};
        auto self = std::unique_ptr<GenshinVisibleChild>(new GenshinVisibleChild(parent));
        if(!self->cookie_ || self->cookie_==UINT64_MAX)return {};
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&ChildProc), &module)) return {};
        WNDCLASSEXW wc{}; wc.cbSize = sizeof(wc); wc.lpfnWndProc = ChildProc; wc.hInstance = module;
        self->module_ = module;
        self->className_ = L"OptiScaler.R564.FgChild." + std::to_wstring(reinterpret_cast<uintptr_t>(self.get()));
        wc.lpszClassName = self->className_.c_str();
        self->classRegistered_ = RegisterClassExW(&wc) != 0;
        if (!self->classRegistered_) return {};
        struct CreationScope {
            GenshinVisibleChild* previous;
            explicit CreationScope(GenshinVisibleChild* owner) : previous(creatingOwner_) { creatingOwner_ = owner; }
            ~CreationScope() { creatingOwner_ = previous; }
        };
        {
            CreationScope creating(self.get());
            self->child_ = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW,
                wc.lpszClassName, L"", WS_CHILD, 0, 0, 1, 1, parent, nullptr, module, self.get());
        }
        if (!self->child_ || !SetPropW(self->child_, Property, self.get()) ||
            !SetPropW(parent, Property, self.get())) return {};
        HWND expected = nullptr;
        if (!retainedChild_.compare_exchange_strong(expected, self->child_)) return {};
        self->ownedChild_ = self->child_;
        retainedOwner_.store(self.get());
        retainedDisplay_.store(self->display_,std::memory_order_release);
        self->subclassed_ = SetWindowSubclass(parent, ParentProc, reinterpret_cast<UINT_PTR>(self.get()),
                                             reinterpret_cast<DWORD_PTR>(self.get())) != FALSE;
        if (!self->subclassed_ || !self->Prepare() || !SetTimer(self->child_,LeaseTimer,100,nullptr)) return {};
        return self;
    }

    static bool IsRetainedChild(HWND hwnd) noexcept { return hwnd && retainedChild_.load() == hwnd; }
    static bool IsPhysicalOutput(IDXGISwapChain* chain,HWND hwnd) noexcept {
        if(!chain || !IsRetainedChild(hwnd))return false;
        DXGI_SWAP_CHAIN_DESC d{};Microsoft::WRL::ComPtr<IUnknown> id;
        if(FAILED(chain->GetDesc(&d)) || d.OutputWindow!=hwnd ||
           FAILED(chain->QueryInterface(IID_PPV_ARGS(&id))))return false;
        auto* storage=Outputs();if(!storage)return false;
        auto& outputs=*storage;std::lock_guard lock(outputs.mutex);
        for(const auto& owner:outputs.owners)if(owner.Get()==id.Get())return true;
        // Only the original still-owned HWND can enroll a new physical chain.
        // Destroyed/recycled HWNDs may use only already retained COM identities.
        if(GetPropW(hwnd,Property)!=retainedOwner_.load() || !IsOwnedChild(hwnd))return false;
        for(auto& owner:outputs.owners)if(!owner){owner=id;return true;}
        return false; // bounded overflow cannot gain native clock authority
    }
    static void ReleasePhysicalOutputs() noexcept {
        // Caller has actual provider + copy completion, before provider release.
        // Move refs out under lock; COM destruction must not reenter the lock.
        std::array<Microsoft::WRL::ComPtr<IUnknown>,4> retired;
        auto* storage=Outputs();if(!storage)return;
        {auto& outputs=*storage;std::lock_guard lock(outputs.mutex);retired.swap(outputs.owners);}
    }
    static bool IsOwnedChild(HWND hwnd) noexcept
    {
        const auto token = GetPropW(hwnd, Property);
        const auto parent = GetParent(hwnd);
        return hwnd && token && parent && GetPropW(parent, Property) == token &&
            (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CHILD);
    }
    void RetireOnOwner(void (*callback)(void*),void* context) noexcept
    {
        // Called once after the wrapper's final external reference is gone.
        // No AddRef resurrection or retry-Release contract is exposed to game.
        retireContext_.store(context,std::memory_order_relaxed);
        retire_.store(callback,std::memory_order_release);
        // Always defer beyond the caller stack, even on the HWND owner. The
        // callback can destroy this child; it must never run inside this method.
        RequestHide();
        if(ownedChild_)PostMessageW(ownedChild_,RetireMessage,reinterpret_cast<WPARAM>(this),static_cast<LPARAM>(cookie_));
        // A destroyed HWND/failed PostMessage conservatively retains this one
        // owner. retainedChild_ forbids a second uncertain child allocation.
    }
    void RequestHide() noexcept {
        bool newlyArmed=false;
        PhysicalContent discard;
        {
            std::lock_guard display(display_->mutex);
            std::lock_guard lock(snapshotMutex_); // data only, no Win32/provider calls
            newlyArmed=!hideLatched_;ArmHideLocked();
            std::swap(discard,pendingContent_); // observer refs leave outside lock; endpoint retains GPU custody
            if(!hideRequested_.load())return; // already acknowledged hidden
        }
        if(newlyArmed)Observe("hide-request");
        // A failed PostMessage can retry the same epoch on the next Present.
        if(OnOwnerThread())Hide();else QueueCommand();
    }
    // An eligible new parent changes the required CONTENT, not the display
    // epoch. Slow owner acknowledgement therefore cannot starve bootstrap by
    // repeatedly revoking its own lease. Fallback already calls RequestHide.
    void ParentWillPresent(const ContentSource& source,bool exactDuplicate=false) noexcept {
        if(exactDuplicate)return;
        std::lock_guard display(display_->mutex);
        std::lock_guard lock(snapshotMutex_);
        parentContent_=source;
    }
    void Hide() noexcept {
        if(!OnOwnerThread()){RequestHide();return;}
        uint64_t request=0;
        PhysicalContent discard;
        std::lock_guard display(display_->mutex);
        {
            std::lock_guard lock(snapshotMutex_);
            ArmHideLocked();request=hideRequested_.load();
            std::swap(discard,pendingContent_);
        }
        // The owner already acknowledged this hide. A duplicate fallback
        // must preserve the fresh hidden snapshot published by Prepare.
        // Match RequestHide: only a pending request mutates visibility.
        if(!request)return;
        if(child_ && GetPropW(child_,Property)==this)ShowWindow(child_,SW_HIDE);
        hideRequested_.compare_exchange_strong(request,0); // never ack a newer concurrent hide
        Publish(false);
        if(request)Observe("owner-hide",request);
    }
    void Invalidate() noexcept { std::lock_guard display(display_->mutex);BumpEpoch();RequestHide(); }
    bool Prepare() noexcept
    {
        if(!OnOwnerThread()){const auto s=ReadSnapshot();QueueCommand();return Current(s);}
        if(hideRequested_.load())Hide();
        if (closed_ || !parent_ || !child_ || !IsWindow(parent_) || !IsWindow(child_) ||
            GetPropW(parent_, Property) != this || GetPropW(child_, Property) != this || GetParent(child_) != parent_ ||
            GetWindowThreadProcessId(child_, nullptr) != thread_ ||
            !(GetWindowLongPtrW(child_, GWL_STYLE) & WS_CHILD) ||
            (GetWindowLongPtrW(child_, GWL_EXSTYLE) & (WS_EX_NOACTIVATE | WS_EX_TRANSPARENT)) !=
                (WS_EX_NOACTIVATE | WS_EX_TRANSPARENT) ||
            (GetWindowLongPtrW(parent_,GWL_EXSTYLE) & (WS_EX_LAYOUTRTL | WS_EX_LAYERED)) ||
            (GetWindowLongPtrW(child_,GWL_EXSTYLE) & (WS_EX_LAYOUTRTL | WS_EX_LAYERED)) ||
            GetWindowThreadProcessId(parent_, nullptr) != thread_ || !IsWindowVisible(parent_) || IsIconic(parent_) ||
            !AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(), GetWindowDpiAwarenessContext(parent_)))
        { return RefusePreparation(); }
        RECT rect{};
        if (!GetClientRect(parent_, &rect) || rect.right <= 0 || rect.bottom <= 0) return RefusePreparation();
        const UINT width = static_cast<UINT>(rect.right), height = static_cast<UINT>(rect.bottom);
        const UINT dpi = GetDpiForWindow(parent_);
        if (!dpi) return RefusePreparation();
        if (width != width_ || height != height_ || dpi != dpi_)
        {
            Invalidate();
            if (!SetWindowPos(child_, nullptr, 0, 0, width, height, SWP_NOACTIVATE | SWP_NOZORDER)) return RefusePreparation();
            // Commit geometry only after the HWND accepted it. Failed
            // placement must retry on the next owner Prepare.
            width_ = width; height_ = height; dpi_ = dpi;
        }
        Publish(true);return true;
    }
    bool Show(uint64_t epoch) noexcept {
        {
            std::lock_guard lock(snapshotMutex_);
            if(!Current(snapshot_) || snapshot_.epoch!=epoch)return false;
            const auto now=GetTickCount64();
            const bool stable=snapshot_.visible && showEpoch_.load()==epoch &&
                !hideLatched_ && now<showUntil_.load();
            hideLatched_=false;
            showUntil_.store(now+LeaseMs);showEpoch_.store(epoch);
            // Refresh the existing bounded lease, but do not re-run Win32 Show/
            // SetWindowPos or queue another owner ACK on every stable frame.
            if(stable)return true;
        }
        if(!OnOwnerThread()){const bool queued=QueueCommand();Observe("show-queued",epoch,queued?1:0);return queued;} // accepted is not visible acknowledgement
        Observe("show-owner",epoch);ApplyShow(epoch);
        return epoch==epoch_.load() && !hideRequested_.load(); // GPU-pending is accepted, not shown
    }
private:
    void ObserveContent(const char* event,const PhysicalContent& content,uint64_t completed,
                        const ContentSource& required) const noexcept {
        // Owner-only, same bounded Present/visibility evidence; no new collector.
        static thread_local ULONGLONG window=0;
        static thread_local unsigned burst=0;
        const auto now=GetTickCount64();
        if(now-window>=1000){window=now;burst=0;}
        if(burst++>=4)return;
        uintptr_t hiddenQueue=0,hiddenFence=0;
        uint64_t hiddenValue=0,hiddenCompleted=0;
        bool hiddenInFlight=false,hiddenUncertain=false;
        for(const auto& tail:display_->hiddenTails)if(!tail.Complete()) {
            hiddenQueue=reinterpret_cast<uintptr_t>(tail.queue.Get());hiddenFence=reinterpret_cast<uintptr_t>(tail.fence.Get());
            hiddenValue=tail.value;hiddenCompleted=tail.fence?tail.fence->GetCompletedValue():UINT64_MAX;
            hiddenInFlight=tail.inFlight;hiddenUncertain=tail.uncertain;break;
        }
        LOG_INFO("R1069 owner-content: pid={} thread={} event={} source_generation={} source_frame={} instance={} picture={} present={} source_submit={} source_copy={} epoch={} stream={} output_generation={} output_sequence={} chain={:X} color={:X} index={} queue={:X} fence={:X} tail={} completed={} raw_flags={:X} required_generation={} required_frame={} required_instance={} required_picture={} hidden_queue={:X} hidden_fence={:X} hidden_tail={} hidden_completed={} hidden_inflight={} hidden_uncertain={} association=same_thread_sdk_call content_age=UNKNOWN visual=UNKNOWN",
            GetCurrentProcessId(),GetCurrentThreadId(),event,content.source.generation,content.source.sourceFrame,
            content.source.instance,content.source.picture,content.source.present,content.source.submit,content.source.copy,
            content.epoch,content.stream,content.outputGeneration,content.sequence,reinterpret_cast<uintptr_t>(content.chain.Get()),
            content.color,content.index,reinterpret_cast<uintptr_t>(content.queue.Get()),
            reinterpret_cast<uintptr_t>(content.fence.Get()),content.value,completed,content.rawFlags,
            required.generation,required.sourceFrame,required.instance,required.picture,
            hiddenQueue,hiddenFence,hiddenValue,hiddenCompleted,hiddenInFlight,hiddenUncertain);
    }
    bool AcceptContent(PhysicalContent& content) noexcept {
        const auto acceptedEpoch=content.epoch;
        {
            std::lock_guard display(display_->mutex);
            std::lock_guard lock(snapshotMutex_);
            if(!Current(snapshot_) || content.epoch!=epoch_.load() || !content.OutputLive() ||
                !(content.source==parentContent_) || !content.source.Known())return false;
            std::swap(pendingContent_,content); // old COM refs leave the lock in content
        }
        // Show merely schedules owner evaluation; a hidden HWND is revealed only
        // by ApplyShow after the exact physical tail has positively completed.
        return Show(acceptedEpoch);
    }
    bool ApplyShow(uint64_t epoch) noexcept {
        if(!OnOwnerThread())return false;
        std::unique_lock display(display_->mutex,std::try_to_lock);
        if(!display.owns_lock())return false; // existing owner timer retries; no spin
        if(!Prepare() || epoch!=epoch_.load() || GetTickCount64()>=showUntil_.load())return false;
        PhysicalContent content;
        ContentSource required;
        bool alreadyVisible=false;
        {
            std::lock_guard lock(snapshotMutex_);
            if(!Current(snapshot_) || snapshot_.epoch!=epoch || hideRequested_.load())return false;
            alreadyVisible=snapshot_.visible;
            content=pendingContent_;required=parentContent_;
        }
        if(!content.OutputLive()) {
            ObserveContent("physical-output-invalidated",content,0,required);Hide();return false;
        }
        if(!display_->HiddenReady()) {
            ObserveContent("hidden-raw-in-flight-or-tail-pending",content,0,required);
            return false; // no CPU lease/epoch substitutes for any physical queue tail
        }
        if(alreadyVisible)return true; // valid existing visible lease, no new GPU claim
        if(!(content.source==required) || content.epoch!=epoch || !content.primed) {
            if(content.sequence)ObserveContent("source-superseded-before-reveal",content,0,required);
            return false;
        }
        uint64_t completed=0;
        if(!content.Ready(completed)) {
            ObserveContent("gpu-pending-or-failed",content,completed,required);
            if(completed==UINT64_MAX)Hide(); // never turn a removed fence into readiness
            return false; // keep transaction pending, not a visibility failure/reset
        }
        // The display gate prevents a NEW natural parent marker between proof and
        // reveal. If reveal wins, later parents are underneath the visible child;
        // if parent wins, exact source equality above refuses the older reveal.
        const bool shown=SetWindowPos(child_,HWND_TOP,0,0,width_,height_,SWP_NOACTIVATE|SWP_SHOWWINDOW)!=FALSE;
        if(!shown || !content.OutputLive() || epoch!=epoch_.load() || hideRequested_.load() || GetTickCount64()>=showUntil_.load()) {
            Hide();return false;
        }
        Publish(true);ObserveContent("shown-after-physical-tail",content,completed,required);return Visible(epoch);
    }
public:
    // SDK entry is separate from content authority: hidden startup/worker raw
    // can progress, but only an exact ReturnedPicture -> authenticated SDK call
    // -> physical output -> completed tail can enter the owner reveal path.
    class PresentPermit final
    {
        friend class Dx11wDx12SC;
        inline static thread_local PresentPermit* current_ = nullptr;
        GenshinVisibleChild& owner_;
        IDXGISwapChain* swapchain_;
        ContentSource source_;
        uint64_t epoch_;
        bool eligible_,armed_=false,called_=false,committed_=false,continuityAccepted_=false;
        bool sdkEntered_=false,physicalFailed_=false,physicalRawFailed_=false,physicalTailFailed_=false;
        bool physicalAdmissionPending_=false;
        unsigned physicalCount_=0,hiddenRawCount_=0;
        PhysicalContent physical_;
        HRESULT actualResult_=E_PENDING;
        const char* commitStage_="not-called";
        static uintptr_t Key(IUnknown* object) noexcept {
            Microsoft::WRL::ComPtr<IUnknown> identity;
            return object && SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&identity)))
                ? reinterpret_cast<uintptr_t>(identity.Get()):0;
        }
        PresentPermit(GenshinVisibleChild& owner,IDXGISwapChain* sc,uint64_t epoch,
                      HRESULT nativeResult,bool nativePreserved,const ContentSource& source)
            :owner_(owner),swapchain_(sc),source_(source),epoch_(epoch),
             eligible_(nativeResult==S_OK && nativePreserved && source.Known() && current_==nullptr)
        {if(eligible_)current_=this;}
    public:
        PresentPermit(const PresentPermit&)=delete;
        PresentPermit& operator=(const PresentPermit&)=delete;
        static void BeforePresent(IDXGISwapChain* sc,bool authenticatedDispatch) noexcept {
            auto* p=current_;
            if(!p || !p->eligible_ || !Key(sc) || Key(sc)!=Key(p->swapchain_))return;
            p->armed_=authenticatedDispatch && p->owner_.Epoch()==p->epoch_;
            p->sdkEntered_=true; // actual SDK entry includes startup/paused; it is NOT display authority
            if(!p->armed_)p->owner_.Hide();
        }
        static void AfterPresent(IDXGISwapChain* sc,HRESULT result) noexcept {
            auto* p=current_;
            if(!p || !Key(sc) || Key(sc)!=Key(p->swapchain_))return;
            p->sdkEntered_=false;p->called_=true;p->actualResult_=result;
            if(!p->armed_ || result!=S_OK)p->owner_.Hide();
        }
        // The captured Genshin endpoint may advance an acknowledged hidden
        // child without a source. It must never borrow newest source/TLS to turn
        // that bootstrap or vendor worker output into a reveal receipt.
        class PhysicalOutput final {
            PresentPermit* permit_=nullptr;
            PhysicalContent content_;
            std::shared_ptr<DisplayGate> hiddenDisplay_;
            size_t hiddenSlot_=0;
            bool submitted_=false,hiddenFinished_=false,hiddenRawEntered_=false;
            void FinishHidden(bool rawCalled,bool tailKnown,ID3D12Fence* fence,uint64_t value) noexcept {
                if(!hiddenDisplay_ || hiddenFinished_)return;
                std::lock_guard display(hiddenDisplay_->mutex);
                auto& tail=hiddenDisplay_->hiddenTails[hiddenSlot_];
                if(rawCalled) {
                    if(tailKnown && fence==tail.fence.Get() && value && value!=UINT64_MAX)tail.value=value;
                    else tail.uncertain=true; // real raw without a known tail never authorizes reveal
                }
                tail.inFlight=false;hiddenFinished_=true;
            }
        public:
            static void Refused(const char* reason,IDXGISwapChain* chain,
                                ID3D12CommandQueue* queue=nullptr,ID3D12Resource* color=nullptr,
                                UINT index=UINT_MAX,uint64_t stream=0,uint64_t generation=0,
                                uint64_t sequence=0,UINT inputFlags=0) noexcept {
                if(auto* p=current_)p->physicalAdmissionPending_=true;
                static std::atomic<uint64_t> reports{0};const auto n=++reports;
                static thread_local ULONGLONG lastReport=0;const auto now=GetTickCount64();
                if(n>8 && n%600!=0 && now-lastReport<1000)return;
                lastReport=now;
                const auto* p=current_;
                LOG_INFO("R1069 physical-output refusal: pid={} thread={} reason={} chain={:X} queue={:X} color={:X} index={} stream={} output_generation={} endpoint_attempt={} input_flags={:X} call_source_known={} call_generation={} call_source_frame={} call_instance={} sdk_entered={} dispatch_authorized={} call_source_thread={} report={} raw_called=false source_association=UNKNOWN visual=UNKNOWN",
                    GetCurrentProcessId(),GetCurrentThreadId(),reason,Key(chain),Key(queue),Key(color),index,
                    stream,generation,sequence,inputFlags,p!=nullptr,p?p->source_.generation:0,
                    p?p->source_.sourceFrame:0,p?p->source_.instance:0,p && p->sdkEntered_,p && p->armed_,
                    p?p->source_.thread:0,n);
            }
            bool Bind(IDXGISwapChain3* chain,ID3D12CommandQueue* queue,ID3D12Resource* color,
                      UINT index,uint64_t stream,uint64_t generation,uint64_t sequence,
                      std::shared_ptr<std::atomic<bool>>& outputLive,ID3D12Fence* fence,UINT inputFlags) noexcept {
                auto* p=current_;permit_=p;
                const auto refuse=[&](const char* reason){
                    Refused(reason,chain,queue,color,index,stream,generation,sequence,inputFlags);return false;
                };
                DXGI_SWAP_CHAIN_DESC d{};
                Microsoft::WRL::ComPtr<ID3D12Device> device,colorDevice,chainDevice;
                if(!chain || !queue || !color || !fence || FAILED(chain->GetDesc(&d)) || !d.Windowed ||
                    d.SwapEffect!=DXGI_SWAP_EFFECT_FLIP_DISCARD || !IsPhysicalOutput(chain,d.OutputWindow) ||
                    queue->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT ||
                    FAILED(queue->GetDevice(IID_PPV_ARGS(&device))) ||
                    FAILED(color->GetDevice(IID_PPV_ARGS(&colorDevice))) ||
                    FAILED(chain->GetDevice(IID_PPV_ARGS(&chainDevice))) || !Key(device.Get()) ||
                    Key(colorDevice.Get())!=Key(device.Get()) || Key(chainDevice.Get())!=Key(device.Get()) ||
                    FAILED(device->GetDeviceRemovedReason()))return refuse("PhysicalColorOrCreationQueueMismatch");
                auto gate=retainedDisplay_.load(std::memory_order_acquire);
                if(!gate)return refuse("NoRetainedDisplayOwner");
                std::unique_lock display(gate->mutex,std::try_to_lock);
                if(!display.owns_lock())return refuse("OwnerOrParentBoundaryBusy");
                if(!gate->live || gate->child!=d.OutputWindow || !IsRetainedChild(d.OutputWindow))
                    return refuse("PhysicalWindowLifetimeChanged");
                bool authenticated=p && p->eligible_ && p->armed_ && p->sdkEntered_ && !p->physicalFailed_ &&
                    p->source_.thread==GetCurrentThreadId() && p->owner_.display_==gate &&
                    Key(device.Get())==p->source_.device12;
                if(authenticated) {
                    std::lock_guard lock(p->owner_.snapshotMutex_);
                    authenticated=p->epoch_==p->owner_.epoch_.load() && !p->owner_.hideRequested_.load() &&
                        p->source_==p->owner_.parentContent_;
                }
                content_.stream=stream;content_.outputGeneration=generation;content_.sequence=sequence;content_.index=index;
                content_.chain=chain;content_.queue=queue;content_.color=Key(color);content_.device=device;
                if(!authenticated || gate->RawInFlight()) {
                    // Never borrow a current/latest source for startup or a vendor
                    // worker. Only the owner's acknowledged hidden child may advance.
                    if(!gate->hiddenAcknowledged || (GetWindowLongPtrW(d.OutputWindow,GWL_STYLE)&WS_VISIBLE))
                        return refuse("UnauthenticatedOutputWaitingForOwnerHide");
                    size_t slot=gate->hiddenTails.size();
                    for(size_t i=0;i<gate->hiddenTails.size();++i) {
                        const auto& tail=gate->hiddenTails[i];
                        if(!tail.inFlight && !tail.uncertain && tail.queue.Get()==queue && tail.fence.Get()==fence) {slot=i;break;}
                    }
                    if(slot==gate->hiddenTails.size())for(size_t i=0;i<gate->hiddenTails.size();++i)
                        if(gate->hiddenTails[i].Complete()){slot=i;break;}
                    if(slot==gate->hiddenTails.size())return refuse("HiddenPhysicalTailCapacityOrUncertain");
                    auto& tail=gate->hiddenTails[slot];
                    if(tail.queue.Get()!=queue || tail.fence.Get()!=fence) {
                        tail.queue=queue;tail.fence=fence;tail.device=device;tail.value=0;tail.uncertain=false;
                    }
                    tail.inFlight=true;hiddenSlot_=slot;hiddenDisplay_=gate;
                    // Revoke every older reveal receipt for this HWND before raw,
                    // including receipts from a different captured physical chain.
                    if(auto live=gate->outputLive.lock())live->store(false,std::memory_order_release);
                    gate->outputLive.reset();
                    if(p)p->physicalFailed_=true; // this SDK call cannot later launder hidden work into content
                    return true;
                }
                if(p->physicalCount_ && (Key(p->physical_.chain.Get())!=Key(chain) ||
                    Key(p->physical_.queue.Get())!=Key(queue))) {
                    p->physicalFailed_=true;return refuse("AmbiguousPhysicalOutputOrder");
                }
                if(!outputLive || !outputLive->load(std::memory_order_acquire)) {
                    try{outputLive=std::make_shared<std::atomic<bool>>(true);}
                    catch(...){return refuse("PhysicalLifetimeAllocationFailed");}
                }
                if(auto old=gate->outputLive.lock();old && old!=outputLive)old->store(false,std::memory_order_release);
                gate->outputLive=outputLive;
                content_.source=p->source_;content_.epoch=p->epoch_;content_.outputLive=outputLive;
                content_.primed=gate->hiddenAcknowledged;return true;
            }
            bool HiddenOnly() const noexcept {return hiddenDisplay_!=nullptr;}
            void RawEntered() noexcept {hiddenRawEntered_=true;}
            bool Prime() const noexcept {return content_.primed;}
            bool MayPresent() const noexcept {
                if(hiddenDisplay_) {
                    std::unique_lock display(hiddenDisplay_->mutex,std::try_to_lock);
                    return display.owns_lock() && hiddenDisplay_->live && hiddenDisplay_->hiddenAcknowledged &&
                        hiddenDisplay_->child==retainedChild_.load() &&
                        !(GetWindowLongPtrW(hiddenDisplay_->child,GWL_STYLE)&WS_VISIBLE);
                }
                if(!permit_)return false;
                std::unique_lock display(permit_->owner_.display_->mutex,std::try_to_lock);
                if(!display.owns_lock())return false;
                std::lock_guard lock(permit_->owner_.snapshotMutex_);
                return content_.OutputLive() && permit_->sdkEntered_ && permit_->epoch_==permit_->owner_.epoch_.load() &&
                    !permit_->owner_.hideRequested_.load() &&
                    permit_->source_==permit_->owner_.parentContent_;
            }
            void Submitted(bool rawCalled,UINT rawFlags,HRESULT rawResult,bool accepted,bool tailValid,
                           ID3D12Fence* fence,uint64_t value) noexcept {
                const uint64_t completed=fence?fence->GetCompletedValue():UINT64_MAX;
                tailValid=tailValid && fence && value && value!=UINT64_MAX && completed!=UINT64_MAX;
                if(permit_ && rawCalled && !tailValid)permit_->physicalTailFailed_=true;
                if(hiddenDisplay_) {
                    if(permit_) {
                        if(rawCalled)++permit_->hiddenRawCount_;
                        if(rawCalled && FAILED(rawResult) && rawResult!=DXGI_ERROR_WAS_STILL_DRAWING)
                            permit_->physicalRawFailed_=true;
                    }
                    // Same endpoint tail and quarantine result as authenticated
                    // output. No source/physical-content receipt is minted here.
                    FinishHidden(rawCalled,tailValid,fence,value);
                    static std::atomic<uint64_t> reports{0};const auto n=++reports;
                    static std::atomic<bool> failureReported{false};
                    const bool firstFailure=rawCalled && (!tailValid || (FAILED(rawResult) && rawResult!=DXGI_ERROR_WAS_STILL_DRAWING)) && !failureReported.exchange(true);
                    if(n<=16 || firstFailure || n%600==0)
                        LOG_INFO("R1069 hidden-physical: pid={} thread={} chain={:X} color={:X} index={} queue={:X} stream={} output_generation={} output_sequence={} sdk_entered={} dispatch_authorized={} raw_called={} raw_flags={:X} raw_hr={:X} tail_fence={:X} tail={} completed={} tail_known={} owner_hidden_ack_at_admission=true display_authorized=false source_association=UNKNOWN visual=UNKNOWN",
                            GetCurrentProcessId(),GetCurrentThreadId(),Key(content_.chain.Get()),content_.color,content_.index,
                            Key(content_.queue.Get()),content_.stream,content_.outputGeneration,content_.sequence,
                            permit_ && permit_->sdkEntered_,permit_ && permit_->armed_,rawCalled,rawFlags,
                            static_cast<unsigned>(rawResult),Key(fence),value,completed,rawCalled && tailValid);
                    return;
                }
                if(!permit_)return;
                auto* p=permit_;
                bool sourceCurrent=false;
                {
                    std::lock_guard lock(p->owner_.snapshotMutex_);
                    sourceCurrent=p->source_==p->owner_.parentContent_ && !p->owner_.hideRequested_.load();
                }
                content_.rawFlags=rawFlags;
                if(rawCalled && FAILED(rawResult) && rawResult!=DXGI_ERROR_WAS_STILL_DRAWING)
                    p->physicalRawFailed_=true; // never disguise a real raw failure as source admission
                const bool valid=content_.OutputLive() && rawCalled && accepted && tailValid && rawResult==S_OK &&
                    sourceCurrent && p->sdkEntered_ && p->owner_.Epoch()==p->epoch_;
                submitted_=valid;
                if(submitted_) {
                    content_.fence=fence;content_.value=value;++p->physicalCount_;
                    p->physical_=content_; // bounded observer refs; endpoint retains real GPU custody
                } else p->physicalFailed_=true;
                static std::atomic<uint64_t> reports{0};const auto n=++reports;
                static std::atomic<bool> failureReported{false};
                const bool firstFailure=!submitted_ && !failureReported.exchange(true);
                if(n<=16 || firstFailure || n%600==0)
                    LOG_INFO("R1069 physical-content: pid={} thread={} source_generation={} source_frame={} picture={} present={} instance={} source_submit={} source_copy={} source_history={} source_config={} source_owner={} source_device={} epoch={} chain={:X} color={:X} index={} queue={:X} stream={} output_generation={} output_sequence={} raw_called={} raw_flags={:X} raw_hr={:X} tail_fence={:X} tail={} completed={} primed={} accepted={} gpu_ready={} association=same_thread_sdk_call content_age=UNKNOWN visual=UNKNOWN",
                        GetCurrentProcessId(),GetCurrentThreadId(),content_.source.generation,content_.source.sourceFrame,
                        content_.source.picture,content_.source.present,content_.source.instance,content_.source.submit,content_.source.copy,
                        content_.source.historyEpoch,content_.source.configEpoch,content_.source.ownerEpoch,content_.source.deviceEpoch,
                        content_.epoch,Key(content_.chain.Get()),content_.color,content_.index,Key(content_.queue.Get()),
                        content_.stream,content_.outputGeneration,content_.sequence,rawCalled,rawFlags,static_cast<unsigned>(rawResult),Key(fence),value,completed,
                        content_.primed,submitted_,submitted_ && completed>=value);
            }
            ~PhysicalOutput(){
                // An unfinished raw poisons the observation; only a reservation
                // that never entered raw can cancel without a real tail.
                if(hiddenDisplay_ && hiddenRawEntered_ && !hiddenFinished_ && permit_)permit_->physicalTailFailed_=true;
                FinishHidden(hiddenRawEntered_,false,nullptr,0);
                if(permit_ && !submitted_)permit_->physicalFailed_=true;
            }
        };
        bool Called() const noexcept {return called_;}
        bool HasPhysicalOutput() const noexcept {return physicalCount_ && !physicalFailed_;}
        bool ContinuityAccepted() const noexcept {return continuityAccepted_;}
        bool PhysicalFailure() const noexcept {return physicalRawFailed_ || physicalTailFailed_;}
        bool OutputAdmissionPending() const noexcept {return physicalAdmissionPending_;}
        unsigned HiddenRawCount() const noexcept {return hiddenRawCount_;}
        const char* CommitStage() const noexcept {return commitStage_;}
        HRESULT ActualResult() const noexcept {return actualResult_;}
        bool Commit() noexcept {
            continuityAccepted_=(commitStage_="eligible",eligible_) && (commitStage_="armed",armed_) &&
                (commitStage_="called",called_) && (commitStage_="actual-result",actualResult_==S_OK) &&
                (commitStage_="physical-content",HasPhysicalOutput()) &&
                (commitStage_="owner-epoch",owner_.Epoch()==epoch_) &&
                (commitStage_="accept-content",owner_.AcceptContent(physical_));
            committed_=continuityAccepted_ && owner_.Visible(epoch_,&commitStage_);
            if(committed_)commitStage_="committed";
            return committed_;
        }
        ~PresentPermit() {
            if(!continuityAccepted_)owner_.Hide();
            if(current_==this)current_=nullptr;
        }
    };

    ~GenshinVisibleChild()
    {
        // Deferred final retirement runs this destructor on the actual HWND owner.
        {
            std::lock_guard display(display_->mutex);
            closed_=true;display_->live=false;display_->hiddenAcknowledged=false;
        }
        Hide();
        if(ownedChild_)ReleasePhysicalOutputs();
        if (subclassed_ && parent_ && GetPropW(parent_, Property) == this)
            RemoveWindowSubclass(parent_, ParentProc, reinterpret_cast<UINT_PTR>(this));
        if (parent_ && GetPropW(parent_, Property) == this) RemovePropW(parent_, Property);
        if (child_) {KillTimer(child_,LeaseTimer);DestroyWindow(child_);}
        if (classRegistered_) UnregisterClassW(className_.c_str(), module_);
        if (ownedChild_) {
            auto expectedDisplay=display_;
            retainedDisplay_.compare_exchange_strong(expectedDisplay,std::shared_ptr<DisplayGate>{});
            void* expected=this;retainedOwner_.compare_exchange_strong(expected,nullptr);
            retainedChild_.compare_exchange_strong(ownedChild_, nullptr);
        }
    }
};
