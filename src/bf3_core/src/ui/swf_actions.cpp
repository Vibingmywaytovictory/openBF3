#include "bf3/core/ui/swf_movie.h"

#include "bf3/core/util/format.h"
#include "bf3/core/util/text.h"
#include "swf_bits.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace bf3::core::ui {

namespace {

using detail::SwfReader;

namespace avm1 {
constexpr uint8_t kEnd = 0x00;
constexpr uint8_t kNextFrame = 0x04;
constexpr uint8_t kPrevFrame = 0x05;
constexpr uint8_t kPlay = 0x06;
constexpr uint8_t kStop = 0x07;
constexpr uint8_t kSetTarget2 = 0x20;
constexpr uint8_t kGotoFrame = 0x81;
constexpr uint8_t kSetTarget = 0x8B;
constexpr uint8_t kGoToLabel = 0x8C;
constexpr uint8_t kDefineFunction2 = 0x8E;
constexpr uint8_t kConstantPool = 0x88;
constexpr uint8_t kWith = 0x94;
constexpr uint8_t kPush = 0x96;
constexpr uint8_t kJump = 0x99;
constexpr uint8_t kDefineFunction = 0x9B;
constexpr uint8_t kIf = 0x9D;
constexpr uint8_t kGotoFrame2 = 0x9F;
}

using PushValue = std::variant<std::monostate, std::string, double>;

class ActionScanner {
public:
	ActionScanner(std::span<const uint8_t> p_code, uint32_t p_frame) :
			code_(p_code) {
		script_.frame = p_frame;
		script_.bytes = uint32_t(p_code.size());
	}

	Result<SwfFrameScript> run() {
		size_t pos = 0;
		uint32_t actions = 0;
		while (pos < code_.size()) {
			if (++actions > kSwfMaxActionsPerScript) {
				return fail(pos, "more than " + std::to_string(kSwfMaxActionsPerScript) + " actions");
			}
			const uint8_t op = code_[pos];
			if (op == avm1::kEnd) {
				return std::move(script_);
			}
			size_t len = 0;
			size_t header = 1;
			if (op >= 0x80) {
				if (code_.size() - pos < 3) {
					return fail(pos, fmt("action 0x%02x has no length", op));
				}
				len = size_t(code_[pos + 1] | (code_[pos + 2] << 8));
				header = 3;
				if (len > code_.size() - pos - header) {
					return fail(pos, fmt("action 0x%02x of %zu bytes runs past the script", op, len));
				}
			}
			const std::span<const uint8_t> payload = code_.subspan(pos + header, len);
			size_t skip_after = 0;
			BF3_TRY(action(op, payload, pos, skip_after));
			pos += header + len;
			if (skip_after > code_.size() - pos) {
				return fail(pos, fmt("a block of %zu bytes runs past the script", skip_after));
			}
			pos += skip_after;
		}
		return std::move(script_);
	}

private:
	Error fail(size_t p_at, const std::string &p_what) const {
		return make_error("swf: frame " + std::to_string(script_.frame) + " actions at " + std::to_string(p_at) + ": " + p_what);
	}

	void emit(SwfFrameOp p_op) {
		if (targeted_) {
			script_.other_actions++;
			return;
		}
		p_op.after_branch = script_.branches != 0;
		script_.ops.push_back(std::move(p_op));
	}

	std::optional<std::string> pool_string(size_t p_index) const {
		if (p_index >= pool_.size()) {
			return std::nullopt;
		}
		return pool_[p_index];
	}

	Status push(std::span<const uint8_t> p_payload, size_t p_at) {
		SwfReader r(p_payload, p_at);
		last_push_ = PushValue{};
		while (!r.failed() && !r.at_end()) {
			const uint8_t type = r.u8();
			switch (type) {
				case 0: last_push_ = std::string(r.cstr()); break;
				case 1: last_push_ = double(r.f32()); break;
				case 2:
				case 3: last_push_ = PushValue{}; break;
				case 4:
				case 5:
					r.u8();
					last_push_ = PushValue{};
					break;
				case 6: {
					const uint32_t hi = r.u32();
					const uint32_t lo = r.u32();
					const uint64_t bits = (uint64_t(hi) << 32) | lo;
					double d = 0.0;
					std::memcpy(&d, &bits, sizeof(d));
					last_push_ = d;
					break;
				}
				case 7: last_push_ = double(int32_t(r.u32())); break;
				case 8:
				case 9: {
					const size_t index = type == 8 ? r.u8() : r.u16();
					const std::optional<std::string> s = pool_string(index);
					if (!s && !r.failed()) {
						return fail(p_at, "push of constant " + std::to_string(index) + " past a pool of " + std::to_string(pool_.size()));
					}
					last_push_ = s ? PushValue(*s) : PushValue{};
					break;
				}
				default: return fail(p_at, fmt("push value type %u is not an AVM1 type", unsigned(type)));
			}
		}
		if (r.failed()) {
			return fail(p_at, "truncated Push");
		}
		return Status::success();
	}

	Status goto_frame2(std::span<const uint8_t> p_payload, size_t p_at, bool p_after_push) {
		SwfReader r(p_payload, p_at);
		const uint8_t flags = r.u8();
		const uint16_t bias = (flags & 0x02) != 0 ? r.u16() : 0;
		if (r.failed()) {
			return fail(p_at, "truncated GotoFrame2");
		}
		SwfFrameOp op;
		op.and_play = (flags & 0x01) != 0;
		if (!p_after_push || std::holds_alternative<std::monostate>(last_push_)) {
			script_.unresolved_goto = true;
			script_.other_actions++;
			return Status::success();
		}
		if (const double *d = std::get_if<double>(&last_push_)) {
			if (!std::isfinite(*d) || *d < 1.0) {
				script_.unresolved_goto = true;
				script_.other_actions++;
				return Status::success();
			}
			op.kind = SwfFrameOpKind::GotoFrame;
			op.frame = uint32_t(*d) - 1 + bias;
			emit(std::move(op));
			return Status::success();
		}
		const std::string &s = std::get<std::string>(last_push_);
		if (s.find_first_of(":/") != std::string::npos) {
			script_.unresolved_goto = true;
			script_.other_actions++;
			return Status::success();
		}
		uint32_t number = 0;
		const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), number);
		if (ec == std::errc() && end == s.data() + s.size() && number >= 1) {
			op.kind = SwfFrameOpKind::GotoFrame;
			op.frame = number - 1 + bias;
		} else {
			op.kind = SwfFrameOpKind::GotoLabel;
			op.label = s;
		}
		emit(std::move(op));
		return Status::success();
	}

	Status function_body(uint8_t p_op, std::span<const uint8_t> p_payload, size_t p_at, size_t &r_skip) {
		SwfReader r(p_payload, p_at);
		r.cstr();
		const uint16_t params = r.u16();
		if (p_op == avm1::kDefineFunction2) {
			r.u8();
			r.u16();
			for (uint32_t i = 0; i < params && !r.failed(); ++i) {
				r.u8();
				r.cstr();
			}
		} else {
			for (uint32_t i = 0; i < params && !r.failed(); ++i) {
				r.cstr();
			}
		}
		r_skip = r.u16();
		if (r.failed() || !r.at_end()) {
			return fail(p_at, fmt("malformed function header (action 0x%02x)", p_op));
		}
		script_.functions++;
		return Status::success();
	}

	Status action(uint8_t p_op, std::span<const uint8_t> p_payload, size_t p_at, size_t &r_skip) {
		const bool after_push = previous_ == avm1::kPush;
		previous_ = p_op;
		switch (p_op) {
			case avm1::kStop: emit({ SwfFrameOpKind::Stop }); return Status::success();
			case avm1::kPlay: emit({ SwfFrameOpKind::Play }); return Status::success();
			case avm1::kNextFrame: emit({ SwfFrameOpKind::NextFrame }); return Status::success();
			case avm1::kPrevFrame: emit({ SwfFrameOpKind::PrevFrame }); return Status::success();
			case avm1::kGotoFrame: {
				if (p_payload.size() != 2) {
					return fail(p_at, "GotoFrame of " + std::to_string(p_payload.size()) + " bytes");
				}
				SwfFrameOp op;
				op.kind = SwfFrameOpKind::GotoFrame;
				op.frame = uint32_t(p_payload[0] | (p_payload[1] << 8));
				emit(std::move(op));
				return Status::success();
			}
			case avm1::kGoToLabel: {
				SwfReader r(p_payload, p_at);
				SwfFrameOp op;
				op.kind = SwfFrameOpKind::GotoLabel;
				op.label = std::string(r.cstr());
				if (r.failed()) {
					return fail(p_at, "truncated GoToLabel");
				}
				emit(std::move(op));
				return Status::success();
			}
			case avm1::kGotoFrame2: return goto_frame2(p_payload, p_at, after_push);
			case avm1::kPush: return push(p_payload, p_at);
			case avm1::kConstantPool: {
				SwfReader r(p_payload, p_at);
				const uint16_t count = r.u16();
				pool_.clear();
				for (uint32_t i = 0; i < count && !r.failed(); ++i) {
					pool_.emplace_back(r.cstr());
				}
				if (r.failed()) {
					return fail(p_at, "truncated ConstantPool");
				}
				script_.other_actions++;
				return Status::success();
			}
			case avm1::kSetTarget: {
				SwfReader r(p_payload, p_at);
				const std::string_view target = r.cstr();
				if (r.failed()) {
					return fail(p_at, "truncated SetTarget");
				}
				targeted_ = !target.empty();
				script_.set_target = true;
				return Status::success();
			}
			case avm1::kSetTarget2:
				targeted_ = true;
				script_.set_target = true;
				return Status::success();
			case avm1::kJump:
			case avm1::kIf:
				if (p_payload.size() != 2) {
					return fail(p_at, fmt("branch 0x%02x of %zu bytes", p_op, p_payload.size()));
				}
				script_.branches++;
				return Status::success();
			case avm1::kDefineFunction:
			case avm1::kDefineFunction2: return function_body(p_op, p_payload, p_at, r_skip);
			case avm1::kWith: {
				if (p_payload.size() != 2) {
					return fail(p_at, "With of " + std::to_string(p_payload.size()) + " bytes");
				}
				r_skip = size_t(p_payload[0] | (p_payload[1] << 8));
				script_.other_actions++;
				return Status::success();
			}
			default: script_.other_actions++; return Status::success();
		}
	}

	std::span<const uint8_t> code_;
	SwfFrameScript script_;
	std::vector<std::string> pool_;
	PushValue last_push_;
	uint8_t previous_ = avm1::kEnd;
	bool targeted_ = false;
};

}

const char *swf_frame_op_name(SwfFrameOpKind p_kind) {
	switch (p_kind) {
		case SwfFrameOpKind::Stop: return "stop";
		case SwfFrameOpKind::Play: return "play";
		case SwfFrameOpKind::GotoFrame: return "goto_frame";
		case SwfFrameOpKind::GotoLabel: return "goto_label";
		case SwfFrameOpKind::NextFrame: return "next_frame";
		case SwfFrameOpKind::PrevFrame: return "prev_frame";
	}
	return "?";
}

Result<SwfFrameScript> scan_frame_actions(std::span<const uint8_t> p_actions, uint32_t p_frame) {
	return ActionScanner(p_actions, p_frame).run();
}

std::optional<uint32_t> SwfTimeline::label_frame(std::string_view p_label) const {
	for (const SwfFrameLabel &l : labels) {
		if (l.name == p_label) {
			return l.frame;
		}
	}
	for (const SwfFrameLabel &l : labels) {
		if (l.name.size() == p_label.size() &&
				std::equal(l.name.begin(), l.name.end(), p_label.begin(), [](char a, char b) { return to_lower_ascii(a) == to_lower_ascii(b); })) {
			return l.frame;
		}
	}
	return std::nullopt;
}

const SwfFrameScript *SwfTimeline::script_at(uint32_t p_frame) const {
	auto it = std::lower_bound(scripts.begin(), scripts.end(), p_frame, [](const SwfFrameScript &s, uint32_t f) { return s.frame < f; });
	return (it != scripts.end() && it->frame == p_frame) ? &*it : nullptr;
}

}
