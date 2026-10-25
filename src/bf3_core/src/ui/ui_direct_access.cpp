#include "bf3/core/ui/ui_direct_access.h"

#include <algorithm>
#include <cmath>

namespace bf3::core::ui {

namespace {

constexpr size_t kMaxErrors = 256;

std::optional<UiDirectAccessOp> op_of(const As2Value &p_value) {
	if (p_value.kind != As2Value::Kind::Number || !std::isfinite(p_value.number)) {
		return std::nullopt;
	}
	const double n = p_value.number;
	if (n != std::trunc(n) || n < 0.0 || n > double(kUiDirectAccessMaxOp)) {
		return std::nullopt;
	}
	return UiDirectAccessOp(int32_t(n));
}

std::string handle_of(const As2Value &p_value) {
	return p_value.kind == As2Value::Kind::String ? p_value.text : std::string();
}

bool is_plain(UiDirectAccessOp p_op) {
	return p_op != UiDirectAccessOp::Array && p_op != UiDirectAccessOp::Member &&
			p_op != UiDirectAccessOp::ArrayLengthInvoke;
}

}

const char *ui_direct_access_op_name(UiDirectAccessOp p_op) {
	switch (p_op) {
		case UiDirectAccessOp::Text:
			return "REGISTER_TEXT";
		case UiDirectAccessOp::GotoAndStop:
			return "REGISTER_GOTOANDSTOP";
		case UiDirectAccessOp::GotoAndPlay:
			return "REGISTER_GOTOANDPLAY";
		case UiDirectAccessOp::Visible:
			return "REGISTER_VISIBLE";
		case UiDirectAccessOp::BumpX:
			return "REGISTER_BUMPX";
		case UiDirectAccessOp::BumpY:
			return "REGISTER_BUMPY";
		case UiDirectAccessOp::Scale:
			return "REGISTER_SCALE";
		case UiDirectAccessOp::ScaleX:
			return "REGISTER_SCALEX";
		case UiDirectAccessOp::ScaleY:
			return "REGISTER_SCALEY";
		case UiDirectAccessOp::Color:
			return "REGISTER_COLOR";
		case UiDirectAccessOp::X:
			return "REGISTER_X";
		case UiDirectAccessOp::Y:
			return "REGISTER_Y";
		case UiDirectAccessOp::Invoke:
			return "REGISTER_INVOKE";
		case UiDirectAccessOp::Array:
			return "REGISTER_ARRAY";
		case UiDirectAccessOp::Member:
			return "REGISTER_MEMBER";
		case UiDirectAccessOp::ArrayLengthInvoke:
			return "REGISTER_ARRAYLENGTHINVOKE";
		case UiDirectAccessOp::Rotation:
			return "REGISTER_ROTATION";
	}
	return "REGISTER_?";
}

void UiDirectAccess::error(std::string p_message) {
	if (errors_.size() < kMaxErrors) {
		errors_.push_back(std::move(p_message));
	}
}

std::optional<uint32_t> UiDirectAccess::register_mc(std::string_view p_clip, const std::vector<As2Value> &p_args) {
	if (regs_.size() >= kUiDirectAccessMaxRegistrations) {
		error("registerMC: more than " + std::to_string(kUiDirectAccessMaxRegistrations) + " registrations");
		return std::nullopt;
	}
	const std::optional<UiDirectAccessOp> kind = p_args.empty() ? std::nullopt : op_of(p_args[0]);
	if (!kind) {
		error("registerMC " + std::string(p_clip) + ": no register type");
		return std::nullopt;
	}
	const auto arg = [&](size_t p_i) -> const As2Value & {
		static const As2Value kUndefined;
		return p_i < p_args.size() ? p_args[p_i] : kUndefined;
	};
	Registration r;
	r.clip = std::string(p_clip);
	r.kind = *kind;
	if (is_plain(*kind)) {
		r.op = *kind;
		r.handle = handle_of(arg(1));
		r.name = *kind == UiDirectAccessOp::Invoke ? arg(2).to_string() : std::string();
	} else if (*kind == UiDirectAccessOp::Member) {
		r.handle = handle_of(arg(1));
		r.name = arg(2).to_string();
		r.size = arg(3).truthy();
	} else if (*kind == UiDirectAccessOp::ArrayLengthInvoke) {
		r.handle = handle_of(arg(1));
		r.name = arg(2).to_string();
	} else {
		const std::optional<UiDirectAccessOp> sub_first = op_of(arg(1));
		const std::optional<UiDirectAccessOp> sub_second = op_of(arg(2));
		const size_t handle_at = sub_first && !sub_second ? 2 : 1;
		const std::optional<UiDirectAccessOp> sub = handle_at == 2 ? sub_first : sub_second;
		if (!sub || *sub == UiDirectAccessOp::Array || *sub == UiDirectAccessOp::ArrayLengthInvoke) {
			error("registerMC " + r.clip + ": REGISTER_ARRAY without an item type");
			return std::nullopt;
		}
		r.handle = handle_of(arg(handle_at));
		r.op = *sub;
		const double index = arg(3).to_number();
		r.index = std::isfinite(index) && index >= 0.0 ? int64_t(index) : -1;
		r.field = arg(4).to_string();
		r.name = arg(5).defined() ? arg(5).to_string() : std::string();
	}
	if (r.handle.empty()) {
		error("registerMC " + r.clip + " " + ui_direct_access_op_name(r.kind) + ": no data handle");
		return std::nullopt;
	}
	r.id = next_id_++;
	regs_.push_back(r);
	apply(r);
	return r.id;
}

bool UiDirectAccess::unregister_mc(uint32_t p_id) {
	const auto it = std::find_if(regs_.begin(), regs_.end(), [&](const Registration &r) { return r.id == p_id; });
	if (it == regs_.end()) {
		return false;
	}
	regs_.erase(it);
	return true;
}

size_t UiDirectAccess::unregister_under(std::string_view p_clip) {
	const std::string prefix = std::string(p_clip) + ".";
	const size_t before = regs_.size();
	regs_.erase(std::remove_if(regs_.begin(), regs_.end(),
						[&](const Registration &r) { return r.clip == p_clip || r.clip.starts_with(prefix); }),
			regs_.end());
	return before - regs_.size();
}

void UiDirectAccess::emit(const Registration &p_reg, UiDirectAccessOp p_op, std::string p_name, As2Value p_value) {
	++applied_;
	target_.direct_access(UiDirectAccessApply{ p_reg.clip, p_op, std::move(p_name), std::move(p_value) });
}

void UiDirectAccess::apply(const Registration &p_reg) {
	const auto it = data_.find(p_reg.handle);
	if (it == data_.end() || !it->second.set) {
		return;
	}
	const Data &d = it->second;
	switch (p_reg.kind) {
		case UiDirectAccessOp::Array: {
			if (p_reg.index < 0 || size_t(p_reg.index) >= d.rows.size()) {
				return;
			}
			const As2Value &v = d.rows[size_t(p_reg.index)].field(p_reg.field);
			if (v.defined()) {
				emit(p_reg, p_reg.op, p_reg.name, v);
			}
			return;
		}
		case UiDirectAccessOp::ArrayLengthInvoke:
			emit(p_reg, UiDirectAccessOp::ArrayLengthInvoke, p_reg.name, As2Value::of(double(d.rows.size())));
			return;
		case UiDirectAccessOp::Member:
			emit(p_reg, UiDirectAccessOp::Member, p_reg.name,
					p_reg.size ? As2Value::of(double(d.rows.size())) : d.value);
			return;
		default:
			emit(p_reg, p_reg.op, p_reg.name, d.value);
			return;
	}
}

void UiDirectAccess::set_value(std::string_view p_handle, const As2Value &p_value) {
	Data &d = data_[std::string(p_handle)];
	if (d.set && !d.array && d.value == p_value) {
		return;
	}
	d.value = p_value;
	d.array = false;
	d.set = true;
	const std::vector<Registration> regs = regs_;
	for (const Registration &r : regs) {
		if (r.handle == p_handle && r.kind != UiDirectAccessOp::Array &&
				r.kind != UiDirectAccessOp::ArrayLengthInvoke) {
			apply(r);
		}
	}
}

void UiDirectAccess::set_array(std::string_view p_handle, std::vector<As2Value> p_rows) {
	if (p_rows.size() > kUiDirectAccessMaxRows) {
		error("direct access array " + std::string(p_handle) + ": " + std::to_string(p_rows.size()) + " rows");
		p_rows.resize(kUiDirectAccessMaxRows);
	}
	Data &d = data_[std::string(p_handle)];
	const bool first = !d.set || !d.array;
	const std::vector<As2Value> old = first ? std::vector<As2Value>() : d.rows;
	const bool resized = first || old.size() != p_rows.size();
	d.rows = std::move(p_rows);
	d.array = true;
	d.set = true;
	const std::vector<Registration> regs = regs_;
	for (const Registration &r : regs) {
		if (r.handle != p_handle) {
			continue;
		}
		if (r.kind == UiDirectAccessOp::ArrayLengthInvoke || (r.kind == UiDirectAccessOp::Member && r.size)) {
			if (resized) {
				apply(r);
			}
			continue;
		}
		if (r.kind != UiDirectAccessOp::Array || r.index < 0 || size_t(r.index) >= d.rows.size()) {
			continue;
		}
		const size_t i = size_t(r.index);
		if (!first && i < old.size() && old[i].field(r.field) == d.rows[i].field(r.field)) {
			continue;
		}
		apply(r);
	}
}

}
