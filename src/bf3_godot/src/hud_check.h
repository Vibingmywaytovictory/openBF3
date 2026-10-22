#pragma once

#include <godot_cpp/classes/color_rect.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>
#include <godot_cpp/classes/texture_rect.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace bf3::godot_side {

class Bf3NativeHud;
class Bf3HudItems;

class Bf3HudCheck : public godot::Node {
	GDCLASS(Bf3HudCheck, godot::Node)

public:
	Bf3HudCheck();
	~Bf3HudCheck() override;

	void _ready() override;
	void _process(double p_delta) override;
	void _exit_tree() override;

protected:
	static void _bind_methods() {}

private:
	struct Work;
	struct Frame;
	struct Tally;
	void start_work();
	void build_script();
	void verify_frame(const Frame &p_frame);
	void check_shot(const Frame &p_frame);
	void finish();
	void add_row(const std::string &p_name, bool p_pass, const std::string &p_detail);

	bool check_ = false;
	std::string shot_dir_;
	int state_ = 0;
	godot::SubViewport *viewport_ = nullptr;
	godot::ColorRect *background_ = nullptr;
	godot::TextureRect *view_ = nullptr;
	Bf3NativeHud *hud_ = nullptr;
	Bf3HudItems *items_ = nullptr;
	std::unique_ptr<Work> work_;
	std::thread worker_;
	std::atomic<bool> work_done_{ false };
	std::vector<Frame> frames_;
	std::vector<Tally> tallies_;
	size_t next_ = 0;
	int settle_ = 0;
	int hold_ = 0;
	bool pending_verify_ = false;
	struct Row {
		std::string name;
		bool pass = false;
		std::string detail;
	};
	std::vector<Row> rows_;
	int exit_code_ = 0;
	double started_s_ = 0;
};

}
