#include "tdm_flow.h"

#include "godot_util.h"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/project_settings.hpp>

namespace bf3::godot_side {

using namespace godot;

std::string config_path(const char *p_res) {
	const String path = ProjectSettings::get_singleton()->globalize_path(p_res);
	return FileAccess::file_exists(path) ? to_std(path) : std::string();
}

TdmArgText tdm_arg_text() {
	TdmArgText a;
	a.has_kill_limit = has_user_arg("kill-limit");
	a.kill_limit = user_arg_value("kill-limit");
	a.has_preround = has_user_arg("preround");
	a.preround = user_arg_value("preround");
	a.has_min_players = has_user_arg("min-players");
	a.min_players = user_arg_value("min-players");
	a.has_bots = has_user_arg("bots");
	a.bots = user_arg_value("bots");
	a.has_deploy_screen = has_user_arg("deploy-screen");
	a.deploy_screen = user_arg_value("deploy-screen");
	return a;
}

}
