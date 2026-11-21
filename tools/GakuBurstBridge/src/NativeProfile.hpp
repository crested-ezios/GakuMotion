#pragma once
#include <windows.h>
#include <cstdint>
#include <cstring>

namespace gbb::profile {
// A measured ABI profile, not a signature scanner. Update only after the SOP's
// offline and in-game verification. Disk hashes and field definitions live in profiles/supported-build-20261008.json.
inline constexpr char Name[] = "gakumas-burst-20261008-windows-x64";
inline constexpr int BoxHeader = 16, StreamSize = 56, HandleSize = 12;
inline constexpr int Hair = 80, Arm = 144, Swing = 208, SwingTime = 556;
inline constexpr uint32_t DeltaSlot = 0x1585c0;
struct Variant { uint32_t begin, end, returned; };
inline constexpr Variant Variants[] = {
    {0x280b0, 0x28e48, 0x281d4}, {0xcc8e0, 0xcd3f3, 0xcc9c8}
};
inline constexpr unsigned char Entry[] = {
    0x55,0x41,0x57,0x41,0x56,0x56,0x57,0x53,0x48,0x81,0xec,0x38,0x03,0,0
};
struct Field { const char* name; int offset; };
inline constexpr Field StreamFields[] = {
    {"m_AnimatorBindingsVersion",0}, {"constant",8}, {"input",16},
    {"output",24}, {"workspace",32}, {"inputStreamAccessor",40}, {"animationHandleBinder",48}
};
inline constexpr Field HandleFields[] = {
    {"m_AnimatorBindingsVersion",0}, {"handleIndex",4}, {"skeletonIndex",8}
};
inline bool Executable(const void* ptr) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!ptr || !VirtualQuery(ptr, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
        (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    const DWORD p = mbi.Protect & 0xff;
    return p == PAGE_EXECUTE || p == PAGE_EXECUTE_READ ||
           p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
}
inline uint32_t ImageSize(HMODULE module) {
    __try {
        if (!module) return 0;
        auto base = reinterpret_cast<uintptr_t>(module);
        auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0) return 0;
        auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        return nt->Signature == IMAGE_NT_SIGNATURE && nt->FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 &&
            nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC ? nt->OptionalHeader.SizeOfImage : 0;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
inline uintptr_t VerifyCaller(uintptr_t caller, HMODULE module, void* deltaEntry) {
    __try {
        if (!module || !deltaEntry) return 0;
        auto base = reinterpret_cast<uintptr_t>(module);
        auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0) return 0;
        auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
            nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
            DeltaSlot + sizeof(void*) > nt->OptionalHeader.SizeOfImage) return 0;
        if (caller < base || caller >= base + nt->OptionalHeader.SizeOfImage) return 0;
        const Variant* variant = nullptr;
        for (const auto& v : Variants) if (caller - base == v.returned) variant = &v;
        if (!variant) return 0;
        DWORD64 foundBase{};
        auto* rf = RtlLookupFunctionEntry(caller, &foundBase, nullptr);
        if (!rf || foundBase != base || rf->BeginAddress != variant->begin || rf->EndAddress != variant->end) return 0;
        auto begin = base + variant->begin;
        if (!Executable(reinterpret_cast<void*>(begin)) ||
            std::memcmp(reinterpret_cast<void*>(begin), Entry, sizeof(Entry))) return 0;
        auto* call = reinterpret_cast<const unsigned char*>(caller - 6);
        if (call[0] != 0xff || call[1] != 0x15) return 0;
        int32_t displacement{};
        std::memcpy(&displacement, call + 2, sizeof(displacement));
        const uintptr_t slot = caller + displacement;
        if (slot != base + DeltaSlot) return 0;
        uintptr_t callee{};
        std::memcpy(&callee, reinterpret_cast<void*>(slot), sizeof(callee));
        return callee == reinterpret_cast<uintptr_t>(deltaEntry) ? begin : 0;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
} // namespace gbb::profile
