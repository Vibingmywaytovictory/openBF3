#include "match_host.h"

#include "bf3_localisation.h"
#include "game_settings.h"
#include "godot_cache.h"
#include "godot_util.h"
#include "level_session.h"
#include "minimap_hud.h"
#include "tdm_flow.h"
#include "ui_assets.h"
#include "video_memory.h"

#include "bf3/core/install.h"
#include "bf3/core/util/key_values.h"
#include "bf3/net/netsim.h"

#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cstdlib>
#include <utility>

namespace bf3::godot_side {

using namespace godot;
namespace host = bf3::host;

namespace {

Bf3MatchHost *g_current = nullptr;

void say(const std::string &p_line) {
	UtilityFunctions::print(to_godot("[match] " + p_line));
}

}

Bf3MatchHost::Bf3MatchHost() = default;

Bf3MatchHost::~Bf3MatchHost() = default;

void Bf3MatchHost::_bind_methods() {}

Bf3MatchHost *Bf3MatchHost::current() {
	return g_current;
}

Bf3LevelSession *Bf3MatchHost::session() const {
	return Object::cast_to<Bf3LevelSession>(get_node_or_null(NodePath("../Session")));
}

std::shared_ptr<host::ClientSession> Bf3MatchHost::client() const {
	std::lock_guard lock(mutex_);
	return client_;
}

bool Bf3MatchHost::parse_connect() {
	const std::string text = user_arg_value("connect");
	const size_t colon = text.rfind(':');
	uint32_t port = 0;
	if (colon == std::string::npos || colon == 0 ||
			bf3::core::parse_u32_value(std::string_view(text).substr(colon + 1), 1.0, 65535.0, port).fault !=
					bf3::core::KeyFault::None) {
		end_match("--connect=" + text + ": expected host:port with a port 1..65535");
		return false;
	}
	connect_host_ = text.substr(0, colon);
	connect_port_ = uint16_t(port);
	return true;
}

void Bf3MatchHost::_ready() {
	if (Engine::get_singleton()->is_editor_hint() || !(has_user_arg("connect"))) {
		set_process(false);
		set_process_unhandled_input(false);
		return;
	}
	g_current = this;
	following_ = true;
	headless_ = DisplayServer::get_singleton()->get_name() == String("headless");
	if (!user_arg_value("netsim").empty()) {
		netsim_ = user_arg_value("netsim");
	}
	if (has_user_arg("seed")) {
		seed_ = uint64_t(std::strtoull(user_arg_value("seed").c_str(), nullptr, 10));
	}
	if (const Bf3LevelSession *s = session()) {
		scene_game_ = s->has_meta("openbf3_game") && bool(s->get_meta("openbf3_game"));
		scene_tdm_ = s->has_meta("openbf3_tdm") && bool(s->get_meta("openbf3_tdm"));
	}
	if (!parse_connect()) {
		return;
	}
	persistent_ui();
	say("connecting to " + connect_host_ + ":" + std::to_string(connect_port_) +
			": level, mode and rules come from the server");
	worker_ = std::thread([this] { connect_client(connect_host_, connect_port_); });
	set_process(true);
}

void Bf3MatchHost::persistent_ui() {
	if (headless_) {
		return;
	}
	if (Bf3Localisation::current() == nullptr) {
		Bf3Localisation *localisation = memnew(Bf3Localisation);
		localisation->set_name("MatchLocalisation");
		add_child(localisation);
	}
	if (Bf3UiAssets::current() == nullptr) {
		Bf3UiAssets *assets = memnew(Bf3UiAssets);
		assets->set_name("MatchUiAssets");
		add_child(assets);
	}
}

host::ClientSessionConfig Bf3MatchHost::client_config() const {
	host::ClientSessionConfig cc;
	cc.host = connect_host_;
	cc.port = connect_port_;
	cc.name = "player";
	if (bf3::core::Result<bf3::net::NetSimConfig> ns = bf3::net::parse_netsim(netsim_)) {
		cc.netsim = *ns;
	}
	cc.netsim.seed = seed_ * 7919u + 1u;
	cc.request_dev_commands = true;
	return cc;
}

void Bf3MatchHost::connect_client(const std::string &p_host, uint16_t p_port) {
	host::ClientSessionConfig cc = client_config();
	cc.host = p_host;
	cc.port = p_port;
	auto made = std::make_shared<host::ClientSession>();
	const bf3::core::Status s = made->start(cc, nullptr);
	std::lock_guard lock(mutex_);
	if (!s.ok()) {
		worker_error_ = s.error().message;
		return;
	}
	client_ = std::move(made);
}

void Bf3MatchHost::_process(double p_delta) {
	(void)p_delta;
	if (ended_) {
		return;
	}
	watch_link();
	if (ended_ || phase_ != Phase::Lobby) {
		return;
	}
	const std::shared_ptr<host::ClientSession> c = client();
	bf3::net::MatchInfo m;
	if (c == nullptr || !c->lobby_match(m)) {
		return;
	}
	MatchLevel level;
	level.level = m.level;
	level.mode = m.mode;
	level.rules_flags = m.rules_flags;
	level.rules_known = true;
	level.index = level_index_;
	free_session();
	make_session(level);
	phase_ = Phase::Playing;
}

void Bf3MatchHost::watch_link() {
	std::string error;
	{
		std::lock_guard lock(mutex_);
		error = worker_error_;
		worker_error_.clear();
	}
	if (!error.empty()) {
		end_match(error);
		return;
	}
	const std::shared_ptr<host::ClientSession> c = client();
	if (c == nullptr) {
		return;
	}
	const host::ClientStats link = c->stats();
	if (!link.connected && link.last_error[0] != '\0') {
		end_match(std::string("the connection to ") + connect_host_ + ":" + std::to_string(connect_port_) +
				" ended: " + link.last_error.data());
	}
}

void Bf3MatchHost::free_session() {
	Node *parent = get_parent();
	if (Bf3LevelSession *s = session()) {
		s->shutdown_level();
		parent->remove_child(s);
		s->queue_free();
	}
	if (Node *minimap = get_node_or_null(NodePath("../MinimapHud"))) {
		parent->remove_child(minimap);
		minimap->queue_free();
	}
}

void Bf3MatchHost::make_session(const MatchLevel &p_level) {
	Node *parent = get_parent();
	Bf3LevelSession *s = memnew(Bf3LevelSession);
	s->set_name("Session");
	s->set_meta("openbf3_game", scene_game_);
	s->set_meta("openbf3_tdm", scene_tdm_);
	s->assign_level(p_level);
	parent->add_child(s);
	parent->move_child(s, get_index() + 1);
	Bf3MinimapHud *minimap = memnew(Bf3MinimapHud);
	minimap->set_name("MinimapHud");
	parent->add_child(minimap);
}

void Bf3MatchHost::end_match(const std::string &p_reason) {
	if (ended_) {
		return;
	}
	ended_ = true;
	UtilityFunctions::print(to_godot("[level] " + p_reason));
	if (Bf3LevelSession *s = session(); s != nullptr && s->level_assigned()) {
		s->finish(1);
		return;
	}
	get_tree()->quit(1);
}

void Bf3MatchHost::level_done(const Bf3LevelSession &p_session) {
	(void)p_session;
}

void Bf3MatchHost::_unhandled_input(const Ref<InputEvent> &p_event) {
	(void)p_event;
}

void Bf3MatchHost::_exit_tree() {
	if (g_current == this) {
		g_current = nullptr;
	}
	if (worker_.joinable()) {
		worker_.join();
	}
	std::shared_ptr<host::ClientSession> c = client();
	if (c != nullptr) {
		c->stop();
	}
}

}
