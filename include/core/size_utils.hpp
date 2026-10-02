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

} // namespace core
} // namespace wowee
