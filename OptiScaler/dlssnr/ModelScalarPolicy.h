#pragma once

#include <Windows.h>
#include <bcrypt.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>

#pragma comment(lib, "bcrypt.lib")

namespace DlssNr::ModelScalars
{
enum class Consumer : unsigned int { Dx12, Vulkan };
inline std::atomic<bool> liveConsumers[2] {};

inline bool Live(Consumer consumer) noexcept
{
    return liveConsumers[static_cast<unsigned int>(consumer)].load(std::memory_order_acquire);
}

inline void Invalidate(Consumer consumer) noexcept
{
    liveConsumers[static_cast<unsigned int>(consumer)].store(false, std::memory_order_release);
}

namespace Detail
{
inline bool Readable(const void* pointer, size_t bytes, HMODULE module) noexcept
{
    auto cursor = reinterpret_cast<uintptr_t>(pointer);
    if (!cursor || bytes > (std::numeric_limits<uintptr_t>::max)() - cursor)
        return false;
    const auto end = cursor + bytes;
    while (cursor < end)
    {
        MEMORY_BASIC_INFORMATION region {};
        if (!VirtualQuery(reinterpret_cast<const void*>(cursor), &region, sizeof(region)) ||
            region.State != MEM_COMMIT || region.Type != MEM_IMAGE || region.AllocationBase != module ||
            (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
            !(region.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                                PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
            return false;
        const auto base = reinterpret_cast<uintptr_t>(region.BaseAddress);
        if (region.RegionSize > (std::numeric_limits<uintptr_t>::max)() - base)
            return false;
        const auto next = base + region.RegionSize;
        if (next <= cursor)
            return false;
        cursor = next;
    }
    return true;
}

inline bool InImage(DWORD rva, size_t bytes, DWORD imageSize) noexcept
{
    return rva <= imageSize && bytes <= imageSize - rva;
}

// Accept only relocation-free code. The reviewed pair has no .text fixups; rejecting any
// overlapping fixup avoids comparing relocated instructions against a raw-file digest under ASLR.
inline bool RelocationsAvoidCode(const unsigned char* base, const IMAGE_NT_HEADERS64* nt,
                                DWORD codeSize, HMODULE module) noexcept
{
    if (nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_BASERELOC)
        return false;
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    if (!directory.VirtualAddress && !directory.Size)
        return true;
    if (!directory.VirtualAddress || !directory.Size || directory.Size > 1024 * 1024 ||
        !InImage(directory.VirtualAddress, directory.Size, nt->OptionalHeader.SizeOfImage) ||
        !Readable(base + directory.VirtualAddress, directory.Size, module))
        return false;
    size_t consumed = 0;
    while (consumed < directory.Size)
    {
        if (directory.Size - consumed < sizeof(IMAGE_BASE_RELOCATION))
            return false;
        const auto block = reinterpret_cast<const IMAGE_BASE_RELOCATION*>(
            base + directory.VirtualAddress + consumed);
        if (block->SizeOfBlock < sizeof(IMAGE_BASE_RELOCATION) ||
            block->SizeOfBlock > directory.Size - consumed ||
            (block->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) % sizeof(WORD) != 0)
            return false;
        const auto entries = reinterpret_cast<const WORD*>(block + 1);
        const auto count = (block->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(WORD);
        for (size_t i = 0; i < count; ++i)
        {
            const auto type = entries[i] >> 12;
            if (type == IMAGE_REL_BASED_ABSOLUTE)
                continue;
            const uint64_t target = uint64_t(block->VirtualAddress) + (entries[i] & 0xfff);
            if (type != IMAGE_REL_BASED_DIR64 || target > nt->OptionalHeader.SizeOfImage ||
                sizeof(uint64_t) > nt->OptionalHeader.SizeOfImage - target ||
                (target < uint64_t(0x1000) + codeSize && target + sizeof(uint64_t) > 0x1000))
                return false;
        }
        consumed += block->SizeOfBlock;
    }
    return true;
}

inline bool ReviewedCode(HMODULE module, DWORD imageSize, DWORD codeSize, DWORD timestamp,
                         const std::array<unsigned char, 32>& expected) noexcept
{
    const auto base = reinterpret_cast<const unsigned char*>(module);
    if (!Readable(base, 4096, module))
        return false;
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
        dos->e_lfanew > 4096 - sizeof(IMAGE_NT_HEADERS64))
        return false;
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt->OptionalHeader.SizeOfImage != imageSize || nt->FileHeader.TimeDateStamp != timestamp ||
        nt->FileHeader.NumberOfSections == 0 ||
        nt->FileHeader.NumberOfSections > 96 ||
        nt->FileHeader.SizeOfOptionalHeader != sizeof(IMAGE_OPTIONAL_HEADER64))
        return false;
    const auto section = IMAGE_FIRST_SECTION(nt);
    const auto sectionOffset = static_cast<size_t>(reinterpret_cast<const unsigned char*>(section) - base);
    const auto sectionBytes = nt->FileHeader.NumberOfSections * sizeof(IMAGE_SECTION_HEADER);
    if (sectionOffset > imageSize || sectionBytes > imageSize - sectionOffset ||
        !Readable(section, sectionBytes, module))
        return false;
    bool found = false;
    for (unsigned int i = 0; i < nt->FileHeader.NumberOfSections; ++i)
        found |= std::memcmp(section[i].Name, ".text\0\0\0", IMAGE_SIZEOF_SHORT_NAME) == 0 &&
                 section[i].VirtualAddress == 0x1000 && section[i].Misc.VirtualSize == codeSize;
    if (!found || !InImage(0x1000, codeSize, imageSize) ||
        !Readable(base + 0x1000, codeSize, module) || !RelocationsAvoidCode(base, nt, codeSize, module))
        return false;

    // R457 recorded the PE-mapped VirtualSize bytes, not raw file padding. Both reviewed .text
    // sections contain NO base relocations, so their digest is invariant under ASLR. New/rebuilt
    // code, hooks, unreadable mappings and unknown images fail closed to the previous lifecycle.
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        return false;
    std::array<unsigned char, 32> digest {};
    const auto status = BCryptHash(algorithm, nullptr, 0, const_cast<PUCHAR>(base + 0x1000), codeSize,
                                   digest.data(), static_cast<ULONG>(digest.size()));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return status >= 0 && digest == expected;
}

inline bool ExportAt(HMODULE module, const char* name, uintptr_t rva) noexcept
{
    return reinterpret_cast<uintptr_t>(GetProcAddress(module, name)) ==
           reinterpret_cast<uintptr_t>(module) + rva;
}

inline bool KeysMatch(HMODULE module, DWORD imageSize, const std::array<DWORD, 5>& rvas) noexcept
{
    constexpr const char* keys[] = { "DLSSNR.Intensity", "DLSSNR.LocalToneStrength",
        "DLSSNR.LocalStructureStrength", "DLSSNR.UseAutoMask", "DLSSNR.SkinStructureStrength" };
    for (size_t i = 0; i < rvas.size(); ++i)
    {
        const auto bytes = std::strlen(keys[i]) + 1;
        if (!InImage(rvas[i], bytes, imageSize))
            return false;
        const auto pointer = reinterpret_cast<const char*>(module) + rvas[i];
        if (!Readable(pointer, bytes, module) || std::memcmp(pointer, keys[i], bytes) != 0)
            return false;
    }
    return true;
}
} // namespace Detail

// Called at successful DX12 feature creation / native Vulkan initialization, never from Evaluate
// or the UI. GetModuleHandleEx adds temporary references to ALREADY loaded modules (no LoadLibrary,
// no permanent PIN); FROM_ADDRESS identifies the exact forwarder. The reviewed forwarder retains
// snippet handles, and both consumers retain their forwarder handle. Invalidate on host load,
// creation/init attempt and shutdown; every new feature/init revalidates instead of trusting an
// address-keyed cache. Forced external unload or in-place modification of live code is unsupported.
inline void Observe(Consumer consumer, HMODULE forwarder, const wchar_t* snippetPath) noexcept
{
    Invalidate(consumer);
    bool supported = false;
    HMODULE heldForwarder = nullptr, snippet = nullptr;
    if (forwarder && snippetPath &&
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           reinterpret_cast<const wchar_t*>(forwarder), &heldForwarder) &&
        heldForwarder == forwarder && GetModuleHandleExW(0, snippetPath, &snippet))
    {
        constexpr std::array<unsigned char, 32> snippetCode {
            0x78,0xc2,0xac,0x87,0xb9,0x9a,0xf4,0x80,0x02,0xfd,0xfa,0xe2,0xf8,0x14,0x07,0x49,
            0x11,0x34,0x97,0x5a,0xd3,0xaa,0x45,0x89,0xcb,0x3e,0x77,0x6b,0x91,0x92,0xc3,0xb8 };
        constexpr std::array<unsigned char, 32> forwarderCode {
            0xc7,0xe4,0xf1,0xc0,0xdc,0x62,0x12,0x42,0x56,0x3f,0x17,0x3d,0xe9,0xa3,0x2f,0x82,
            0x19,0xf4,0xb5,0x49,0x57,0xdd,0xf3,0x77,0xe9,0x02,0x28,0x1a,0x5c,0x49,0xeb,0xc6 };
        supported = Detail::ReviewedCode(snippet, 276271104, 873452, 0x6ab6187c, snippetCode) &&
            Detail::ReviewedCode(heldForwarder, 172032, 85625, 0x6ad4950d, forwarderCode) &&
            Detail::KeysMatch(snippet, 276271104, {0xda340,0xda358,0xda378,0xda398,0xda3c8}) &&
            Detail::KeysMatch(heldForwarder, 172032, {0x1eee8,0x1ef30,0x1ef10,0x1eb88,0x1ef50}) &&
            Detail::ExportAt(snippet, "NVSDK_NGX_D3D12_EvaluateFeature", 0x15280) &&
            Detail::ExportAt(snippet, "NVSDK_NGX_VULKAN_EvaluateFeature", 0x24e70) &&
            Detail::ExportAt(heldForwarder, "dlssnr_call_create", 0x2aa0) &&
            Detail::ExportAt(heldForwarder, "dlssnr_call_evaluate_v2", 0x3060);
    }
    if (snippet)
        FreeLibrary(snippet);
    if (heldForwarder)
        FreeLibrary(heldForwarder);
    liveConsumers[static_cast<unsigned int>(consumer)].store(supported, std::memory_order_release);
}
} // namespace DlssNr::ModelScalars
