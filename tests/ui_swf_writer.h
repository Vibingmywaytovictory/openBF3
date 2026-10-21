#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ui1test {

using Bytes = std::vector<uint8_t>;

struct BitWriter {
	Bytes b;
	int bit = 0;
	void ub(uint32_t p_v, int p_n) {
		for (int i = p_n - 1; i >= 0; --i) {
			if (bit == 0) {
				b.push_back(0);
			}
			if ((p_v >> i) & 1u) {
				b.back() |= uint8_t(0x80u >> bit);
			}
			bit = (bit + 1) % 8;
		}
	}
	void sb(int32_t p_v, int p_n) { ub(uint32_t(p_v) & (p_n >= 32 ? 0xFFFFFFFFu : ((1u << p_n) - 1)), p_n); }
	void align() { bit = 0; }
};

inline int sbits(int32_t p_v) {
	int n = 1;
	while (n < 32 && !(p_v >= -(int64_t(1) << (n - 1)) && p_v < (int64_t(1) << (n - 1)))) {
		++n;
	}
	return n;
}
inline int sbits_max(std::initializer_list<int32_t> p_values) {
	int n = 1;
	for (const int32_t v : p_values) {
		n = std::max(n, sbits(v));
	}
	return n;
}

struct Writer {
	Bytes b;
	void u8(uint32_t v) { b.push_back(uint8_t(v)); }
	void u16(uint32_t v) {
		u8(v & 0xff);
		u8((v >> 8) & 0xff);
	}
	void s16(int32_t v) { u16(uint32_t(uint16_t(int16_t(v)))); }
	void u32(uint32_t v) {
		u16(v & 0xffff);
		u16(v >> 16);
	}
	void str(std::string_view s) {
		b.insert(b.end(), s.begin(), s.end());
		u8(0);
	}
	void pstr8(std::string_view s) {
		u8(uint32_t(s.size()));
		b.insert(b.end(), s.begin(), s.end());
	}
	void raw(const Bytes &p) { b.insert(b.end(), p.begin(), p.end()); }
	void bits(const BitWriter &w) { raw(w.b); }
};

inline Bytes rect(float x0, float x1, float y0, float y1) {
	const int32_t v[4] = { int32_t(std::lround(x0 * 20)), int32_t(std::lround(x1 * 20)), int32_t(std::lround(y0 * 20)), int32_t(std::lround(y1 * 20)) };
	const int n = sbits_max({ v[0], v[1], v[2], v[3] });
	BitWriter w;
	w.ub(uint32_t(n), 5);
	for (const int32_t x : v) {
		w.sb(x, n);
	}
	return w.b;
}

struct Mat {
	float a = 1, b = 0, c = 0, d = 1, tx = 0, ty = 0;
};

inline Bytes matrix(const Mat &p_m) {
	BitWriter w;
	const bool scale = p_m.a != 1.0f || p_m.d != 1.0f;
	w.ub(scale ? 1 : 0, 1);
	if (scale) {
		const int32_t a = int32_t(std::lround(double(p_m.a) * 65536.0));
		const int32_t d = int32_t(std::lround(double(p_m.d) * 65536.0));
		const int n = sbits_max({ a, d });
		w.ub(uint32_t(n), 5);
		w.sb(a, n);
		w.sb(d, n);
	}
	const bool rot = p_m.b != 0.0f || p_m.c != 0.0f;
	w.ub(rot ? 1 : 0, 1);
	if (rot) {
		const int32_t b = int32_t(std::lround(double(p_m.b) * 65536.0));
		const int32_t c = int32_t(std::lround(double(p_m.c) * 65536.0));
		const int n = sbits_max({ b, c });
		w.ub(uint32_t(n), 5);
		w.sb(b, n);
		w.sb(c, n);
	}
	const int32_t tx = int32_t(std::lround(double(p_m.tx) * 20.0));
	const int32_t ty = int32_t(std::lround(double(p_m.ty) * 20.0));
	const int n = (tx == 0 && ty == 0) ? 0 : sbits_max({ tx, ty });
	w.ub(uint32_t(n), 5);
	if (n > 0) {
		w.sb(tx, n);
		w.sb(ty, n);
	}
	return w.b;
}

inline Bytes cxform_alpha(const int16_t p_mul[4], const int16_t p_add[4]) {
	BitWriter w;
	w.ub(1, 1);
	w.ub(1, 1);
	const int n = sbits_max({ p_mul[0], p_mul[1], p_mul[2], p_mul[3], p_add[0], p_add[1], p_add[2], p_add[3] });
	w.ub(uint32_t(n), 4);
	for (int i = 0; i < 4; ++i) {
		w.sb(p_mul[i], n);
	}
	for (int i = 0; i < 4; ++i) {
		w.sb(p_add[i], n);
	}
	return w.b;
}

inline Bytes cat(std::initializer_list<Bytes> p_parts) {
	Bytes out;
	for (const Bytes &p : p_parts) {
		out.insert(out.end(), p.begin(), p.end());
	}
	return out;
}

inline void tag(Bytes &r_out, uint16_t p_code, const Bytes &p_body, bool p_long = false) {
	if (p_long || p_body.size() >= 0x3f) {
		const uint16_t cl = uint16_t((p_code << 6) | 0x3f);
		r_out.push_back(uint8_t(cl));
		r_out.push_back(uint8_t(cl >> 8));
		const uint32_t n = uint32_t(p_body.size());
		for (int i = 0; i < 4; ++i) {
			r_out.push_back(uint8_t(n >> (8 * i)));
		}
	} else {
		const uint16_t cl = uint16_t((p_code << 6) | p_body.size());
		r_out.push_back(uint8_t(cl));
		r_out.push_back(uint8_t(cl >> 8));
	}
	r_out.insert(r_out.end(), p_body.begin(), p_body.end());
}

inline Bytes show_frame() {
	Bytes t;
	tag(t, 1, {});
	return t;
}
inline Bytes end_tag() {
	Bytes t;
	tag(t, 0, {});
	return t;
}

struct Place {
	uint16_t depth = 1;
	std::optional<uint16_t> character;
	std::optional<Mat> matrix;
	bool color = false;
	std::optional<uint16_t> ratio;
	std::optional<std::string> name;
	std::optional<uint16_t> clip_depth;
	bool move = false;

	std::optional<std::string> class_name;
	bool filters = false;
	std::optional<uint8_t> blend;
	bool clip_actions = false;
};

inline Bytes place_object2(const Place &p, bool p_v3 = false) {
	Writer w;
	uint8_t f = 0;
	f |= p.move ? 0x01 : 0;
	f |= p.character ? 0x02 : 0;
	f |= p.matrix ? 0x04 : 0;
	f |= p.color ? 0x08 : 0;
	f |= p.ratio ? 0x10 : 0;
	f |= p.name ? 0x20 : 0;
	f |= p.clip_depth ? 0x40 : 0;
	f |= p.clip_actions ? 0x80 : 0;
	w.u8(f);
	if (p_v3) {
		uint8_t f2 = 0;
		f2 |= p.filters ? 0x01 : 0;
		f2 |= p.blend ? 0x02 : 0;
		f2 |= p.class_name ? 0x08 : 0;
		w.u8(f2);
	}
	w.u16(p.depth);
	if (p_v3 && p.class_name) {
		w.str(*p.class_name);
	}
	if (p.character) {
		w.u16(*p.character);
	}
	if (p.matrix) {
		w.raw(matrix(*p.matrix));
	}
	if (p.color) {
		const int16_t mul[4] = { 128, 256, 256, 200 };
		const int16_t add[4] = { 10, -5, 0, 0 };
		w.raw(cxform_alpha(mul, add));
	}
	if (p.ratio) {
		w.u16(*p.ratio);
	}
	if (p.name) {
		w.str(*p.name);
	}
	if (p.clip_depth) {
		w.u16(*p.clip_depth);
	}
	if (p_v3 && p.filters) {
		w.u8(1);
		w.u8(1);
		w.u32(0x10000);
		w.u32(0x10000);
		w.u8(1 << 3);
	}
	if (p_v3 && p.blend) {
		w.u8(*p.blend);
	}
	if (p.clip_actions) {
		w.u16(0);
		w.u32(1);
		w.u32(0);
	}
	Bytes t;
	tag(t, p_v3 ? 70 : 26, w.b);
	return t;
}

inline Bytes place_object1(uint16_t p_char, uint16_t p_depth, const Mat &p_m) {
	Writer w;
	w.u16(p_char);
	w.u16(p_depth);
	w.raw(matrix(p_m));
	Bytes t;
	tag(t, 4, w.b);
	return t;
}
inline Bytes remove_object1(uint16_t p_char, uint16_t p_depth) {
	Writer w;
	w.u16(p_char);
	w.u16(p_depth);
	Bytes t;
	tag(t, 5, w.b);
	return t;
}
inline Bytes remove_object2(uint16_t p_depth) {
	Writer w;
	w.u16(p_depth);
	Bytes t;
	tag(t, 28, w.b);
	return t;
}
inline Bytes frame_label(std::string_view p_name) {
	Writer w;
	w.str(p_name);
	Bytes t;
	tag(t, 43, w.b);
	return t;
}
inline Bytes define_sprite(uint16_t p_id, uint16_t p_frames, const Bytes &p_inner) {
	Writer w;
	w.u16(p_id);
	w.u16(p_frames);
	w.raw(p_inner);
	Bytes t;
	tag(t, 39, w.b, true);
	return t;
}
inline Bytes export_assets(const std::vector<std::pair<uint16_t, std::string>> &p_symbols) {
	Writer w;
	w.u16(uint32_t(p_symbols.size()));
	for (const auto &[id, name] : p_symbols) {
		w.u16(id);
		w.str(name);
	}
	Bytes t;
	tag(t, 56, w.b);
	return t;
}
inline Bytes import_assets(std::string_view p_url, const std::vector<std::pair<uint16_t, std::string>> &p_symbols, bool p_v2 = true) {
	Writer w;
	w.str(p_url);
	if (p_v2) {
		w.u8(1);
		w.u8(0);
	}
	w.u16(uint32_t(p_symbols.size()));
	for (const auto &[id, name] : p_symbols) {
		w.u16(id);
		w.str(name);
	}
	Bytes t;
	tag(t, p_v2 ? 71 : 57, w.b);
	return t;
}
inline Bytes exporter_info(std::string_view p_swf) {
	Writer w;
	w.u16(0x36f);
	w.u32(0);
	w.u16(13);
	w.pstr8("");
	w.pstr8(p_swf);
	w.u16(0);
	Bytes t;
	tag(t, 1000, w.b);
	return t;
}
inline Bytes external_image2(uint16_t p_id, uint16_t p_w, uint16_t p_h, std::string_view p_file) {
	Writer w;
	w.u16(p_id);
	w.u16(9);
	w.u16(13);
	w.u16(p_w);
	w.u16(p_h);
	w.pstr8("");
	w.pstr8(p_file);
	w.u8(0);
	Bytes t;
	tag(t, 1009, w.b);
	return t;
}
inline Bytes external_image1(uint16_t p_id, uint16_t p_w, uint16_t p_h, std::string_view p_file) {
	Writer w;
	w.u16(p_id);
	w.u16(13);
	w.u16(p_w);
	w.u16(p_h);
	w.pstr8("");
	w.pstr8(p_file);
	Bytes t;
	tag(t, 1001, w.b);
	return t;
}
inline Bytes sub_image(uint16_t p_id, uint16_t p_image, uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2) {
	Writer w;
	w.u16(p_id);
	w.u16(p_image);
	w.u16(x1);
	w.u16(y1);
	w.u16(x2);
	w.u16(y2);
	Bytes t;
	tag(t, 1008, w.b);
	return t;
}

struct BitmapFill {
	uint16_t bitmap = 0;
	Mat m{ 20, 0, 0, 20, 0, 0 };
	uint8_t type = 0x41;
};

inline Bytes define_shape(int p_version, uint16_t p_id, float p_w, float p_h, const std::vector<BitmapFill> &p_fills,
		std::optional<BitmapFill> p_new_style_fill = std::nullopt) {
	static const uint16_t codes[] = { 0, 2, 22, 32, 83 };
	Writer w;
	w.u16(p_id);
	w.raw(rect(0, p_w, 0, p_h));
	if (p_version == 4) {
		w.raw(rect(0, p_w, 0, p_h));
		w.u8(0);
	}
	auto fills = [&](const std::vector<BitmapFill> &p_list, bool p_solid) {
		w.u8(uint32_t(p_list.size() + (p_solid ? 1 : 0)));
		if (p_solid) {
			w.u8(0x00);
			w.u8(1);
			w.u8(2);
			w.u8(3);
			if (p_version >= 3) {
				w.u8(255);
			}
		}
		for (const BitmapFill &f : p_list) {
			w.u8(f.type);
			w.u16(f.bitmap);
			w.raw(matrix(f.m));
		}
	};
	auto lines = [&]() {
		w.u8(1);
		w.u16(20);
		if (p_version == 4) {
			w.u8(0);
			w.u8(0);
			w.u8(1);
			w.u8(2);
			w.u8(3);
			w.u8(255);
		} else {
			w.u8(1);
			w.u8(2);
			w.u8(3);
			if (p_version >= 3) {
				w.u8(255);
			}
		}
	};
	fills(p_fills, true);
	lines();
	const int fill_bits = 3;
	const int line_bits = 1;
	w.u8((fill_bits << 4) | line_bits);
	BitWriter r;

	r.ub(0, 1);
	r.ub(0x01 | 0x04, 5);
	r.ub(1, 5);
	r.sb(0, 1);
	r.sb(0, 1);
	r.ub(1, fill_bits);
	const int32_t wx = int32_t(std::lround(p_w * 20));
	const int32_t hy = int32_t(std::lround(p_h * 20));
	auto line = [&](int32_t dx, int32_t dy) {
		const int n = std::max(2, sbits_max({ dx, dy }));
		r.ub(1, 1);
		r.ub(1, 1);
		r.ub(uint32_t(n - 2), 4);
		r.ub(1, 1);
		r.sb(dx, n);
		r.sb(dy, n);
	};
	line(wx, 0);
	line(0, hy);
	if (p_new_style_fill && p_version >= 2) {

		r.ub(0, 1);
		r.ub(0x10, 5);
		w.bits(r);
		r = BitWriter{};
		fills({ *p_new_style_fill }, false);
		w.u8(0);
		w.u8((1 << 4) | 0);
		r.ub(0, 1);
		r.ub(0x04, 5);
		r.ub(1, 1);
	}
	line(-wx, 0);
	line(0, -hy);
	r.ub(0, 1);
	r.ub(0, 5);
	w.bits(r);
	Bytes t;
	tag(t, codes[p_version], w.b);
	return t;
}

struct EditText {
	uint16_t id = 1;
	std::optional<uint16_t> font_id;
	std::optional<std::string> font_class;
	float height = 20;
	bool color = true;
	std::optional<uint16_t> max_length;
	bool layout = true;
	uint8_t align = 1;
	std::string variable = "v";
	std::optional<std::string> text = "000";
	uint16_t extra_flags = 0x0001 | 0x0002;
};
inline Bytes define_edit_text(const EditText &p) {
	Writer w;
	w.u16(p.id);
	w.raw(rect(-1, 101, -2, 22));
	uint16_t flags = p.extra_flags;
	flags |= p.text ? 0x8000 : 0;
	flags |= p.color ? 0x0400 : 0;
	flags |= p.max_length ? 0x0200 : 0;
	flags |= p.font_id ? 0x0100 : 0;
	flags |= p.font_class ? 0x0080 : 0;
	flags |= p.layout ? 0x0020 : 0;
	w.u8(flags >> 8);
	w.u8(flags & 0xff);
	if (p.font_id) {
		w.u16(*p.font_id);
	}
	if (p.font_class) {
		w.str(*p.font_class);
	}
	if (p.font_id || p.font_class) {
		w.u16(uint32_t(std::lround(p.height * 20)));
	}
	if (p.color) {
		w.u8(0xff);
		w.u8(0x80);
		w.u8(0x10);
		w.u8(0xc0);
	}
	if (p.max_length) {
		w.u16(*p.max_length);
	}
	if (p.layout) {
		w.u8(p.align);
		w.u16(40);
		w.u16(60);
		w.u16(20);
		w.s16(-20);
	}
	w.str(p.variable);
	if (p.text) {
		w.str(*p.text);
	}
	Bytes t;
	tag(t, 37, w.b);
	return t;
}
inline Bytes scaling_grid(uint16_t p_id, float x0, float x1, float y0, float y1) {
	Writer w;
	w.u16(p_id);
	w.raw(rect(x0, x1, y0, y1));
	Bytes t;
	tag(t, 78, w.b);
	return t;
}
inline Bytes do_action(size_t p_bytes) {
	Bytes t;
	tag(t, 12, Bytes(p_bytes, 0));
	return t;
}
inline Bytes do_init_action(uint16_t p_sprite, size_t p_bytes) {
	Writer w;
	w.u16(p_sprite);
	w.raw(Bytes(p_bytes, 0));
	Bytes t;
	tag(t, 59, w.b);
	return t;
}
inline Bytes define_text(uint16_t p_id) {
	Writer w;
	w.u16(p_id);
	w.raw(rect(0, 10, 0, 10));
	w.raw(matrix({}));
	w.u8(1);
	w.u8(1);
	w.u8(0);
	Bytes t;
	tag(t, 11, w.b);
	return t;
}
inline Bytes align_zones(uint16_t p_font, int p_records) {
	Writer w;
	w.u16(p_font);
	w.u8(0x40);
	for (int i = 0; i < p_records; ++i) {
		w.u8(2);
		w.u32(0);
		w.u32(0);
		w.u8(3);
	}
	Bytes t;
	tag(t, 73, w.b);
	return t;
}
inline Bytes font_name(uint16_t p_font, std::string_view p_name, std::string_view p_copyright) {
	Writer w;
	w.u16(p_font);
	w.str(p_name);
	w.str(p_copyright);
	Bytes t;
	tag(t, 88, w.b);
	return t;
}

struct Seg {
	int32_t x, y;
	bool curve = false;
	int32_t cx = 0, cy = 0;
};
struct Contour {
	int32_t x0, y0;
	std::vector<Seg> segs;
};
using Glyph = std::vector<Contour>;

struct ShapeFill {
	uint8_t type = 0x00;
	uint8_t r = 0, g = 0, b = 0, a = 255;
	uint16_t bitmap = 0;
	Mat m{ 20, 0, 0, 20, 0, 0 };
	std::vector<std::pair<uint8_t, std::array<uint8_t, 4>>> stops;
	int16_t focal = 0;
};

struct ShapeLine {
	uint16_t width = 20;
	uint8_t r = 0, g = 0, b = 0, a = 255;
	uint8_t start_cap = 0, join = 0, end_cap = 0;
	uint16_t miter = 0;
	bool no_hscale = false, no_vscale = false, pixel_hinting = false, no_close = false;
	std::optional<ShapeFill> fill;
};

struct ShapePath {
	int32_t x0 = 0, y0 = 0;
	uint32_t fill0 = 0, fill1 = 0, line = 0;
	std::vector<Seg> segs;
};

inline int ubits(uint32_t p_v) {
	int n = 0;
	while (n < 32 && (uint64_t(1) << n) <= p_v) {
		++n;
	}
	return n;
}

inline void shape_fill_style(Writer &w, int p_version, const ShapeFill &f) {
	w.u8(f.type);
	if (f.type == 0x00) {
		w.u8(f.r);
		w.u8(f.g);
		w.u8(f.b);
		if (p_version >= 3) {
			w.u8(f.a);
		}
	} else if (f.type == 0x10 || f.type == 0x12 || f.type == 0x13) {
		w.raw(matrix(f.m));
		w.u8(uint32_t(f.stops.size()) & 0x0f);
		for (const auto &[ratio, c] : f.stops) {
			w.u8(ratio);
			w.u8(c[0]);
			w.u8(c[1]);
			w.u8(c[2]);
			if (p_version >= 3) {
				w.u8(c[3]);
			}
		}
		if (f.type == 0x13) {
			w.s16(f.focal);
		}
	} else {
		w.u16(f.bitmap);
		w.raw(matrix(f.m));
	}
}

inline Bytes define_shape_paths(int p_version, uint16_t p_id, float p_w, float p_h, const std::vector<ShapeFill> &p_fills,
		const std::vector<ShapeLine> &p_lines, const std::vector<ShapePath> &p_paths) {
	static const uint16_t codes[] = { 0, 2, 22, 32, 83 };
	Writer w;
	w.u16(p_id);
	w.raw(rect(0, p_w, 0, p_h));
	if (p_version == 4) {
		w.raw(rect(0, p_w, 0, p_h));
		w.u8(0);
	}
	w.u8(uint32_t(p_fills.size()));
	for (const ShapeFill &f : p_fills) {
		shape_fill_style(w, p_version, f);
	}
	w.u8(uint32_t(p_lines.size()));
	for (const ShapeLine &l : p_lines) {
		w.u16(l.width);
		if (p_version == 4) {
			BitWriter fb;
			fb.ub(l.start_cap, 2);
			fb.ub(l.join, 2);
			fb.ub(l.fill ? 1 : 0, 1);
			fb.ub(l.no_hscale ? 1 : 0, 1);
			fb.ub(l.no_vscale ? 1 : 0, 1);
			fb.ub(l.pixel_hinting ? 1 : 0, 1);
			fb.ub(0, 5);
			fb.ub(l.no_close ? 1 : 0, 1);
			fb.ub(l.end_cap, 2);
			w.bits(fb);
			if (l.join == 2) {
				w.u16(l.miter);
			}
			if (l.fill) {
				shape_fill_style(w, p_version, *l.fill);
			} else {
				w.u8(l.r);
				w.u8(l.g);
				w.u8(l.b);
				w.u8(l.a);
			}
		} else {
			w.u8(l.r);
			w.u8(l.g);
			w.u8(l.b);
			if (p_version >= 3) {
				w.u8(l.a);
			}
		}
	}
	const int fill_bits = std::max(1, ubits(uint32_t(p_fills.size())));
	const int line_bits = ubits(uint32_t(p_lines.size()));
	w.u8(uint32_t((fill_bits << 4) | line_bits));
	BitWriter r;
	for (const ShapePath &path : p_paths) {
		r.ub(0, 1);
		r.ub(0x01 | 0x02 | 0x04 | (line_bits > 0 ? 0x08 : 0), 5);
		const int n = sbits_max({ path.x0, path.y0 });
		r.ub(uint32_t(n), 5);
		r.sb(path.x0, n);
		r.sb(path.y0, n);
		r.ub(path.fill0, fill_bits);
		r.ub(path.fill1, fill_bits);
		if (line_bits > 0) {
			r.ub(path.line, line_bits);
		}
		int32_t x = path.x0;
		int32_t y = path.y0;
		for (const Seg &s : path.segs) {
			if (s.curve) {
				const int32_t cdx = s.cx - x, cdy = s.cy - y, adx = s.x - s.cx, ady = s.y - s.cy;
				const int nb = std::max(2, sbits_max({ cdx, cdy, adx, ady }));
				r.ub(1, 1);
				r.ub(0, 1);
				r.ub(uint32_t(nb - 2), 4);
				r.sb(cdx, nb);
				r.sb(cdy, nb);
				r.sb(adx, nb);
				r.sb(ady, nb);
			} else {
				const int32_t dx = s.x - x, dy = s.y - y;
				const int nb = std::max(2, sbits_max({ dx, dy }));
				r.ub(1, 1);
				r.ub(1, 1);
				r.ub(uint32_t(nb - 2), 4);
				if (dx != 0 && dy != 0) {
					r.ub(1, 1);
					r.sb(dx, nb);
					r.sb(dy, nb);
				} else if (dx == 0) {
					r.ub(0, 1);
					r.ub(1, 1);
					r.sb(dy, nb);
				} else {
					r.ub(0, 1);
					r.ub(0, 1);
					r.sb(dx, nb);
				}
			}
			x = s.x;
			y = s.y;
		}
	}
	r.ub(0, 1);
	r.ub(0, 5);
	w.bits(r);
	Bytes t;
	tag(t, codes[p_version], w.b, true);
	return t;
}


inline Bytes glyph_shape(const Glyph &p_glyph) {
	BitWriter r;
	r.ub(1, 4);
	r.ub(0, 4);
	int32_t x = 0;
	int32_t y = 0;
	bool first = true;
	for (const Contour &c : p_glyph) {
		r.ub(0, 1);
		r.ub(first ? (0x01 | 0x04) : 0x01, 5);
		const int n = sbits_max({ c.x0, c.y0 });
		r.ub(uint32_t(n), 5);
		r.sb(c.x0, n);
		r.sb(c.y0, n);
		if (first) {
			r.ub(1, 1);
		}
		first = false;
		x = c.x0;
		y = c.y0;
		for (const Seg &s : c.segs) {
			if (s.curve) {
				const int32_t cdx = s.cx - x, cdy = s.cy - y, adx = s.x - s.cx, ady = s.y - s.cy;
				const int nb = std::max(2, sbits_max({ cdx, cdy, adx, ady }));
				r.ub(1, 1);
				r.ub(0, 1);
				r.ub(uint32_t(nb - 2), 4);
				r.sb(cdx, nb);
				r.sb(cdy, nb);
				r.sb(adx, nb);
				r.sb(ady, nb);
			} else {
				const int32_t dx = s.x - x, dy = s.y - y;
				const int nb = std::max(2, sbits_max({ dx, dy }));
				r.ub(1, 1);
				r.ub(1, 1);
				r.ub(uint32_t(nb - 2), 4);
				r.ub(1, 1);
				r.sb(dx, nb);
				r.sb(dy, nb);
			}
			x = s.x;
			y = s.y;
		}
	}
	r.ub(0, 1);
	r.ub(0, 5);
	return r.b;
}

struct Font3 {
	uint16_t id = 1;
	std::string name = "Synth Sans Medium";
	std::vector<Glyph> glyphs;
	std::vector<uint16_t> codes;
	std::vector<int16_t> advances;
	struct Kern {
		uint16_t l, r;
		int16_t v;
	};
	std::vector<Kern> kerning;
	bool layout = true;
	bool bold = false;
	int16_t ascent = 22240;
	int16_t descent = 4320;
	int16_t leading = 0;
};

inline Bytes define_font3(const Font3 &p) {
	Writer w;
	w.u16(p.id);
	w.u8((p.layout ? 0x80 : 0) | 0x08 | 0x04 | (p.bold ? 0x01 : 0));
	w.u8(1);
	w.pstr8(p.name);
	const size_t n = p.glyphs.size();
	w.u16(uint32_t(n));
	std::vector<Bytes> shapes;
	for (const Glyph &g : p.glyphs) {
		shapes.push_back(glyph_shape(g));
	}
	uint32_t off = uint32_t(4 * (n + 1));
	for (size_t i = 0; i < n; ++i) {
		w.u32(off);
		off += uint32_t(shapes[i].size());
	}
	w.u32(off);
	for (const Bytes &s : shapes) {
		w.raw(s);
	}
	for (size_t i = 0; i < n; ++i) {
		w.u16(p.codes[i]);
	}
	if (p.layout) {
		w.u16(uint32_t(p.ascent));
		w.u16(uint32_t(p.descent));
		w.s16(p.leading);
		for (size_t i = 0; i < n; ++i) {
			w.s16(i < p.advances.size() ? p.advances[i] : 0);
		}
		for (size_t i = 0; i < n; ++i) {
			w.raw(rect(0, 1, 0, 1));
		}
		w.u16(uint32_t(p.kerning.size()));
		for (const auto &k : p.kerning) {
			w.u16(k.l);
			w.u16(k.r);
			w.s16(k.v);
		}
	}
	Bytes t;
	tag(t, 75, w.b, true);
	return t;
}

inline Bytes define_font3_no_glyphs(uint16_t p_id, std::string_view p_name, size_t p_trailing) {
	Writer w;
	w.u16(p_id);
	w.u8(0x04);
	w.u8(0);
	w.pstr8(p_name);
	w.u16(0);
	w.u16(0);
	w.raw(Bytes(p_trailing, 0x5a));
	Bytes t;
	tag(t, 75, w.b, true);
	return t;
}

inline Bytes define_font3_codes_only(uint16_t p_id, std::string_view p_name, const std::vector<uint16_t> &p_codes) {
	Writer w;
	w.u16(p_id);
	w.u8(0x04);
	w.u8(0);
	w.pstr8(p_name);
	w.u16(uint32_t(p_codes.size()));
	w.u16(0);
	w.u16(4);
	for (const uint16_t c : p_codes) {
		w.u16(c);
	}
	Bytes t;
	tag(t, 75, w.b, true);
	return t;
}

inline Bytes zlib_stored(const Bytes &p_data);
inline Bytes movie(const Bytes &p_tags, uint16_t p_frames = 1, bool p_cfx = false, float p_w = 1280, float p_h = 720) {
	Bytes body = rect(0, p_w, 0, p_h);
	body.push_back(0);
	body.push_back(30);
	body.push_back(uint8_t(p_frames));
	body.push_back(uint8_t(p_frames >> 8));
	body.insert(body.end(), p_tags.begin(), p_tags.end());
	Bytes out = { uint8_t(p_cfx ? 'C' : 'G'), 'F', 'X', 8 };
	const uint32_t len = uint32_t(body.size() + 8);
	for (int i = 0; i < 4; ++i) {
		out.push_back(uint8_t(len >> (8 * i)));
	}
	const Bytes stored = p_cfx ? zlib_stored(body) : body;
	out.insert(out.end(), stored.begin(), stored.end());
	return out;
}
inline uint32_t adler32(const Bytes &p_data) {
	uint32_t a = 1, b = 0;
	for (const uint8_t c : p_data) {
		a = (a + c) % 65521;
		b = (b + a) % 65521;
	}
	return (b << 16) | a;
}
inline Bytes zlib_stored(const Bytes &p_data) {
	Bytes out = { 0x78, 0x01 };
	size_t at = 0;
	do {
		const size_t n = std::min<size_t>(65535, p_data.size() - at);
		const bool last = at + n == p_data.size();
		out.push_back(last ? 1 : 0);
		out.push_back(uint8_t(n));
		out.push_back(uint8_t(n >> 8));
		out.push_back(uint8_t(~n));
		out.push_back(uint8_t((~n) >> 8));
		out.insert(out.end(), p_data.begin() + ptrdiff_t(at), p_data.begin() + ptrdiff_t(at + n));
		at += n;
	} while (at < p_data.size());
	const uint32_t ad = adler32(p_data);
	for (int i = 3; i >= 0; --i) {
		out.push_back(uint8_t(ad >> (8 * i)));
	}
	return out;
}

inline Bytes zlib_bomb(size_t p_out_bytes) {
	Bytes out = { 0x78, 0x01 };
	uint32_t acc = 0;
	int nbits = 0;
	auto put = [&](uint32_t p_bits, int p_n) {
		acc |= p_bits << nbits;
		nbits += p_n;
		while (nbits >= 8) {
			out.push_back(uint8_t(acc));
			acc >>= 8;
			nbits -= 8;
		}
	};
	auto huff = [&](uint32_t p_code, int p_n) {
		uint32_t rev = 0;
		for (int i = 0; i < p_n; ++i) {
			rev |= ((p_code >> i) & 1u) << (p_n - 1 - i);
		}
		put(rev, p_n);
	};
	put(1, 1);
	put(1, 2);
	huff(0x30 + 0, 8);
	for (size_t made = 1; made < p_out_bytes; made += 258) {
		huff(0xC0 + (285 - 280), 8);
		huff(0, 5);
	}
	huff(0, 7);
	put(0, 7);
	for (int i = 0; i < 4; ++i) {
		out.push_back(0);
	}
	return out;
}

inline Bytes gfx_from_body(const Bytes &p_body, uint8_t p_version = 8) {
	Bytes out = { 'G', 'F', 'X', p_version };
	const uint32_t len = uint32_t(p_body.size() + 8);
	for (int i = 0; i < 4; ++i) {
		out.push_back(uint8_t(len >> (8 * i)));
	}
	out.insert(out.end(), p_body.begin(), p_body.end());
	return out;
}

#if defined(__SANITIZE_ADDRESS__)
inline constexpr bool kAsan = true;
#else
inline constexpr bool kAsan = false;
#endif

inline size_t fuzz_iters(size_t p_default) {
	char buf[32] = {};
	size_t len = 0;
	if (getenv_s(&len, buf, sizeof(buf), "OPENBF3_FUZZ_ITERS") == 0 && len > 1) {
		const long v = std::strtol(buf, nullptr, 10);
		if (v > 0) {
			return size_t(v);
		}
	}
	return p_default;
}

template <class Rng>
void mutate(Bytes &r_bytes, Rng &r_rng) {
	if (r_bytes.empty()) {
		return;
	}
	const unsigned mode = unsigned(r_rng() % 5);
	if (mode == 0) {
		r_bytes.resize(size_t(r_rng() % r_bytes.size()));
		return;
	}
	const unsigned count = 1 + unsigned(r_rng() % 8);
	for (unsigned i = 0; i < count; ++i) {
		const size_t at = (r_rng() % 3 == 0) ? size_t(r_rng() % std::min<size_t>(r_bytes.size(), 0x200)) : size_t(r_rng() % r_bytes.size());
		if (mode == 1) {
			r_bytes[at] ^= uint8_t(1u << (r_rng() % 8));
		} else if (mode == 2) {
			r_bytes[at] = uint8_t(r_rng());
		} else if (mode == 3 && at + 4 <= r_bytes.size()) {
			const uint32_t v = (r_rng() % 2) ? 0xFFFFFFFFu : uint32_t(r_rng());
			std::memcpy(r_bytes.data() + at, &v, 4);
		} else {
			r_bytes[at] = 0;
		}
	}
}

inline Bytes cfx_with_stream(uint32_t p_declared, const Bytes &p_stream) {
	Bytes out = { 'C', 'F', 'X', 8 };
	for (int i = 0; i < 4; ++i) {
		out.push_back(uint8_t(p_declared >> (8 * i)));
	}
	out.insert(out.end(), p_stream.begin(), p_stream.end());
	return out;
}

}
