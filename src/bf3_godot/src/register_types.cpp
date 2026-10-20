#include "bf3_localisation.h"

#include "audio_demo.h"
#include "audio_stream.h"
#include "collision_overlay.h"
#include "dedicated_main_loop.h"
#include "game_session.h"
#include "level_session.h"
#include "minimap_control.h"
#include "minimap_hud.h"
#include "s3_crowd.h"
#include "viewmodel_demo.h"
#include "weapon_inspector.h"
#include "world_icons.h"

#include "ui_assets.h"
#include "ui_font_check.h"

#include "m2a_viewmodel_demo.h"
#include "viewmodel.h"

#include "fx_demo.h"
#include "fx_ref.h"
#include "fx_renderer.h"

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

	GDREGISTER_CLASS(bf3::godot_side::Bf3LevelSession);

	GDREGISTER_CLASS(bf3::godot_side::Bf3Crowd);

	GDREGISTER_CLASS(bf3::godot_side::Bf3UiAssets);
	GDREGISTER_CLASS(bf3::godot_side::Bf3UiFontCheck);

	GDREGISTER_CLASS(bf3::godot_side::Bf3MinimapControl);
	GDREGISTER_CLASS(bf3::godot_side::Bf3WorldIcons);
	GDREGISTER_CLASS(bf3::godot_side::Bf3MinimapHud);

	GDREGISTER_CLASS(bf3::godot_side::Bf3Viewmodel);
	GDREGISTER_CLASS(bf3::godot_side::Bf3M2aViewmodelDemo);

	GDREGISTER_CLASS(bf3::godot_side::Bf3FxRenderer);
	GDREGISTER_CLASS(bf3::godot_side::Bf3FxDemo);
	GDREGISTER_CLASS(bf3::godot_side::Bf3FxRef);

	void openbf3_register_m2a_hud();
	openbf3_register_m2a_hud();

	GDREGISTER_CLASS(bf3::godot_side::Bf3AudioStream);
	GDREGISTER_CLASS(bf3::godot_side::Bf3AudioPlayback);
	GDREGISTER_CLASS(bf3::godot_side::Bf3AudioDemo);

	void openbf3_register_m2a_tp();
	openbf3_register_m2a_tp();

	void openbf3_register_m2a_fx3();
	openbf3_register_m2a_fx3();

	void openbf3_register_m2b_ui();
	openbf3_register_m2b_ui();

	void openbf3_register_pt1_iff();
	openbf3_register_pt1_iff();
}

static void uninitialize_openbf3(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}

	bf3::host::LoopbackClient::stop_all();
	bf3::host::ClientSession::stop_all();
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
