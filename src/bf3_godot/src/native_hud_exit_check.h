#pragma once

#include <godot_cpp/classes/node.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace bf3::godot_side {

class Bf3NativeHud;

inline constexpr uint32_t kNativeHudExitMaxWaitFrames = 36000;

class Bf3NativeHudExitCheck : public godot::Node {
	GDCLASS(Bf3NativeHudExitCheck, godot::Node)

public:
	void _ready() override;
	void _process(double p_delta) override;

protected:
	static void _bind_methods();

private:
	struct Row {
		std::string name;
		bool pass = false;
		std::string detail;
	};
	Bf3NativeHud *open_hud();
	void free_hud();
	void finish();
	void add(std::string p_name, bool p_pass, std::string p_detail);

	int32_t phase_ = 0;
	uint32_t frames_ = 0;
	Bf3NativeHud *hud_ = nullptr;
	std::vector<Row> rows_;
};

}
