// The runner inserts the actual source/output Reflex blocks. Only the provider
// lifecycle lock and the SDK's cross-thread callback are simulated here.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>

enum class FGOutput { DLSSG, NoFG };
enum class FGInput { Upscaler };
enum class SwapchainInteropApi { None, Dx11wDx12, VkwDx12 };
struct Feature {
    virtual ~Feature() = default;
    bool active = true;
    unsigned applied = 5;
    bool IsActive() const { return active; }
};
struct DLSSG_Dx12 : Feature {};
struct State {
    static State& Instance() { static State state; return state; }
    bool externalFrameGeneration = false;
    SwapchainInteropApi swapchainInteropApi = SwapchainInteropApi::Dx11wDx12;
    FGInput activeFgInput = FGInput::Upscaler;
    FGOutput activeFgOutput = FGOutput::DLSSG;
    Feature* currentFG = nullptr;
    const void* currentFGSwapchain = nullptr;
};
struct Util { static auto ExePath() { return std::filesystem::path(L"YuanShen.exe"); } };
std::recursive_mutex continuity;
std::atomic<unsigned> updates{0}, applied{0};
bool lastActive = false, lastVulkan = false;
struct ReflexHooks {
    static void update(bool active, bool vulkan) {
        std::unique_lock lock(continuity, std::defer_lock);
        if (active) lock.lock(); // The real update's GuardExecution(Lifecycle).
        lastActive = active; lastVulkan = vulkan;
        applied = active ? State::Instance().currentFG->applied : 0;
        ++updates;
    }
};
void Output(bool providerOutput, bool willPresent, Feature* fg, bool vulkanGame) {
    // Old production block uses these three locals in the negative control.
    [[maybe_unused]] bool starRailProviderOutput = providerOutput;
    [[maybe_unused]] const void* cq = &continuity;
    (void)providerOutput;
    // INSERT_OUTPUT_REFLEX
}
void Source(bool willPresent, const void* This, Feature* fg, Feature* executionOwner) {
    auto& state = State::Instance();
    const auto fgPacingActive = [&] { return fg && fg->IsActive(); };
    // INSERT_SOURCE_REFLEX
}
unsigned checks = 0;
void Check(bool ok, const char* message) {
    ++checks;
    if (!ok) throw std::runtime_error(message);
}
int main() try {
    using namespace std::chrono_literals;
    auto& state = State::Instance();
    DLSSG_Dx12 fg;
    state.currentFG = &fg;
    state.currentFGSwapchain = &fg;
    for (auto api : {SwapchainInteropApi::Dx11wDx12, SwapchainInteropApi::None, SwapchainInteropApi::VkwDx12}) {
        state.swapchainInteropApi = api;
        updates = 0;
        std::unique_lock sourceGuard(continuity);
        // Dispatch just applied 5 generated frames; source update must see it.
        Source(true, &fg, &fg, &fg);
        Check(updates == 1 && applied == 5 && lastActive, "source owns one update after Dispatch");
        Check(lastVulkan == (api == SwapchainInteropApi::VkwDx12), "Vulkan cap forwarding preserved");
        std::promise<void> callbackFinished;
        auto done = callbackFinished.get_future();
        std::thread worker([&] {
            for (unsigned i = 0; i != 6; ++i) Output(true, true, &fg, false);
            callbackFinished.set_value();
        });
        // Model SDK waiting for its output worker while the source owns continuity.
        const bool completedInsideSdk = done.wait_for(1s) == std::future_status::ready;
        sourceGuard.unlock(); // Always release/join, including the old deadlocking path.
        worker.join();
        Check(completedInsideSdk, "SDK output worker must finish while source holds continuity");
        Check(updates == 1, "six physical outputs cannot count as six source cap updates");
    }
    updates = 0;
    fg.active = false;
    Source(true, &fg, &fg, &fg);
    Check(updates == 1 && !lastActive && applied == 0, "Off/paused source releases divided cap");
    Output(true, true, &fg, false);
    state.externalFrameGeneration = true;
    state.activeFgOutput = FGOutput::NoFG;
    Output(true, true, &fg, false);
    Check(updates == 1, "creation-bound old output stays excluded after mode changes");
    Source(true, &fg, &fg, &fg);
    Check(updates == 1, "external FG does not acquire source cap ownership");
    state.externalFrameGeneration = false;
    state.activeFgOutput = FGOutput::DLSSG;
    Source(false, &fg, &fg, &fg);
    Source(true, &state, &fg, &fg);
    Source(true, &fg, &fg, nullptr);
    Feature differentProvider;
    Source(true, &fg, &differentProvider, &differentProvider);
    Check(updates == 1, "TEST, unrelated source and stale provider do not update");
    state.currentFG = &differentProvider;
    Source(true, &fg, &differentProvider, &differentProvider);
    Check(updates == 1, "non-DLSSG provider retains its existing path");
    Output(false, true, &fg, false);
    Check(updates == 2 && !lastActive, "ordinary output retains inactive update");
    Output(false, true, nullptr, false);
    Check(updates == 3 && !lastActive, "ordinary no-provider output retains release");
    std::printf("PASS %u FG Reflex ownership CPU checks\n", checks);
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "FAIL: %s\n", e.what());
    return 1;
}
