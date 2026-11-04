// R691 candidate. Included inside il2cpp_callsite's anonymous namespace,
// after g_inst_mutex/g_render_token_epoch. No SDK constants or FG seal writer.
#include "CameraProducerProfile.inl"

using CameraConfigureFn = void(__fastcall*)(void*, void*, std::uintptr_t, std::uintptr_t);
using CameraSetterFn = void(__fastcall*)(void*, const float*, std::uintptr_t, std::uintptr_t);
using CameraScalarFn = float(__fastcall*)(void*, std::uintptr_t, std::uintptr_t, std::uintptr_t);
using CameraMatrixGetterFn = const float*(__fastcall*)(void*);
using CameraAffineInverseFn = bool(__fastcall*)(const float*, float*);
CameraConfigureFn g_camera_configure_original = nullptr;
CameraSetterFn g_camera_setter_original = nullptr;
CameraScalarFn g_camera_fov_original = nullptr, g_camera_aspect_original = nullptr;
CameraScalarFn g_camera_near_original = nullptr, g_camera_far_original = nullptr;
CameraMatrixGetterFn g_camera_view_getter = nullptr, g_camera_projection_getter = nullptr;
CameraAffineInverseFn g_camera_affine_inverse = nullptr;
std::uintptr_t g_camera_profile_base = 0; // Unused legacy six-hook installer only.
std::atomic_uintptr_t g_render_camera_profile_base{0}; // Release-published, never rewritten.
std::atomic<const PassiveCameraProfile*> g_render_camera_profile{nullptr}; // Immutable, release-published.
std::atomic_bool g_render_camera_enabled{false};
std::uint64_t g_camera_observation_serial=0; // g_inst_mutex, saturates.
bool render_camera_enabled() noexcept;
std::atomic_bool g_cpu_camera_enabled{false};
std::atomic_bool g_cpu_camera_stopped{false}; // Terminal shutdown, including failed installs.
std::mutex g_cpu_camera_install_mutex; // Only installers; never held by callbacks/shutdown.
bool g_cpu_camera_hooks_installed = false; // g_cpu_camera_install_mutex.
bool cpu_camera_enabled() noexcept {
    return !g_cpu_camera_stopped.load(std::memory_order_acquire) &&
        g_cpu_camera_enabled.load(std::memory_order_acquire);
}
std::uint64_t g_camera_configure_serial = 0; // g_inst_mutex.

struct CameraConfigureScope {
    CameraConfigureScope* previous = nullptr;
    void* instance = nullptr;
    void* managed = nullptr;
    void* native = nullptr;
    std::uint64_t epoch = 0;
    CameraSnapshot snapshot{};
    CapturedParams setterParams{};
    unsigned setterCount = 0, scalarMask = 0;
    bool rejected = false;
};
thread_local CameraConfigureScope* g_camera_scope = nullptr;
struct PendingCamera {
    CameraSnapshot snapshot{};
    CapturedParams params{};
    bool occupied = false, ambiguous = false;
};
PendingCamera g_pending_cameras[16]{}; // Same g_inst_mutex as RenderToken.

bool camera_memory(const void* from, void* to, std::size_t bytes) noexcept {
    // No C++ objects inside SEH; never catch an exception from original game code.
    __try { if (!from) return false; std::memcpy(to, from, bytes); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool camera_finite(float v) noexcept {
    std::uint32_t bits=0;std::memcpy(&bits,&v,4);return (bits&0x7f800000u)!=0x7f800000u;
}
bool camera_finite_matrix(const float* p) noexcept {
    float maximum = 0;
    for (unsigned i=0;i<16;++i) {
        if (!camera_finite(p[i])) return false;
        maximum = (std::max)(maximum,std::fabs(p[i]));
    }
    return maximum>0;
}
bool camera_params(void* instance, CapturedParams& cp) noexcept {
    const auto* p=static_cast<const unsigned char*>(instance);
    cp.instance=reinterpret_cast<std::uint64_t>(instance);
    // Re-read the same Render layout as on_render_enter; update its recipe once.
    // Camera ownership, finite-value checks and fail-closed admission stay below.
    const auto& fields=GenshinBridgeRecipes::Render;
    return instance && camera_memory(p+fields.renderWidth,&cp.render_w,4) &&
        camera_memory(p+fields.renderHeight,&cp.render_h,4) &&
        camera_memory(p+fields.displayWidth,&cp.display_w,4) &&
        camera_memory(p+fields.displayHeight,&cp.display_h,4) &&
        camera_memory(p+fields.frameIndex,&cp.frame_index,4) &&
        camera_memory(p+fields.jitterX,&cp.jitter_x,4) &&
        camera_memory(p+fields.jitterY,&cp.jitter_y,4) && cp.render_w && cp.render_h && cp.display_w && cp.display_h &&
        camera_finite(cp.jitter_x) && camera_finite(cp.jitter_y);
}
bool camera_same_params(const CapturedParams& a,const CapturedParams& b) noexcept {
    return a.instance==b.instance && a.frame_index==b.frame_index && a.render_w==b.render_w &&
        a.render_h==b.render_h && a.display_w==b.display_w && a.display_h==b.display_h &&
        std::memcmp(&a.jitter_x,&b.jitter_x,4)==0 && std::memcmp(&a.jitter_y,&b.jitter_y,4)==0;
}
void camera_revoke_locked() noexcept {
    for (auto& p:g_pending_cameras) p={};
}
void camera_freeze_locked(RenderToken& token, std::uint64_t epoch) noexcept {
    token.camera={};
    if (!cpu_camera_enabled()) return;
    unsigned matches=0; PendingCamera* selected=nullptr; bool ambiguous=false;
    for (auto& p:g_pending_cameras) {
        if (!p.occupied || p.params.instance!=token.params.instance) continue;
        if (p.snapshot.captureEpoch!=epoch || p.snapshot.captureTick>token.capture_tick ||
            token.capture_tick-p.snapshot.captureTick>250) { p={}; continue; }
        // Same instance/frame with different jitter/size is a mismatch, not an
        // invitation to borrow another camera or reconstruct a guessed value.
        if (p.params.frame_index!=token.params.frame_index) continue;
        ++matches; selected=&p;
        ambiguous=ambiguous || p.ambiguous || !camera_same_params(p.params,token.params);
    }
    if (matches==1 && !ambiguous && selected->snapshot.cpuValid) {
        token.camera=selected->snapshot;
        token.camera.renderGeneration=token.generation;
        token.camera.renderThread=GetCurrentThreadId();
    }
    // Consume once, even on ambiguity. No "latest camera" fallback.
    for (auto& p:g_pending_cameras)
        if (p.occupied && p.params.instance==token.params.instance && p.params.frame_index==token.params.frame_index) p={};
}
struct CameraGpuProfileRead {
    std::uintptr_t graphicsDevice=0, config=0, table=0, method=0;
    unsigned char clip=0, reverse=0;
    bool valid=false;
};
CameraGpuProfileRead camera_gpu_profile_read(std::uintptr_t base=0,
    const GenshinBridgeRecipes::PassiveCameraLayout& profile=GenshinBridgeRecipes::CameraOctober2026) noexcept {
    CameraGpuProfileRead s{};
    if (!base) base=g_camera_profile_base;
    if (!base || !camera_memory(reinterpret_cast<const void*>(base+profile.graphicsDeviceRva),&s.graphicsDevice,sizeof(s.graphicsDevice)) ||
        !camera_memory(reinterpret_cast<const void*>(base+profile.configRva),&s.config,sizeof(s.config)) ||
        !s.graphicsDevice || !s.config ||
        !camera_memory(reinterpret_cast<const void*>(s.graphicsDevice),&s.table,sizeof(s.table)) || !s.table ||
        !camera_memory(reinterpret_cast<const void*>(s.table+profile.gpuMethodSlot),&s.method,sizeof(s.method)) ||
        s.method!=base+profile.gpuMethodRva || !camera_memory(reinterpret_cast<const void*>(s.config+profile.clip),&s.clip,1) ||
        !camera_memory(reinterpret_cast<const void*>(s.config+profile.reverse),&s.reverse,1)) return s;
    s.valid=true;return s;
}
bool camera_gpu_profile_same(const CameraGpuProfileRead& a,const CameraGpuProfileRead& b) noexcept {
    return a.valid && b.valid && a.graphicsDevice==b.graphicsDevice && a.config==b.config &&
        a.table==b.table && a.method==b.method && a.clip==b.clip && a.reverse==b.reverse;
}
// R729: passive reads of the exact pinned native getter slots. Cache/mirror
// identity is observation provenance, NEVER a current-frame or writer claim.
bool render_camera_enabled() noexcept {
    return !g_cpu_camera_stopped.load(std::memory_order_acquire) &&
        g_render_camera_enabled.load(std::memory_order_acquire);
}
struct RenderCameraIdentity {
    std::uint64_t owner=0, primary=0, selected=0, native=0;
    std::uint32_t mainThread=0;
    std::uint8_t selector=0, automatic=0, projectionDirty=0, viewDirty=0;
};
// R807 first actual passive-read refusal per stage, bounded32 entries/process.
// No retries, extra memory reads, getter calls, owner adoption or admission.
void camera_observation_note(unsigned stage,const char* reason,const CapturedParams& cp,
    std::uint64_t actual=0,std::uint64_t expected=0) noexcept {
    if(stage>=32)return;static std::atomic_uint32_t seen{0};const auto bit=uint32_t(1)<<stage;
    if(seen.fetch_or(bit,std::memory_order_relaxed)&bit)return;
    LOG_INFO("R807 camera observation: pid={} stage={} reason={} instance={} context={} frame={} tid={} actual={} expected={} no_authority=true",
        GetCurrentProcessId(),stage,reason,cp.instance,cp.context,cp.frame_index,GetCurrentThreadId(),actual,expected);
}
bool camera_render_identity(std::uintptr_t base,const CapturedParams& cp,RenderCameraIdentity& s,
    const GenshinBridgeRecipes::PassiveCameraLayout& profile) noexcept {
    if (!base || !cp.instance || !cp.context){camera_observation_note(0,"input",cp,base);return false;}
    const auto read=[](std::uint64_t address,void* out,std::size_t bytes) noexcept {
        return camera_memory(reinterpret_cast<const void*>(address),out,bytes);
    };
    std::uint64_t ownerContext=0, ownerInstance=0, contextInstance=0, ownerPrimary=0;
    if (!read(cp.context+profile.contextOwner,&s.owner,8) || !s.owner){camera_observation_note(1,"context-owner",cp,s.owner);return false;}
    if (!read(s.owner+profile.ownerContext,&ownerContext,8) || ownerContext!=cp.context){camera_observation_note(2,"owner-context",cp,ownerContext,cp.context);return false;}
    if (!read(s.owner+profile.ownerInstance,&ownerInstance,8) || ownerInstance!=cp.instance){camera_observation_note(3,"owner-instance",cp,ownerInstance,cp.instance);return false;}
    if (!read(cp.context+profile.contextInstance,&contextInstance,8) || contextInstance!=cp.instance){camera_observation_note(4,"context-instance",cp,contextInstance,cp.instance);return false;}
    if (!read(cp.context+profile.contextPrimary,&s.primary,8) || !s.primary){camera_observation_note(5,"primary-camera",cp,s.primary);return false;}
    if (!read(s.owner+profile.ownerPrimary,&ownerPrimary,8) || ownerPrimary!=s.primary){camera_observation_note(6,"owner-primary",cp,ownerPrimary,s.primary);return false;}
    if (!read(cp.context+profile.contextSelector,&s.selector,1)){camera_observation_note(7,"selector-read",cp);return false;}
    s.selected=s.primary;
    if (s.selector && !read(cp.context+profile.contextAlternate,&s.selected,8)){camera_observation_note(8,"selected-read",cp);return false;}
    if (!s.selected || !read(s.selected+profile.managedNative,&s.native,8) || !s.native){camera_observation_note(9,"native-camera",cp,s.native,s.selected);return false;}
    if (!read(base+profile.mainThreadRva,&s.mainThread,4) || !s.mainThread){camera_observation_note(10,"main-thread",cp,s.mainThread);return false;}
    if (!read(s.native+profile.automatic,&s.automatic,1) || !read(s.native+profile.projectionDirty,&s.projectionDirty,1) ||
        !read(s.native+profile.viewDirty,&s.viewDirty,1)){camera_observation_note(11,"native-cache-flags",cp,s.native);return false;}
    return true;
}

bool camera_render_identity_same(const RenderCameraIdentity& a,const RenderCameraIdentity& b) noexcept {
    return a.owner==b.owner && a.primary==b.primary && a.selected==b.selected && a.native==b.native &&
        a.mainThread==b.mainThread && a.selector==b.selector && a.automatic==b.automatic &&
        a.projectionDirty==b.projectionDirty && a.viewDirty==b.viewDirty;
}
CameraSnapshot camera_observe_render(const CapturedParams& cp,std::uint64_t epoch) noexcept {
    CameraSnapshot out{};
    if (!render_camera_enabled() || epoch==UINT64_MAX){camera_observation_note(12,"disabled-or-epoch",cp,epoch);return out;}
    const auto base=g_render_camera_profile_base.load(std::memory_order_acquire);
    const auto* selectedProfile=g_render_camera_profile.load(std::memory_order_acquire);
    if (!selectedProfile) return out;
    const auto& profile=selectedProfile->layout;
    const auto thread=GetCurrentThreadId();
    RenderCameraIdentity before{},after{};
    if (!camera_render_identity(base,cp,before,profile)) return {};
    const auto gpuBefore=camera_gpu_profile_read(base,profile);
    if (!gpuBefore.valid){camera_observation_note(13,"gpu-profile-method",cp,gpuBefore.method,base+profile.gpuMethodRva);camera_observation_note(14,"gpu-profile-objects",cp,gpuBefore.graphicsDevice,gpuBefore.config);return {};}
    const bool main=thread==before.mainThread;
    const auto pSlot=before.native+(main?profile.mainProjection:profile.workerProjection);
    const auto vSlot=before.native+(main?profile.mainView:profile.workerView);
    float projectionAgain[16]{},viewAgain[16]{};
    if (!camera_memory(reinterpret_cast<const void*>(pSlot),out.cpuStoredProjection,64) ||
        !camera_memory(reinterpret_cast<const void*>(vSlot),out.cpuWorldToView,64) ||
        !camera_finite_matrix(out.cpuStoredProjection) || !camera_finite_matrix(out.cpuWorldToView) ||
        !camera_memory(reinterpret_cast<const void*>(pSlot),projectionAgain,64) ||
        !camera_memory(reinterpret_cast<const void*>(vSlot),viewAgain,64) ||
        std::memcmp(projectionAgain,out.cpuStoredProjection,64) ||
        std::memcmp(viewAgain,out.cpuWorldToView,64)){camera_observation_note(15,"matrix-read-finite-or-stability",cp,pSlot,vSlot);return {};}
    CapturedParams paramsAfter{};
    if (!camera_params(reinterpret_cast<void*>(cp.instance),paramsAfter) || !camera_same_params(cp,paramsAfter) ||
        !camera_render_identity(base,cp,after,profile) || !camera_render_identity_same(before,after)){camera_observation_note(16,"params-or-identity-reread",cp);return {};}
    const auto gpuAfter=camera_gpu_profile_read(base,profile);
    if (!camera_gpu_profile_same(gpuBefore,gpuAfter) || !render_camera_enabled() ||
        epoch!=g_render_token_epoch.load(std::memory_order_acquire)){camera_observation_note(17,"profile-or-epoch-reread",cp,epoch);return {};}
    out.observedProfile=profile.version;
    out.provenance=CameraProvenance::RenderObservation;
    out.instance=cp.instance;out.managedCamera=before.selected;out.nativeCamera=before.native;
    out.observationContext=cp.context;out.observationOwner=before.owner;
    out.primaryManagedCamera=before.primary;out.selectedManagedCamera=before.selected;out.selector=before.selector;
    out.observedProjectionSlot=pSlot;out.observedViewSlot=vSlot;
    out.observedSlotMode=main?CameraSlotMode::MainCache:CameraSlotMode::WorkerMirror;
    out.observedMainThread=before.mainThread;out.observedProjectionAutomatic=before.automatic;
    out.observedProjectionDirty=before.projectionDirty;out.observedViewDirty=before.viewDirty;
    out.captureEpoch=epoch;out.captureTick=GetTickCount64();out.renderThread=thread;out.sourceFrame=cp.frame_index;
    out.jitterX=cp.jitter_x;out.jitterY=cp.jitter_y;
    out.graphicsDevice=gpuAfter.graphicsDevice;out.gpuClipControl=gpuAfter.clip;out.gpuReverseControl=gpuAfter.reverse;
    out.gpuProfileValid=true;out.cpuValid=true;
    // No observed noTAA/scalars/inverse, Configure serial/thread, GPU writer or
    // history claim. Root qualifies unchanged P/V against actual returned draws.
    return out;
}
void camera_freeze_render_locked(RenderToken& token,const CameraSnapshot& observed,std::uint64_t epoch) noexcept {
    token.camera={};
    if (!render_camera_enabled() || observed.provenance!=CameraProvenance::RenderObservation ||
        !observed.cpuValid || !observed.gpuProfileValid || epoch==UINT64_MAX || observed.captureEpoch!=epoch ||
        epoch!=g_render_token_epoch.load(std::memory_order_relaxed) || observed.instance!=token.params.instance ||
        observed.observationContext!=token.params.context || observed.sourceFrame!=token.params.frame_index ||
        observed.renderThread!=GetCurrentThreadId() || observed.captureTick>token.capture_tick ||
        token.capture_tick-observed.captureTick>250 || observed.jitterX!=token.params.jitter_x ||
        observed.jitterY!=token.params.jitter_y || g_camera_observation_serial==UINT64_MAX) return;
    token.camera=observed;
    token.camera.renderGeneration=token.generation;
    token.camera.observationSerial=++g_camera_observation_serial;
}

void camera_publish(CameraConfigureScope& scope) noexcept {
    auto& out=scope.snapshot;
    if (scope.rejected || scope.setterCount!=1 || scope.scalarMask!=15 || !scope.native) return;
    CapturedParams cp{};
    if (!camera_params(scope.instance,cp) || !camera_same_params(scope.setterParams,cp)) return;
    auto* native=static_cast<unsigned char*>(scope.native);
    const auto gpuBefore=camera_gpu_profile_read();
    // These two real getters have already been authenticated against the full
    // executable profile. Main-thread return addresses distinguish the actual
    // refreshed camera from a stale render-thread mirror (+18A0/+18E0).
    const auto* projection=g_camera_projection_getter(scope.native);
    const auto* view=g_camera_view_getter(scope.native);
    float storedNow[16]{};
    unsigned char automaticProjection=1;
    if (projection!=reinterpret_cast<const float*>(native+0x8d8) ||
        view!=reinterpret_cast<const float*>(native+0x898) ||
        !camera_memory(projection,storedNow,64) ||
        std::memcmp(storedNow,out.cpuStoredProjection,64)!=0 ||
        !camera_memory(native+0xec3,&automaticProjection,1) || automaticProjection!=0 ||
        !camera_memory(view,out.cpuWorldToView,64)) return;
    if (!camera_finite_matrix(out.cpuProjectionJittered) || !camera_finite_matrix(out.cpuStoredProjection) ||
        !camera_finite_matrix(out.cpuWorldToView) || !g_camera_affine_inverse(out.cpuWorldToView,out.cpuViewToWorld) ||
        !camera_finite_matrix(out.cpuViewToWorld)) return;
    // Configure constructs perspective explicitly. Its jitter coefficient is
    // +2*j in column 2 rows 0/1 (0x6DF4D91..DD6), CPU clip depth is [-1,+1].
    // This removes only this Configure's TAA term. GPU/Y/depth conversion is
    // NOT inferred from it. Keep the independently stored corrected P too.
    if (out.cpuProjectionJittered[11]!=-1.0f || out.cpuProjectionJittered[15]!=0.0f ||
        !camera_finite(out.nearPlane) || !camera_finite(out.farPlane) || out.nearPlane<=0 ||
        out.farPlane<=out.nearPlane || !camera_finite(out.fovDegrees) || out.fovDegrees<=0 ||
        out.fovDegrees>=180 || !camera_finite(out.aspect) || out.aspect<=0) return;
    out.provenance=CameraProvenance::ConfigureSetter;
    out.instance=cp.instance; out.managedCamera=reinterpret_cast<std::uint64_t>(scope.managed);
    out.nativeCamera=reinterpret_cast<std::uint64_t>(scope.native);
    out.sourceFrame=cp.frame_index; out.configureThread=GetCurrentThreadId();
    out.captureEpoch=scope.epoch; out.captureTick=GetTickCount64();
    out.jitterX=cp.jitter_x; out.jitterY=cp.jitter_y;
    std::memcpy(out.cpuProjectionNoTaa,out.cpuProjectionJittered,64);
    out.cpuProjectionNoTaa[8]-=2.0f*cp.jitter_x;
    out.cpuProjectionNoTaa[9]-=2.0f*cp.jitter_y;
    std::memcpy(out.cpuStoredProjectionNoTaa,out.cpuStoredProjection,64);
    // C only translates X/Y by clip W: C*e0=e0 and C*e1=e1.
    // Thus C*(Pj - [2*jx*e0+2*jy*e1]*e2^T) removes exactly
    // the same two column-2 terms after C; it does not remove C itself.
    out.cpuStoredProjectionNoTaa[8]-=2.0f*cp.jitter_x;
    out.cpuStoredProjectionNoTaa[9]-=2.0f*cp.jitter_y;
    out.cpuNoTaaValid=camera_finite_matrix(out.cpuProjectionNoTaa) &&
        camera_finite_matrix(out.cpuStoredProjectionNoTaa);
    out.cpuValid=out.cpuNoTaaValid;
    const auto gpuAfter=camera_gpu_profile_read();
    out.gpuProfileValid=camera_gpu_profile_same(gpuBefore,gpuAfter);
    if (out.gpuProfileValid) {
        out.graphicsDevice=gpuAfter.graphicsDevice;
        out.gpuClipControl=gpuAfter.clip; out.gpuReverseControl=gpuAfter.reverse;
    }
    // gpuProfileValid is only the authenticated adapter-input profile.
    // No GPU constants, fake DeviceZ, previous matrix, reset flag, or seal.
    out.gpuProjectionValid=false; out.historyValid=false;
    try {
        std::lock_guard lock(g_inst_mutex);
        if (!cpu_camera_enabled() || scope.epoch==UINT64_MAX ||
            scope.epoch!=g_render_token_epoch.load(std::memory_order_relaxed) || g_camera_configure_serial==UINT64_MAX) return;
        out.configureSerial=++g_camera_configure_serial;
        PendingCamera* slot=nullptr; bool duplicate=false;
        for (auto& p:g_pending_cameras) {
            if (p.occupied && p.snapshot.captureEpoch!=scope.epoch) p={};
            if (p.occupied && p.params.instance==cp.instance && p.params.frame_index==cp.frame_index) {
                p.ambiguous=true; duplicate=true;
            }
            if (!p.occupied && !slot) slot=&p;
        }
        if (slot) { slot->params=cp; slot->snapshot=out; slot->occupied=true; slot->ambiguous=duplicate; }
    } catch (...) { /* Optional camera loss must not block original SR. */ }
}

__declspec(noinline) void __fastcall camera_configure_hook(void* instance,void* managed,
                                                         std::uintptr_t opaqueR8,std::uintptr_t opaqueR9) {
    const auto before=GetLastError();
    CameraConfigureScope scope{};
    scope.previous=g_camera_scope; scope.instance=instance; scope.managed=managed;
    scope.epoch=g_render_token_epoch.load(std::memory_order_acquire);
    if (scope.previous) { scope.previous->rejected=true; scope.rejected=true; }
    scope.rejected=scope.rejected || !cpu_camera_enabled() || !instance || !managed;
    if (managed && !camera_memory(static_cast<unsigned char*>(managed)+0x10,&scope.native,sizeof(scope.native))) scope.rejected=true;
    g_camera_scope=&scope;
    SetLastError(before);
    // Typed Detours trampoline, NEVER reuse the old rcx/rdx-only camera stub.
    // Four integer argument lanes, including opaque IL2CPP metadata lanes, are
    // explicitly forwarded. No stack or floating argument is read by profile.
    // Win64 compiler preserves nonvolatile GPRs, XMM6..15 and stack alignment.
    // SEH finally restores TLS on native faults too (/EHsc alone would not
    // unwind a C++ scope guard when game code catches an access violation).
    __try {
        g_camera_configure_original(instance,managed,opaqueR8,opaqueR9);
        const auto after=GetLastError();
        if (!scope.rejected) camera_publish(scope);
        SetLastError(after);
    } __finally {
        g_camera_scope=scope.previous;
    }
}
__declspec(noinline) void __fastcall camera_setter_hook(void* camera,const float* matrix,
                                                      std::uintptr_t opaqueR8,std::uintptr_t opaqueR9) {
    const auto caller=reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    const auto before=GetLastError();
    auto* scope=g_camera_scope;
    const bool ours=scope && !scope->rejected && camera==scope->native &&
        caller==g_camera_profile_base+0x6df4eac;
    if (scope && !scope->rejected && !ours) scope->rejected=true;
    float p[16]{};
    const bool copied=ours && camera_memory(matrix,p,64) && camera_params(scope->instance,scope->setterParams);
    SetLastError(before);
    g_camera_setter_original(camera,matrix,opaqueR8,opaqueR9);
    const auto after=GetLastError();
    if (ours) {
        ++scope->setterCount;
        if (!copied || scope->setterCount!=1) scope->rejected=true;
        else {
            std::memcpy(scope->snapshot.cpuProjectionJittered,p,64);
            // Exact native setter has just written C*Pj, before Configure's
            // final FC7 setter or any getter refresh. Revalidate at publish.
            if (!camera_memory(static_cast<unsigned char*>(camera)+0x8d8,
                               scope->snapshot.cpuStoredProjection,64)) scope->rejected=true;
        }
    }
    SetLastError(after);
}
float camera_observe_scalar(float value,void* managed,std::uintptr_t caller,unsigned bit,std::uint32_t returnRva) noexcept {
    auto* scope=g_camera_scope;
    if (scope && !scope->rejected && scope->managed==managed && caller==g_camera_profile_base+returnRva) {
        if (scope->scalarMask&bit) scope->rejected=true;
        scope->scalarMask|=bit;
        switch (bit) {
        case 1:scope->snapshot.fovDegrees=value;break;
        case 2:scope->snapshot.aspect=value;break;
        case 4:scope->snapshot.nearPlane=value;break;
        case 8:scope->snapshot.farPlane=value;break;
        }
    }
    return value;
}
#define R691_SCALAR_HOOK(name, original, bit, returnRva) \
__declspec(noinline) float __fastcall name(void* camera,std::uintptr_t a2,std::uintptr_t a3,std::uintptr_t a4) { \
    const auto caller=reinterpret_cast<std::uintptr_t>(_ReturnAddress()); \
    const float value=original(camera,a2,a3,a4); \
    const auto error=GetLastError(); \
    camera_observe_scalar(value,camera,caller,bit,returnRva); \
    SetLastError(error); return value; \
}
R691_SCALAR_HOOK(camera_fov_hook,g_camera_fov_original,1,0x6df4d3c)
R691_SCALAR_HOOK(camera_aspect_hook,g_camera_aspect_original,2,0x6df4d55)
R691_SCALAR_HOOK(camera_near_hook,g_camera_near_original,4,0x6df4d65)
R691_SCALAR_HOOK(camera_far_hook,g_camera_far_original,8,0x6df4d71)
#undef R691_SCALAR_HOOK

// R708: exact failure domain/status; no invented Win32 error for a predicate.
bool camera_transient_win32(DWORD error) noexcept {
    return error==ERROR_BUSY || error==ERROR_SHARING_VIOLATION || error==ERROR_LOCK_VIOLATION ||
        error==ERROR_NOT_ENOUGH_MEMORY || error==ERROR_OUTOFMEMORY || error==ERROR_BAD_LENGTH ||
        error==ERROR_OPERATION_ABORTED;
}
bool camera_fail(CameraInstallResult& out,const char* phase,const char* domain,
                 std::uint32_t status=0,bool retryable=false,std::uint32_t rva=0,DWORD thread=0) noexcept {
    out.phase=phase;out.domain=domain;out.status=status;out.retryable=retryable;
    out.rva=rva;out.threadId=thread;return false;
}
bool camera_peer_retryable(DWORD error) noexcept {
    // R715: a snapshot/opened peer may exit before SuspendThread. Error 5 alone
    // does NOT prove exit rather than a live thread's access policy. Refuse the
    // whole attempt in either case; only a fresh, bounded attempt may recover.
    // Keep access denied OUT of the general file/profile/attach/commit policy.
    return camera_transient_win32(error) || error==ERROR_ACCESS_DENIED ||
        error==ERROR_INVALID_HANDLE || error==ERROR_INVALID_PARAMETER;
}
bool camera_win32_fail(CameraInstallResult& out,const char* phase,DWORD error,DWORD thread=0) noexcept {
    return camera_fail(out,phase,"win32",error,
        thread ? camera_peer_retryable(error) : camera_transient_win32(error),0,thread);
}
struct CameraPeerThread {
    HANDLE handle;
    DWORD id; // Snapshot identity, retained with the opened object through abort/commit.
};
bool camera_peer_live(const CameraPeerThread& peer,CameraInstallResult& out) noexcept {
    DWORD exitCode=0;
    if (!GetExitCodeThread(peer.handle,&exitCode))
        return camera_win32_fail(out,"thread-exit-query",GetLastError(),peer.id);
    // This is an early refusal, NOT permission to skip an exited peer. Code 259
    // is ambiguous; even STILL_ACTIVE must pass DetourUpdateThread below.
    if (exitCode!=STILL_ACTIVE)
        return camera_fail(out,"thread-exited-before-enlist","predicate",0,true,0,peer.id);
    return true;
}
bool camera_profile_matches(std::uint64_t base,const Config& cfg,CameraInstallResult& out,bool observation=false,
    const PassiveCameraProfile* profile=nullptr) noexcept {
    if (!base || base!=reinterpret_cast<std::uint64_t>(GetModuleHandleW(nullptr)) || cfg.camera_rva!=(profile?profile->layout.cameraRva:0x6df4b20))
        return camera_fail(out,"profile-base-rva","predicate");
    IMAGE_DOS_HEADER dos{}; IMAGE_NT_HEADERS64 nt{};
    if (!camera_memory(reinterpret_cast<const void*>(base),&dos,sizeof(dos)) || dos.e_magic!=IMAGE_DOS_SIGNATURE ||
        dos.e_lfanew<=0 || dos.e_lfanew>0x1000 || !camera_memory(reinterpret_cast<const void*>(base+dos.e_lfanew),&nt,sizeof(nt)) ||
        nt.Signature!=IMAGE_NT_SIGNATURE || nt.FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64 ||
        nt.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC || nt.OptionalHeader.SizeOfImage!=(profile?profile->layout.imageSize:k_camera_image_size) ||
        nt.FileHeader.TimeDateStamp!=(profile?profile->layout.timestamp:k_camera_timestamp))
        return camera_fail(out,"profile-pe","predicate");
    wchar_t path[32768]{};
    const auto n=GetModuleFileNameW(nullptr,path,32768);
    if (!n) return camera_win32_fail(out,"profile-path",GetLastError());
    if (n>=32768) return camera_fail(out,"profile-path-truncated","predicate");
    const auto* name=wcsrchr(path,L'\\'); name=name?name+1:path;
    if (_wcsicmp(name,L"YuanShen.exe")!=0) return camera_fail(out,"profile-executable-name","predicate");
    // Old profile retains its full-file SHA and loaded pins; the new profile
    // requires exact disk/loaded dependency bytes. No foreign code is skipped.
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
    if (file==INVALID_HANDLE_VALUE) return camera_win32_fail(out,"profile-file-open",GetLastError());
    if (profile && profile->exactPins) {
        // The new closed profile authenticates its versioned dependency bytes,
        // in the file AND loaded image. Only Render's owned entry patch is
        // excluded; unknown versions/foreign dependency changes still refuse.
        struct FileOwner { HANDLE value; ~FileOwner(){CloseHandle(value);} } owner{file};
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file,&size)) return camera_win32_fail(out,"profile-file-size",GetLastError());
        if (size.QuadPart<0 || static_cast<std::uint64_t>(size.QuadPart)!=profile->layout.fileBytes)
            return camera_fail(out,"profile-file-size","predicate");
        IMAGE_DOS_HEADER diskDos{}; IMAGE_NT_HEADERS64 diskNt{}; DWORD headerBytes=0;
        if (!ReadFile(file,&diskDos,sizeof(diskDos),&headerBytes,nullptr))
            return camera_win32_fail(out,"profile-exact-dos-read",GetLastError());
        if (headerBytes!=sizeof(diskDos) || diskDos.e_magic!=IMAGE_DOS_SIGNATURE ||
            diskDos.e_lfanew<=0 || diskDos.e_lfanew>0x1000)
            return camera_fail(out,"profile-exact-dos","predicate");
        LARGE_INTEGER ntOffset{};ntOffset.QuadPart=diskDos.e_lfanew;
        if (!SetFilePointerEx(file,ntOffset,nullptr,FILE_BEGIN))
            return camera_win32_fail(out,"profile-exact-pe-seek",GetLastError());
        if (!ReadFile(file,&diskNt,sizeof(diskNt),&headerBytes,nullptr))
            return camera_win32_fail(out,"profile-exact-pe-read",GetLastError());
        if (headerBytes!=sizeof(diskNt) || diskNt.Signature!=IMAGE_NT_SIGNATURE ||
            diskNt.FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64 ||
            diskNt.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
            diskNt.OptionalHeader.SizeOfImage!=profile->layout.imageSize ||
            diskNt.FileHeader.TimeDateStamp!=profile->layout.timestamp)
            return camera_fail(out,"profile-exact-pe","predicate");
        unsigned char disk[4096]{}, loaded[4096]{};
        for (std::size_t index=0;index<profile->exactPinCount;++index) {
            const auto& pin=profile->exactPins[index];
            if (!pin.expected || !pin.bytes || pin.rva>=profile->layout.imageSize ||
                pin.bytes>profile->layout.imageSize-pin.rva || pin.fileOffset>=profile->layout.fileBytes ||
                pin.bytes>profile->layout.fileBytes-pin.fileOffset)
                return camera_fail(out,"profile-exact-range","predicate",0,false,pin.rva);
            LARGE_INTEGER offset{};offset.QuadPart=pin.fileOffset;
            if (!SetFilePointerEx(file,offset,nullptr,FILE_BEGIN))
                return camera_win32_fail(out,"profile-file-seek",GetLastError());
            for (std::uint32_t done=0;done<pin.bytes;) {
                const DWORD chunk=static_cast<DWORD>((std::min)(std::size_t(pin.bytes-done),sizeof(disk)));
                DWORD got=0;
                if (!ReadFile(file,disk,chunk,&got,nullptr))
                    return camera_win32_fail(out,"profile-exact-file-read",GetLastError());
                if (got!=chunk || std::memcmp(disk,pin.expected+done,chunk))
                    return camera_fail(out,"profile-exact-file-bytes","predicate",0,false,pin.rva+done);
                if (!camera_memory(reinterpret_cast<const void*>(base+pin.rva+done),loaded,chunk) ||
                    std::memcmp(loaded,pin.expected+done,chunk))
                    return camera_fail(out,"profile-exact-loaded-bytes","predicate",0,false,pin.rva+done);
                done+=chunk;
            }
        }
        return true;
    }
    BCRYPT_HASH_HANDLE hash=nullptr;
    auto status=BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE,&hash,nullptr,0,nullptr,0,0);
    if (status<0) {
        CloseHandle(file);
        return camera_fail(out,"profile-file-hash-create","ntstatus",static_cast<std::uint32_t>(status));
    }
    unsigned char block[65536]{};DWORD got=0;std::uint64_t bytes=0;bool good=true;
    for (;;) {
        if (!ReadFile(file,block,sizeof(block),&got,nullptr)) {
            const auto error=GetLastError();good=camera_win32_fail(out,"profile-file-read",error);break;
        }
        if (!got) break;
        bytes+=got;
        status=BCryptHashData(hash,block,got,0);
        if (status<0) {good=camera_fail(out,"profile-file-hash-data","ntstatus",static_cast<std::uint32_t>(status));break;}
    }
    unsigned char actual[32]{};
    static constexpr unsigned char expected[32]={0x7f,0x89,0x93,0x8d,0xa6,0x06,0xc1,0x28,0x16,0x59,0x60,0x7d,0x46,0x47,0x02,0xcd,0xdb,0x9a,0x09,0xa7,0xd4,0x32,0x0a,0x19,0x66,0x30,0xf6,0x0a,0x81,0x1e,0xc3,0x8e};
    if (good) {
        status=BCryptFinishHash(hash,actual,32,0);
        if (status<0) good=camera_fail(out,"profile-file-hash-finish","ntstatus",static_cast<std::uint32_t>(status));
        else if (bytes!=445006744 || std::memcmp(actual,expected,32)!=0)
            good=camera_fail(out,"profile-file-size-sha256","predicate");
    }
    BCryptDestroyHash(hash);CloseHandle(file);
    if (!good) return false;
    unsigned char code[4096]{};
    const auto* pins=profile?profile->legacyPins:(observation?k_render_camera_code_pins:k_camera_code_pins);
    const auto count=profile?profile->legacyPinCount:(observation?sizeof(k_render_camera_code_pins)/sizeof(CameraCodePin):sizeof(k_camera_code_pins)/sizeof(CameraCodePin));
    for (std::size_t index=0;index<count;++index) {
        const auto& pin=pins[index];
        unsigned char digest[32]{};
        if (!pin.bytes || pin.bytes>sizeof(code) || pin.rva>=k_camera_image_size || pin.bytes>k_camera_image_size-pin.rva)
            return camera_fail(out,"profile-loaded-range","predicate",0,false,pin.rva);
        if (!camera_memory(reinterpret_cast<const void*>(base+pin.rva),code,pin.bytes))
            return camera_fail(out,"profile-loaded-read","predicate",0,false,pin.rva);
        status=BCryptHash(BCRYPT_SHA256_ALG_HANDLE,nullptr,0,code,pin.bytes,digest,32);
        if (status<0) return camera_fail(out,"profile-loaded-hash","ntstatus",static_cast<std::uint32_t>(status),false,pin.rva);
        if (std::memcmp(digest,pin.sha,32)!=0)
            return camera_fail(out,"profile-loaded-sha256","predicate",0,false,pin.rva);
    }
    return true;
}
bool camera_collect_threads(std::vector<CameraPeerThread>& handles,CameraInstallResult& out) {
    // Complete enumeration/open before the first suspension. Keep all previous
    // rights/refusal gates; an inaccessible/exited peer is never silently skipped.
    HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);
    if (snapshot==INVALID_HANDLE_VALUE) return camera_win32_fail(out,"thread-snapshot",GetLastError());
    THREADENTRY32 entry{};entry.dwSize=sizeof(entry);
    bool good=true;
    if (!Thread32First(snapshot,&entry)) good=camera_win32_fail(out,"thread-first",GetLastError());
    else for (;;) {
        if (entry.dwSize<sizeof(THREADENTRY32)) {good=camera_fail(out,"thread-entry-size","predicate");break;}
        if (entry.th32OwnerProcessID==GetCurrentProcessId() && entry.th32ThreadID!=GetCurrentThreadId()) {
            const auto id=entry.th32ThreadID;
            HANDLE thread=OpenThread(THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT|THREAD_SET_CONTEXT|THREAD_QUERY_INFORMATION,FALSE,id);
            if (!thread) {good=camera_win32_fail(out,"thread-open",GetLastError(),id);break;}
            // Thread IDs can be recycled between snapshot and OpenThread.
            const auto owner=GetProcessIdOfThread(thread);
            if (!owner) {const auto error=GetLastError();CloseHandle(thread);good=camera_win32_fail(out,"thread-owner-query",error,id);break;}
            if (owner!=GetCurrentProcessId()) {
                CloseHandle(thread);good=camera_fail(out,"thread-owner-changed","predicate",0,true,0,id);break;
            }
            if (handles.size()==handles.capacity()) {
                CloseHandle(thread);good=camera_fail(out,"thread-capacity","predicate");break;
            }
            const CameraPeerThread peer{thread,id};
            if (!camera_peer_live(peer,out)) {CloseHandle(thread);good=false;break;}
            handles.push_back(peer); // reserve completed before this function.
        }
        entry.dwSize=sizeof(entry);
        if (!Thread32Next(snapshot,&entry)) {
            const auto error=GetLastError();
            if (error!=ERROR_NO_MORE_FILES) good=camera_win32_fail(out,"thread-next",error);
            break;
        }
    }
    CloseHandle(snapshot);return good;
}
// The product compatibility entry enables observation only. The historical
// six-hook installer below has no caller; no transaction/peer retry is used.
bool enable_render_camera_observation(std::uint64_t base,const Config& cfg,CameraInstallResult& out) {
    out={};
    std::unique_lock installLock(g_cpu_camera_install_mutex,std::try_to_lock);
    if (!installLock.owns_lock()) return camera_fail(out,"observer-enable-busy","predicate");
    if (g_cpu_camera_stopped.load(std::memory_order_acquire)) return camera_fail(out,"shutdown-terminal","predicate");
    if (!cfg.capture_camera) return camera_fail(out,"not-requested","predicate");
    if (cfg.skip_render || !g_active.load(std::memory_order_acquire) ||
        g_render!=reinterpret_cast<std::uint8_t*>(base+cfg.render_rva))
        return camera_fail(out,"observer-render-identity","predicate");
    // Selection is only a candidate. The existing matcher authenticates PE,
    // executable name/file and all versioned dependencies before publication.
    const PassiveCameraProfile* profile=nullptr;
    for (const auto& candidate:k_camera_profiles)
        if (cfg.render_rva==candidate.layout.renderRva && cfg.camera_rva==candidate.layout.cameraRva &&
            cfg.update_cmd_buffer_rva==candidate.layout.ucbRva) profile=&candidate;
    if (!profile) return camera_fail(out,"observer-unsupported-profile","predicate");
    if (g_render_camera_enabled.load(std::memory_order_acquire)) {
        if (g_render_camera_profile_base.load(std::memory_order_acquire)!=base ||
            g_render_camera_profile.load(std::memory_order_acquire)!=profile)
            return camera_fail(out,"observer-enabled-identity","predicate");
        out.phase="observation-already-enabled";return true;
    }
    if (!camera_profile_matches(base,cfg,out,true,profile)) {out.retryable=false;return false;}
    HMODULE pinned=nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(&camera_observe_render),&pinned)) {
        camera_win32_fail(out,"observer-module-pin",GetLastError());out.retryable=false;return false;
    }
    if (g_cpu_camera_stopped.load(std::memory_order_acquire)) return camera_fail(out,"shutdown-terminal","predicate");
    g_render_camera_profile.store(profile,std::memory_order_release);
    g_render_camera_profile_base.store(base,std::memory_order_release);
    g_render_camera_enabled.store(true,std::memory_order_release);
    if (!render_camera_enabled()) return camera_fail(out,"shutdown-after-enable","predicate");
    out.phase="observation-enabled";return true;
}
bool install_cpu_camera_producer(std::uint64_t base,const Config& cfg,CameraInstallResult& out) {
    out={};
    std::unique_lock installLock(g_cpu_camera_install_mutex,std::try_to_lock);
    if (!installLock.owns_lock()) return camera_fail(out,"installer-busy","predicate",0,true);
    if (g_cpu_camera_stopped.load(std::memory_order_acquire)) return camera_fail(out,"shutdown-terminal","predicate");
    if (!cfg.capture_camera) return camera_fail(out,"not-requested","predicate");
    if (g_cpu_camera_hooks_installed) {
        if (!cpu_camera_enabled() || g_camera_profile_base!=base || cfg.camera_rva!=0x6df4b20)
            return camera_fail(out,"installed-identity-or-disabled","predicate");
        out.phase="already-installed";return true;
    }
    if (!camera_profile_matches(base,cfg,out)) return false;
    HMODULE pinned=nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(&camera_configure_hook),&pinned))
        return camera_win32_fail(out,"module-pin",GetLastError());
    std::vector<CameraPeerThread> threads;
    try {threads.reserve(4096);} catch (...) {return camera_fail(out,"thread-reserve","cxx",0,true);}
    // All handles outlive commit/abort; their destructor never closes a suspended
    // peer before Detours resumes it. No caller log runs during the transaction.
    struct ThreadHandles {
        std::vector<CameraPeerThread>& values;
        ~ThreadHandles(){for(const auto& peer:values)CloseHandle(peer.handle);}
    } handles{threads};
    if (!camera_collect_threads(threads,out)) return false;
    if (g_cpu_camera_stopped.load(std::memory_order_acquire)) return camera_fail(out,"shutdown-terminal","predicate");
    const auto begin=DetourTransactionBegin();
    if (begin!=NO_ERROR) {
        camera_fail(out,"transaction-begin","detours",static_cast<std::uint32_t>(begin),
                    begin==ERROR_INVALID_OPERATION || camera_transient_win32(begin));
        // Verified local 4.0.1 source AND linked-library disassembly: any other
        // Begin error occurs AFTER it acquires ownership. Release that ownership.
        // INVALID_OPERATION denotes an existing transaction: NEVER abort it.
        if (begin!=ERROR_INVALID_OPERATION) {
            out.abortStatus=static_cast<std::uint32_t>(DetourTransactionAbort());
            if (out.abortStatus!=NO_ERROR) out.retryable=false;
        }
        return false;
    }
    bool committed=false;
    // Aborts on every failed/exceptional owned path. Created after handle owner
    // so its destructor resumes peers before handles are closed.
    struct Transaction {
        bool& committed;CameraInstallResult& out;
        ~Transaction(){if(!committed){out.abortStatus=static_cast<std::uint32_t>(DetourTransactionAbort());
            if(out.abortStatus!=NO_ERROR)out.retryable=false;}}
    } transaction{committed,out};
    auto update=DetourUpdateThread(GetCurrentThread());
    if (update!=NO_ERROR) return camera_fail(out,"thread-enlist-current","detours",static_cast<std::uint32_t>(update),camera_transient_win32(update),0,GetCurrentThreadId());
    for (const auto& peer:threads) {
        if (!camera_peer_live(peer,out)) return false;
        update=DetourUpdateThread(peer.handle);
        if (update!=NO_ERROR)
            // Detours records a failed SuspendThread in its pending error.
            // Never continue/commit that transaction or retry the same handle.
            // RAII Abort resumes every already-enlisted peer before return;
            // an Abort failure clears retryable before the caller can schedule.
            return camera_fail(out,"thread-enlist-peer","detours",static_cast<std::uint32_t>(update),
                               camera_peer_retryable(update),0,peer.id);
    }
    // Initialize function pointers only while our transaction is owned. Failed
    // attach/aborted commit leaves no active wrappers. No foreign chain is used.
    g_camera_profile_base=base;
    g_camera_configure_original=reinterpret_cast<CameraConfigureFn>(base+0x6df4b20);
    g_camera_setter_original=reinterpret_cast<CameraSetterFn>(base+0x199f7a0);
    g_camera_fov_original=reinterpret_cast<CameraScalarFn>(base+0x1455120);
    g_camera_aspect_original=reinterpret_cast<CameraScalarFn>(base+0x1456520);
    g_camera_near_original=reinterpret_cast<CameraScalarFn>(base+0x1455190);
    g_camera_far_original=reinterpret_cast<CameraScalarFn>(base+0x1455200);
    g_camera_projection_getter=reinterpret_cast<CameraMatrixGetterFn>(base+0x199e820);
    g_camera_view_getter=reinterpret_cast<CameraMatrixGetterFn>(base+0x199e5e0);
    g_camera_affine_inverse=reinterpret_cast<CameraAffineInverseFn>(base+0x16d82f0);
#define R708_ATTACH(original,wrapper,rva) \
    {const auto error=DetourAttach(reinterpret_cast<PVOID*>(&original),reinterpret_cast<PVOID>(wrapper)); \
     if(error!=NO_ERROR)return camera_fail(out,"attach-" #wrapper,"detours",static_cast<std::uint32_t>(error),camera_transient_win32(error),rva);}
    R708_ATTACH(g_camera_configure_original,camera_configure_hook,0x6df4b20);
    R708_ATTACH(g_camera_setter_original,camera_setter_hook,0x199f7a0);
    R708_ATTACH(g_camera_fov_original,camera_fov_hook,0x1455120);
    R708_ATTACH(g_camera_aspect_original,camera_aspect_hook,0x1456520);
    R708_ATTACH(g_camera_near_original,camera_near_hook,0x1455190);
    R708_ATTACH(g_camera_far_original,camera_far_hook,0x1455200);
#undef R708_ATTACH
    if (g_cpu_camera_stopped.load(std::memory_order_acquire)) return camera_fail(out,"shutdown-before-commit","predicate");
    PVOID* failed=nullptr;
    const auto commit=DetourTransactionCommitEx(&failed);
    // The linked 4.0.1 CommitEx consumes an owned transaction even on pending
    // error. Do not Abort a second time or touch another thread's transaction.
    committed=true;
    if (commit!=NO_ERROR) {
        std::uint32_t rva=0;
        if(failed==reinterpret_cast<PVOID*>(&g_camera_configure_original))rva=0x6df4b20;
        else if(failed==reinterpret_cast<PVOID*>(&g_camera_setter_original))rva=0x199f7a0;
        else if(failed==reinterpret_cast<PVOID*>(&g_camera_fov_original))rva=0x1455120;
        else if(failed==reinterpret_cast<PVOID*>(&g_camera_aspect_original))rva=0x1456520;
        else if(failed==reinterpret_cast<PVOID*>(&g_camera_near_original))rva=0x1455190;
        else if(failed==reinterpret_cast<PVOID*>(&g_camera_far_original))rva=0x1455200;
        return camera_fail(out,"transaction-commit","detours",static_cast<std::uint32_t>(commit),camera_transient_win32(commit),rva);
    }
    g_cpu_camera_hooks_installed=true;
    g_cpu_camera_enabled.store(true,std::memory_order_release);
    // All callback/read paths require !stopped as well as enabled. A concurrent
    // terminal shutdown can never be revived by this store. Trampolines and
    // pinned module remain process-lifetime; no detach/free/wait at shutdown.
    if (!cpu_camera_enabled()) return camera_fail(out,"shutdown-after-commit","predicate");
    out.phase="installed";return true;
}
