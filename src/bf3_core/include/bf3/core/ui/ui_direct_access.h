#pragma once

#include "bf3/core/ui/hud_screen.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core::ui {

enum class UiDirectAccessOp : int32_t {
	Text = 0,
	GotoAndStop = 1,
	GotoAndPlay = 2,
	Visible = 3,
	BumpX = 4,
	BumpY = 5,
	Scale = 6,
	ScaleX = 7,
	ScaleY = 8,
	Color = 9,
	X = 10,
	Y = 11,
	Invoke = 12,
	Array = 13,
	Member = 14,
	ArrayLengthInvoke = 15,
	Rotation = 16,
};

inline constexpr int32_t kUiDirectAccessMaxOp = 16;
inline constexpr size_t kUiDirectAccessMaxRegistrations = 1u << 16;
inline constexpr size_t kUiDirectAccessMaxRows = 4096;

const char *ui_direct_access_op_name(UiDirectAccessOp p_op);

struct UiDirectAccessApply {
	std::string clip;
	UiDirectAccessOp op = UiDirectAccessOp::Text;
	std::string name;
	As2Value value;
};

class UiDirectAccessTarget {
public:
	virtual ~UiDirectAccessTarget() = default;
	virtual void direct_access(const UiDirectAccessApply &p_apply) = 0;
};

class UiDirectAccess {
public:
	explicit UiDirectAccess(UiDirectAccessTarget &r_target) : target_(r_target) {}

	std::optional<uint32_t> register_mc(std::string_view p_clip, const std::vector<As2Value> &p_args);
	bool unregister_mc(uint32_t p_id);
	size_t unregister_under(std::string_view p_clip);

	void set_value(std::string_view p_handle, const As2Value &p_value);
	void set_array(std::string_view p_handle, std::vector<As2Value> p_rows);

	size_t registrations() const { return regs_.size(); }
	uint64_t applied() const { return applied_; }
	const std::vector<std::string> &errors() const { return errors_; }

private:
	struct Registration {
		uint32_t id = 0;
		std::string clip;
		UiDirectAccessOp kind = UiDirectAccessOp::Text;
		std::string handle;
		UiDirectAccessOp op = UiDirectAccessOp::Text;
		int64_t index = -1;
		std::string field;
		std::string name;
		bool size = false;
	};
	struct Data {
		As2Value value;
		std::vector<As2Value> rows;
		bool array = false;
		bool set = false;
	};

	void apply(const Registration &p_reg);
	void emit(const Registration &p_reg, UiDirectAccessOp p_op, std::string p_name, As2Value p_value);
	void error(std::string p_message);

	UiDirectAccessTarget &target_;
	std::vector<Registration> regs_;
	std::map<std::string, Data, std::less<>> data_;
	uint32_t next_id_ = 1;
	uint64_t applied_ = 0;
	std::vector<std::string> errors_;
};

}
