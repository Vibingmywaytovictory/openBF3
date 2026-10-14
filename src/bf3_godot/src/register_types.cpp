// GDExtension entry of openBF3 (entry_symbol "openbf3_library_init" in openbf3.gdextension).

#include "dedicated_main_loop.h"
#include "game_session.h"

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
}

static void uninitialize_openbf3(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
	// Safety net (plan §2.3): no server or client thread may outlive the extension.
	bf3::host::LoopbackClient::stop_all();
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
