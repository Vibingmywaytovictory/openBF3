#pragma once

#include "bf3/core/ui/hud_icons.h"

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/texture2d.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace bf3::godot_side {

class Bf3HudItems : public godot::Node {
	GDCLASS(Bf3HudItems, godot::Node)

public:
	Bf3HudItems();
	~Bf3HudItems() override;

	void _ready() override;
	void _process(double p_delta) override;
	void _exit_tree() override;

	bool is_loaded() const { return finished_; }
	const bf3::core::ui::ItemIconIndex *item_index() const;
	godot::Ref<godot::Texture2D> icon(uint32_t p_item_id) const;
	size_t icon_count() const { return icons_.size(); }
	size_t icons_failed() const { return icons_failed_; }
	std::string load_error() const { return error_; }
	const std::string &localised(uint32_t p_sid_hash);

protected:
	static void _bind_methods();

private:
	struct Loaded;
	void join_loader();
	void start_loader();
	void finish_load();

	std::thread loader_;
	std::atomic<bool> loader_done_{ false };
	std::unique_ptr<Loaded> loaded_;
	bool finished_ = false;
	std::unordered_map<uint32_t, godot::Ref<godot::Texture2D>> icons_;
	size_t icons_failed_ = 0;
	std::string error_;
	std::unordered_map<uint32_t, std::string> names_;
};

}
