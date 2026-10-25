#pragma once

#include <godot_cpp/classes/node.hpp>

#include <string>
#include <vector>

namespace bf3::godot_side {

class Bf3SettingsCheck : public godot::Node {
	GDCLASS(Bf3SettingsCheck, godot::Node)

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
	void add(std::string p_name, bool p_pass, std::string p_detail);
	void run();
	void finish();

	std::vector<Row> rows_;
	bool done_ = false;
};

}
