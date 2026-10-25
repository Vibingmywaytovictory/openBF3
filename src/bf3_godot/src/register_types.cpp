#include "bf3_localisation.h"

#include "audio_stream.h"
#include "collision_overlay.h"
#include "dedicated_main_loop.h"
#include "game_session.h"
#include "level_session.h"
#include "minimap_control.h"
#include "minimap_hud.h"
#include "crowd_scene.h"
#include "weapon_inspector.h"
#include "world_icons.h"

#include "swf_view.h"
#include "ui_assets.h"

#include "viewmodel.h"

#include "fx_renderer.h"

#if OPENBF3_BUILD_CHECKS
#include "audio_demo.h"
#include "fx_demo.h"
#include "fx_ref.h"
#include "hud_check.h"
#include "hud_tdm_demo.h"
#include "m2a_viewmodel_demo.h"
#include "swf_view_check.h"
#include "ui_font_check.h"
#include "viewmodel_demo.h"
#include "widget_shots_check.h"
#endif

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
	GDREGISTER_CLASS(bf3::godot_side::Bf3Translation);
	GDREGISTER_CLASS(bf3::godot_side::Bf3Localisation);
	GDREGISTER_CLASS(bf3::godot_side::Bf3WeaponInspector);
	GDREGISTER_CLASS(bf3::godot_side::Bf3CollisionOverlay);

	GDREGISTER_CLASS(bf3::godot_side::Bf3LevelSession);

	GDREGISTER_CLASS(bf3::godot_side::Bf3Crowd);

	GDREGISTER_CLASS(bf3::godot_side::Bf3UiAssets);
	GDREGISTER_CLASS(bf3::godot_side::Bf3SwfView);

	GDREGISTER_CLASS(bf3::godot_side::Bf3MinimapControl);
	GDREGISTER_CLASS(bf3::godot_side::Bf3WorldIcons);
	GDREGISTER_CLASS(bf3::godot_side::Bf3MinimapHud);

	GDREGISTER_CLASS(bf3::godot_side::Bf3Viewmodel);

	GDREGISTER_CLASS(bf3::godot_side::Bf3FxRenderer);

	void openbf3_register_hud();
	openbf3_register_hud();

	void openbf3_register_native_hud();
	openbf3_register_native_hud();
	void openbf3_register_native_screens();
	openbf3_register_native_screens();

	GDREGISTER_CLASS(bf3::godot_side::Bf3AudioStream);
	GDREGISTER_CLASS(bf3::godot_side::Bf3AudioPlayback);

	void openbf3_register_tdm_hud();
	openbf3_register_tdm_hud();

	void openbf3_register_ui_effects();
	openbf3_register_ui_effects();

#if OPENBF3_BUILD_CHECKS
	GDREGISTER_CLASS(bf3::godot_side::Bf3ViewmodelDemo);
	GDREGISTER_CLASS(bf3::godot_side::Bf3UiFontCheck);
	GDREGISTER_CLASS(bf3::godot_side::Bf3SwfViewCheck);
	GDREGISTER_CLASS(bf3::godot_side::Bf3M2aViewmodelDemo);
	GDREGISTER_CLASS(bf3::godot_side::Bf3FxDemo);
	GDREGISTER_CLASS(bf3::godot_side::Bf3FxRef);
	GDREGISTER_CLASS(bf3::godot_side::Bf3HudCheck);
	GDREGISTER_CLASS(bf3::godot_side::Bf3AudioDemo);
	GDREGISTER_CLASS(bf3::godot_side::Bf3TdmHudDemo);
	GDREGISTER_CLASS(bf3::godot_side::Bf3WidgetShotsCheck);

	void openbf3_register_m2a_tp();
	openbf3_register_m2a_tp();

	void openbf3_register_m2a_fx3();
	openbf3_register_m2a_fx3();

	void openbf3_register_ui_effects_check();
	openbf3_register_ui_effects_check();
#endif
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
