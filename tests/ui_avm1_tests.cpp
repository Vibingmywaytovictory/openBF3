#include <doctest/doctest.h>

#include "bf3/core/install_snapshot.h"
#include "bf3/core/ui/avm1.h"
#include "bf3/core/ui/swf_library.h"
#include "bf3/core/ui/swf_movie.h"
#include "test_support.h"
#include "ui_swf_writer.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <vector>

using namespace bf3::core;
using namespace bf3::core::ui;
using namespace ui1test;

namespace {

struct Asm {
	Bytes b;
	std::map<std::string, uint32_t> labels;
	std::vector<std::pair<size_t, std::string>> fixups;

	uint32_t here() const { return uint32_t(b.size()); }
	Asm &op(uint8_t p_op) {
		b.push_back(p_op);
		return *this;
	}
	Asm &rec(uint8_t p_op, const Bytes &p_payload) {
		b.push_back(p_op);
		b.push_back(uint8_t(p_payload.size()));
		b.push_back(uint8_t(p_payload.size() >> 8));
		b.insert(b.end(), p_payload.begin(), p_payload.end());
		return *this;
	}
	Asm &raw(const Bytes &p_bytes) {
		b.insert(b.end(), p_bytes.begin(), p_bytes.end());
		return *this;
	}
	Asm &label(const std::string &p_name) {
		labels[p_name] = here();
		return *this;
	}
	Asm &branch(uint8_t p_op, const std::string &p_label) {
		rec(p_op, { 0, 0 });
		fixups.emplace_back(b.size() - 2, p_label);
		return *this;
	}
	Asm &constants(const std::vector<std::string> &p_strings) {
		Writer w;
		w.u16(uint32_t(p_strings.size()));
		for (const std::string &s : p_strings) {
			w.str(s);
		}
		return rec(avm1_op::kConstantPool, w.b);
	}
	Asm &push_str(const std::string &p_s) {
		Writer w;
		w.u8(0);
		w.str(p_s);
		return rec(avm1_op::kPush, w.b);
	}
	Asm &push_int(int32_t p_v) {
		Writer w;
		w.u8(7);
		w.u32(uint32_t(p_v));
		return rec(avm1_op::kPush, w.b);
	}
	Asm &push_reg(uint8_t p_r) { return rec(avm1_op::kPush, { 4, p_r }); }
	Asm &push_c8(uint8_t p_i) { return rec(avm1_op::kPush, { 8, p_i }); }
	Asm &push_null() { return rec(avm1_op::kPush, { 2 }); }
	Asm &store(uint8_t p_r) { return rec(avm1_op::kStoreRegister, { p_r }); }
	Asm &function2(const std::string &p_name, const std::vector<std::pair<uint8_t, std::string>> &p_params, uint8_t p_registers, uint16_t p_flags,
			const Bytes &p_body) {
		Writer w;
		w.str(p_name);
		w.u16(uint32_t(p_params.size()));
		w.u8(p_registers);
		w.u16(p_flags);
		for (const auto &[reg, name] : p_params) {
			w.u8(reg);
			w.str(name);
		}
		w.u16(uint32_t(p_body.size()));
		rec(avm1_op::kDefineFunction2, w.b);
		return raw(p_body);
	}
	Asm &function1(const std::string &p_name, const std::vector<std::string> &p_params, const Bytes &p_body) {
		Writer w;
		w.str(p_name);
		w.u16(uint32_t(p_params.size()));
		for (const std::string &p : p_params) {
			w.str(p);
		}
		w.u16(uint32_t(p_body.size()));
		rec(avm1_op::kDefineFunction, w.b);
		return raw(p_body);
	}
	Bytes done() {
		for (const auto &[at, name] : fixups) {
			REQUIRE(labels.contains(name));
			const int32_t off = int32_t(labels[name]) - int32_t(at + 2);
			b[at] = uint8_t(uint16_t(int16_t(off)));
			b[at + 1] = uint8_t(uint16_t(int16_t(off)) >> 8);
		}
		return b;
	}
};

Avm1Code decode_ok(const Bytes &p_bytes) {
	Result<Avm1Code> c = decode_avm1(p_bytes, 100);
	REQUIRE_MESSAGE(c.ok(), (c.ok() ? std::string() : c.error().message));
	CHECK(c->record_bytes() == p_bytes.size());
	return std::move(*c);
}

std::string error_of(const Bytes &p_bytes) {
	Result<Avm1Code> c = decode_avm1(p_bytes, 100);
	return c ? std::string() : c.error().message;
}

bool contains(const std::string &p_text, const char *p_part) {
	return p_text.find(p_part) != std::string::npos;
}

Bytes function_rich_body() {
	Asm a;
	a.constants({ "a", "bc" });
	a.function2("f", { { 1, "x" }, { 0, "y" } }, 3, avm1_function_flag::kPreloadThis | avm1_function_flag::kSuppressArguments | avm1_function_flag::kPreloadGlobal,
			Asm().push_reg(2).store(1).op(avm1_op::kReturn).b);
	Asm inner;
	inner.push_str("x").op(avm1_op::kGetVariable).op(avm1_op::kReturn);
	Asm g;
	g.push_str("x").function2("", {}, 2, 0, inner.b).op(avm1_op::kPop);
	a.function1("g", { "x", "z" }, g.b);
	a.label("top").push_int(1).branch(avm1_op::kIf, "out");
	a.rec(avm1_op::kWith, { 2, 0 }).op(avm1_op::kPlay).op(avm1_op::kStop);
	Writer t;
	t.u8(avm1_try_flag::kCatchBlock | avm1_try_flag::kFinallyBlock);
	t.u16(1);
	t.u16(1);
	t.u16(1);
	t.str("e");
	a.rec(avm1_op::kTry, t.b).op(avm1_op::kNextFrame).op(avm1_op::kPreviousFrame).op(avm1_op::kStop);
	a.branch(avm1_op::kJump, "top");
	a.label("out").op(avm1_op::kEnd);
	return a.done();
}

Bytes class_body() {
	Asm a;
	a.constants({ "_global", "Widget", "Object", "Hud", "Ammo", "Base", "prototype", "setup", "m_x", "count", "value", "addProperty", "ASSetPropFlags" });
	a.push_c8(0).op(avm1_op::kGetVariable).push_c8(1).op(avm1_op::kGetMember).op(avm1_op::kNot).op(avm1_op::kNot).branch(avm1_op::kIf, "have_widget");
	a.push_c8(0).op(avm1_op::kGetVariable).push_c8(1).push_int(0).push_c8(2).op(avm1_op::kNewObject).op(avm1_op::kSetMember);
	a.label("have_widget");
	a.push_c8(0).op(avm1_op::kGetVariable).push_c8(1).op(avm1_op::kGetMember).push_c8(3).push_int(0).push_c8(2).op(avm1_op::kNewObject).op(avm1_op::kSetMember);
	a.push_c8(0).op(avm1_op::kGetVariable).push_c8(1).op(avm1_op::kGetMember).push_c8(3).op(avm1_op::kGetMember).push_c8(4);
	a.function2("", {}, 2, avm1_function_flag::kSuppressArguments, Asm().push_reg(1).op(avm1_op::kPop).b);
	a.store(1).op(avm1_op::kSetMember);
	a.push_reg(1).push_c8(0).op(avm1_op::kGetVariable).push_c8(1).op(avm1_op::kGetMember).push_c8(3).op(avm1_op::kGetMember).push_c8(5).op(avm1_op::kGetMember).op(avm1_op::kExtends);
	a.push_reg(1).push_c8(6).op(avm1_op::kGetMember).store(2).op(avm1_op::kPop);
	a.push_reg(2).push_c8(7);
	a.function2("", { { 1, "p_a" } }, 3, 0, Asm().push_reg(1).push_int(4).op(avm1_op::kAdd2).op(avm1_op::kReturn).b);
	a.op(avm1_op::kSetMember);
	a.push_reg(2).push_c8(8).push_int(5).op(avm1_op::kSetMember);
	a.push_reg(1).push_c8(9).push_int(0).op(avm1_op::kSetMember);
	a.push_null().push_null().push_c8(10).push_int(3).push_reg(2).push_c8(11).op(avm1_op::kCallMethod).op(avm1_op::kPop);
	a.push_int(1).push_null().push_reg(2).push_int(3).push_c8(12).op(avm1_op::kCallFunction).op(avm1_op::kPop);
	a.op(avm1_op::kEnd);
	return a.done();
}

Bytes register_body() {
	Asm a;
	a.push_str("Widget").op(avm1_op::kGetVariable).push_str("Hud").op(avm1_op::kGetMember).push_str("Ammo").op(avm1_op::kGetMember);
	a.push_str("AmmoLink").push_int(2).push_str("Object").op(avm1_op::kGetVariable).push_str("registerClass").op(avm1_op::kCallMethod).op(avm1_op::kPop);
	a.op(avm1_op::kEnd);
	return a.b;
}

Bytes clip_place(bool p_v3, const std::vector<std::tuple<uint32_t, uint8_t, Bytes>> &p_records, uint8_t p_filter = 2) {
	Writer w;
	w.u8(0x02 | 0x20 | 0x80);
	if (p_v3) {
		w.u8(0x01);
	}
	w.u16(3);
	w.u16(5);
	w.str("Ammo_01");
	if (p_v3) {
		w.u8(2);
		w.u8(0);
		w.raw(Bytes(23, 0));
		w.u8(p_filter);
		w.raw(Bytes(15, 0));
	}
	w.u16(0);
	uint32_t all = 0;
	for (const auto &[events, key, body] : p_records) {
		all |= events;
	}
	w.u32(all);
	for (const auto &[events, key, body] : p_records) {
		w.u32(events);
		const bool has_key = (events & avm1_clip_event::kKeyPress) != 0;
		w.u32(uint32_t(body.size() + (has_key ? 1 : 0)));
		if (has_key) {
			w.u8(key);
		}
		w.raw(body);
	}
	w.u32(0);
	Bytes t;
	tag(t, p_v3 ? 70 : 26, w.b);
	return t;
}

Bytes action_tag(uint16_t p_code, const Bytes &p_body, std::optional<uint16_t> p_sprite = std::nullopt) {
	Writer w;
	if (p_sprite) {
		w.u16(*p_sprite);
	}
	w.raw(p_body);
	Bytes t;
	tag(t, p_code, w.b);
	return t;
}

const Avm1MovieCensus &movie_row(const Avm1Census &p_census, const char *p_name) {
	const Avm1MovieCensus *m = p_census.find(p_name);
	REQUIRE_MESSAGE(m != nullptr, p_name);
	return *m;
}

}

TEST_SUITE("ui-avm1-unit") {
	TEST_CASE("avm1: every action record of the format, each with its length") {
		const uint8_t plain[] = { 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x17, 0x18, 0x1C,
			0x1D, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x3A, 0x3B,
			0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F, 0x50, 0x51, 0x52, 0x53,
			0x54, 0x55, 0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69 };
		Bytes all(std::begin(plain), std::end(plain));
		all.push_back(0);
		const Avm1Code c = decode_ok(all);
		REQUIRE(c.actions.size() == all.size());
		for (size_t i = 0; i < all.size(); ++i) {
			CHECK(c.actions[i].op == all[i]);
			CHECK(c.actions[i].offset == i);
			CHECK(c.actions[i].size() == 1);
			CHECK(avm1_op_name(all[i]) != nullptr);
		}
		uint32_t named = 0;
		for (unsigned op = 0; op < 256; ++op) {
			named += avm1_op_name(uint8_t(op)) != nullptr ? 1 : 0;
		}
		CHECK(named == all.size() + 18);

		Asm a;
		a.rec(avm1_op::kGotoFrame, { 5, 1 });
		Writer url;
		url.str("http://a");
		url.str("_blank");
		a.rec(avm1_op::kGetUrl, url.b);
		a.rec(avm1_op::kWaitForFrame, { 3, 0, 9 });
		a.rec(avm1_op::kSetTarget, { 't', 0 });
		a.rec(avm1_op::kGoToLabel, { 'o', 'n', 0 });
		a.rec(avm1_op::kWaitForFrame2, { 2 });
		a.rec(avm1_op::kGetUrl2, { 0x41 });
		a.rec(avm1_op::kGotoFrame2, { 0x03, 7, 0 });
		a.rec(avm1_op::kGotoFrame2, { 0x00 });
		a.store(2);
		a.rec(avm1_op::kCall, {});
		a.op(avm1_op::kEnd);
		const Avm1Code r = decode_ok(a.b);
		REQUIRE(r.actions.size() == 12);
		CHECK(r.actions[0].frame == 261);
		CHECK(r.actions[0].size() == 5);
		CHECK(r.actions[1].strings == std::vector<std::string>{ "http://a", "_blank" });
		CHECK(r.actions[2].frame == 3);
		CHECK(r.actions[2].skip_count == 9);
		CHECK(r.actions[3].strings[0] == "t");
		CHECK(r.actions[4].strings[0] == "on");
		CHECK(r.actions[5].skip_count == 2);
		CHECK(r.actions[6].mode == 0x41);
		CHECK(r.actions[7].mode == 0x03);
		CHECK(r.actions[7].scene_bias == 7);
		CHECK(r.actions[8].size() == 4);
		CHECK(r.actions[9].reg == 2);
		CHECK(r.actions[10].size() == 3);
		CHECK(r.actions[11].offset == a.b.size() - 1);
		CHECK(r.base == 100);

		CHECK(contains(error_of({ 0x01 }), "unknown action 0x01 at 100"));
		CHECK(contains(error_of({ 0x07, 0x80, 0, 0 }), "unknown action 0x80 at 101"));
		CHECK(contains(error_of({ 0x9E, 1, 0, 0 }), "call carries 1 payload bytes"));
		CHECK(contains(error_of({ 0x81, 3, 0, 1, 2, 3 }), "gotoFrame payload of 3 bytes has 1 bytes after its fields"));
		CHECK(contains(error_of({ 0x81, 1, 0, 1 }), "gotoFrame payload of 1 bytes is shorter than its fields"));
		CHECK(contains(error_of({ 0x9F, 1, 0, 0x02 }), "gotoFrame2 payload of 1 bytes is shorter"));
		CHECK(contains(error_of({ 0x8C, 2, 0, 'a', 'b' }), "goToLabel payload of 2 bytes is shorter"));
		CHECK(contains(error_of({ 0x96, 5, 0, 0 }), "push length 5 past the end of the body (1 bytes left) at 100"));
		CHECK(contains(error_of({ 0x00, 0x96, 0 }), "push record header past the end of the body at 101"));
		CHECK(decode_ok({}).actions.empty());
	}

	TEST_CASE("avm1: push values of every type, resolved against the constant pool") {
		Asm a;
		a.constants({ "a", "bc" });
		Writer p;
		p.u8(0);
		p.str("s");
		p.u8(1);
		float f = 1.5f;
		uint32_t fb = 0;
		std::memcpy(&fb, &f, 4);
		p.u32(fb);
		p.u8(2);
		p.u8(3);
		p.u8(4);
		p.u8(3);
		p.u8(5);
		p.u8(1);
		p.u8(6);
		double d = 3.25;
		uint64_t db = 0;
		std::memcpy(&db, &d, 8);
		p.u32(uint32_t(db >> 32));
		p.u32(uint32_t(db));
		p.u8(7);
		p.u32(uint32_t(-7));
		p.u8(8);
		p.u8(1);
		p.u8(9);
		p.u16(0);
		p.u8(8);
		p.u8(9);
		a.rec(avm1_op::kPush, p.b);
		a.op(avm1_op::kEnd);
		const Avm1Code c = decode_ok(a.b);
		REQUIRE(c.actions.size() == 3);
		CHECK(c.actions[0].strings == std::vector<std::string>{ "a", "bc" });
		const std::vector<Avm1Value> &v = c.actions[1].values;
		REQUIRE(v.size() == 11);
		CHECK(v[0].type == Avm1PushType::String);
		CHECK(v[0].text == "s");
		CHECK(v[1].type == Avm1PushType::Float);
		CHECK(v[1].number == 1.5);
		CHECK(v[2].type == Avm1PushType::Null);
		CHECK(v[3].type == Avm1PushType::Undefined);
		CHECK(v[4].type == Avm1PushType::Register);
		CHECK(v[4].index == 3);
		CHECK(v[5].type == Avm1PushType::Boolean);
		CHECK(v[5].index == 1);
		CHECK(v[6].type == Avm1PushType::Double);
		CHECK(v[6].number == 3.25);
		CHECK(v[7].type == Avm1PushType::Integer);
		CHECK(v[7].number == -7.0);
		CHECK(v[8].type == Avm1PushType::Constant8);
		CHECK(v[8].text == "bc");
		CHECK(v[8].is_string());
		CHECK(v[9].type == Avm1PushType::Constant16);
		CHECK(v[9].text == "a");
		CHECK(!v[10].resolved);
		CHECK(!v[10].is_string());
		CHECK(v[7].is_number());
		CHECK(c.unresolved_constants == 1);
		CHECK(c.registers_out_of_range == 0);

		CHECK(contains(error_of({ 0x96, 1, 0, 10 }), "push value type 10 at 100"));
		CHECK(contains(error_of({ 0x96, 2, 0, 0, 'x' }), "push payload of 2 bytes is shorter"));
		CHECK(contains(error_of({ 0x96, 3, 0, 6, 0, 0 }), "push payload of 3 bytes is shorter"));
		CHECK(contains(error_of({ 0x88, 3, 0, 2, 0, 0 }), "constantPool payload of 3 bytes is shorter"));
		CHECK(contains(error_of({ 0x88, 4, 0, 1, 0, 0, 0 }), "constantPool payload of 4 bytes has 1 bytes after"));
	}

	TEST_CASE("avm1: DefineFunction and DefineFunction2 with registers, flags and nested scopes") {
		const Bytes body = function_rich_body();
		const Avm1Code c = decode_ok(body);
		REQUIRE(c.scopes.size() == 4);
		const Avm1Action *f = c.function_action(1);
		REQUIRE(f != nullptr);
		CHECK(f->op == avm1_op::kDefineFunction2);
		CHECK(f->strings[0] == "f");
		REQUIRE(f->params.size() == 2);
		CHECK(f->params[0].reg == 1);
		CHECK(f->params[0].name == "x");
		CHECK(f->params[1].reg == 0);
		CHECK(f->params[1].name == "y");
		CHECK(f->register_count == 3);
		CHECK(f->function_flags == (avm1_function_flag::kPreloadThis | avm1_function_flag::kSuppressArguments | avm1_function_flag::kPreloadGlobal));
		CHECK(f->code_size == 10);
		CHECK(c.scopes[1].bytes() == 10);
		CHECK(c.scopes[1].parent == 0);
		CHECK(c.scopes[1].depth == 1);
		const Avm1Action *g = c.function_action(2);
		REQUIRE(g != nullptr);
		CHECK(g->op == avm1_op::kDefineFunction);
		CHECK(g->params.size() == 2);
		CHECK(g->params[1].name == "z");
		CHECK(c.scopes[3].parent == 2);
		CHECK(c.scopes[3].depth == 2);
		CHECK(c.function_action(0) == nullptr);
		uint32_t in_inner = 0;
		for (const Avm1Action &a : c.actions) {
			in_inner += a.scope == 3 ? 1 : 0;
		}
		CHECK(in_inner == 3);
		CHECK(c.registers_out_of_range == 0);

		Asm bad;
		bad.function2("f", {}, 2, 0, Asm().push_reg(2).op(avm1_op::kReturn).b).store(4).op(avm1_op::kEnd);
		CHECK(decode_ok(bad.b).registers_out_of_range == 2);
		Asm param;
		param.function2("f", { { 3, "a" } }, 3, 0, {});
		CHECK(decode_ok(param.b).registers_out_of_range == 1);

		Asm past;
		past.function2("f", {}, 1, 0, Asm().op(avm1_op::kStop).b);
		Bytes cut = past.b;
		cut.pop_back();
		CHECK(contains(error_of(cut), "defineFunction2 block of 1 bytes runs past its enclosing block"));
		Asm reserved;
		reserved.function2("f", {}, 1, 0x0200, {});
		CHECK(contains(error_of(reserved.b), "defineFunction2 reserved flag bits 0x02"));
		Asm nested;
		nested.function1("outer", {}, Asm().function1("inner", {}, Asm().op(avm1_op::kStop).op(avm1_op::kStop).b).b);
		nested.b[nested.b.size() - 4] = 3;
		CHECK(contains(error_of(nested.b), "defineFunction block of 3 bytes runs past its enclosing block (ends at 128) at 113"));
	}

	TEST_CASE("avm1: jumps, If, With and Try with their targets inside the body") {
		const Avm1Code c = decode_ok(function_rich_body());
		uint32_t branches = 0;
		for (const Avm1Action &a : c.actions) {
			if (a.op == avm1_op::kJump || a.op == avm1_op::kIf) {
				++branches;
				CHECK(a.target != kAvm1NoTarget);
				CHECK((a.target == c.bytes || c.find(a.target) != kAvm1NoTarget));
			}
			if (a.op == avm1_op::kWith) {
				CHECK(a.block_size == 2);
			}
			if (a.op == avm1_op::kTry) {
				CHECK(a.try_size == 1);
				CHECK(a.catch_size == 1);
				CHECK(a.finally_size == 1);
				CHECK(a.strings[0] == "e");
			}
		}
		CHECK(branches == 2);

		Asm to_end;
		to_end.function2("f", {}, 1, 0, Asm().rec(avm1_op::kJump, { 0, 0 }).b).branch(avm1_op::kJump, "end").label("end");
		const Avm1Code e = decode_ok(to_end.done());
		CHECK(e.actions[1].target == e.scopes[1].end);
		CHECK(e.actions[2].target == e.bytes);

		Asm mid;
		mid.branch(avm1_op::kJump, "x").push_int(1).op(avm1_op::kEnd);
		Bytes mid_bytes = mid.b;
		mid_bytes[1 + 2] = 2;
		CHECK(contains(error_of(mid_bytes), "jump offset 2 targets 107, which is not an action record of its block at 100"));
		Bytes before = { 0x99, 2, 0, 0xF0, 0xFF };
		CHECK(contains(error_of(before), "jump offset -16 targets 89, outside its block [100, 105] at 100"));
		Asm out_of_fn;
		out_of_fn.function2("f", {}, 1, 0, Asm().rec(avm1_op::kIf, { 1, 0 }).b).op(avm1_op::kStop);
		CHECK(contains(error_of(out_of_fn.b), "if offset 1 targets"));
		CHECK(contains(error_of(out_of_fn.b), "outside its block"));
		Asm into_fn;
		into_fn.rec(avm1_op::kJump, { 12, 0 }).function2("f", {}, 1, 0, Asm().op(avm1_op::kStop).op(avm1_op::kStop).b);
		CHECK(contains(error_of(into_fn.b), "which is not an action record of its block"));

		CHECK(contains(error_of({ 0x94, 2, 0, 2, 0, 0x96, 1, 0, 2, 0 }), "an action record crosses the end of a block at 107 at 109"));
		CHECK(contains(error_of({ 0x94, 2, 0, 9, 0, 0x07 }), "with block of 9 bytes runs past its enclosing block (ends at 106)"));
		Writer t;
		t.u8(avm1_try_flag::kCatchInRegister | avm1_try_flag::kCatchBlock);
		t.u16(1);
		t.u16(5);
		t.u16(0);
		t.u8(3);
		Asm reg_try;
		reg_try.rec(avm1_op::kTry, t.b).op(avm1_op::kStop).push_reg(0);
		const Avm1Code rt = decode_ok(reg_try.b);
		CHECK(rt.actions[0].reg == 3);
		CHECK(rt.actions[0].strings.empty());
		Asm split_try;
		Writer t2;
		t2.u8(avm1_try_flag::kCatchBlock);
		t2.u16(2);
		t2.u16(3);
		t2.u16(0);
		t2.str("e");
		split_try.rec(avm1_op::kTry, t2.b).push_reg(0).op(avm1_op::kStop);
		CHECK(contains(error_of(split_try.b), "try block boundary is not on an action record"));
		Writer t3;
		t3.u8(0x08);
		t3.u16(0);
		t3.u16(0);
		t3.u16(0);
		t3.str("e");
		CHECK(contains(error_of(Asm().rec(avm1_op::kTry, t3.b).b), "try reserved flag bits 0x08"));
	}

	TEST_CASE("avm1: classes from the _global package assignments, prototype members, extends and Object.registerClass") {
		const Bytes body = class_body();
		const Bytes reg = register_body();
		Bytes tags;
		const Bytes root_action = action_tag(swf_tag::kDoAction, { avm1_op::kStop, avm1_op::kEnd });
		tags.insert(tags.end(), root_action.begin(), root_action.end());
		Bytes inner = show_frame();
		const Bytes sprite_action = action_tag(swf_tag::kDoAction, { avm1_op::kPlay, avm1_op::kEnd });
		inner.insert(inner.end(), sprite_action.begin(), sprite_action.end());
		const Bytes sf = show_frame();
		const Bytes et = end_tag();
		inner.insert(inner.end(), sf.begin(), sf.end());
		inner.insert(inner.end(), et.begin(), et.end());
		const Bytes sprite = define_sprite(5, 2, inner);
		tags.insert(tags.end(), sprite.begin(), sprite.end());
		const Bytes exports = export_assets({ { 5, "__Packages.Widget.Hud.Ammo" } });
		tags.insert(tags.end(), exports.begin(), exports.end());
		const Bytes init = action_tag(swf_tag::kDoInitAction, body, uint16_t(5));
		tags.insert(tags.end(), init.begin(), init.end());
		const Bytes init2 = action_tag(swf_tag::kDoInitAction, reg, uint16_t(5));
		tags.insert(tags.end(), init2.begin(), init2.end());
		const Bytes construct = Asm().push_str("m_param").push_int(3).op(avm1_op::kSetVariable).op(avm1_op::kEnd).b;
		const Bytes key = Asm().op(avm1_op::kStop).op(avm1_op::kEnd).b;
		const Bytes place = clip_place(false, { { avm1_clip_event::kConstruct, 0, construct }, { avm1_clip_event::kKeyPress | avm1_clip_event::kLoad, 13, key } });
		tags.insert(tags.end(), place.begin(), place.end());
		const Bytes place3 = clip_place(true, { { avm1_clip_event::kEnterFrame, 0, key } });
		tags.insert(tags.end(), place3.begin(), place3.end());
		Asm package;
		package.push_str("_global").op(avm1_op::kGetVariable).push_str("fb").push_int(0).push_str("Object").op(avm1_op::kNewObject).op(avm1_op::kSetMember);
		package.push_str("fb").op(avm1_op::kGetVariable).push_str("Delegate").function2("", {}, 1, 0, {}).op(avm1_op::kSetMember);
		package.push_str("other").op(avm1_op::kGetVariable).push_str("Thing").function2("", {}, 1, 0, {}).op(avm1_op::kSetMember).op(avm1_op::kEnd);
		const Bytes init3 = action_tag(swf_tag::kDoInitAction, package.b, uint16_t(5));
		tags.insert(tags.end(), init3.begin(), init3.end());
		tags.insert(tags.end(), sf.begin(), sf.end());
		tags.insert(tags.end(), et.begin(), et.end());
		Result<SwfMovie> movie = SwfMovie::parse(ui1test::movie(tags));
		REQUIRE_MESSAGE(movie.ok(), (movie.ok() ? std::string() : movie.error().message));
		Result<Avm1Movie> avm1 = read_avm1(*movie);
		REQUIRE_MESSAGE(avm1.ok(), (avm1.ok() ? std::string() : avm1.error().message));
		const Avm1Movie &m = *avm1;

		REQUIRE(m.bodies.size() == 8);
		CHECK(m.bodies[0].kind == Avm1BodyKind::DoAction);
		CHECK(m.bodies[0].timeline == 0);
		CHECK(m.bodies[0].frame == 0);
		CHECK(m.bodies[0].length == 2);
		CHECK(m.bodies[1].kind == Avm1BodyKind::DoAction);
		CHECK(m.bodies[1].timeline == 5);
		CHECK(m.bodies[1].frame == 1);
		CHECK(m.bodies[2].kind == Avm1BodyKind::DoInitAction);
		CHECK(m.bodies[2].sprite == 5);
		CHECK(m.bodies[2].length == body.size());
		CHECK(m.bodies[2].tag_length == body.size() + 2);
		CHECK(m.bodies[2].offset == m.bodies[2].tag_offset + 2);
		CHECK(m.bodies[2].code.base == m.bodies[2].offset);
		CHECK(m.bodies[4].kind == Avm1BodyKind::ClipAction);
		CHECK(m.bodies[4].event_flags == avm1_clip_event::kConstruct);
		CHECK(m.bodies[4].instance_name == "Ammo_01");
		CHECK(m.bodies[4].depth == 3);
		CHECK(m.bodies[4].sprite == 5);
		CHECK(m.bodies[4].length == construct.size());
		CHECK(m.bodies[5].event_flags == (avm1_clip_event::kKeyPress | avm1_clip_event::kLoad));
		CHECK(m.bodies[5].key_code == 13);
		CHECK(m.bodies[5].length == key.size());
		CHECK(m.bodies[6].event_flags == avm1_clip_event::kEnterFrame);
		CHECK(avm1_clip_event_names(m.bodies[5].event_flags) == "load|keyPress");
		CHECK(avm1_clip_event_names(0x80000000u) == "0x80000000");
		CHECK(m.do_action_tags == 2);
		CHECK(m.do_action_bytes == 4);
		CHECK(m.do_init_action_tags == 3);
		CHECK(m.do_init_action_bytes == body.size() + reg.size() + package.b.size());
		CHECK(m.clip_action_tags == 2);
		CHECK(m.clip_action_bytes == construct.size() + 2 * key.size());

		CHECK(m.packages == std::vector<std::string>{ "Widget", "Widget.Hud", "fb" });
		REQUIRE(m.classes.size() == 2);
		CHECK(m.classes[1].name == "fb.Delegate");
		CHECK(m.classes[1].body == 7);
		const Avm1Class &k = m.classes[0];
		CHECK(k.name == "Widget.Hud.Ammo");
		CHECK(k.base == "Widget.Hud.Base");
		CHECK(k.body == 2);
		CHECK(k.constructor_bytes == 6);
		REQUIRE(k.methods.size() == 1);
		CHECK(k.methods[0].name == "setup");
		CHECK(k.methods[0].bytes == 15);
		REQUIRE(k.fields.size() == 1);
		CHECK(k.fields[0].name == "m_x");
		CHECK(k.fields[0].value == "5");
		REQUIRE(k.static_fields.size() == 1);
		CHECK(k.static_fields[0].name == "count");
		CHECK(k.properties == std::vector<std::string>{ "value" });
		CHECK(m.class_body_bytes(k) == body.size());
		CHECK(m.find_class("Ammo") == &k);
		CHECK(m.find_class("Widget.Hud.Ammo") == &k);
		CHECK(m.find_class("Base") == nullptr);
		REQUIRE(m.registrations.size() == 1);
		CHECK(m.registrations[0].linkage == "AmmoLink");
		CHECK(m.registrations[0].class_name == "Widget.Hud.Ammo");
		CHECK(m.registrations[0].body == 3);

		const std::string listing = format_avm1_code(m.bodies[2].code);
		CHECK(contains(listing, "constantPool 13: 0=\"_global\""));
		CHECK(contains(listing, "defineFunction2 \"\" (r1:p_a) registers 3 flags none body 15 bytes"));
		CHECK(contains(listing, "    push r:1\n"));
		CHECK(contains(listing, "if L"));
		const std::string header = format_avm1_body_header(*movie, m, 2);
		CHECK(contains(header, "DoInitAction for sprite 5 \"__Packages.Widget.Hud.Ammo\""));
		CHECK(contains(format_avm1_body_header(*movie, m, 5), "ClipAction load|keyPress on depth 3 \"Ammo_01\""));
		CHECK(contains(format_avm1_class(k), "class Widget.Hud.Ammo extends Widget.Hud.Base"));
		CHECK(contains(format_avm1_class(k), "method setup 15 bytes"));

		SwfMovie bad_filter = *movie;
		const Bytes place_bad = clip_place(true, { { avm1_clip_event::kEnterFrame, 0, key } }, 9);
		auto at = std::search(bad_filter.body.begin(), bad_filter.body.end(), place3.begin(), place3.end());
		REQUIRE(at != bad_filter.body.end());
		std::copy(place_bad.begin(), place_bad.end(), at);
		Result<Avm1Movie> ba = read_avm1(bad_filter);
		REQUIRE(!ba.ok());
		CHECK(contains(ba.error().message, "PlaceObject3 filter id 9"));
	}

	TEST_CASE("avm1: every truncation and 2,000 seeded mutants of the synthetic bodies fail cleanly or decode") {
		std::mt19937_64 rng(0xa5a1);
		for (const Bytes &body : { function_rich_body(), class_body(), register_body() }) {
			size_t decoded = 0;
			for (size_t n = 0; n <= body.size(); ++n) {
				Result<Avm1Code> c = decode_avm1(std::span<const uint8_t>(body).first(n));
				if (c) {
					++decoded;
					CHECK(c->record_bytes() == n);
				} else {
					CHECK(!c.error().message.empty());
				}
			}
			CHECK(decoded >= 2);
			const auto t0 = std::chrono::steady_clock::now();
			for (int i = 0; i < 2000; ++i) {
				Bytes b = body;
				mutate(b, rng);
				Result<Avm1Code> c = decode_avm1(b);
				if (c) {
					CHECK(c->record_bytes() == b.size());
					(void)format_avm1_code(*c);
				} else {
					CHECK(!c.error().message.empty());
				}
			}
			const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
			if (!kAsan) {
				CHECK(s < 1.0);
			}
		}
	}
}

TEST_SUITE("ui-avm1") {
	TEST_CASE("avm1: every action body of the 434 retail movies, the class census and install unchanged") {
		bf3::test::Mounted fx;
		if (!bf3::test::open_or_skip(fx)) {
			return;
		}
		Result<SwfLibrary> lib = SwfLibrary::open(fx.install);
		REQUIRE_MESSAGE(lib.ok(), (lib.ok() ? std::string() : lib.error().message));
		Result<Avm1Census> census = run_avm1_census(*lib);
		REQUIRE(census.ok());
		const Avm1Census &c = *census;
		MESSAGE(format_avm1_census(c));
		CHECK(c.movies.size() == 434);
		CHECK(c.errors == 0);
		CHECK(c.records_mismatched == 0);
		CHECK(c.unresolved_constants == 0);
		CHECK(c.registers_out_of_range == 0);
		CHECK(c.do_action_tags == 2456);
		CHECK(c.do_init_action_tags == 1538);
		CHECK(c.tag_action_bytes() == 1638265);
		CHECK(c.clip_action_tags == 1244);
		CHECK(c.clip_action_bytes == 97086);
		CHECK(c.actions == 303074);
		CHECK(c.functions == 6174);
		CHECK(c.classes == 872);
		CHECK(c.package_bodies == 872);
		CHECK(c.package_bodies_matched == 872);
		CHECK(c.registrations == 666);
		CHECK(c.classes + c.registrations == c.do_init_action_tags);

		const Avm1MovieCensus &asl = movie_row(c, "ui/assets/actionscriptlibrary");
		CHECK(asl.do_init_action_tags == 237);
		CHECK(asl.do_init_action_bytes == 643485);
		CHECK(asl.classes == 236);
		CHECK(asl.class_bodies == 236);
		CHECK(asl.class_bytes == 643396);
		CHECK(asl.registrations == 1);
		const Avm1MovieCensus &masl = movie_row(c, "ui/assets/menuactionscriptlibrary");
		CHECK(masl.do_init_action_tags == 111);
		CHECK(masl.do_init_action_bytes == 281447);
		CHECK(masl.classes == 110);
		CHECK(masl.class_bodies == 110);
		CHECK(masl.class_bytes == 281358);
		CHECK(masl.registrations == 1);

		Result<std::shared_ptr<const SwfMovie>> movie = lib->load("ui/assets/actionscriptlibrary");
		REQUIRE(movie.ok());
		Result<Avm1Movie> avm1 = read_avm1(**movie);
		REQUIRE(avm1.ok());
		const std::pair<const char *, uint32_t> study[] = {
			{ "Widget.Hud.Ammo", 5085 },
			{ "Widget.Hud.Compass", 3776 },
			{ "Widget.Message.HudMessage", 3709 },
			{ "Widget.Hud.Minimap", 3104 },
			{ "Widget.Hud.SquadList", 2178 },
			{ "Shared.HudBackground", 1777 },
			{ "Widget.Hud.HudBackgroundWidget", 1774 },
			{ "Widget.Hud.Health", 1343 },
			{ "Widget.Message.MessageTypes.KillLogMessage", 2329 },
			{ "Widget.Message.MessageTypes.LocalKillLogMessage", 2536 },
			{ "Widget.Message.HudAlertMessage", 7928 },
			{ "Widget.TicketCounter.TicketCounter", 7685 },
			{ "Widget.ScreenSpecific.KillCam", 6250 },
			{ "Widget.Bar.KitBar", 8656 },
			{ "Widget.List.Rows.ScoreBoardRow", 5051 },
			{ "Widget.List.ScoreboardHeader", 5701 },
			{ "Widget.List.Rows.TopPlayersRow", 3658 },
			{ "Widget.TicketCounter.TicketCounterSpawnScreen", 4835 },
			{ "Widget.SpawnTimer.SpawnTimer", 3779 },
			{ "Widget.Crosshair.Crosshair", 2305 },
			{ "Widget.Hud.MatchPreroundInformation", 1383 },
			{ "Widget.List.List", 24421 },
			{ "Widget.List.ListLogic", 12144 },
		};
		uint32_t total = 0;
		uint32_t entry_points = 0;
		for (const auto &[name, bytes] : study) {
			const std::string class_name = name;
			CAPTURE(class_name);
			const Avm1Class *k = avm1->find_class(name);
			REQUIRE(k != nullptr);
			CHECK(k->name == class_name);
			CHECK(avm1->class_body_bytes(*k) == bytes);
			CHECK(avm1->bodies[k->body].kind == Avm1BodyKind::DoInitAction);
			CHECK((*movie)->export_name(avm1->bodies[k->body].sprite) == "__Packages." + class_name);
			total += avm1->class_body_bytes(*k);
			for (const Avm1Member &m : k->methods) {
				entry_points += m.name == "setupRefreshData" || (m.name.starts_with("update") && m.name.ends_with("Data")) ? 1 : 0;
			}
		}
		CHECK(total == 121407);
		CHECK(entry_points == 160);

		for (const Avm1Class &k : avm1->classes) {
			CHECK((*movie)->export_name(avm1->bodies[k.body].sprite) == "__Packages." + k.name);
		}
		REQUIRE(avm1->registrations.size() == 1);
		const Avm1Registration &root = avm1->registrations[0];
		CHECK(root.linkage == "ActionscriptLibrary");
		CHECK(root.class_name == "Main.ActionscriptLibrary");
		CHECK(avm1->find_class(root.class_name) != nullptr);
		CHECK(avm1->bodies[root.body].length == 89);
		CHECK((*movie)->export_name(avm1->bodies[root.body].sprite) == "ActionscriptLibrary");

		const SnapshotDiff diff = diff_snapshots(fx.before, fx.reader.snapshot());
		MESSAGE("install.unchanged: " << fx.before.files.size() << " index files, " << (diff.clean() ? "clean" : diff.summary()));
		CHECK_MESSAGE(diff.clean(), diff.summary());
	}
}

TEST_SUITE("ui-swf-fuzz") {
	TEST_CASE("avm1: seeded mutants of the retail action bodies fail cleanly or decode") {
		bf3::test::Mounted fx;
		if (!bf3::test::open_or_skip(fx)) {
			return;
		}
		Result<SwfLibrary> lib = SwfLibrary::open(fx.install);
		REQUIRE(lib.ok());
		std::mt19937_64 rng(0xa5a2);
		const size_t iters = fuzz_iters(300);
		for (const char *name : { "ui/assets/actionscriptlibrary", "ui/assets/hudmpscreen", "ui/assets/ammo" }) {
			const std::string movie_name = name;
			CAPTURE(movie_name);
			Result<std::shared_ptr<const SwfMovie>> original = lib->load(name);
			REQUIRE(original.ok());
			Result<Avm1Movie> avm1 = read_avm1(**original);
			REQUIRE(avm1.ok());
			REQUIRE(!avm1->bodies.empty());
			const Bytes &body = (*original)->body;
			size_t decoded = 0;
			size_t refused = 0;
			const auto t0 = std::chrono::steady_clock::now();
			for (size_t i = 0; i < iters; ++i) {
				if (i % 2 == 0) {
					const Avm1Body &src = avm1->bodies[size_t(rng() % avm1->bodies.size())];
					Bytes b(body.begin() + (src.offset - 8), body.begin() + (src.offset - 8 + src.length));
					mutate(b, rng);
					Result<Avm1Code> c = decode_avm1(b, src.offset);
					if (c) {
						++decoded;
						CHECK(c->record_bytes() == b.size());
					} else {
						++refused;
						CHECK(!c.error().message.empty());
					}
					continue;
				}
				Bytes mb = body;
				mutate(mb, rng);
				Result<SwfMovie> m = SwfMovie::parse(gfx_from_body(mb, (*original)->version));
				if (!m) {
					++refused;
					continue;
				}
				Result<Avm1Movie> a = read_avm1(*m);
				if (a) {
					++decoded;
				} else {
					++refused;
					CHECK(!a.error().message.empty());
				}
			}
			const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
			MESSAGE(movie_name << ": " << iters << " mutants, " << decoded << " decoded, " << refused << " refused, " << s << " s");
			CHECK(decoded + refused == iters);
		}
		const SnapshotDiff diff = diff_snapshots(fx.before, fx.reader.snapshot());
		CHECK_MESSAGE(diff.clean(), diff.summary());
	}
}
