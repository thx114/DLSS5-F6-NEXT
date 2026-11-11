#pragma once
#include <cstddef>
#include <cstdint>

// Game-executable FFX_FSR2 recipe only. This is NOT a NativeContext,
// StateSwap or PS-setter module recipe and does not establish a new ABI.
namespace GenshinBridgeRecipes
{
struct MethodGap
{
    std::uint32_t size;
    std::int32_t tolerance;
};
struct MethodRecipe
{
    const char* recipeId;
    const char* sourceRepository;
    const char* sourceCommit;
    const char* sourcePath;
    MethodGap gaps[29];
    std::uint32_t trailingMethodSize; // Historical 30th size; not scored upstream or locally.
    std::size_t minimumPointers;
    int minimumExact;
    std::int64_t maximumDrift;
    std::size_t cameraIndex, ucbIndex, renderIndex;
};
// R485 remotely inspected this fixed commit (2026-11-01); it is a source
// reference, NOT a claim about the latest remote HEAD or supported game builds.
// The existing 30-method window scores 29 gaps. Preserve that window and its
// extra-pointer bound rather than changing detection coverage in this cleanup.
inline constexpr MethodRecipe GameMethods {
    "genshin-ffx-fsr2-methods-7-layout-r487",
    "AizawaHikaru233/genshin_fsr_brigde",
    "26b0b7b7d7a0f30f05f213237c5449cc7d6b43a6",
    "Dx11FsrBridge/Il2CppCallSiteHook.cpp",
    {
        {0x10,0x10},{0x10,0x10},{0x10,0x10},{0x10,0x10},{0x10,0x10},
        {0x10,0x10},{0x10,0x10},{0x10,0x10},{0x10,0x10},{0xD0,0x40},
        {0x10,0x10},{0x170,0x100},{0x30,0x40},{0x20,0x10},{0x10,0x10},
        {0x130,0x100},{0x20,0x10},{0xB0,0x40},{0x70,0x40},{0x3E0,0x100},
        {0x1D0,0x100},{0x1CE0,0x100},{0x18C0,0x100},{0x120,0x100},{0x70,0x40},
        {0xFA0,0x400},{0x280,0x100},{0x260,0x100},{0x110,0x100}
    },
    0xA0, 64, 20, 0x1000, 19, 24, 25
};
inline constexpr std::size_t MethodGapCount = sizeof(GameMethods.gaps) / sizeof(GameMethods.gaps[0]);
inline constexpr std::size_t MethodCount = MethodGapCount + 1;

// Same fixed upstream source: on_render_enter field reads and install heads.
// These are the CURRENT known offsets, not automatic field/ABI discovery.
struct RenderFields
{
    std::size_t jitterX, jitterY, frameIndex, renderWidth, renderHeight, displayWidth, displayHeight;
};
inline constexpr RenderFields Render {0x24, 0x28, 0x50, 0x58, 0x5C, 0x68, 0x6C};
inline constexpr std::uint8_t RenderHead[] = {
    0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x56,0x57,0x55,0x53
};
inline constexpr std::uint8_t UcbHead[] = {0x56,0x57,0x55,0x53,0x48,0x83,0xEC,0x28};
inline constexpr std::size_t RenderPatchLength = sizeof(RenderHead);

// Preserve LOCAL pre-existing camera/projection trampoline boundaries; these
// are not imported from the upstream commit above. Passive camera observation,
// Render token/FIFO ownership and stub implementations remain in their owners.
// Camera's seven pushes plus sub rsp,0x140 occupy 14 bytes; a 12-byte patch
// would split the sub instruction. Keep its independent trampoline boundary.
inline constexpr std::uint8_t CameraHead[] = {
    0x41,0x57,0x41,0x56,0x56,0x57,0x53,0x48,0x81,0xEC,0x40,0x01,0x00,0x00
};
inline constexpr std::uint8_t ProjectionHead[] = {
    0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,0xEC,0x20,0x48,0x8B,0xFA,0x48,0x8B,0xD9
};
inline constexpr std::size_t CameraPatchLength = sizeof(CameraHead);
inline constexpr std::size_t ProjectionPatchLength = sizeof(ProjectionHead);

// R1034 passive camera layouts, each bound to its exact executable/code profile.
// These are static disassembly facts, not automatic ABI discovery or frame proof.
enum class PassiveCameraVersion : std::uint8_t { None, October2026, November2026 };
struct PassiveCameraLayout
{
    PassiveCameraVersion version;
    std::uint32_t imageSize, timestamp;
    std::uint64_t fileBytes;
    std::uint32_t renderRva, ucbRva, cameraRva;
    std::uint32_t graphicsDeviceRva, configRva, gpuMethodRva, mainThreadRva;
    std::uint32_t contextOwner, ownerContext, ownerInstance, contextInstance;
    std::uint32_t contextPrimary, ownerPrimary, contextSelector, contextAlternate, managedNative;
    std::uint32_t mainProjection, mainView, workerProjection, workerView;
    std::uint32_t automatic, projectionDirty, viewDirty, gpuMethodSlot, clip, reverse;
};
inline constexpr PassiveCameraLayout CameraOctober2026 {
    PassiveCameraVersion::October2026, 0x1B0AD000, 0x6AA122D0, 445006744,
    0x6DF8860, 0x6DF87F0, 0x6DF4B20,
    0x566B590, 0x54CCE50, 0x14FFA50, 0x568A07C,
    0x178, 0x270, 0x1B8, 0x1F8, 0x10, 0x268, 0x226, 0x140, 0x10,
    0x8D8, 0x898, 0x18E0, 0x18A0, 0xEC0, 0xEC3, 0xEC2, 0x178, 0xE0, 0xE1
};
inline constexpr PassiveCameraLayout CameraNovember2026 {
    PassiveCameraVersion::November2026, 0x1B65F000, 0x6AD886FB, 450927000,
    0x6F22900, 0x6F22890, 0x6F1EC20,
    0x5750F70, 0x55AFED0, 0x1510C90, 0x576FA5C,
    0x1A0, 0x270, 0x1B8, 0x120, 0x1B0, 0x268, 0x222, 0x128, 0x10,
    0x8D8, 0x898, 0x18E8, 0x18A8, 0xEC0, 0xEC3, 0xEC2, 0x178, 0xE0, 0xE1
};
inline constexpr const PassiveCameraLayout* CameraLayout(PassiveCameraVersion version) noexcept
{
    switch (version) {
    case PassiveCameraVersion::October2026: return &CameraOctober2026;
    case PassiveCameraVersion::November2026: return &CameraNovember2026;
    default: return nullptr;
    }
}

// Historical Config defaults only, copied from the ACTUAL dirty local source.
// Current TryInstallEntry requires successful detection and overwrites the
// three game-method RVAs; detection failure never authorizes these fallbacks.
struct LegacyRvas
{
    std::uint32_t render, ucb, camera, projectionSetter;
};
inline constexpr LegacyRvas Legacy70 {0x06B59670, 0x06B59600, 0x06B558D0, 0x013DC510};
} // namespace GenshinBridgeRecipes
