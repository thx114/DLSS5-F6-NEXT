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
#include <inputs/FG/GenshinFgTicket.h>
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
        bool Matches(const GenshinBridgeFg::Key& k) const noexcept {
            return Known() && k.Known() && device11==k.device && context11==k.context && native==k.swapchain &&
                backbuffer==k.backbuffer && selected==k.selected && deviceEpoch==k.deviceEpoch &&
                ownerEpoch==k.ownerEpoch && configEpoch==k.configEpoch && historyEpoch==k.historyEpoch &&
                instance==k.instance && generation==k.generation && sourceFrame==k.sourceFrame &&
                picture==k.picture && present==k.present && submit==k.submitSerial && copy==k.copyOrderedSerial &&
                thread==k.thread && subresource==k.selectedSubresource;
        }
        bool operator==(const ContentSource& b) const noexcept {
            return std::tie(device11,context11,device12,native,backbuffer,selected,deviceEpoch,ownerEpoch,
                configEpoch,historyEpoch,instance,generation,sourceFrame,picture,present,submit,copy,thread,subresource)==
                std::tie(b.device11,b.context11,b.device12,b.native,b.backbuffer,b.selected,b.deviceEpoch,b.ownerEpoch,
                b.configEpoch,b.historyEpoch,b.instance,b.generation,b.sourceFrame,b.picture,b.present,b.submit,b.copy,b.thread,b.subresource);
        }
    };
private:
    // A raw output belongs to a captured SDK stream. No input picture/token is
    // attached: the SDK headers expose no such join for generated worker frames.
    struct PhysicalContent {
        GenshinBridgeFg::SdkStreamOwner streamOwner{};
        std::shared_ptr<std::atomic<bool>> streamLive;
        uint64_t epoch=0,stream=0,outputGeneration=0,sequence=0,value=0;
        ULONGLONG until=0; // observation freshness only, never resource retirement
        UINT index=0,rawFlags=0;
        bool primed=false,revealed=false;
        Microsoft::WRL::ComPtr<IDXGISwapChain3> chain;
        uintptr_t color=0; // no observer backbuffer ref may block ResizeBuffers
        std::shared_ptr<std::atomic<bool>> outputLive;
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        Microsoft::WRL::ComPtr<ID3D12Device> device;
        GenshinBridgeFg::PhysicalStream Identity() const noexcept {
            return {stream,outputGeneration,reinterpret_cast<uintptr_t>(chain.Get()),
                reinterpret_cast<uintptr_t>(queue.Get()),reinterpret_cast<uintptr_t>(fence.Get())};
        }
        bool StreamKnown() const noexcept {
            return streamOwner.Known() && streamLive && streamLive->load(std::memory_order_acquire);
        }
        bool OutputLive() const noexcept {return outputLive && outputLive->load(std::memory_order_acquire);}
        bool Ready(uint64_t& completed) const noexcept {
            if(!StreamKnown() || !OutputLive() || !Identity().Known() || !color || !device ||
                !value || value==UINT64_MAX)return false;
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
        std::recursive_timed_mutex mutex;
        HWND child=nullptr;
        bool live=false,hiddenAcknowledged=false;
        std::weak_ptr<std::atomic<bool>> outputLive;
        // Bound before the real SDK call; accepted input activates reveal only
        // after its saved S_OK and the wrapper's post-call validation. A raw may
        // finish earlier and leave one bounded observation in pendingContent_.
        GenshinVisibleChild* owner=nullptr; // dereferenced only under mutex
        GenshinBridgeFg::SdkOutputBinding binding;
        uint64_t scopeEpoch=0;
        GenshinBridgeFg::SdkStreamLease inputLease;
        std::shared_ptr<std::atomic<bool>> streamLive;
        GenshinBridgeFg::OutputRevealWindow revealWindow;
        void RevokeStream() noexcept {
            if(streamLive)streamLive->store(false,std::memory_order_release);
            streamLive.reset();inputLease.Revoke();revealWindow.Release();
        }
        struct RawTail {
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
        std::array<RawTail,4> hiddenTails;
        // Same actual queue/fence as the bound physical stream. Keep the LAST
        // raw tail even when pendingContent_ retains an earlier output receipt.
        // Later raws remain admitted; a CPU return only clears inFlight.
        RawTail streamTail;
        bool HiddenReady() noexcept {
            bool ready=streamTail.Complete();
            for(auto& tail:hiddenTails) {
                if(!tail.Complete())ready=false;
                else tail={}; // release observations only after actual completion (or no raw)
            }
            return ready;
        }
        bool RawInFlight() const noexcept {
            if(streamTail.inFlight)return true;
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
        display_->RevokeStream();
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
        display_->child=child_;display_->owner=this;
        display_->live=child_ && !closed_ && GetPropW(child_,Property)==this;
        display_->hiddenAcknowledged=display_->live && !(GetWindowLongPtrW(child_,GWL_STYLE)&WS_VISIBLE);
        const bool visible=child_ && IsWindowVisible(child_)!=FALSE;
        std::lock_guard lock(snapshotMutex_);
        snapshot_={parent_,child_,epoch_.load(),width_,height_,dpi_,GetTickCount64(),
            ready && !closed_ && !hideRequested_.load(),visible};
        display_->revealWindow.OwnerChanged();
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
        return s.ready && s.parent==GetForegroundWindow() && s.epoch && s.epoch!=UINT64_MAX && s.epoch==epoch_.load() &&
            !hideRequested_.load() && GetTickCount64()-s.published<=SnapshotMs;
    }
    void OwnerCommand() noexcept {
        commandPosted_.store(false);if(showEpoch_.load() || hideRequested_.load())Observe("owner-command");
        if(hideRequested_.load())Hide();
        if(GetForegroundWindow()!=parent_){Hide();Publish(false);return;}
        if(!Prepare()){Observe("owner-prepare-refused");return;}
        const auto e=showEpoch_.load();
        if(e && e==epoch_.load())ApplyShow(e);
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
            self->OwnerCommand();
            if(GetTickCount64()>=self->showUntil_.load() && IsWindowVisible(hwnd)) {
                self->Observe("timer-hide");self->ExpireVisibleOutput();
            }
            return 0;
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
        std::lock_guard display(display_->mutex);
        WindowSnapshot s;
        bool outputLive=false;
        {std::lock_guard lock(snapshotMutex_);s=snapshot_;
            outputLive=pendingContent_.epoch==epoch && StreamCurrent(pendingContent_);}
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
    // Authenticate the next SDK input against the actual natural parent.
    // This does not label an asynchronous output with that input's picture;
    // output reveal remains bound to its captured SDK stream and physical tail.
    void ParentWillPresent(const ContentSource& source,bool exactDuplicate=false) noexcept {
        if(exactDuplicate)return;
        std::lock_guard display(display_->mutex);
        std::lock_guard lock(snapshotMutex_);
        parentContent_=source;
    }
    // Called before constructing a new SDK input scope. Pausing inputs here
    // avoids making a synchronous SDK Present fail merely to drain its output.
    // The caller still presents the natural parent exactly once. Workers can
    // finish an already-admitted raw, but cannot continually move the last tail.
    bool YieldForOutputReveal() noexcept {
        std::lock_guard display(display_->mutex);
        uint64_t epoch=0;
        {
            std::lock_guard snapshot(snapshotMutex_);
            const auto now=GetTickCount64();
            if(pendingContent_.revealed || !Current(snapshot_) || !StreamCurrent(pendingContent_) ||
                now>=pendingContent_.until || !display_->revealWindow.Hold(pendingContent_.sequence,
                    now,pendingContent_.until,display_->inputLease.Until()))return false;
            epoch=pendingContent_.epoch;
        }
        RequestReveal(epoch); // owner is try-only; expiry resumes admission without a Show
        return true;
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
    // An old visible output expiring is not loss of the SDK creation owner.
    // Hide its pixels, but preserve a still-current input transaction/stream so
    // a slow SDK call can finish without revoking its asynchronous worker.
    void ExpireVisibleOutput() noexcept {
        PhysicalContent discard; // release observer COM refs after display unlock
        std::lock_guard display(display_->mutex);
        if(!display_->inputLease.Current(GetTickCount64())){Hide();return;}
        {
            std::lock_guard snapshot(snapshotMutex_);
            std::swap(discard,pendingContent_);
            showEpoch_.store(0);showUntil_.store(0);
            display_->revealWindow.Release();
        }
        if(child_ && GetPropW(child_,Property)==this)ShowWindow(child_,SW_HIDE);
        Publish(true); // only acknowledges HWND state; no fence or reveal authority
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
private:
    bool StreamCurrent(const PhysicalContent& content,bool requireInput=true) const noexcept {
        return display_->live && display_->owner==this && content.StreamKnown() && content.OutputLive() &&
            content.streamLive==display_->streamLive && content.streamOwner==display_->binding.Owner() &&
            display_->binding.Matches(content.streamOwner.outputOwner,content.Identity()) &&
            display_->scopeEpoch==content.epoch && content.epoch==epoch_.load() && !hideRequested_.load() &&
            (!requireInput || display_->inputLease.Accepted()) && display_->inputLease.Current(GetTickCount64());
    }
    // Queue only an owner evaluation. In particular, SDK S_OK and raw CPU return
    // cannot start or renew the visible lease; ApplyShow requires the real tail.
    bool RequestReveal(uint64_t epoch) noexcept {
        {
            std::lock_guard lock(snapshotMutex_);
            if(!Current(snapshot_) || snapshot_.epoch!=epoch)return false;
            showEpoch_.store(epoch);
        }
        return QueueCommand(); // timer retries a failed/delayed owner message
    }
    void ObserveContent(const char* event,const PhysicalContent& content,uint64_t completed) const noexcept {
        static thread_local ULONGLONG window=0;
        static thread_local unsigned burst=0;
        const auto now=GetTickCount64();
        if(now-window>=1000){window=now;burst=0;}
        if(burst++>=4)return;
        LOG_INFO("F6 owner-output: pid={} thread={} event={} provider={} output_owner={} epoch={} stream={} generation={} sequence={} chain={:X} color={:X} index={} queue={:X} fence={:X} tail={} completed={} raw_flags={:X} last_raw_queue={:X} last_raw_fence={:X} last_raw_tail={} last_raw_completed={} last_raw_inflight={} last_raw_uncertain={} input_accepted={} association=sdk_output_stream source=UNKNOWN kind=UNKNOWN visual=UNKNOWN",
            GetCurrentProcessId(),GetCurrentThreadId(),event,content.streamOwner.provider,content.streamOwner.outputOwner,
            content.epoch,content.stream,content.outputGeneration,content.sequence,reinterpret_cast<uintptr_t>(content.chain.Get()),
            content.color,content.index,reinterpret_cast<uintptr_t>(content.queue.Get()),
            reinterpret_cast<uintptr_t>(content.fence.Get()),content.value,completed,content.rawFlags,
            reinterpret_cast<uintptr_t>(display_->streamTail.queue.Get()),reinterpret_cast<uintptr_t>(display_->streamTail.fence.Get()),
            display_->streamTail.value,display_->streamTail.fence?display_->streamTail.fence->GetCompletedValue():UINT64_MAX,
            display_->streamTail.inFlight,display_->streamTail.uncertain,display_->inputLease.Accepted());
    }
    bool AcceptContent(PhysicalContent& content) noexcept {
        const auto epoch=content.epoch;
        std::lock_guard display(display_->mutex);
        {
            std::lock_guard lock(snapshotMutex_);
            if(!Current(snapshot_) || !StreamCurrent(content,false))return false;
            // Keep one still-fresh, unrevealed output. ApplyShow checks both
            // its tail and the last raw tail; the bounded admission window
            // prevents continuous tail chasing between SDK input calls.
            // This is output-stream ordering, never a FIFO join to SDK inputs.
            if(!pendingContent_.revealed && StreamCurrent(pendingContent_,false) &&
                GetTickCount64()<pendingContent_.until)return true;
            std::swap(pendingContent_,content); // endpoint retains all GPU custody
        }
        return !display_->inputLease.Accepted() || RequestReveal(epoch);
    }
    bool ApplyShow(uint64_t epoch) noexcept {
        if(!OnOwnerThread())return false;
        std::unique_lock display(display_->mutex,std::try_to_lock);
        if(!display.owns_lock())return false;
        if(!Prepare() || epoch!=epoch_.load())return false;
        PhysicalContent content;
        bool alreadyVisible=false;
        {
            std::lock_guard lock(snapshotMutex_);
            if(!Current(snapshot_) || snapshot_.epoch!=epoch || hideRequested_.load())return false;
            alreadyVisible=snapshot_.visible;content=pendingContent_;
        }
        if(!StreamCurrent(content)){Hide();return false;}
        if(content.revealed)return alreadyVisible; // no lease renewal from the same receipt
        if(GetTickCount64()>=content.until) {
            ObserveContent("output-observation-expired",content,0);return false;
        }
        const auto& lastRaw=display_->streamTail;
        if(lastRaw.queue.Get()!=content.queue.Get() || lastRaw.fence.Get()!=content.fence.Get() ||
            lastRaw.value<content.value || !display_->HiddenReady()) {
            ObserveContent("last-physical-raw-tail-pending",content,0);return false;
        }
        if(!alreadyVisible && (!content.primed || !display_->hiddenAcknowledged))return false;
        uint64_t completed=0;
        if(!content.Ready(completed)) {
            ObserveContent("physical-tail-pending-or-failed",content,completed);
            if(completed==UINT64_MAX)Hide();
            return false;
        }
        // Bind/raw/revocation use this same gate. No new physical raw can cross
        // the completed-tail proof and owner reveal. This is not DWM/pixel proof.
        const bool shown=alreadyVisible ||
            SetWindowPos(child_,HWND_TOP,0,0,width_,height_,SWP_NOACTIVATE|SWP_SHOWWINDOW)!=FALSE;
        if(!shown || !StreamCurrent(content) || GetTickCount64()>=content.until){Hide();return false;}
        {
            std::lock_guard lock(snapshotMutex_);
            pendingContent_.revealed=true;hideLatched_=false;
            display_->revealWindow.Release();
            showUntil_.store(GetTickCount64()+LeaseMs);
        }
        Publish(true);ObserveContent("shown-after-physical-tail",content,completed);return Visible(epoch);
    }
public:
    // Input and output are separate transactions. Hooks save the exact SDK
    // bracket on its calling thread. PhysicalOutput uses only the captured
    // provider/stream/queue identity, including for nested synchronous output.
    class PresentPermit final
    {
        friend class Dx11wDx12SC;
        inline static thread_local PresentPermit* current_=nullptr;
        GenshinVisibleChild& owner_;
        IDXGISwapChain* swapchain_;
        ContentSource source_;
        std::shared_ptr<const GenshinBridgeFg::SdkInputSource> sdkSource_;
        std::shared_ptr<std::atomic<bool>> streamScope_;
        uint64_t epoch_;
        bool eligible_,armed_=false,called_=false,accepted_=false,sdkEntered_=false;
        bool physicalRawFailed_=false,physicalTailFailed_=false,physicalAdmissionPending_=false;
        unsigned hiddenRawCount_=0;
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
        bool ArmStream() noexcept {
            std::lock_guard display(owner_.display_->mutex);
            auto& gate=*owner_.display_;
            std::lock_guard snapshot(owner_.snapshotMutex_);
            const auto binding=sdkSource_->StreamOwner();
            if(!gate.live || gate.owner!=&owner_ || !owner_.Current(owner_.snapshot_) ||
                owner_.Epoch()!=epoch_ || owner_.hideRequested_.load() || source_!=owner_.parentContent_ ||
                !sdkSource_->Matches(sdkSource_->Identity(),sdkSource_->SdkFrame()) ||
                owner_.snapshot_.width!=binding.width || owner_.snapshot_.height!=binding.height ||
                (gate.streamLive && gate.scopeEpoch!=epoch_) ||
                !gate.binding.BindOwner(binding,source_.device12))return false;
            if(!gate.streamLive) {
                try{gate.streamLive=std::make_shared<std::atomic<bool>>(true);}catch(...){return false;}
                gate.scopeEpoch=epoch_;
            }
            // This exact fresh parent/dispatch renews the stream before entry.
            // A display freshness timeout alone cannot reject the new input.
            if(!gate.inputLease.Begin(GetTickCount64(),LeaseMs))return false;
            streamScope_=gate.streamLive;return true;
        }
    public:
        PresentPermit(const PresentPermit&)=delete;
        PresentPermit& operator=(const PresentPermit&)=delete;
        static void BeforePresent(IDXGISwapChain* sc,std::shared_ptr<const GenshinBridgeFg::SdkInputSource> source) noexcept {
            auto* p=current_;
            if(!p || !p->eligible_ || p->sdkEntered_ || p->called_ || !Key(sc) || Key(sc)!=Key(p->swapchain_))return;
            p->sdkSource_=std::move(source);
            p->armed_=p->sdkSource_ && p->source_.Matches(p->sdkSource_->Identity()) && p->ArmStream();
            p->sdkEntered_=true;
            // Provisional scope precedes the call: a worker may finish before
            // S_OK. Such a receipt is retained, but cannot reveal the child yet.
            if(!p->armed_)p->owner_.Hide();
        }
        static void AfterPresent(IDXGISwapChain* sc,HRESULT result) noexcept {
            auto* p=current_;
            if(!p || !p->sdkEntered_ || !Key(sc) || Key(sc)!=Key(p->swapchain_))return;
            p->sdkEntered_=false;p->called_=true;p->actualResult_=result;
            if(!p->armed_ || result!=S_OK)p->owner_.Hide();
        }
        bool AcceptStream() noexcept {
            commitStage_="sdk-input";
            if(current_!=this || !eligible_ || !armed_ || !called_ || sdkEntered_ || actualResult_!=S_OK ||
                PhysicalFailure() || !sdkSource_)return false;
            std::lock_guard display(owner_.display_->mutex);
            auto& gate=*owner_.display_;
            bool activate=false,hasPending=false;
            {
                std::lock_guard snapshot(owner_.snapshotMutex_);
                commitStage_="stream-scope";
                if(!gate.live || gate.owner!=&owner_ || !streamScope_ || gate.streamLive!=streamScope_ ||
                    !streamScope_->load(std::memory_order_acquire) || gate.binding.Owner()!=sdkSource_->StreamOwner() ||
                    gate.scopeEpoch!=epoch_ || owner_.Epoch()!=epoch_ || owner_.hideRequested_.load() ||
                    !owner_.Current(owner_.snapshot_) || source_!=owner_.parentContent_ ||
                    !gate.inputLease.Current(GetTickCount64()))return false;
                activate=!gate.inputLease.Accepted();
                if(!gate.inputLease.Accept(GetTickCount64(),LeaseMs))return false;
                accepted_=true;commitStage_="sdk-input-accepted";
                hasPending=owner_.StreamCurrent(owner_.pendingContent_) &&
                    !owner_.pendingContent_.revealed && GetTickCount64()<owner_.pendingContent_.until;
            }
            // Handles raw-before-return without inventing another raw or using
            // an input fence. The owner still checks this saved output's tail.
            if(activate && hasPending)owner_.RequestReveal(epoch_);
            return true;
        }
        class PhysicalOutput final {
            PresentPermit* inputPermit_=nullptr; // same-stack diagnostics only; never retained on a worker
            PhysicalContent content_;
            std::shared_ptr<DisplayGate> display_;
            size_t hiddenSlot_=0;
            bool scoped_=false,rawEntered_=false,finished_=false;
            ULONGLONG handoffUntil_=0;
            ULONGLONG HandoffBudget() const noexcept {
                const auto now=GetTickCount64();return now<handoffUntil_?handoffUntil_-now:0;
            }
            void FinishHidden(bool tailKnown,uint64_t value) noexcept {
                auto& tail=display_->hiddenTails[hiddenSlot_];
                if(rawEntered_) {
                    if(tailKnown)tail.value=value;
                    else tail.uncertain=true;
                }
                tail.inFlight=false;
            }
            static void RevokeRetired(DisplayGate& gate) noexcept {
                gate.RevokeStream();
                if(auto live=gate.outputLive.lock())live->store(false,std::memory_order_release);
                gate.outputLive.reset();gate.streamTail={}; // caller positively checked its real tail
                if(gate.owner) {
                    {std::lock_guard snapshot(gate.owner->snapshotMutex_);gate.owner->ArmHideLocked();}
                    gate.owner->QueueCommand(); // no synchronous HWND mutation under endpoint locks
                }
            }
        public:
            // These callbacks only revoke observations. Call after exact old
            // endpoint tails have completed; they never grant GPU retirement.
            static void BuffersRetired(uint64_t outputOwner,uint64_t stream,uint64_t generation) noexcept {
                auto gate=retainedDisplay_.load(std::memory_order_acquire);if(!gate)return;
                std::lock_guard display(gate->mutex);
                if(gate->HiddenReady() && gate->binding.RetireBuffers(outputOwner,stream,generation))
                    RevokeRetired(*gate);
            }
            static void ProviderRetired(uint64_t outputOwner) noexcept {
                auto gate=retainedDisplay_.load(std::memory_order_acquire);if(!gate)return;
                std::lock_guard display(gate->mutex);
                // Whole-provider drain also clears the first input's provisional
                // scope when no physical raw has ever bound a stream.
                if(gate->HiddenReady() && gate->binding.RetireProvider(outputOwner))RevokeRetired(*gate);
            }
            static void Refused(const char* reason,IDXGISwapChain* chain,
                                ID3D12CommandQueue* queue=nullptr,ID3D12Resource* color=nullptr,
                                UINT index=UINT_MAX,uint64_t stream=0,uint64_t generation=0,
                                uint64_t sequence=0,UINT inputFlags=0) noexcept {
                if(auto* p=current_)p->physicalAdmissionPending_=true;
                static std::atomic<uint64_t> reports{0};const auto n=++reports;
                static thread_local ULONGLONG lastReport=0;const auto now=GetTickCount64();
                if(n>8 && n%600!=0 && now-lastReport<1000)return;
                lastReport=now;
                LOG_INFO("F6 physical-output refusal: pid={} thread={} reason={} chain={:X} queue={:X} color={:X} index={} stream={} generation={} sequence={} flags={:X} report={} raw_called=false source=UNKNOWN visual=UNKNOWN",
                    GetCurrentProcessId(),GetCurrentThreadId(),reason,Key(chain),Key(queue),Key(color),index,
                    stream,generation,sequence,inputFlags,n);
            }
            bool Bind(IDXGISwapChain3* chain,ID3D12CommandQueue* queue,ID3D12Resource* color,
                      UINT index,uint64_t stream,uint64_t generation,uint64_t sequence,uint64_t outputOwner,
                      std::shared_ptr<std::atomic<bool>>& outputLive,ID3D12Fence* fence,UINT inputFlags) noexcept {
                const auto refuse=[&](const char* reason){
                    Refused(reason,chain,queue,color,index,stream,generation,sequence,inputFlags);return false;
                };
                DXGI_SWAP_CHAIN_DESC d{};
                Microsoft::WRL::ComPtr<ID3D12Device> device,colorDevice,chainDevice,fenceDevice;
                if(display_ || !outputOwner || outputOwner==UINT64_MAX || !stream || stream==UINT64_MAX ||
                    !generation || !sequence || !chain || !queue || !color || !fence ||
                    FAILED(chain->GetDesc(&d)) || !d.Windowed || d.SwapEffect!=DXGI_SWAP_EFFECT_FLIP_DISCARD ||
                    !IsPhysicalOutput(chain,d.OutputWindow) || index>=d.BufferCount ||
                    queue->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT ||
                    FAILED(queue->GetDevice(IID_PPV_ARGS(&device))) ||
                    FAILED(color->GetDevice(IID_PPV_ARGS(&colorDevice))) ||
                    FAILED(chain->GetDevice(IID_PPV_ARGS(&chainDevice))) ||
                    FAILED(fence->GetDevice(IID_PPV_ARGS(&fenceDevice))) || !Key(device.Get()) ||
                    Key(colorDevice.Get())!=Key(device.Get()) || Key(chainDevice.Get())!=Key(device.Get()) ||
                    Key(fenceDevice.Get())!=Key(device.Get()) || fence->GetCompletedValue()==UINT64_MAX ||
                    FAILED(device->GetDeviceRemovedReason()))return refuse("PhysicalColorOrCreationQueueMismatch");
                auto gate=retainedDisplay_.load(std::memory_order_acquire);
                if(!gate)return refuse("NoRetainedDisplayOwner");
                // One total scheduling budget per raw, shared by lock/reveal/
                // hide and the pre-Present recheck. Contention cannot multiply
                // the existing display lease into several serial pauses.
                handoffUntil_=GetTickCount64()+LeaseMs;
                // Endpoint::PresentImpl is the only caller. Worker SDK entry
                // holds no DisplayGate lock: both waits below have depth one.
                // Same-thread HWND reentry may recurse, but never waits below.
                std::unique_lock display(gate->mutex,std::defer_lock);
                if(!display.try_lock_for(std::chrono::milliseconds(HandoffBudget())))return refuse("OwnerOrParentBoundaryBusy");
                if(!gate->live || !gate->owner || gate->child!=d.OutputWindow || !IsRetainedChild(d.OutputWindow))
                    return refuse("PhysicalWindowLifetimeChanged");
                if(gate->binding.Owner().Known() && gate->binding.Owner().outputOwner!=outputOwner)
                    return refuse("SdkOutputOwnerMismatch");
                // The SDK may already have queued this raw when the render
                // thread yields new inputs. Returning WAS_STILL_DRAWING here
                // poisons its next host Present and restarts FG. Let the owner
                // finish the bounded reveal instead, outside the display lock.
                // Never wait for the HWND owner from that owner's own stack.
                if(gate->revealWindow.BlocksRaw(GetTickCount64())) {
                    if(gate->owner->OnOwnerThread())gate->revealWindow.Release();
                    else {
                        const auto budget=(std::min)(HandoffBudget(),gate->revealWindow.Remaining(GetTickCount64()));
                        gate->revealWindow.AwaitOwner(display,budget,[&] {
                            return !gate->live || !gate->owner ||
                                !gate->revealWindow.BlocksRaw(GetTickCount64());
                        });
                        if(gate->revealWindow.BlocksRaw(GetTickCount64()))
                            return refuse("OwnerRevealHandoffIncomplete");
                    }
                }
                if(!gate->live || !gate->owner || gate->child!=d.OutputWindow || !IsRetainedChild(d.OutputWindow))
                    return refuse("PhysicalWindowLifetimeChangedAfterHandoff");
                if(gate->binding.Owner().Known() && gate->binding.Owner().outputOwner!=outputOwner)
                    return refuse("SdkOutputOwnerMismatchAfterHandoff");
                auto* p=current_;
                if(p && p->armed_ && p->sdkEntered_ && p->owner_.display_==gate) {
                    // Exact creation owner applies to synchronous calls too. TLS
                    // does not associate the output with this input's pixels.
                    if(!p->sdkSource_ || p->sdkSource_->StreamOwner().outputOwner!=outputOwner)
                        return refuse("SynchronousSdkOutputOwnerMismatch");
                    inputPermit_=p;
                }
                content_.stream=stream;content_.outputGeneration=generation;content_.sequence=sequence;content_.index=index;
                content_.chain=chain;content_.queue=queue;content_.color=Key(color);content_.device=device;content_.fence=fence;
                const auto streamAuthorized=[&] {return gate->streamLive && gate->streamLive->load(std::memory_order_acquire) &&
                    gate->scopeEpoch==gate->owner->Epoch() && !gate->owner->hideRequested_.load() &&
                    GetForegroundWindow()==GetAncestor(gate->child,GA_ROOT) && gate->inputLease.Current(GetTickCount64()) &&
                    gate->binding.Device12()==Key(device.Get()) &&
                    gate->binding.Owner().outputOwner==outputOwner &&
                    d.BufferDesc.Width==gate->binding.Owner().width && d.BufferDesc.Height==gate->binding.Owner().height;};
                // Source-less bootstrap remains hidden. Normal asynchronous
                // owner acknowledgement is scheduling, not an SDK failure.
                // Worker RequestHide only posts an owner command; it performs
                // no cross-thread HWND mutation or GPU/endpoint wait. Owner
                // publication needs only this gate and its HWND snapshot.
                if(!streamAuthorized() && !gate->hiddenAcknowledged) {
                    gate->owner->RequestHide();
                    // RequestHide/ArmHideLocked have returned: only Bind's
                    // original depth-one lock remains at this second wait.
                    if(!gate->owner->OnOwnerThread())
                        gate->revealWindow.AwaitOwner(display,HandoffBudget(),[&] {
                            return !gate->live || !gate->owner || gate->hiddenAcknowledged;
                        });
                    if(!gate->live || !gate->owner || gate->child!=d.OutputWindow || !IsRetainedChild(d.OutputWindow))
                        return refuse("PhysicalWindowLifetimeChangedAfterHide");
                    if(gate->binding.Owner().Known() && gate->binding.Owner().outputOwner!=outputOwner)
                        return refuse("SdkOutputOwnerMismatchAfterHide");
                }
                if(streamAuthorized()) {
                    if(gate->RawInFlight())return refuse("PhysicalRawAlreadyInFlight");
                    if(!gate->binding.BindPhysical(outputOwner,content_.Identity()))
                        return refuse("OutputStreamLifetimeChangedRequiresRetirement");
                    if(!outputLive || !outputLive->load(std::memory_order_acquire)) {
                        try{outputLive=std::make_shared<std::atomic<bool>>(true);}
                        catch(...){return refuse("PhysicalLifetimeAllocationFailed");}
                    }
                    if(auto old=gate->outputLive.lock();old && old!=outputLive)old->store(false,std::memory_order_release);
                    gate->outputLive=outputLive;
                    content_.streamOwner=gate->binding.Owner();content_.streamLive=gate->streamLive;
                    content_.epoch=gate->scopeEpoch;content_.outputLive=outputLive;
                    content_.primed=gate->hiddenAcknowledged;
                    gate->streamTail.queue=queue;gate->streamTail.fence=fence;gate->streamTail.device=device;
                    gate->streamTail.inFlight=true;
                    display_=gate;scoped_=true;return true;
                }
                // No authenticated scope: only acknowledged-hidden bootstrap
                // may call raw. Its exact queue tails also block later reveal.
                if(!gate->hiddenAcknowledged || (GetWindowLongPtrW(d.OutputWindow,GWL_STYLE)&WS_VISIBLE))
                    return refuse("UnauthenticatedOutputWaitingForOwnerHide");
                size_t slot=gate->hiddenTails.size();
                for(size_t i=0;i<gate->hiddenTails.size();++i) {
                    const auto& tail=gate->hiddenTails[i];
                    if(!tail.inFlight && !tail.uncertain && tail.queue.Get()==queue && tail.fence.Get()==fence){slot=i;break;}
                }
                if(slot==gate->hiddenTails.size())for(size_t i=0;i<gate->hiddenTails.size();++i)
                    if(gate->hiddenTails[i].Complete()){slot=i;break;}
                if(slot==gate->hiddenTails.size())return refuse("HiddenPhysicalTailCapacityOrUncertain");
                auto& tail=gate->hiddenTails[slot];
                if(tail.queue.Get()!=queue || tail.fence.Get()!=fence) {
                    tail.queue=queue;tail.fence=fence;tail.device=device;tail.value=0;tail.uncertain=false;
                }
                tail.inFlight=true;hiddenSlot_=slot;display_=gate;
                if(auto live=gate->outputLive.lock())live->store(false,std::memory_order_release);
                gate->outputLive.reset();return true;
            }
            bool HiddenOnly() const noexcept {return display_ && !scoped_;}
            bool StreamScoped() const noexcept {return scoped_;}
            bool Prime() const noexcept {return content_.primed;}
            bool MayPresent() const noexcept {
                if(!display_ || finished_)return false;
                std::unique_lock display(display_->mutex,std::defer_lock);
                if(!display.try_lock_for(std::chrono::milliseconds(HandoffBudget())) || !display_->live)return false;
                if(scoped_)return display_->owner && display_->owner->StreamCurrent(content_,false) &&
                    GetForegroundWindow()==GetAncestor(display_->child,GA_ROOT);
                return display_->hiddenAcknowledged && display_->child==retainedChild_.load() &&
                    !(GetWindowLongPtrW(display_->child,GWL_STYLE)&WS_VISIBLE);
            }
            bool RawEntered() noexcept {
                if(rawEntered_ || !MayPresent())return false;
                rawEntered_=true;return true;
            }
            void Submitted(bool rawCalled,UINT rawFlags,HRESULT rawResult,bool accepted,bool tailValid,
                           ID3D12Fence* fence,uint64_t value) noexcept {
                if(finished_ || !display_)return;
                finished_=true;
                const auto completed=fence?fence->GetCompletedValue():UINT64_MAX;
                tailValid=tailValid && rawCalled && rawEntered_ && fence==content_.fence.Get() &&
                    value && value!=UINT64_MAX && completed!=UINT64_MAX;
                if(inputPermit_) {
                    if(rawCalled && !tailValid)inputPermit_->physicalTailFailed_=true;
                    if(rawCalled && FAILED(rawResult) && rawResult!=DXGI_ERROR_WAS_STILL_DRAWING)
                        inputPermit_->physicalRawFailed_=true;
                    if(rawCalled && !scoped_)++inputPermit_->hiddenRawCount_;
                }
                PhysicalContent candidate; // release displaced observer refs after unlocking the gate
                std::lock_guard display(display_->mutex);
                if(!scoped_){FinishHidden(tailValid,value);return;}
                auto& lastRaw=display_->streamTail;
                if(rawCalled) {
                    if(tailValid && lastRaw.queue.Get()==content_.queue.Get() && lastRaw.fence.Get()==fence &&
                        value>lastRaw.value)
                        lastRaw.value=value; // exact successful Signal on the actual raw queue
                    else lastRaw.uncertain=true;
                }
                lastRaw.inFlight=false; // never a GPU completion claim
                content_.rawFlags=rawFlags;content_.value=value;content_.until=GetTickCount64()+LeaseMs;
                const bool valid=rawCalled && accepted && tailValid && rawResult==S_OK && display_->owner &&
                    display_->owner->StreamCurrent(content_,false);
                bool observed=false;
                if(valid){candidate=content_;observed=display_->owner->AcceptContent(candidate);}
                else if(display_->owner)display_->owner->Hide();
                static std::atomic<uint64_t> reports{0};const auto n=++reports;
                static thread_local ULONGLONG lastReport=0;
                const auto now=GetTickCount64();
                if(n<=16 || n%600==0 || (!valid && now-lastReport>=1000)) {
                    lastReport=now;
                    LOG_INFO("F6 stream-output: thread={} stream={} generation={} sequence={} provider={} output_owner={} epoch={} chain={:X} queue={:X} raw_called={} raw_hr={:X} fence={:X} tail={} completed={} observed={} input_accepted={} association=sdk_output_stream source=UNKNOWN kind=UNKNOWN visual=UNKNOWN",
                        GetCurrentThreadId(),content_.stream,content_.outputGeneration,content_.sequence,
                        content_.streamOwner.provider,content_.streamOwner.outputOwner,content_.epoch,
                        Key(content_.chain.Get()),Key(content_.queue.Get()),rawCalled,static_cast<unsigned>(rawResult),
                        Key(fence),value,completed,observed,display_->inputLease.Accepted());
                }
            }
            ~PhysicalOutput(){
                if(!display_ || finished_)return;
                std::lock_guard display(display_->mutex);
                if(scoped_) {
                    if(rawEntered_)display_->streamTail.uncertain=true;
                    display_->streamTail.inFlight=false;
                    if(display_->owner)display_->owner->Hide();
                } else FinishHidden(false,0);
                if(rawEntered_ && inputPermit_)inputPermit_->physicalTailFailed_=true;
            }
        };
        bool Called() const noexcept {return called_;}
        bool StreamAccepted() const noexcept {
            if(!accepted_ || !sdkSource_)return false;
            std::lock_guard display(owner_.display_->mutex);
            const auto& gate=*owner_.display_;
            return gate.live && gate.inputLease.Accepted() && gate.streamLive==streamScope_ && streamScope_ &&
                streamScope_->load(std::memory_order_acquire) && gate.binding.Owner()==sdkSource_->StreamOwner() &&
                gate.scopeEpoch==epoch_ && owner_.Epoch()==epoch_ && !owner_.hideRequested_.load() &&
                gate.inputLease.Current(GetTickCount64());
        }
        bool PhysicalFailure() const noexcept {return physicalRawFailed_ || physicalTailFailed_;}
        bool OutputAdmissionPending() const noexcept {return physicalAdmissionPending_;}
        unsigned HiddenRawCount() const noexcept {return hiddenRawCount_;}
        const char* CommitStage() const noexcept {return commitStage_;}
        HRESULT ActualResult() const noexcept {return actualResult_;}
        ~PresentPermit() {
            {
                std::lock_guard display(owner_.display_->mutex);
                const bool accepted=StreamAccepted();
                const bool sameScope=streamScope_ && owner_.display_->streamLive==streamScope_;
                if(sameScope)
                    owner_.display_->inputLease.End(accepted);
                // Revocation already closed a replaced scope. A late return or
                // exception from that old epoch cannot hide a newer input.
                if(!accepted && owner_.Epoch()==epoch_ && (!streamScope_ || sameScope))owner_.Hide();
            }
            if(current_==this)current_=nullptr;
        }
    };

    ~GenshinVisibleChild()
    {
        // Deferred final retirement runs this destructor on the actual HWND owner.
        {
            std::lock_guard display(display_->mutex);
            closed_=true;display_->live=false;display_->hiddenAcknowledged=false;
            display_->owner=nullptr;display_->RevokeStream();
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
