#pragma once

#include "bf3/core/ui/swf_movie.h"
#include "swf_bits.h"

namespace bf3::core::ui::detail {

Status read_shape(SwfReader &r, int p_version, WorkBudget &r_budget, SwfShape &r_shape);

Status read_fill_style(SwfReader &r, int p_version, SwfFillStyle &r_fill);

}
