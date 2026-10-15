// GDExtension entry of openBF3 (entry_symbol "openbf3_library_init" in openbf3.gdextension).

#include "bf3_localisation.h"
#include "collision_overlay.h"
#include "dedicated_main_loop.h"
#include "game_session.h"
#include "level_session.h"
#include "viewmodel_demo.h"
#include "weapon_inspector.h"
// UI ui1
#include "ui_assets.h"
#include "ui_font_check.h"

#include "bf3/host/client_session.h"
#include "bf3/host/loopback_client.h"
#include "bf3/host/server_host.h"

#include <gdextension_interface.h>

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>

using namespace godot;

static void initialize_openbf3(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
	GDREGISTER_CLASS(bf3::godot_side::GameSession);
	GDREGISTER_CLASS(bf3::godot_side::DedicatedMainLoop);
	GDREGISTER_CLASS(bf3::godot_side::Bf3ViewmodelDemo);
	GDREGISTER_CLASS(bf3::godot_side::Bf3Translation);
	GDREGISTER_CLASS(bf3::godot_side::Bf3Localisation);
	GDREGISTER_CLASS(bf3::godot_side::Bf3WeaponInspector);
	GDREGISTER_CLASS(bf3::godot_side::Bf3CollisionOverlay);
	// M1 T6
	GDREGISTER_CLASS(bf3::godot_side::Bf3LevelSession);
	// UI ui1
	GDREGISTER_CLASS(bf3::godot_side::Bf3UiAssets);
	GDREGISTER_CLASS(bf3::godot_side::Bf3UiFontCheck);
}

static void uninitialize_openbf3(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
	// Safety net (plan §2.3): no server or client thread may outlive the extension.
	bf3::host::LoopbackClient::stop_all();
	bf3::host::ClientSession::stop_all(); // M1 T6: the M1 client threads, before the server
	bf3::host::ServerHost::stop_all();
}

extern "C" {
GDExtensionBool GDE_EXPORT openbf3_library_init(GDExtensionInterfaceGetProcAddress p_get_proc_address, GDExtensionClassLibraryPtr p_library,
		GDExtensionInitialization *r_initialization) {
	GDExtensionBinding::InitObject init_obj(p_get_proc_address, p_library, r_initialization);
	init_obj.register_initializer(initialize_openbf3);
	init_obj.register_terminator(uninitialize_openbf3);
	init_obj.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);
	return init_obj.init();
}
}
