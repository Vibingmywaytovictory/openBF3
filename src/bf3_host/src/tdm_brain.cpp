// The TDM bots (docs/design/m2b.md §5; owner bb). W1.0 writes only the hook GameServer::init links against
// (docs/design/m2b-w1.md); bb replaces this file's body with the brains.
#include "bf3/host/tdm_brain.h"

namespace bf3::host {

core::Status add_tdm_bots(GameServer &, const GameServerConfig &p_config) {
	if (p_config.tdm_bots == nullptr || p_config.tdm_bots->count == 0) { // M2b W1.0 stub
		return core::Status::success(); // M2b W1.0 stub
	} // M2b W1.0 stub
	return core::make_error("add_tdm_bots: W1.0 stub (bb has not landed the TDM brains)"); // M2b W1.0 stub
}

} // namespace bf3::host
