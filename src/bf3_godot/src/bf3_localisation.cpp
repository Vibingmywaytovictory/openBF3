#include "bf3_localisation.h"

#include "godot_util.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/translation_server.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <chrono>
#include <cstdio>

namespace bf3::godot_side {

using namespace godot;

namespace {

Bf3Localisation *g_current = nullptr;

}

bool Bf3Translation::looks_like_sid(const std::string &p_text) {
	if (p_text.size() < 4 || p_text.compare(0, 3, "ID_") != 0) {
		return false;
	}
	for (const char c : p_text) {
		if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) {
			return false;
		}
	}
	return true;
}

StringName Bf3Translation::_get_message(const StringName &p_src_message, const StringName &p_context) const {
	(void)p_context;
	if (!db_) {
		return StringName();
	}
	const std::string sid = to_std(String(p_src_message));
	if (!looks_like_sid(sid) || !db_->resolves(sid)) {
		return StringName();
	}
	const std::optional<std::string_view> text = db_->find_sid(sid);
	return StringName(to_godot(std::string(*text)));
}

StringName Bf3Translation::_get_plural_message(const StringName &p_src_message, const StringName &p_src_plural_message, int32_t p_n,
		const StringName &p_context) const {

	return _get_message(p_n == 1 ? p_src_message : p_src_plural_message, p_context);
}

Bf3Localisation::Bf3Localisation() = default;

Bf3Localisation::~Bf3Localisation() {
	join_loader();
	if (g_current == this) {
		g_current = nullptr;
	}
}

Bf3Localisation *Bf3Localisation::current() {
	return g_current;
}

void Bf3Localisation::_bind_methods() {
	ClassDB::bind_method(D_METHOD("is_loaded"), &Bf3Localisation::is_loaded);
	ClassDB::bind_method(D_METHOD("has_failed"), &Bf3Localisation::has_failed);
	ClassDB::bind_method(D_METHOD("get_error"), &Bf3Localisation::get_error);
	ClassDB::bind_method(D_METHOD("get_entry_count"), &Bf3Localisation::get_entry_count);
	ADD_SIGNAL(MethodInfo("database_loaded", PropertyInfo(Variant::BOOL, "ok")));
}

void Bf3Localisation::_notification(int p_what) {

	if (p_what == NOTIFICATION_PREDELETE) {
		join_loader();
		unregister();
	}
}

void Bf3Localisation::_enter_tree() {

	if (!Engine::get_singleton()->is_editor_hint() && g_current == nullptr) {
		g_current = this;
	}
}

void Bf3Localisation::_ready() {
	if (Engine::get_singleton()->is_editor_hint()) {
		return;
	}

	bf3::core::InstallLocateOptions opts;
	opts.cmdline_path = user_arg_value("bf3-install");
	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (ps->has_setting("openbf3/install/path")) {
		opts.setting_path = to_std(String(ps->get_setting("openbf3/install/path")));
	}
	loader_ = std::thread([this, opts]() {
		const auto start = std::chrono::steady_clock::now();
		if (!reader_.locate(opts)) {
			error_ = "install not usable: " + reader_.describe();
		} else {
			snapshot_ = reader_.snapshot();
			bf3::core::Result<std::shared_ptr<bf3::core::Install>> install = bf3::core::Install::open(reader_);
			if (!install) {
				error_ = "open install: " + install.error().message;
			} else {
				install_ = *install;
				bf3::core::Result<bf3::core::ui::TextDatabase> db = bf3::core::ui::open_text_database(install_);
				if (!db) {
					error_ = "text database: " + db.error().message;
				} else {
					db_ = std::make_shared<const bf3::core::ui::TextDatabase>(std::move(*db));
				}
			}
		}
		load_seconds_ = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
		loader_done_.store(true, std::memory_order_release);
	});
	set_process(true);
}

void Bf3Localisation::_process(double p_delta) {
	(void)p_delta;
	if (registered_ || failed_ || !loader_done_.load(std::memory_order_acquire)) {
		return;
	}
	join_loader();
	set_process(false);
	if (!db_) {
		failed_ = true;
		UtilityFunctions::push_warning(to_godot("[openbf3] localisation: " + error_));
		emit_signal("database_loaded", false);
		return;
	}
	translation_.instantiate();
	translation_->set_locale("en");
	translation_->set_database(db_);
	TranslationServer::get_singleton()->add_translation(translation_);
	registered_ = true;
	char line[160];
	std::snprintf(line, sizeof(line), "[openbf3] localisation: %zu strings (tag \"%s\") for locale en in %.2f s", db_->size(), db_->tag().c_str(),
			load_seconds_);
	UtilityFunctions::print(to_godot(line));
	emit_signal("database_loaded", true);
}

void Bf3Localisation::_exit_tree() {
	join_loader();
	unregister();
	if (g_current == this) {
		g_current = nullptr;
	}
}

String Bf3Localisation::get_error() const {
	return to_godot(error_);
}

int64_t Bf3Localisation::get_entry_count() const {
	return registered_ && db_ ? static_cast<int64_t>(db_->size()) : 0;
}

void Bf3Localisation::join_loader() {
	if (loader_.joinable()) {
		loader_.join();
	}
}

void Bf3Localisation::unregister() {
	if (registered_ && translation_.is_valid() && TranslationServer::get_singleton() != nullptr) {
		TranslationServer::get_singleton()->remove_translation(translation_);
	}
	registered_ = false;
	translation_.unref();
}

}
