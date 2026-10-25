#pragma once

#include <godot_cpp/classes/node.hpp>

#include <memory>

namespace bf3::godot_side {

class Bf3UiEffectsCheck : public godot::Node {
	GDCLASS(Bf3UiEffectsCheck, godot::Node)

public:
	Bf3UiEffectsCheck();
	~Bf3UiEffectsCheck() override;

	void _ready() override;
	void _process(double p_delta) override;

protected:
	static void _bind_methods();

private:
	struct State;
	void row(const std::string &p_name, bool p_ok, const std::string &p_detail);
	void finish();
	std::unique_ptr<State> state_;
};

}
