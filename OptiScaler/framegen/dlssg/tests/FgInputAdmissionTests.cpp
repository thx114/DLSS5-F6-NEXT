// The actual CanSubmitInputs body is extracted by test_fg_input_admission.py.
// Only provider state and hook status are injected; no SDK or GPU is executed.
#include <Windows.h>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <stdexcept>

enum class FGOutput { DLSSG, Other };
enum class ReflexOwnership { Available, ReleasePending };
struct State
{
    void* currentFG=nullptr;
    bool externalFrameGeneration=false;
    FGOutput activeFgOutput=FGOutput::DLSSG;
    static State& Instance() { static State state; return state; }
};
HRESULT injectedHookStatus=S_OK;
struct FGHooks { static HRESULT SwapchainHookStatus() noexcept { return injectedHookStatus; } };
std::atomic<bool> g_dlssgTeardownQuarantined=false;
struct DLSSG_Dx12
{
    std::mutex _inputContinuityMutex;
    bool _hookRetiring=false, _ownerChanging=false, _teardownCompleting=false;
    bool _teardownPending=false, _teardownQuarantined=false, _unretainedOffPending=false;
    bool _starRailUiBusy=false, _starRailUiFailed=false;
    ReflexOwnership _reflexOwnership=ReflexOwnership::Available;
    struct { bool valid=true; bool SourceClockValid() const { return valid; } } _inputContinuity;
    bool CanSubmitInputs();
};
#include "production-input-admission.inc"

unsigned checks=0;
void Check(bool value, const char* message)
{
    ++checks;
    if (!value) throw std::runtime_error(message);
}
int main()
{
    try
    {
        DLSSG_Dx12 owner;
        auto& state=State::Instance();
        state.currentFG=&owner;
        Check(owner.CanSubmitInputs(), "ready source accepts valid inputs");
        injectedHookStatus=HRESULT_FROM_WIN32(ERROR_NOT_READY);
        Check(owner.CanSubmitInputs(), "initial source warmup is preserved");
        for (const HRESULT failed : {HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_MEMORY), E_FAIL, E_ABORT})
        {
            injectedHookStatus=failed;
            Check(!owner.CanSubmitInputs(), "failed source hook rejects FG inputs");
            state.externalFrameGeneration=true;
            Check(owner.CanSubmitInputs(), "external owner does not borrow internal hook failure");
            state.externalFrameGeneration=false;
            state.activeFgOutput=FGOutput::Other;
            Check(owner.CanSubmitInputs(), "other output does not borrow DLSSG failure");
            state.activeFgOutput=FGOutput::DLSSG;
            state.currentFG=nullptr;
            Check(owner.CanSubmitInputs(), "unselected provider does not borrow current source status");
            state.currentFG=&owner;
        }
        for (const HRESULT status : {S_OK, HRESULT_FROM_WIN32(ERROR_NOT_READY)})
        {
            injectedHookStatus=status;
            for (auto member : {&DLSSG_Dx12::_hookRetiring, &DLSSG_Dx12::_ownerChanging,
                    &DLSSG_Dx12::_teardownCompleting, &DLSSG_Dx12::_teardownPending,
                    &DLSSG_Dx12::_teardownQuarantined, &DLSSG_Dx12::_unretainedOffPending,
                    &DLSSG_Dx12::_starRailUiBusy, &DLSSG_Dx12::_starRailUiFailed})
            {
                owner.*member=true;
                Check(!owner.CanSubmitInputs(), "existing lifecycle or UI refusal is preserved");
                owner.*member=false;
            }
            g_dlssgTeardownQuarantined=true;
            Check(!owner.CanSubmitInputs(), "global teardown quarantine is preserved");
            g_dlssgTeardownQuarantined=false;
            owner._reflexOwnership=ReflexOwnership::ReleasePending;
            Check(!owner.CanSubmitInputs(), "pending Reflex release is preserved");
            owner._reflexOwnership=ReflexOwnership::Available;
            owner._inputContinuity.valid=false;
            Check(!owner.CanSubmitInputs(), "invalid source clock is preserved");
            owner._inputContinuity.valid=true;
            Check(owner.CanSubmitInputs(), "valid source recovers after transient refusal");
        }
        std::printf("PASS %u actual FG input admission checks; CPU mocks, no GPU or SDK\n",checks);
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr,"FAIL actual FG input admission: %s (check %u)\n",error.what(),checks);
        return 1;
    }
}
