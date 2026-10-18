// The firefight stub bots (docs/design/m2a.md §10.2; owner bt). W3.0 contract stub (docs/design/m2a-w3.md G3): only
// the hook GameServer::init calls has a body here, so the tree links; bt replaces this file.

#include "bf3/host/firefight_brain.h"

namespace bf3::host {

core::Status add_firefight_bots(GameServer &, const GameServerConfig &p_config) {
	if (p_config.firefight == nullptr || p_config.firefight->count == 0) {
		return core::Status::success();
	}
	return core::make_error("firefight bots: not built yet (W3.0 stub; bt's firefight_brain.cpp)");
}

} // namespace bf3::host
