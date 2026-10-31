#pragma once

#include <godot_cpp/classes/node.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bf3::godot_side {

inline constexpr uint32_t kUiOpenCensusMaxWaitFrames = 36000;
inline constexpr int32_t kUiOpenCensusWidth = 1280;
inline constexpr int32_t kUiOpenCensusHeight = 720;

class Bf3UiOpenCensusCheck : public godot::Node {
	GDCLASS(Bf3UiOpenCensusCheck, godot::Node)

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
	void census_screen(const std::string &p_partition);
	void add(std::string p_name, bool p_pass, std::string p_detail);
	void finish();

	std::vector<std::string> partitions_;
	std::vector<Row> rows_;
	size_t next_ = 0;
	uint32_t waited_ = 0;
	size_t synced_ = 0;
	size_t with_items_ = 0;
	uint64_t thrown_viewports_ = 0;
	uint64_t filter_stages_ = 0;
	uint64_t filter_materials_ = 0;
	bool done_ = false;
};

}
