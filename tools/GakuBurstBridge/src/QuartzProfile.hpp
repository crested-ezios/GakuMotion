#pragma once
#include "NativeProfile.hpp"

namespace gbb::profile {
// Independently inspected on 3ED9E674D3855871FB257CFCD4CD30A4289FEDFD0A50981BD5BC47748A7D100F.
// The earlier Campus stage includes Quartz drivers; it is NOT the Swing ABI/subobject.
struct QuartzVariant {
    uint32_t swing, wrapper, wrapperEnd, quartz, quartzEnd, quartzReturn, swingReturn;
    unsigned char prefix[29];
    unsigned char quartzArgs[9]; uint32_t quartzArgsSize;
    unsigned char swingArgs[16]; uint32_t swingArgsSize;
};
inline constexpr QuartzVariant QuartzVariants[] = {
    {0x280b0,0x697d0,0x69c77,0x75cd0,0x93121,0x69a89,0x69ac7,
     {0x55,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x56,0x57,0x53,0xb8,0x48,0x1b,0,0,0xe8,0xce,0x7a,0x0c,0,0x48,0x29,0xc4,0x48,0x8d,0xac,0x24},
     {0x48,0x89,0xf9,0x48,0x8d,0x55,0x50},7,
     {0x48,0x81,0xc7,0xd0,0,0,0,0x48,0x89,0xf9,0x48,0x8d,0x55,0x50},14},
    {0xcc8e0,0x10ce00,0x10d267,0x117660,0x12a063,0x10d065,0x10d09d,
     {0x55,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x56,0x57,0x53,0xb8,0x78,0x22,0,0,0xe8,0x3e,0x61,0x02,0,0x48,0x29,0xc4,0x48,0x8d,0xac,0x24},
     {0x48,0x89,0xf9,0x48,0x89,0xf2,0xc5,0xf8,0x77},9,
     {0x48,0x81,0xc7,0xd0,0,0,0,0x48,0x89,0xf9,0x48,0x89,0xf2,0xc5,0xf8,0x77},16}
};
inline bool ExactFunction(uintptr_t base, uint32_t begin, uint32_t end) {
    DWORD64 found{}; auto* rf = RtlLookupFunctionEntry(base + begin, &found, nullptr);
    return rf && found == base && rf->BeginAddress == begin && rf->EndAddress == end;
}
inline bool RelativeCall(uintptr_t base, uint32_t returned, uint32_t target) {
    auto* p = reinterpret_cast<const unsigned char*>(base + returned - 5);
    int32_t delta{}; std::memcpy(&delta,p+1,4);
    return p[0] == 0xe8 && base + returned + delta == base + target;
}
inline const QuartzVariant* VerifyQuartz(HMODULE module, uintptr_t swing) {
    __try {
        const auto base = reinterpret_cast<uintptr_t>(module); const auto size = ImageSize(module);
        if (!base || !size) return nullptr;
        for (const auto& v : QuartzVariants) {
            if (swing != base + v.swing) continue;
            if (v.quartzEnd > size || v.wrapperEnd > size || !Executable(reinterpret_cast<void*>(base+v.quartz)) ||
                !ExactFunction(base,v.quartz,v.quartzEnd) || !ExactFunction(base,v.wrapper,v.wrapperEnd) ||
                std::memcmp(reinterpret_cast<void*>(base+v.quartz),v.prefix,sizeof(v.prefix)) ||
                !RelativeCall(base,v.quartzReturn,v.quartz) || !RelativeCall(base,v.swingReturn,v.swing) ||
                std::memcmp(reinterpret_cast<void*>(base+v.quartzReturn-5-v.quartzArgsSize),v.quartzArgs,v.quartzArgsSize) ||
                std::memcmp(reinterpret_cast<void*>(base+v.swingReturn-5-v.swingArgsSize),v.swingArgs,v.swingArgsSize)) return nullptr;
            return &v;
        }
        return nullptr;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
} // namespace gbb::profile
