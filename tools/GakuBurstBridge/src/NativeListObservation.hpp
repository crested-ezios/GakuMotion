#pragma once
#include <cstdint>
#include <cstddef>
#include <limits>
namespace gbb {
struct ListHeader { uintptr_t payload{}; int32_t length{}, capacity{}; uint32_t allocator{}, padding{}; };
static_assert(sizeof(ListHeader) == 24);
inline bool ValidListHeader(const ListHeader& h, size_t stride) {
    return stride && stride <= 65536 && h.length >= 0 && h.capacity >= h.length && h.capacity <= 1000000 &&
        (!h.capacity || h.payload) && static_cast<size_t>(h.capacity) <= (std::numeric_limits<size_t>::max)() / stride &&
        static_cast<size_t>(h.capacity) * stride <= (std::numeric_limits<uintptr_t>::max)() - h.payload;
}
struct ListObservation {
    uintptr_t container{}, payload{}; int32_t length{}, capacity{}; size_t stride{};
    uint64_t samples{}, failures{}, changes{}; bool layoutVerified{};
    int lastError{}; // 0 valid, 1 no allocation, 2 unreadable header, 3 invalid length/capacity
};
}
