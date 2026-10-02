#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace wowee {
namespace core {

/**
 * Convert a 64-bit byte count to size_t, saturating instead of wrapping.
 * Byte budgets above 4 GiB (a 12 GB cache cap, 16 GB of RAM) are legal on 64-bit targets but
 * would silently become a small number, or 0, in a 32-bit size_t. A no-op on 64-bit.
 */
constexpr size_t clampToSizeT(uint64_t bytes) {
    return static_cast<size_t>(std::min<uint64_t>(bytes, std::numeric_limits<size_t>::max()));
}

/**
 * True when the byte range [offset, offset + length) lies inside a buffer of `total` bytes.
 * Written as a subtraction because `offset + length > total` wraps: in a 32-bit size_t, offset 0xFFFFFFFF and
 * length 2 add up to 1, which "fits". Exact on every target, for any 64-bit inputs.
 */
constexpr bool rangeFits(uint64_t offset, uint64_t length, size_t total) {
    return offset <= total && length <= total - offset;
}

} // namespace core
} // namespace wowee
