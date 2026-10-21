#include "bf3/core/ui/avm1.h"

#include "swf_bits.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>

namespace bf3::core::ui {

namespace {

using detail::SwfReader;

constexpr uint32_t kSwfHeaderBytes = 8;

Error movie_error(const std::string &p_what, size_t p_body_at) {
	return make_error("avm1: " + p_what + " at " + std::to_string(p_body_at + kSwfHeaderBytes));
}

Status skip_filter_list(SwfReader &r) {
	const uint8_t n = r.u8();
	for (unsigned i = 0; i < n && !r.failed(); ++i) {
		const size_t at = r.abs_pos();
		const uint8_t id = r.u8();
		switch (id) {
			case 0: r.skip(23); break;
			case 1: r.skip(9); break;
			case 2: r.skip(15); break;
			case 3: r.skip(27); break;
			case 4:
			case 7: {
				const uint8_t colours = r.u8();
				r.skip(size_t(colours) * 5 + 19);
				break;
			}
			case 5: {
				const uint8_t mx = r.u8();
				const uint8_t my = r.u8();
				r.skip(8 + size_t(mx) * my * 4 + 5);
				break;
			}
			case 6: r.skip(80); break;
			default: return movie_error("PlaceObject3 filter id " + std::to_string(id), at);
		}
	}
	return Status::success();
}

class Walker {
public:
	Walker(const SwfMovie &p_movie, Avm1Movie &r_out) :
			movie_(p_movie), out_(r_out), body_(p_movie.body) {}

	Status run() {
		SwfReader r(body_);
		(void)r.rect();
		r.skip(4);
		if (r.failed()) {
			return movie_error("body shorter than its header", 0);
		}
		return timeline(r.pos(), body_.size(), 0);
	}

private:
	Status timeline(size_t p_begin, size_t p_end, uint16_t p_owner) {
		size_t p = p_begin;
		uint32_t frame = 0;
		while (p < p_end) {
			if (p_end - p < 2) {
				return movie_error("tag header past the end of its timeline", p);
			}
			const uint16_t code_len = uint16_t(body_[p] | (body_[p + 1] << 8));
			const uint16_t code = uint16_t(code_len >> 6);
			size_t len = code_len & 0x3f;
			size_t header = 2;
			if (len == 0x3f) {
				if (p_end - p < 6) {
					return movie_error("long tag header past the end of its timeline", p);
				}
				len = uint32_t(body_[p + 2] | (body_[p + 3] << 8) | (body_[p + 4] << 16) | (uint32_t(body_[p + 5]) << 24));
				header = 6;
			}
			if (len > p_end - p - header) {
				return movie_error("tag length " + std::to_string(len) + " past the end of its timeline", p);
			}
			const size_t at = p + header;
			switch (code) {
				case swf_tag::kShowFrame:
					frame++;
					break;
				case swf_tag::kDoAction:
					BF3_TRY(add_body(Avm1BodyKind::DoAction, p_owner, frame, 0, at, len, at, len));
					out_.do_action_tags++;
					out_.do_action_bytes += len;
					break;
				case swf_tag::kDoInitAction: {
					if (len < 2) {
						return movie_error("DoInitAction shorter than its sprite id", at);
					}
					const uint16_t sprite = uint16_t(body_[at] | (body_[at + 1] << 8));
					BF3_TRY(add_body(Avm1BodyKind::DoInitAction, p_owner, frame, sprite, at, len, at + 2, len - 2));
					out_.do_init_action_tags++;
					out_.do_init_action_bytes += len - 2;
					break;
				}
				case swf_tag::kPlaceObject2:
				case swf_tag::kPlaceObject3:
					BF3_TRY(place(code, p_owner, frame, at, len));
					break;
				case swf_tag::kDefineSprite: {
					if (p_owner != 0) {
						return movie_error("DefineSprite inside a sprite", at);
					}
					if (len < 4) {
						return movie_error("DefineSprite shorter than its header", at);
					}
					const uint16_t id = uint16_t(body_[at] | (body_[at + 1] << 8));
					BF3_TRY(timeline(at + 4, at + len, id));
					break;
				}
				default:
					break;
			}
			p = at + len;
			if (code == swf_tag::kEnd) {
				break;
			}
		}
		return Status::success();
	}

	Status add_body(Avm1BodyKind p_kind, uint16_t p_owner, uint32_t p_frame, uint16_t p_sprite, size_t p_tag_at, size_t p_tag_len, size_t p_at, size_t p_len) {
		Avm1Body b;
		b.kind = p_kind;
		b.timeline = p_owner;
		b.frame = p_frame;
		b.sprite = p_sprite;
		b.tag_offset = uint32_t(p_tag_at + kSwfHeaderBytes);
		b.tag_length = uint32_t(p_tag_len);
		b.offset = uint32_t(p_at + kSwfHeaderBytes);
		b.length = uint32_t(p_len);
		Result<Avm1Code> code = decode_avm1(std::span<const uint8_t>(body_).subspan(p_at, p_len), b.offset);
		if (!code) {
			return code.error();
		}
		b.code = std::move(*code);
		out_.bodies.push_back(std::move(b));
		return Status::success();
	}

	Status place(uint16_t p_code, uint16_t p_owner, uint32_t p_frame, size_t p_at, size_t p_len) {
		const std::span<const uint8_t> data = std::span<const uint8_t>(body_).subspan(p_at, p_len);
		SwfReader r(data, p_at);
		const bool v3 = p_code == swf_tag::kPlaceObject3;
		const uint8_t f = r.u8();
		const uint8_t f2 = v3 ? r.u8() : 0;
		if ((f & 0x80) == 0) {
			return Status::success();
		}
		const uint16_t depth = r.u16();
		const bool has_char = (f & 0x02) != 0;
		if (v3 && ((f2 & 0x08) != 0 || ((f2 & 0x10) != 0 && has_char))) {
			(void)r.cstr();
		}
		uint16_t character = 0;
		if (has_char) {
			character = r.u16();
		}
		if ((f & 0x04) != 0) {
			(void)r.matrix();
		}
		if ((f & 0x08) != 0) {
			(void)r.cxform(true);
		}
		if ((f & 0x10) != 0) {
			(void)r.u16();
		}
		std::string name;
		if ((f & 0x20) != 0) {
			name = std::string(r.cstr());
		}
		if ((f & 0x40) != 0) {
			(void)r.u16();
		}
		if (v3) {
			if ((f2 & 0x01) != 0) {
				BF3_TRY(skip_filter_list(r));
			}
			if ((f2 & 0x02) != 0) {
				(void)r.u8();
			}
			if ((f2 & 0x04) != 0) {
				(void)r.u8();
			}
			if ((f2 & 0x20) != 0) {
				(void)r.u8();
			}
			if ((f2 & 0x40) != 0) {
				r.skip(4);
			}
		}
		const bool wide = movie_.version >= 6;
		(void)r.u16();
		(void)(wide ? r.u32() : r.u16());
		if (r.failed()) {
			return movie_error("PlaceObject clip actions header past the end of the tag", p_at);
		}
		out_.clip_action_tags++;
		while (true) {
			const size_t record_at = r.pos();
			const uint32_t events = wide ? r.u32() : r.u16();
			if (r.failed()) {
				return movie_error("clip action list runs past the end of its PlaceObject", p_at + record_at);
			}
			if (events == 0) {
				break;
			}
			uint32_t size = r.u32();
			uint8_t key = 0;
			if ((events & avm1_clip_event::kKeyPress) != 0) {
				if (size == 0) {
					return movie_error("clip action record with a key code and 0 bytes", p_at + record_at);
				}
				key = r.u8();
				size--;
			}
			if (r.failed() || size > r.left()) {
				return movie_error("clip action record of " + std::to_string(size) + " bytes past the end of its PlaceObject", p_at + record_at);
			}
			const size_t actions_at = p_at + r.pos();
			BF3_TRY(add_body(Avm1BodyKind::ClipAction, p_owner, p_frame, character, p_at, p_len, actions_at, size));
			Avm1Body &b = out_.bodies.back();
			b.depth = depth;
			b.instance_name = name;
			b.event_flags = events;
			b.key_code = key;
			out_.clip_action_bytes += size;
			r.skip(size);
		}
		if (!r.at_end()) {
			return movie_error("PlaceObject has " + std::to_string(r.left()) + " bytes after its clip actions", p_at + r.pos());
		}
		return Status::success();
	}

	const SwfMovie &movie_;
	Avm1Movie &out_;
	const std::vector<uint8_t> &body_;
};

enum class SymKind : uint8_t {
	Unknown,
	String,
	Number,
	Path,
	Function,
	Prototype,
	Object,
};

struct Sym {
	SymKind kind = SymKind::Unknown;
	std::string text;
	uint32_t scope = 0;
};

std::string class_path(const std::string &p_path) {
	static constexpr std::string_view kGlobal = "_global.";
	if (p_path.starts_with(kGlobal)) {
		return p_path.substr(kGlobal.size());
	}
	return p_path;
}

std::string value_text(const Sym &p_sym) {
	switch (p_sym.kind) {
		case SymKind::String: return "\"" + p_sym.text + "\"";
		case SymKind::Number: return p_sym.text;
		case SymKind::Path: return class_path(p_sym.text);
		case SymKind::Function: return "function";
		case SymKind::Prototype: return p_sym.text + ".prototype";
		case SymKind::Object: return "new " + p_sym.text;
		case SymKind::Unknown: break;
	}
	return "?";
}

std::string number_text(double p_v) {
	char buf[40];
	std::snprintf(buf, sizeof(buf), "%.17g", p_v);
	std::string s = buf;
	char shorter[40];
	std::snprintf(shorter, sizeof(shorter), "%g", p_v);
	if (std::strtod(shorter, nullptr) == p_v) {
		s = shorter;
	}
	return s;
}

class ClassFinder {
public:
	ClassFinder(Avm1Movie &r_movie, uint32_t p_body) :
			movie_(r_movie), body_index_(p_body), code_(r_movie.bodies[p_body].code) {}

	void run() {
		for (const Avm1Action &a : code_.actions) {
			if (a.scope != 0) {
				continue;
			}
			step(a);
		}
	}

private:
	Sym pop() {
		if (stack_.empty()) {
			return {};
		}
		Sym s = std::move(stack_.back());
		stack_.pop_back();
		return s;
	}
	void push(Sym p_s) { stack_.push_back(std::move(p_s)); }
	void pop_n(size_t p_n) {
		for (size_t i = 0; i < p_n && !stack_.empty(); ++i) {
			stack_.pop_back();
		}
	}
	size_t pop_count() {
		const Sym n = pop();
		if (n.kind != SymKind::Number) {
			return 0;
		}
		const double v = std::strtod(n.text.c_str(), nullptr);
		return v >= 0 && v < 4096 ? size_t(v) : 0;
	}
	std::vector<Sym> pop_args(size_t p_n) {
		std::vector<Sym> args;
		for (size_t i = 0; i < p_n; ++i) {
			args.push_back(pop());
		}
		return args;
	}

	Sym from_value(const Avm1Value &p_v) {
		Sym s;
		switch (p_v.type) {
			case Avm1PushType::String:
				s.kind = SymKind::String;
				s.text = p_v.text;
				break;
			case Avm1PushType::Constant8:
			case Avm1PushType::Constant16:
				if (p_v.resolved) {
					s.kind = SymKind::String;
					s.text = p_v.text;
				}
				break;
			case Avm1PushType::Float:
			case Avm1PushType::Double:
			case Avm1PushType::Integer:
				s.kind = SymKind::Number;
				s.text = number_text(p_v.number);
				break;
			case Avm1PushType::Boolean:
				s.kind = SymKind::Number;
				s.text = p_v.index != 0 ? "true" : "false";
				break;
			case Avm1PushType::Null:
				s.kind = SymKind::Number;
				s.text = "null";
				break;
			case Avm1PushType::Undefined:
				s.kind = SymKind::Number;
				s.text = "undefined";
				break;
			case Avm1PushType::Register:
				if (p_v.index < registers_.size()) {
					s = registers_[p_v.index];
				}
				break;
		}
		return s;
	}

	Avm1Class *class_named(const std::string &p_name) {
		for (Avm1Class &c : movie_.classes) {
			if (c.name == p_name && c.body == body_index_) {
				return &c;
			}
		}
		return nullptr;
	}
	Avm1Class *class_of_function(uint32_t p_scope) {
		auto it = ctor_class_.find(p_scope);
		return it == ctor_class_.end() ? nullptr : class_named(it->second);
	}
	Avm1Class *class_of(const Sym &p_s) {
		if (p_s.kind == SymKind::Function) {
			return class_of_function(p_s.scope);
		}
		if (p_s.kind == SymKind::Path) {
			return class_named(class_path(p_s.text));
		}
		return nullptr;
	}
	uint32_t scope_bytes(uint32_t p_scope) const { return code_.scopes[p_scope].bytes(); }

	void set_member(const Sym &p_obj, const Sym &p_name, const Sym &p_value) {
		if (p_name.kind != SymKind::String) {
			return;
		}
		if (p_obj.kind == SymKind::Prototype) {
			Avm1Class *c = class_named(p_obj.text);
			if (c == nullptr) {
				return;
			}
			if (p_value.kind == SymKind::Function) {
				c->methods.push_back({ p_name.text, p_value.scope, scope_bytes(p_value.scope) });
			} else {
				c->fields.push_back({ p_name.text, value_text(p_value) });
			}
			return;
		}
		if (Avm1Class *c = class_of(p_obj); c != nullptr) {
			if (p_value.kind == SymKind::Function) {
				c->static_methods.push_back({ p_name.text, p_value.scope, scope_bytes(p_value.scope) });
			} else {
				c->static_fields.push_back({ p_name.text, value_text(p_value) });
			}
			return;
		}
		if (p_obj.kind != SymKind::Path) {
			return;
		}
		const bool in_global = p_obj.text == "_global" || p_obj.text.starts_with("_global.");
		const std::string container = class_path(p_obj.text);
		const bool in_package = std::find(movie_.packages.begin(), movie_.packages.end(), container) != movie_.packages.end();
		if (!in_global && !in_package) {
			return;
		}
		const std::string full = class_path(p_obj.text + "." + p_name.text);
		if (p_value.kind == SymKind::Function) {
			Avm1Class c;
			c.name = full;
			c.body = body_index_;
			c.constructor_scope = p_value.scope;
			c.constructor_bytes = scope_bytes(p_value.scope);
			movie_.classes.push_back(std::move(c));
			ctor_class_[p_value.scope] = full;
		} else if (p_value.kind == SymKind::Object && p_value.text == "Object") {
			if (std::find(movie_.packages.begin(), movie_.packages.end(), full) == movie_.packages.end()) {
				movie_.packages.push_back(full);
			}
		}
	}

	void call_method(const Sym &p_obj, const Sym &p_name, const std::vector<Sym> &p_args) {
		if (p_name.kind != SymKind::String) {
			return;
		}
		if (p_name.text == "registerClass" && p_obj.kind == SymKind::Path && p_obj.text == "Object" && p_args.size() == 2 && p_args[0].kind == SymKind::String) {
			std::string cls;
			if (p_args[1].kind == SymKind::Path) {
				cls = class_path(p_args[1].text);
			} else if (const Avm1Class *c = class_of(p_args[1]); c != nullptr) {
				cls = c->name;
			}
			movie_.registrations.push_back({ p_args[0].text, cls, body_index_ });
			return;
		}
		if (p_name.text == "addProperty" && !p_args.empty() && p_args[0].kind == SymKind::String) {
			Avm1Class *c = p_obj.kind == SymKind::Prototype ? class_named(p_obj.text) : class_of(p_obj);
			if (c != nullptr) {
				c->properties.push_back(p_args[0].text);
			}
		}
	}

	void step(const Avm1Action &a) {
		switch (a.op) {
			case avm1_op::kConstantPool:
			case avm1_op::kJump:
			case avm1_op::kEnd:
				return;
			case avm1_op::kPush:
				for (const Avm1Value &v : a.values) {
					push(from_value(v));
				}
				return;
			case avm1_op::kPop:
			case avm1_op::kIf:
			case avm1_op::kTrace:
			case avm1_op::kThrow:
			case avm1_op::kDefineLocal2:
			case avm1_op::kSetTarget2:
			case avm1_op::kReturn:
				pop();
				return;
			case avm1_op::kPushDuplicate: {
				Sym top = stack_.empty() ? Sym{} : stack_.back();
				push(std::move(top));
				return;
			}
			case avm1_op::kStackSwap: {
				Sym x = pop();
				Sym y = pop();
				push(std::move(x));
				push(std::move(y));
				return;
			}
			case avm1_op::kStoreRegister:
				if (registers_.size() <= a.reg) {
					registers_.resize(size_t(a.reg) + 1);
				}
				registers_[a.reg] = stack_.empty() ? Sym{} : stack_.back();
				return;
			case avm1_op::kGetVariable: {
				const Sym name = pop();
				Sym s;
				if (name.kind == SymKind::String) {
					s.kind = SymKind::Path;
					s.text = name.text;
				}
				push(std::move(s));
				return;
			}
			case avm1_op::kSetVariable:
			case avm1_op::kDefineLocal:
				pop_n(2);
				return;
			case avm1_op::kGetMember: {
				const Sym name = pop();
				const Sym obj = pop();
				Sym s;
				const Avm1Class *c = name.kind == SymKind::String && name.text == "prototype" ? class_of(obj) : nullptr;
				if (c != nullptr) {
					s.kind = SymKind::Prototype;
					s.text = c->name;
				} else if (obj.kind == SymKind::Path && name.kind == SymKind::String) {
					s.kind = SymKind::Path;
					s.text = obj.text + "." + name.text;
				}
				push(std::move(s));
				return;
			}
			case avm1_op::kSetMember: {
				const Sym value = pop();
				const Sym name = pop();
				const Sym obj = pop();
				set_member(obj, name, value);
				return;
			}
			case avm1_op::kDefineFunction:
			case avm1_op::kDefineFunction2:
				if (a.strings.empty() || a.strings[0].empty()) {
					Sym s;
					s.kind = SymKind::Function;
					s.scope = defined_scope(a);
					push(std::move(s));
				}
				return;
			case avm1_op::kNewObject: {
				const Sym name = pop();
				pop_args(pop_count());
				Sym s;
				s.kind = SymKind::Object;
				s.text = name.kind == SymKind::String ? name.text : "?";
				push(std::move(s));
				return;
			}
			case avm1_op::kNewMethod: {
				pop_n(2);
				pop_args(pop_count());
				push({});
				return;
			}
			case avm1_op::kCallFunction: {
				pop();
				pop_args(pop_count());
				push({});
				return;
			}
			case avm1_op::kCallMethod: {
				const Sym name = pop();
				const Sym obj = pop();
				const std::vector<Sym> args = pop_args(pop_count());
				call_method(obj, name, args);
				push({});
				return;
			}
			case avm1_op::kInitArray:
				pop_args(pop_count());
				push({});
				return;
			case avm1_op::kInitObject:
				pop_args(pop_count() * 2);
				push({});
				return;
			case avm1_op::kExtends: {
				const Sym super_class = pop();
				const Sym sub_class = pop();
				if (Avm1Class *c = class_of(sub_class); c != nullptr) {
					c->base = super_class.kind == SymKind::Path ? class_path(super_class.text) : value_text(super_class);
				}
				return;
			}
			case avm1_op::kImplementsOp: {
				const Sym ctor = pop();
				const std::vector<Sym> ifaces = pop_args(pop_count());
				if (Avm1Class *c = class_of(ctor); c != nullptr) {
					for (const Sym &i : ifaces) {
						c->interfaces.push_back(i.kind == SymKind::Path ? class_path(i.text) : value_text(i));
					}
				}
				return;
			}
			case avm1_op::kNot:
			case avm1_op::kToInteger:
			case avm1_op::kToNumber:
			case avm1_op::kToString:
			case avm1_op::kTypeOf:
			case avm1_op::kIncrement:
			case avm1_op::kDecrement:
			case avm1_op::kStringLength:
			case avm1_op::kMBStringLength:
			case avm1_op::kCharToAscii:
			case avm1_op::kAsciiToChar:
			case avm1_op::kMBCharToAscii:
			case avm1_op::kMBAsciiToChar:
			case avm1_op::kTargetPath:
			case avm1_op::kDelete2:
			case avm1_op::kRandomNumber:
				pop();
				push({});
				return;
			case avm1_op::kAdd:
			case avm1_op::kSubtract:
			case avm1_op::kMultiply:
			case avm1_op::kDivide:
			case avm1_op::kEquals:
			case avm1_op::kLess:
			case avm1_op::kAnd:
			case avm1_op::kOr:
			case avm1_op::kStringEquals:
			case avm1_op::kStringAdd:
			case avm1_op::kStringLess:
			case avm1_op::kAdd2:
			case avm1_op::kLess2:
			case avm1_op::kEquals2:
			case avm1_op::kModulo:
			case avm1_op::kBitAnd:
			case avm1_op::kBitOr:
			case avm1_op::kBitXor:
			case avm1_op::kBitLShift:
			case avm1_op::kBitRShift:
			case avm1_op::kBitURShift:
			case avm1_op::kInstanceOf:
			case avm1_op::kStrictEquals:
			case avm1_op::kGreater:
			case avm1_op::kStringGreater:
			case avm1_op::kCastOp:
			case avm1_op::kDelete:
			case avm1_op::kGetProperty:
				pop_n(2);
				push({});
				return;
			case avm1_op::kGetTime:
				push({});
				return;
			case avm1_op::kSetProperty:
				pop_n(3);
				return;
			default:
				stack_.clear();
				return;
		}
	}

	uint32_t defined_scope(const Avm1Action &p_a) const {
		const uint32_t index = uint32_t(&p_a - code_.actions.data());
		auto it = std::lower_bound(code_.scopes.begin() + 1, code_.scopes.end(), index, [](const Avm1Scope &s, uint32_t i) { return s.action < i; });
		return it != code_.scopes.end() && it->action == index ? uint32_t(it - code_.scopes.begin()) : 0;
	}

	Avm1Movie &movie_;
	uint32_t body_index_;
	const Avm1Code &code_;
	std::vector<Sym> stack_;
	std::vector<Sym> registers_;
	std::map<uint32_t, std::string> ctor_class_;
};

}

const char *avm1_body_kind_name(Avm1BodyKind p_kind) {
	switch (p_kind) {
		case Avm1BodyKind::DoAction: return "DoAction";
		case Avm1BodyKind::DoInitAction: return "DoInitAction";
		case Avm1BodyKind::ClipAction: return "ClipAction";
	}
	return "?";
}

const Avm1Class *Avm1Movie::find_class(std::string_view p_name) const {
	for (const Avm1Class &c : classes) {
		if (c.name == p_name) {
			return &c;
		}
	}
	for (const Avm1Class &c : classes) {
		const size_t dot = c.name.rfind('.');
		if (dot != std::string::npos && std::string_view(c.name).substr(dot + 1) == p_name) {
			return &c;
		}
	}
	return nullptr;
}

uint32_t Avm1Movie::class_body_bytes(const Avm1Class &p_class) const {
	return p_class.body < bodies.size() ? bodies[p_class.body].length : 0;
}

Result<Avm1Movie> read_avm1(const SwfMovie &p_movie) {
	Avm1Movie out;
	Walker w(p_movie, out);
	BF3_TRY(w.run());
	for (uint32_t i = 0; i < out.bodies.size(); ++i) {
		ClassFinder f(out, i);
		f.run();
	}
	return out;
}

}
