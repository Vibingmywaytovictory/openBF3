#pragma once

#include <godot_cpp/classes/node.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::godot_side {

class Bf3SwfView;
class Bf3UiPages;

inline constexpr uint32_t kUiOpenCensusMaxWaitFrames = 36000;
inline constexpr int32_t kUiOpenCensusWidth = 1280;
inline constexpr int32_t kUiOpenCensusHeight = 720;
inline constexpr uint64_t kUiOpenCensusPrimitiveTriangles = 32;
inline constexpr uint32_t kUiOpenCensusCloseSteps = 600;
inline constexpr uint32_t kUiOpenCensusMaxPasses = 512;
inline constexpr int32_t kUiOpenCensusSmallPage = 32;
inline constexpr double kUiOpenCensusWholeMove = 12.0;
inline constexpr double kUiOpenCensusFractionalMove = 0.5;
inline constexpr double kUiOpenCensusCloseStepSeconds = 1.0 / 30.0;

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
	void census_hidden_screen(const std::string &p_partition, std::string_view p_live_target);
	void census_filter_swap(const std::string &p_partition);
	void census_pages();
	void census_paged_screens();
	void census_page_life(Bf3UiPages &r_pages);
	void census_hidden_pages(Bf3UiPages &r_pages);
	void census_page_overflow(Bf3UiPages &r_pages);
	Bf3SwfView *open_paged_view(const std::string &p_leaf);
	void settle_paged_view(Bf3UiPages &r_pages, Bf3SwfView &r_view);
	void finish();

	std::vector<std::string> partitions_;
	std::vector<Row> rows_;
	std::string walk_differs_;
	size_t next_ = 0;
	uint32_t waited_ = 0;
	size_t synced_ = 0;
	size_t with_items_ = 0;
	uint64_t thrown_viewports_ = 0;
	uint64_t filter_stages_ = 0;
	uint64_t filter_materials_ = 0;
	uint64_t filter_kernels_ = 0;
	bool done_ = false;
};

}
