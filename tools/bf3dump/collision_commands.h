#pragma once
// bf3dump collision command over bf3_core level/collision_soup.h: the collision soup of one level x
// mode in the canonical form of research/probes/tools/level-oracle `collision` (byte-identical
// when the C++ soup matches IW4L's pieces).

#include "bf3/core/install.h"
#include "bf3/core/level/collision_soup.h"

#include <memory>
#include <string>

namespace bf3dump {

// The canonical listing: sorted T placement lines, K tiles, totals, surface and body histograms.
std::string collision_listing(const bf3::core::level::LevelCollision &p_collision);
// `collision <LEVEL> <MODE>`: the listing on stdout, counts and timings on stderr.
int cmd_collision(const std::shared_ptr<bf3::core::Install> &p_install, const std::string &p_level, const std::string &p_mode);

} // namespace bf3dump
