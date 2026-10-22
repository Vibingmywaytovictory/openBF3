#pragma once

#include <godot_cpp/classes/node.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace bf3::godot_side {

class Bf3SwfViewCheck : public godot::Node {
	GDCLASS(Bf3SwfViewCheck, godot::Node)

public:
	Bf3SwfViewCheck();
	~Bf3SwfViewCheck() override;

	void _ready() override;
	void _process(double p_delta) override;

protected:
	static void _bind_methods();

private:
	struct State;
	void setup();
	void finish();
	std::unique_ptr<State> state_;
};

}
