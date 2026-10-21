#include "bf3/core/thread_pool.h"
#include "bf3/core/ui/avm1.h"
#include "bf3/core/ui/swf_library.h"
#include "bf3/core/util/format.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <set>

namespace bf3::core::ui {

namespace {

std::string quote_text(std::string_view p_text) {
	std::string out = "\"";
	for (const char ch : p_text) {
		const unsigned char c = static_cast<unsigned char>(ch);
		switch (c) {
			case '\n': out += "\\n"; break;
			case '\r': out += "\\r"; break;
			case '\t': out += "\\t"; break;
			case '"': out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			default:
				if (c < 0x20 || c == 0x7F) {
					out += fmt("\\x%02X", unsigned(c));
				} else {
					out += ch;
				}
		}
	}
	return out + "\"";
}

std::string number(double p_v) {
	std::string s = fmt("%g", p_v);
	if (std::strtod(s.c_str(), nullptr) != p_v) {
		s = fmt("%.17g", p_v);
	}
	return s;
}

std::string value(const Avm1Value &p_v) {
	switch (p_v.type) {
		case Avm1PushType::String: return quote_text(p_v.text);
		case Avm1PushType::Float: return "f:" + number(p_v.number);
		case Avm1PushType::Null: return "null";
		case Avm1PushType::Undefined: return "undefined";
		case Avm1PushType::Register: return fmt("r:%u", p_v.index);
		case Avm1PushType::Boolean: return p_v.index != 0 ? "true" : "false";
		case Avm1PushType::Double: return "d:" + number(p_v.number);
		case Avm1PushType::Integer: return fmt("%d", int32_t(p_v.index));
		case Avm1PushType::Constant8:
		case Avm1PushType::Constant16: return p_v.resolved ? quote_text(p_v.text) : fmt("c:%u?", p_v.index);
	}
	return "?";
}

std::string function_flags(uint16_t p_flags) {
	static constexpr std::pair<uint16_t, const char *> kNames[] = {
		{ avm1_function_flag::kPreloadThis, "preload_this" },
		{ avm1_function_flag::kSuppressThis, "suppress_this" },
		{ avm1_function_flag::kPreloadArguments, "preload_arguments" },
		{ avm1_function_flag::kSuppressArguments, "suppress_arguments" },
		{ avm1_function_flag::kPreloadSuper, "preload_super" },
		{ avm1_function_flag::kSuppressSuper, "suppress_super" },
		{ avm1_function_flag::kPreloadRoot, "preload_root" },
		{ avm1_function_flag::kPreloadParent, "preload_parent" },
		{ avm1_function_flag::kPreloadGlobal, "preload_global" },
	};
	std::string out;
	for (const auto &[bit, name] : kNames) {
		if ((p_flags & bit) != 0) {
			out += out.empty() ? "" : "|";
			out += name;
		}
	}
	return out.empty() ? "none" : out;
}

std::string operands(const Avm1Action &a) {
	switch (a.op) {
		case avm1_op::kPush: {
			std::string out;
			for (const Avm1Value &v : a.values) {
				out += out.empty() ? "" : ", ";
				out += value(v);
			}
			return out;
		}
		case avm1_op::kConstantPool: {
			std::string out = fmt("%zu:", a.strings.size());
			for (size_t i = 0; i < a.strings.size(); ++i) {
				out += fmt(" %zu=", i) + quote_text(a.strings[i]);
			}
			return out;
		}
		case avm1_op::kDefineFunction:
		case avm1_op::kDefineFunction2: {
			std::string out = quote_text(a.strings.empty() ? std::string() : a.strings[0]) + " (";
			for (size_t i = 0; i < a.params.size(); ++i) {
				out += i == 0 ? "" : ", ";
				if (a.op == avm1_op::kDefineFunction2 && a.params[i].reg != 0) {
					out += fmt("r%u:", unsigned(a.params[i].reg));
				}
				out += a.params[i].name;
			}
			out += ")";
			if (a.op == avm1_op::kDefineFunction2) {
				out += fmt(" registers %u flags ", unsigned(a.register_count)) + function_flags(a.function_flags);
			}
			return out + fmt(" body %u bytes", unsigned(a.code_size));
		}
		case avm1_op::kJump:
		case avm1_op::kIf: return fmt("L%u", a.target);
		case avm1_op::kStoreRegister: return fmt("r:%u", unsigned(a.reg));
		case avm1_op::kGotoFrame: return fmt("%u", unsigned(a.frame));
		case avm1_op::kGotoFrame2: return fmt("%s%s", (a.mode & 0x01) != 0 ? "play" : "stop", (a.mode & 0x02) != 0 ? fmt(" scene bias %u", unsigned(a.scene_bias)).c_str() : "");
		case avm1_op::kGetUrl: return quote_text(a.strings[0]) + " " + quote_text(a.strings[1]);
		case avm1_op::kGetUrl2: return fmt("0x%02X", unsigned(a.mode));
		case avm1_op::kWaitForFrame: return fmt("%u skip %u", unsigned(a.frame), unsigned(a.skip_count));
		case avm1_op::kWaitForFrame2: return fmt("skip %u", unsigned(a.skip_count));
		case avm1_op::kSetTarget:
		case avm1_op::kGoToLabel: return quote_text(a.strings[0]);
		case avm1_op::kWith: return fmt("block %u bytes", unsigned(a.block_size));
		case avm1_op::kTry: {
			std::string out = fmt("try %u catch %u finally %u", unsigned(a.try_size), unsigned(a.catch_size), unsigned(a.finally_size));
			if ((a.try_flags & avm1_try_flag::kCatchInRegister) != 0) {
				out += fmt(" catch in r:%u", unsigned(a.reg));
			} else if (!a.strings.empty()) {
				out += " catch as " + a.strings[0];
			}
			return out;
		}
		default: return {};
	}
}

}

std::string format_avm1_code(const Avm1Code &p_code) {
	std::set<uint32_t> labels;
	for (const Avm1Action &a : p_code.actions) {
		if (a.target != kAvm1NoTarget) {
			labels.insert(a.target);
		}
	}
	std::string out;
	for (const Avm1Action &a : p_code.actions) {
		const std::string indent(size_t(p_code.scopes[a.scope].depth) * 2, ' ');
		if (labels.contains(a.offset)) {
			out += indent + fmt("L%u:\n", a.offset);
		}
		std::string ops = operands(a);
		out += fmt("%6u  ", a.offset) + indent + avm1_op_name(a.op) + (ops.empty() ? "" : " " + ops) + "\n";
	}
	if (labels.contains(p_code.bytes)) {
		out += fmt("L%u:\n", p_code.bytes);
	}
	return out;
}

std::string format_avm1_class(const Avm1Class &p_class) {
	std::string out = "class " + p_class.name;
	if (!p_class.base.empty()) {
		out += " extends " + p_class.base;
	}
	for (size_t i = 0; i < p_class.interfaces.size(); ++i) {
		out += (i == 0 ? " implements " : ", ") + p_class.interfaces[i];
	}
	out += fmt("\n  body %u, constructor %u bytes (scope %u)\n", p_class.body, p_class.constructor_bytes, p_class.constructor_scope);
	for (const Avm1Member &m : p_class.methods) {
		out += fmt("  method %s %u bytes (scope %u)\n", m.name.c_str(), m.bytes, m.scope);
	}
	for (const Avm1Member &m : p_class.static_methods) {
		out += fmt("  static method %s %u bytes (scope %u)\n", m.name.c_str(), m.bytes, m.scope);
	}
	for (const Avm1Field &f : p_class.fields) {
		out += "  field " + f.name + " = " + f.value + "\n";
	}
	for (const Avm1Field &f : p_class.static_fields) {
		out += "  static field " + f.name + " = " + f.value + "\n";
	}
	for (const std::string &p : p_class.properties) {
		out += "  property " + p + "\n";
	}
	return out;
}

std::string format_avm1_body_header(const SwfMovie &p_movie, const Avm1Movie &p_avm1, uint32_t p_body) {
	const Avm1Body &b = p_avm1.bodies[p_body];
	std::string out = fmt("body %u: %s", p_body, avm1_body_kind_name(b.kind));
	switch (b.kind) {
		case Avm1BodyKind::DoAction:
			out += fmt(" in sprite %u frame %u", unsigned(b.timeline), b.frame);
			break;
		case Avm1BodyKind::DoInitAction: {
			out += fmt(" for sprite %u", unsigned(b.sprite));
			const std::string_view name = p_movie.export_name(b.sprite);
			if (!name.empty()) {
				out += " " + quote_text(name);
			}
			break;
		}
		case Avm1BodyKind::ClipAction: {
			out += " " + avm1_clip_event_names(b.event_flags) + fmt(" on depth %u", unsigned(b.depth));
			if (!b.instance_name.empty()) {
				out += " " + quote_text(b.instance_name);
			}
			if (b.sprite != 0) {
				out += fmt(" (character %u", unsigned(b.sprite));
				const SwfExport *symbol = nullptr;
				if (const SwfImport *im = p_movie.find_import(b.sprite, &symbol); im != nullptr && symbol != nullptr) {
					out += " imported " + quote_text(symbol->name) + " from " + quote_text(im->url);
				} else if (const std::string_view name = p_movie.export_name(b.sprite); !name.empty()) {
					out += " " + quote_text(name);
				}
				out += ")";
			}
			out += fmt(" in sprite %u frame %u", unsigned(b.timeline), b.frame);
			if ((b.event_flags & avm1_clip_event::kKeyPress) != 0) {
				out += fmt(" key %u", unsigned(b.key_code));
			}
			break;
		}
	}
	return out + fmt(" at %u, %u bytes, %zu actions, %zu functions\n", b.offset, b.length, b.code.actions.size(), b.code.scopes.size() - 1);
}

const Avm1MovieCensus *Avm1Census::find(std::string_view p_name) const {
	for (const Avm1MovieCensus &m : movies) {
		if (m.name == p_name) {
			return &m;
		}
	}
	return nullptr;
}

Result<Avm1Census> run_avm1_census(const SwfLibrary &p_library, unsigned p_threads) {
	const auto start = std::chrono::steady_clock::now();
	const std::vector<SwfMovieSource> &sources = p_library.movies();
	Avm1Census census;
	census.movies = parallel_map<Avm1MovieCensus>(sources.size(), [&](size_t i) {
		Avm1MovieCensus c;
		c.name = sources[i].name;
		Result<std::vector<uint8_t>> payload = p_library.read_payload(c.name);
		if (!payload) {
			c.error = payload.error().message;
			return c;
		}
		Result<SwfMovie> movie = SwfMovie::parse(*payload);
		if (!movie) {
			c.error = movie.error().message;
			return c;
		}
		Result<Avm1Movie> avm1 = read_avm1(*movie);
		if (!avm1) {
			c.error = avm1.error().message;
			return c;
		}
		const Avm1Movie &m = *avm1;
		c.bodies = uint32_t(m.bodies.size());
		c.do_action_bytes = m.do_action_bytes;
		c.do_init_action_bytes = m.do_init_action_bytes;
		c.clip_action_bytes = m.clip_action_bytes;
		c.do_action_tags = m.do_action_tags;
		c.do_init_action_tags = m.do_init_action_tags;
		c.clip_action_tags = m.clip_action_tags;
		c.classes = uint32_t(m.classes.size());
		c.registrations = uint32_t(m.registrations.size());
		std::set<uint32_t> class_bodies;
		for (const Avm1Class &k : m.classes) {
			if (class_bodies.insert(k.body).second) {
				c.class_bytes += m.class_body_bytes(k);
			}
		}
		c.class_bodies = uint32_t(class_bodies.size());
		for (uint32_t b = 0; b < m.bodies.size(); ++b) {
			const std::string_view exported = m.bodies[b].kind == Avm1BodyKind::DoInitAction ? movie->export_name(m.bodies[b].sprite) : std::string_view();
			static constexpr std::string_view kPackages = "__Packages.";
			if (!exported.starts_with(kPackages)) {
				continue;
			}
			c.package_bodies++;
			uint32_t defined = 0;
			bool named = false;
			for (const Avm1Class &k : m.classes) {
				if (k.body == b) {
					defined++;
					named = named || k.name == exported.substr(kPackages.size());
				}
			}
			c.package_bodies_matched += defined == 1 && named ? 1 : 0;
		}
		for (const Avm1Body &b : m.bodies) {
			const uint32_t expected = b.kind == Avm1BodyKind::DoAction ? b.tag_length : b.kind == Avm1BodyKind::DoInitAction ? b.tag_length - 2 : b.length;
			if (b.code.record_bytes() != b.length || b.length != expected) {
				c.records_mismatched++;
			}
			c.actions += b.code.actions.size();
			c.functions += uint32_t(b.code.scopes.size() - 1);
			c.unresolved_constants += b.code.unresolved_constants;
			c.registers_out_of_range += b.code.registers_out_of_range;
			for (const Avm1Action &a : b.code.actions) {
				c.ops[a.op]++;
			}
		}
		return c;
	}, p_threads);
	for (const Avm1MovieCensus &c : census.movies) {
		census.errors += c.error.empty() ? 0 : 1;
		census.actions += c.actions;
		census.do_action_bytes += c.do_action_bytes;
		census.do_init_action_bytes += c.do_init_action_bytes;
		census.clip_action_bytes += c.clip_action_bytes;
		census.do_action_tags += c.do_action_tags;
		census.do_init_action_tags += c.do_init_action_tags;
		census.clip_action_tags += c.clip_action_tags;
		census.records_mismatched += c.records_mismatched;
		census.functions += c.functions;
		census.classes += c.classes;
		census.package_bodies += c.package_bodies;
		census.package_bodies_matched += c.package_bodies_matched;
		census.registrations += c.registrations;
		census.unresolved_constants += c.unresolved_constants;
		census.registers_out_of_range += c.registers_out_of_range;
		for (size_t op = 0; op < c.ops.size(); ++op) {
			census.ops[op] += c.ops[op];
		}
	}
	census.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	return census;
}

std::string format_avm1_census(const Avm1Census &p_census) {
	std::string out = fmt("avm1: %zu movies, %u errors, %.3f s\n", p_census.movies.size(), p_census.errors, p_census.seconds);
	out += fmt("tags: %u DoAction (%llu bytes), %u DoInitAction (%llu bytes), together %llu bytes\n", p_census.do_action_tags,
			(unsigned long long)p_census.do_action_bytes, p_census.do_init_action_tags, (unsigned long long)p_census.do_init_action_bytes,
			(unsigned long long)p_census.tag_action_bytes());
	out += fmt("clip actions: %u PlaceObject tags, %llu bytes; all AVM1 %llu bytes\n", p_census.clip_action_tags,
			(unsigned long long)p_census.clip_action_bytes, (unsigned long long)(p_census.tag_action_bytes() + p_census.clip_action_bytes));
	out += fmt("records: %llu actions, %u functions, %u bodies whose records do not sum to the tag, %u unresolved constants, %u registers out of range\n",
			(unsigned long long)p_census.actions, p_census.functions, p_census.records_mismatched, p_census.unresolved_constants, p_census.registers_out_of_range);
	out += fmt("classes: %u; __Packages DoInitAction bodies: %u, %u of them define exactly the class they export; Object.registerClass calls: %u\n",
			p_census.classes, p_census.package_bodies, p_census.package_bodies_matched, p_census.registrations);
	std::vector<std::pair<uint32_t, uint8_t>> ops;
	for (size_t op = 0; op < p_census.ops.size(); ++op) {
		if (p_census.ops[op] != 0) {
			ops.emplace_back(p_census.ops[op], uint8_t(op));
		}
	}
	std::sort(ops.begin(), ops.end(), [](const auto &a, const auto &b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });
	out += fmt("ops: %zu kinds\n", ops.size());
	for (const auto &[count, op] : ops) {
		out += fmt("  0x%02X %-16s %u\n", unsigned(op), avm1_op_name(op), count);
	}
	std::vector<const Avm1MovieCensus *> by_classes;
	for (const Avm1MovieCensus &c : p_census.movies) {
		if (c.classes != 0) {
			by_classes.push_back(&c);
		}
	}
	std::sort(by_classes.begin(), by_classes.end(), [](const auto *a, const auto *b) { return a->class_bytes != b->class_bytes ? a->class_bytes > b->class_bytes : a->name < b->name; });
	out += fmt("movies defining classes: %zu\n", by_classes.size());
	for (const Avm1MovieCensus *c : by_classes) {
		out += fmt("  %-48s %4u classes in %4u bodies, %8llu bytes\n", c->name.c_str(), c->classes, c->class_bodies, (unsigned long long)c->class_bytes);
	}
	for (const Avm1MovieCensus &c : p_census.movies) {
		if (!c.error.empty()) {
			out += "error " + c.name + ": " + c.error + "\n";
		}
	}
	return out;
}

}
