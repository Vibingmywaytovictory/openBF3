#pragma once

#include "bf3/core/ids.h"

#include <span>

namespace bf3::core {

Sha1 sha1_of(std::span<const uint8_t> p_bytes);

}
