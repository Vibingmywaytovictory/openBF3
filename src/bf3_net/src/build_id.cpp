#include "build_id.h"
#include "bf3/core/util/splitmix64.h"
#include "bf3/net/protocol.h"

namespace bf3::net {

uint64_t game_build() {
	return core::splitmix64(kBuildSourceHash ^ kProtocolVersion);
}

}
