#include "bf3/core/ui/avm1.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace bf3::core::ui {

namespace {

struct OpName {
	uint8_t op;
	const char *name;
};

constexpr OpName kOpNames[] = {
	{ avm1_op::kEnd, "end" },
	{ avm1_op::kNextFrame, "nextFrame" },
	{ avm1_op::kPreviousFrame, "previousFrame" },
	{ avm1_op::kPlay, "play" },
	{ avm1_op::kStop, "stop" },
	{ avm1_op::kToggleQuality, "toggleQuality" },
	{ avm1_op::kStopSounds, "stopSounds" },
	{ avm1_op::kAdd, "add" },
	{ avm1_op::kSubtract, "subtract" },
	{ avm1_op::kMultiply, "multiply" },
	{ avm1_op::kDivide, "divide" },
	{ avm1_op::kEquals, "equals" },
	{ avm1_op::kLess, "less" },
	{ avm1_op::kAnd, "and" },
	{ avm1_op::kOr, "or" },
	{ avm1_op::kNot, "not" },
	{ avm1_op::kStringEquals, "stringEquals" },
	{ avm1_op::kStringLength, "stringLength" },
	{ avm1_op::kStringExtract, "stringExtract" },
	{ avm1_op::kPop, "pop" },
	{ avm1_op::kToInteger, "toInteger" },
	{ avm1_op::kGetVariable, "getVariable" },
	{ avm1_op::kSetVariable, "setVariable" },
	{ avm1_op::kSetTarget2, "setTarget2" },
	{ avm1_op::kStringAdd, "stringAdd" },
	{ avm1_op::kGetProperty, "getProperty" },
	{ avm1_op::kSetProperty, "setProperty" },
	{ avm1_op::kCloneSprite, "cloneSprite" },
	{ avm1_op::kRemoveSprite, "removeSprite" },
	{ avm1_op::kTrace, "trace" },
	{ avm1_op::kStartDrag, "startDrag" },
	{ avm1_op::kEndDrag, "endDrag" },
	{ avm1_op::kStringLess, "stringLess" },
	{ avm1_op::kThrow, "throw" },
	{ avm1_op::kCastOp, "castOp" },
	{ avm1_op::kImplementsOp, "implementsOp" },
	{ avm1_op::kRandomNumber, "randomNumber" },
	{ avm1_op::kMBStringLength, "mbStringLength" },
	{ avm1_op::kCharToAscii, "charToAscii" },
	{ avm1_op::kAsciiToChar, "asciiToChar" },
	{ avm1_op::kGetTime, "getTime" },
	{ avm1_op::kMBStringExtract, "mbStringExtract" },
	{ avm1_op::kMBCharToAscii, "mbCharToAscii" },
	{ avm1_op::kMBAsciiToChar, "mbAsciiToChar" },
	{ avm1_op::kDelete, "delete" },
	{ avm1_op::kDelete2, "delete2" },
	{ avm1_op::kDefineLocal, "defineLocal" },
	{ avm1_op::kCallFunction, "callFunction" },
	{ avm1_op::kReturn, "return" },
	{ avm1_op::kModulo, "modulo" },
	{ avm1_op::kNewObject, "newObject" },
	{ avm1_op::kDefineLocal2, "defineLocal2" },
	{ avm1_op::kInitArray, "initArray" },
	{ avm1_op::kInitObject, "initObject" },
	{ avm1_op::kTypeOf, "typeOf" },
	{ avm1_op::kTargetPath, "targetPath" },
	{ avm1_op::kEnumerate, "enumerate" },
	{ avm1_op::kAdd2, "add2" },
	{ avm1_op::kLess2, "less2" },
	{ avm1_op::kEquals2, "equals2" },
	{ avm1_op::kToNumber, "toNumber" },
	{ avm1_op::kToString, "toString" },
	{ avm1_op::kPushDuplicate, "pushDuplicate" },
	{ avm1_op::kStackSwap, "stackSwap" },
	{ avm1_op::kGetMember, "getMember" },
	{ avm1_op::kSetMember, "setMember" },
	{ avm1_op::kIncrement, "increment" },
	{ avm1_op::kDecrement, "decrement" },
	{ avm1_op::kCallMethod, "callMethod" },
	{ avm1_op::kNewMethod, "newMethod" },
	{ avm1_op::kInstanceOf, "instanceOf" },
	{ avm1_op::kEnumerate2, "enumerate2" },
	{ avm1_op::kBitAnd, "bitAnd" },
	{ avm1_op::kBitOr, "bitOr" },
	{ avm1_op::kBitXor, "bitXor" },
	{ avm1_op::kBitLShift, "bitLShift" },
	{ avm1_op::kBitRShift, "bitRShift" },
	{ avm1_op::kBitURShift, "bitURShift" },
	{ avm1_op::kStrictEquals, "strictEquals" },
	{ avm1_op::kGreater, "greater" },
	{ avm1_op::kStringGreater, "stringGreater" },
	{ avm1_op::kExtends, "extends" },
	{ avm1_op::kGotoFrame, "gotoFrame" },
	{ avm1_op::kGetUrl, "getUrl" },
	{ avm1_op::kStoreRegister, "storeRegister" },
	{ avm1_op::kConstantPool, "constantPool" },
	{ avm1_op::kWaitForFrame, "waitForFrame" },
	{ avm1_op::kSetTarget, "setTarget" },
	{ avm1_op::kGoToLabel, "goToLabel" },
	{ avm1_op::kWaitForFrame2, "waitForFrame2" },
	{ avm1_op::kDefineFunction2, "defineFunction2" },
	{ avm1_op::kTry, "try" },
	{ avm1_op::kWith, "with" },
	{ avm1_op::kPush, "push" },
	{ avm1_op::kJump, "jump" },
	{ avm1_op::kGetUrl2, "getUrl2" },
	{ avm1_op::kDefineFunction, "defineFunction" },
	{ avm1_op::kIf, "if" },
	{ avm1_op::kCall, "call" },
	{ avm1_op::kGotoFrame2, "gotoFrame2" },
};

struct OpTable {
	std::array<const char *, 256> names{};
	constexpr OpTable() {
		for (const OpName &n : kOpNames) {
			names[n.op] = n.name;
		}
	}
};

constexpr OpTable kOpTable;

class Payload {
public:
	Payload(std::span<const uint8_t> p_data) :
			data_(p_data) {}

	bool failed() const { return failed_; }
	size_t left() const { return data_.size() - pos_; }
	bool at_end() const { return pos_ == data_.size(); }

	uint8_t u8() {
		if (!need(1)) {
			return 0;
		}
		return data_[pos_++];
	}
	uint16_t u16() {
		if (!need(2)) {
			return 0;
		}
		const uint16_t v = uint16_t(data_[pos_] | (data_[pos_ + 1] << 8));
		pos_ += 2;
		return v;
	}
	uint32_t u32() {
		if (!need(4)) {
			return 0;
		}
		uint32_t v = 0;
		std::memcpy(&v, data_.data() + pos_, 4);
		pos_ += 4;
		return v;
	}
	float f32() {
		const uint32_t bits = u32();
		float f = 0.0f;
		std::memcpy(&f, &bits, 4);
		return f;
	}
	double swapped_f64() {
		const uint32_t high = u32();
		const uint32_t low = u32();
		const uint64_t bits = (uint64_t(high) << 32) | low;
		double d = 0.0;
		std::memcpy(&d, &bits, 8);
		return d;
	}
	std::string str() {
		if (failed_) {
			return {};
		}
		const uint8_t *begin = data_.data() + pos_;
		const void *nul = std::memchr(begin, 0, data_.size() - pos_);
		if (nul == nullptr) {
			failed_ = true;
			pos_ = data_.size();
			return {};
		}
		const size_t len = size_t(static_cast<const uint8_t *>(nul) - begin);
		pos_ += len + 1;
		return std::string(reinterpret_cast<const char *>(begin), len);
	}

private:
	bool need(size_t p_count) {
		if (failed_ || data_.size() - pos_ < p_count) {
			failed_ = true;
			pos_ = data_.size();
			return false;
		}
		return true;
	}

	std::span<const uint8_t> data_;
	size_t pos_ = 0;
	bool failed_ = false;
};

struct OpenRegion {
	uint32_t end = 0;
	uint32_t scope = 0;
	bool function = false;
};

class Decoder {
public:
	Decoder(std::span<const uint8_t> p_bytes, uint32_t p_base) :
			bytes_(p_bytes) {
		code_.base = p_base;
		code_.bytes = uint32_t(p_bytes.size());
		Avm1Scope top;
		top.begin = 0;
		top.end = code_.bytes;
		code_.scopes.push_back(top);
	}

	Result<Avm1Code> run() {
		if (bytes_.size() > 0xFFFFFFFFull - code_.base) {
			return fail("body larger than 4 GiB", 0);
		}
		uint32_t p = 0;
		const uint32_t n = code_.bytes;
		while (p < n) {
			BF3_TRY(close_regions(p));
			Avm1Action a;
			a.offset = p;
			a.op = bytes_[p];
			a.scope = current_scope();
			if (kOpTable.names[a.op] == nullptr) {
				return fail("unknown action 0x" + hex2(a.op), p);
			}
			if (a.op >= 0x80) {
				if (n - p < 3) {
					return fail(std::string(kOpTable.names[a.op]) + " record header past the end of the body", p);
				}
				a.length = uint16_t(bytes_[p + 1] | (bytes_[p + 2] << 8));
				if (a.length > n - p - 3) {
					return fail(std::string(kOpTable.names[a.op]) + " length " + std::to_string(a.length) + " past the end of the body (" + std::to_string(n - p - 3) + " bytes left)", p);
				}
				BF3_TRY(payload(a, bytes_.subspan(p + 3, a.length)));
			}
			p = a.end();
			BF3_TRY(open_regions(a));
			code_.actions.push_back(std::move(a));
		}
		BF3_TRY(close_regions(p));
		BF3_TRY(check_branches());
		check_registers();
		return std::move(code_);
	}

private:
	static std::string hex2(uint8_t p_v) {
		char buf[4];
		std::snprintf(buf, sizeof(buf), "%02X", unsigned(p_v));
		return buf;
	}

	Error fail(const std::string &p_what, uint32_t p_at) const {
		return make_error("avm1: " + p_what + " at " + std::to_string(uint64_t(code_.base) + p_at));
	}

	uint32_t current_scope() const {
		for (auto it = regions_.rbegin(); it != regions_.rend(); ++it) {
			if (it->function) {
				return it->scope;
			}
		}
		return 0;
	}

	uint32_t enclosing_end() const { return regions_.empty() ? code_.bytes : regions_.back().end; }

	Status close_regions(uint32_t p_at) {
		while (!regions_.empty() && regions_.back().end <= p_at) {
			if (regions_.back().end < p_at) {
				return fail("an action record crosses the end of a block at " + std::to_string(uint64_t(code_.base) + regions_.back().end), p_at);
			}
			regions_.pop_back();
		}
		return Status::success();
	}

	Status open_region(const Avm1Action &p_a, uint32_t p_size, bool p_function) {
		const uint64_t end = uint64_t(p_a.end()) + p_size;
		if (end > enclosing_end()) {
			return fail(std::string(kOpTable.names[p_a.op]) + " block of " + std::to_string(p_size) + " bytes runs past its enclosing block (ends at " + std::to_string(uint64_t(code_.base) + enclosing_end()) + ")", p_a.offset);
		}
		OpenRegion r;
		r.end = uint32_t(end);
		r.function = p_function;
		if (p_function) {
			Avm1Scope s;
			s.begin = p_a.end();
			s.end = r.end;
			s.action = uint32_t(code_.actions.size());
			s.parent = p_a.scope;
			s.depth = code_.scopes[p_a.scope].depth + 1;
			r.scope = uint32_t(code_.scopes.size());
			code_.scopes.push_back(s);
		}
		regions_.push_back(r);
		return Status::success();
	}

	Status open_regions(const Avm1Action &p_a) {
		switch (p_a.op) {
			case avm1_op::kDefineFunction:
			case avm1_op::kDefineFunction2:
				return open_region(p_a, p_a.code_size, true);
			case avm1_op::kWith:
				return open_region(p_a, p_a.block_size, false);
			case avm1_op::kTry: {
				const uint32_t total = uint32_t(p_a.try_size) + p_a.catch_size + p_a.finally_size;
				BF3_TRY(open_region(p_a, total, false));
				try_bounds_.push_back(p_a.end() + p_a.try_size);
				try_bounds_.push_back(p_a.end() + p_a.try_size + p_a.catch_size);
				return Status::success();
			}
			default:
				return Status::success();
		}
	}

	Status payload(Avm1Action &r_a, std::span<const uint8_t> p_data) {
		Payload r(p_data);
		const char *name = kOpTable.names[r_a.op];
		switch (r_a.op) {
			case avm1_op::kGotoFrame:
				r_a.frame = r.u16();
				break;
			case avm1_op::kGetUrl:
				r_a.strings.push_back(r.str());
				r_a.strings.push_back(r.str());
				break;
			case avm1_op::kWaitForFrame:
				r_a.frame = r.u16();
				r_a.skip_count = r.u8();
				break;
			case avm1_op::kSetTarget:
			case avm1_op::kGoToLabel:
				r_a.strings.push_back(r.str());
				break;
			case avm1_op::kWaitForFrame2:
				r_a.skip_count = r.u8();
				break;
			case avm1_op::kGetUrl2:
				r_a.mode = r.u8();
				break;
			case avm1_op::kGotoFrame2:
				r_a.mode = r.u8();
				if ((r_a.mode & 0x02) != 0) {
					r_a.scene_bias = r.u16();
				}
				break;
			case avm1_op::kStoreRegister:
				r_a.reg = r.u8();
				break;
			case avm1_op::kJump:
			case avm1_op::kIf:
				r_a.branch_offset = int16_t(r.u16());
				break;
			case avm1_op::kWith:
				r_a.block_size = r.u16();
				break;
			case avm1_op::kConstantPool: {
				const uint16_t count = r.u16();
				r_a.strings.reserve(std::min<size_t>(count, r.left()));
				for (uint32_t i = 0; i < count && !r.failed(); ++i) {
					r_a.strings.push_back(r.str());
				}
				if (!r.failed()) {
					constants_ = r_a.strings;
				}
				break;
			}
			case avm1_op::kPush:
				while (!r.at_end() && !r.failed()) {
					Avm1Value v;
					const uint8_t type = r.u8();
					if (type > uint8_t(Avm1PushType::Constant16)) {
						return fail("push value type " + std::to_string(type), r_a.offset);
					}
					v.type = Avm1PushType(type);
					switch (v.type) {
						case Avm1PushType::String:
							v.text = r.str();
							break;
						case Avm1PushType::Float:
							v.number = double(r.f32());
							break;
						case Avm1PushType::Null:
						case Avm1PushType::Undefined:
							break;
						case Avm1PushType::Register:
						case Avm1PushType::Boolean:
						case Avm1PushType::Constant8:
							v.index = r.u8();
							break;
						case Avm1PushType::Double:
							v.number = r.swapped_f64();
							break;
						case Avm1PushType::Integer:
							v.index = r.u32();
							v.number = double(int32_t(v.index));
							break;
						case Avm1PushType::Constant16:
							v.index = r.u16();
							break;
					}
					if (v.type == Avm1PushType::Constant8 || v.type == Avm1PushType::Constant16) {
						if (v.index < constants_.size()) {
							v.text = constants_[v.index];
							v.resolved = true;
						} else {
							code_.unresolved_constants++;
						}
					}
					r_a.values.push_back(std::move(v));
				}
				break;
			case avm1_op::kDefineFunction: {
				r_a.strings.push_back(r.str());
				const uint16_t count = r.u16();
				for (uint32_t i = 0; i < count && !r.failed(); ++i) {
					Avm1Param p;
					p.name = r.str();
					r_a.params.push_back(std::move(p));
				}
				r_a.code_size = r.u16();
				break;
			}
			case avm1_op::kDefineFunction2: {
				r_a.strings.push_back(r.str());
				const uint16_t count = r.u16();
				r_a.register_count = r.u8();
				r_a.function_flags = r.u16();
				for (uint32_t i = 0; i < count && !r.failed(); ++i) {
					Avm1Param p;
					p.reg = r.u8();
					p.name = r.str();
					r_a.params.push_back(std::move(p));
				}
				r_a.code_size = r.u16();
				if ((r_a.function_flags & avm1_function_flag::kReserved) != 0 && !r.failed()) {
					return fail("defineFunction2 reserved flag bits 0x" + hex2(uint8_t(r_a.function_flags >> 8)), r_a.offset);
				}
				break;
			}
			case avm1_op::kTry:
				r_a.try_flags = r.u8();
				r_a.try_size = r.u16();
				r_a.catch_size = r.u16();
				r_a.finally_size = r.u16();
				if ((r_a.try_flags & avm1_try_flag::kCatchInRegister) != 0) {
					r_a.reg = r.u8();
				} else {
					r_a.strings.push_back(r.str());
				}
				if ((r_a.try_flags & 0xF8) != 0 && !r.failed()) {
					return fail("try reserved flag bits 0x" + hex2(r_a.try_flags), r_a.offset);
				}
				break;
			default:
				if (!p_data.empty()) {
					return fail(std::string(name) + " carries " + std::to_string(p_data.size()) + " payload bytes, the format has none", r_a.offset);
				}
				return Status::success();
		}
		if (r.failed()) {
			return fail(std::string(name) + " payload of " + std::to_string(p_data.size()) + " bytes is shorter than its fields", r_a.offset);
		}
		if (!r.at_end()) {
			return fail(std::string(name) + " payload of " + std::to_string(p_data.size()) + " bytes has " + std::to_string(r.left()) + " bytes after its fields", r_a.offset);
		}
		return Status::success();
	}

	Status check_branches() {
		for (uint32_t b : try_bounds_) {
			if (b != code_.bytes && code_.find(b) == kAvm1NoTarget) {
				return fail("try block boundary is not on an action record", b);
			}
		}
		for (Avm1Action &a : code_.actions) {
			if (a.op != avm1_op::kJump && a.op != avm1_op::kIf) {
				continue;
			}
			const int64_t target = int64_t(a.end()) + a.branch_offset;
			const Avm1Scope &s = code_.scopes[a.scope];
			if (target < int64_t(s.begin) || target > int64_t(s.end)) {
				return fail(std::string(kOpTable.names[a.op]) + " offset " + std::to_string(a.branch_offset) + " targets " + std::to_string(int64_t(code_.base) + target) + ", outside its block [" + std::to_string(uint64_t(code_.base) + s.begin) + ", " + std::to_string(uint64_t(code_.base) + s.end) + "]", a.offset);
			}
			const uint32_t t = uint32_t(target);
			if (t != s.end) {
				const uint32_t at = code_.find(t);
				if (at == kAvm1NoTarget || code_.actions[at].scope != a.scope) {
					return fail(std::string(kOpTable.names[a.op]) + " offset " + std::to_string(a.branch_offset) + " targets " + std::to_string(uint64_t(code_.base) + t) + ", which is not an action record of its block", a.offset);
				}
			}
			a.target = t;
		}
		return Status::success();
	}

	void check_registers() {
		for (const Avm1Action &a : code_.actions) {
			const Avm1Action *f = code_.function_action(a.scope);
			const uint32_t limit = (f != nullptr && f->op == avm1_op::kDefineFunction2) ? uint32_t(f->register_count) : 4u;
			if (a.op == avm1_op::kStoreRegister && a.reg >= limit) {
				code_.registers_out_of_range++;
			}
			for (const Avm1Value &v : a.values) {
				if (v.type == Avm1PushType::Register && v.index >= limit) {
					code_.registers_out_of_range++;
				}
			}
			if (a.op == avm1_op::kDefineFunction2) {
				for (const Avm1Param &p : a.params) {
					if (p.reg != 0 && p.reg >= a.register_count) {
						code_.registers_out_of_range++;
					}
				}
			}
		}
	}

	std::span<const uint8_t> bytes_;
	Avm1Code code_;
	std::vector<OpenRegion> regions_;
	std::vector<uint32_t> try_bounds_;
	std::vector<std::string> constants_;
};

}

const char *avm1_op_name(uint8_t p_op) {
	return kOpTable.names[p_op];
}

std::string avm1_clip_event_names(uint32_t p_flags) {
	static constexpr std::pair<uint32_t, const char *> kNames[] = {
		{ avm1_clip_event::kLoad, "load" },
		{ avm1_clip_event::kEnterFrame, "enterFrame" },
		{ avm1_clip_event::kUnload, "unload" },
		{ avm1_clip_event::kMouseMove, "mouseMove" },
		{ avm1_clip_event::kMouseDown, "mouseDown" },
		{ avm1_clip_event::kMouseUp, "mouseUp" },
		{ avm1_clip_event::kKeyDown, "keyDown" },
		{ avm1_clip_event::kKeyUp, "keyUp" },
		{ avm1_clip_event::kData, "data" },
		{ avm1_clip_event::kInitialize, "initialize" },
		{ avm1_clip_event::kPress, "press" },
		{ avm1_clip_event::kRelease, "release" },
		{ avm1_clip_event::kReleaseOutside, "releaseOutside" },
		{ avm1_clip_event::kRollOver, "rollOver" },
		{ avm1_clip_event::kRollOut, "rollOut" },
		{ avm1_clip_event::kDragOver, "dragOver" },
		{ avm1_clip_event::kDragOut, "dragOut" },
		{ avm1_clip_event::kKeyPress, "keyPress" },
		{ avm1_clip_event::kConstruct, "construct" },
	};
	std::string out;
	uint32_t known = 0;
	for (const auto &[bit, name] : kNames) {
		known |= bit;
		if ((p_flags & bit) != 0) {
			out += out.empty() ? "" : "|";
			out += name;
		}
	}
	if ((p_flags & ~known) != 0) {
		char buf[24];
		std::snprintf(buf, sizeof(buf), "0x%08X", p_flags & ~known);
		out += out.empty() ? "" : "|";
		out += buf;
	}
	return out;
}

bool Avm1Value::is_number() const {
	return type == Avm1PushType::Float || type == Avm1PushType::Double || type == Avm1PushType::Integer;
}

uint32_t Avm1Code::record_bytes() const {
	uint32_t sum = 0;
	for (const Avm1Action &a : actions) {
		sum += a.size();
	}
	return sum;
}

uint32_t Avm1Code::find(uint32_t p_offset) const {
	auto it = std::lower_bound(actions.begin(), actions.end(), p_offset, [](const Avm1Action &a, uint32_t o) { return a.offset < o; });
	if (it == actions.end() || it->offset != p_offset) {
		return kAvm1NoTarget;
	}
	return uint32_t(it - actions.begin());
}

const Avm1Action *Avm1Code::function_action(uint32_t p_scope) const {
	if (p_scope == 0 || p_scope >= scopes.size()) {
		return nullptr;
	}
	return &actions[scopes[p_scope].action];
}

Result<Avm1Code> decode_avm1(std::span<const uint8_t> p_bytes, uint32_t p_base) {
	Decoder d(p_bytes, p_base);
	return d.run();
}

}
