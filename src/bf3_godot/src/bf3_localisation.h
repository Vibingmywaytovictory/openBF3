#pragma once
// Localisation (plan §2.5 "Translation._get_message subclass", §3.8): BF3's UITextDatabase served
// to Godot's TranslationServer, so tr("ID_P_WNAME_M16A4") returns "M16A3".
//
// Bf3Localisation (a Node; main.tscn has one) locates the install like GameSession does, opens it and
// loads the text database on its own thread (no Godot API there). Back on the main thread it
// registers a Bf3Translation for locale "en" with the TranslationServer and emits "database_loaded".
// The opened Install is shared read-only with other consumers (the weapon inspector).
//
// Bf3Translation answers only SIDs (ID_[A-Z0-9_]*) the database resolves; anything else returns an
// empty StringName so other translations can still answer, and tr() then returns the message
// itself: a missing SID shows as the SID (plan §2.1).

#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/ui/text_db.h"

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/translation.hpp>
#include <godot_cpp/variant/string_name.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace bf3::godot_side {

class Bf3Translation : public godot::Translation {
	GDCLASS(Bf3Translation, godot::Translation)

public:
	void set_database(std::shared_ptr<const bf3::core::ui::TextDatabase> p_db) { db_ = std::move(p_db); }
	const bf3::core::ui::TextDatabase *database() const { return db_.get(); }

	godot::StringName _get_message(const godot::StringName &p_src_message, const godot::StringName &p_context) const override;
	godot::StringName _get_plural_message(const godot::StringName &p_src_message, const godot::StringName &p_src_plural_message, int32_t p_n,
			const godot::StringName &p_context) const override;

	// "ID_" followed by [A-Z0-9_]* (the SID census rule).
	static bool looks_like_sid(const std::string &p_text);

protected:
	static void _bind_methods() {}

private:
	std::shared_ptr<const bf3::core::ui::TextDatabase> db_;
};

class Bf3Localisation : public godot::Node {
	GDCLASS(Bf3Localisation, godot::Node)

public:
	Bf3Localisation();
	~Bf3Localisation() override;

	void _enter_tree() override;
	void _ready() override;
	void _process(double p_delta) override;
	void _exit_tree() override;

	// The node in the tree (main thread only); null when none.
	static Bf3Localisation *current();

	// Main thread, after "database_loaded".
	bool is_loaded() const { return registered_; }
	bool has_failed() const { return failed_; }
	godot::String get_error() const;
	int64_t get_entry_count() const;
	std::shared_ptr<const bf3::core::Install> install() const { return install_; }
	std::shared_ptr<const bf3::core::ui::TextDatabase> database() const { return db_; }
	const bf3::core::InstallReader &reader() const { return reader_; }
	// Snapshot taken when the install was located (the "before" of read-only checks).
	const bf3::core::InstallSnapshot &install_snapshot_at_start() const { return snapshot_; }
	double load_seconds() const { return load_seconds_; }

protected:
	static void _bind_methods();
	void _notification(int p_what);

private:
	void join_loader();
	void unregister();

	std::thread loader_;
	std::atomic<bool> loader_done_{ false };
	// Written by the loader before loader_done_ is set; read on the main thread after.
	bf3::core::InstallReader reader_;
	bf3::core::InstallSnapshot snapshot_;
	std::shared_ptr<const bf3::core::Install> install_;
	std::shared_ptr<const bf3::core::ui::TextDatabase> db_;
	std::string error_;
	double load_seconds_ = 0;

	godot::Ref<Bf3Translation> translation_;
	bool registered_ = false;
	bool failed_ = false;
};

} // namespace bf3::godot_side
