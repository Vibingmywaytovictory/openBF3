#pragma once
// SHA-1 (FIPS 180-4) of a byte range: checks stored payloads against their manifest/catalog sha1.

#include "bf3/core/ids.h"

#include <span>

namespace bf3::core {

Sha1 sha1_of(std::span<const uint8_t> p_bytes);

} // namespace bf3::core
