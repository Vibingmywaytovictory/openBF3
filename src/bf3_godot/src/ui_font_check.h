#pragma once

#include <godot_cpp/classes/control.hpp>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace bf3::godot_side {

class Bf3UiAssets;

class Bf3UiFontCheck : public godot::Control {
	GDCLASS(Bf3UiFontCheck, godot::Control)

public:
	Bf3UiFontCheck();
	~Bf3UiFontCheck() override;

	void _ready() override;
	void _process(double p_delta) override;
	void _draw() override;

protected:
	static void _bind_methods();

private:
	struct Row {
		std::string name;
		bool pass = false;
		std::string detail;
	};
	void run_checks(Bf3UiAssets &p_assets);
	void finish();

	struct Work;
	void start_work(Bf3UiAssets &p_assets);
	void draw_fonts_page(Bf3UiAssets &p_assets);
	void draw_coverage_page(Bf3UiAssets &p_assets);
	void draw_icons_page(Bf3UiAssets &p_assets);
	bool save_shot(const std::string &p_file);

	std::vector<Row> rows_;
	std::string shot_dir_;
	bool check_ = false;
	int page_ = 0;
	int frames_on_page_ = 0;
	bool done_ = false;
	std::unique_ptr<Work> work_;
	std::thread worker_;
	std::atomic<bool> work_done_{ false };
	bool checked_ = false;
	int exit_code_ = 0;
};

}
