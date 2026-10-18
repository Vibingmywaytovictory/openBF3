// The level session's wave-3 hooks (G7, W3.0 contract, docs/design/m2a-w3.md; owner gx). See level_session_m2a.h.
// W3.0: every hook does nothing, so the session runs exactly as before; gx replaces the bodies.

#include "level_session_m2a.h"

namespace bf3::godot_side {

void Bf3LevelSession::M2aSessionDeleter::operator()(M2aSession *p_session) const {
	delete p_session;
}

void Bf3LevelSession::m2a_parse_args() {}

void Bf3LevelSession::m2a_load_options(bf3::host::SharedLoadOptions &) {}

bool Bf3LevelSession::m2a_load_assets(const bf3::host::LevelDatabase &, bf3::core::Cache *, unsigned, std::string *) {
	return true;
}

void Bf3LevelSession::m2a_server_config(bf3::host::GameServerConfig &) {}

void Bf3LevelSession::m2a_client_config(bf3::host::ClientSessionConfig &) const {}

void Bf3LevelSession::m2a_presented(double, uint64_t) {}

bool Bf3LevelSession::m2a_frame(double, uint64_t) {
	return false;
}

void Bf3LevelSession::m2a_input(const godot::Ref<godot::InputEvent> &) {}

void Bf3LevelSession::m2a_shutdown() {}

} // namespace bf3::godot_side
