#pragma once

#include "input_mapper.h"

#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/node.hpp>

#include <string>
#include <vector>

namespace bf3::godot_side {

class Bf3LookRateCheck : public godot::Node {
	GDCLASS(Bf3LookRateCheck, godot::Node)

public:
	void _ready() override;
	void _process(double p_delta) override;
	void _unhandled_input(const godot::Ref<godot::InputEvent> &p_event) override;

protected:
	static void _bind_methods();

private:
	struct Row {
		std::string name;
		bool pass = false;
		std::string detail;
	};
	void add(std::string p_name, bool p_pass, std::string p_detail);
	void run();
	void finish();
	ViewAngles push_motion(float p_dx, float p_dy);

	std::vector<Row> rows_;
	InputMapper mapper_;
	uint64_t routed_ = 0;
	bool done_ = false;
};

}
