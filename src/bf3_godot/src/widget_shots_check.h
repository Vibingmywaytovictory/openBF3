#pragma once

#include <godot_cpp/classes/node.hpp>

#include <memory>

namespace bf3::godot_side {

class Bf3WidgetShotsCheck : public godot::Node {
	GDCLASS(Bf3WidgetShotsCheck, godot::Node)

public:
	Bf3WidgetShotsCheck();
	~Bf3WidgetShotsCheck() override;

	void _ready() override;
	void _process(double p_delta) override;

protected:
	static void _bind_methods();

private:
	struct State;
	bool open_scoreboard();
	bool open_popup();
	void capture(const char *p_file);
	void finish();
	std::unique_ptr<State> state_;
};

}
