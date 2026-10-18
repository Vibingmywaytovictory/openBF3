#pragma once
// The level session's wave-3 state (G7, W3.0 contract, docs/design/m2a-w3.md; owner gx). Bf3LevelSession holds one
// M2aSession through m2a_ and calls the m2a_* hooks (level_session.h's M2a w3 block) from one tagged line each in
// level_session.cpp. gx fills the struct (Bf3Viewmodel, Bf3Hud, Bf3GameAudio, Bf3GameFx, Bf3GameCrowd, M2aCheck, the
// parsed args) and the hook bodies (level_session_m2a.cpp); W3.0 leaves it empty and the hooks do nothing.
//
// Hook order in one session (loader thread: no Godot API):
//   main    _ready -> parse_args -> m2a_parse_args
//   loader  run_loader: ... m2a_load_options(shared options) -> load_shared_level -> S1 bot level ->
//           m2a_load_assets(db, cache, threads) -> start_session: ... m2a_server_config(game) -> ServerHost::start ->
//           client_config(...) -> m2a_client_config(cc) -> ClientSession::start
//   main    _process: read_presentation -> mapper -> set_input -> m2a_presented (every frame from session start:
//           drains the PresentationEvents ring so it never overflows during the load, E14) ... after Done:
//           cameras -> draw_others -> update_hud -> S1 -> m2a_frame (true: the M2a check or the play scene handled
//           the frame, M1's checks and shots are skipped)
//           _unhandled_input: mapper -> m2a_input
//   main    shutdown: loader joined -> m2a_shutdown -> client and server stop
// restart_client calls client_config, so m2a_client_config runs again for the new client.

#include "level_session.h"

namespace bf3::godot_side {

struct Bf3LevelSession::M2aSession {};

} // namespace bf3::godot_side
