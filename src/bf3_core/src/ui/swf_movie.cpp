// SwfMovie symbol-table reader (bf3/core/ui/swf_movie.h; docs/design/ui-swf-fonts.md §5.1, §9). Clean-room from the
// public SWF file format specification (v19) and the GFx tag layouts measured on the retail install. Inflate:
// libdeflate (MIT), as payload.cpp.
#include "bf3/core/ui/swf_movie.h"

#include "swf_bits.h"
#include "swf_font_detail.h"

#include <libdeflate.h>

#include <algorithm>
#include <memory>
#include <string>

namespace bf3::core::ui {

namespace {

using detail::SwfReader;

struct DecompressorDeleter {
	void operator()(libdeflate_decompressor *p) const { libdeflate_free_decompressor(p); }
};

libdeflate_decompressor *swf_decompressor() {
	thread_local std::unique_ptr<libdeflate_decompressor, DecompressorDeleter> d(libdeflate_alloc_decompressor());
	return d.get();
}

std::string tag_label(uint16_t p_code) {
	const char *name = swf_tag_name(p_code);
	return name != nullptr ? std::string(name) : "tag " + std::to_string(p_code);
}

template <class T>
size_t vec_bytes(const std::vector<T> &p_v) {
	return p_v.capacity() * sizeof(T);
}
size_t str_bytes(const std::string &p_s) {
	return p_s.capacity() > 15 ? p_s.capacity() + 1 : 0; // beyond the small-string buffer
}

class Parser {
public:
	Parser(SwfMovie &r_movie, WorkBudget &r_budget) :
			m_(r_movie), budget_(r_budget), counts_(1024, 0) {}

	Status run(size_t p_begin) {
		BF3_TRY(timeline(p_begin, m_.body.size(), m_.root, false));
		finish();
		return Status::success();
	}

private:
	Error fail(const std::string &p_what, size_t p_at) const { return make_error("swf: " + p_what + " at " + std::to_string(p_at)); }
	Error tag_fail(uint16_t p_code, size_t p_at, const std::string &p_what) const {
		return make_error("swf: " + tag_label(p_code) + " at " + std::to_string(p_at) + ": " + p_what);
	}
	Status charge(size_t p_units, size_t p_at) {
		Status st = budget_.charge(p_units);
		if (!st) {
			return fail("work budget exhausted (" + st.error().message + ")", p_at);
		}
		return Status::success();
	}
	Status charge_text(std::string_view p_text, size_t p_at) { return charge(level::text_units(p_text.size()), p_at); }

	Status timeline(size_t p_begin, size_t p_end, SwfTimeline &r_tl, bool p_sprite) {
		const std::span<const uint8_t> body(m_.body);
		size_t p = p_begin;
		while (p < p_end) {
			if (p_end - p < 2) {
				return fail("tag header past the end of its timeline", p);
			}
			const uint16_t code_len = uint16_t(body[p] | (body[p + 1] << 8));
			const uint16_t code = uint16_t(code_len >> 6);
			uint64_t len = code_len & 0x3f;
			size_t header = 2;
			if (len == 0x3f) {
				if (p_end - p < 6) {
					return fail("long tag header past the end of its timeline", p);
				}
				len = uint32_t(body[p + 2] | (body[p + 3] << 8) | (body[p + 4] << 16) | (uint32_t(body[p + 5]) << 24));
				header = 6;
			}
			if (len > p_end - p - header) {
				return tag_fail(code, p, "length " + std::to_string(len) + " past the end of its timeline (" + std::to_string(p_end - p - header) + " bytes left)");
			}
			if (++tags_ > kSwfMaxTags) {
				return fail("more than " + std::to_string(kSwfMaxTags) + " tags", p);
			}
			BF3_TRY(charge(1, p));
			counts_[code]++;
			const size_t at = p + header;
			BF3_TRY(tag(code, at, size_t(len), r_tl, p_sprite));
			p = at + size_t(len);
			if (code == swf_tag::kEnd) {
				break;
			}
		}
		if (!p_sprite) {
			m_.trailing_bytes = uint32_t(p_end - p);
		}
		return Status::success();
	}

	Status tag(uint16_t p_code, size_t p_at, size_t p_len, SwfTimeline &r_tl, bool p_sprite) {
		const std::span<const uint8_t> data = std::span<const uint8_t>(m_.body).subspan(p_at, p_len);
		SwfReader r(data, p_at);
		Status st = Status::success();
		switch (p_code) {
			case swf_tag::kEnd:
			case swf_tag::kSetBackgroundColor:
			case swf_tag::kFileAttributes:
			case swf_tag::kMetadata:
				return Status::success();
			case swf_tag::kShowFrame:
				if (++r_tl.frames > kSwfMaxFrames) {
					return tag_fail(p_code, p_at, "more than " + std::to_string(kSwfMaxFrames) + " frames");
				}
				return Status::success();
			case swf_tag::kDefineShape:
				st = shape(r, 1);
				break;
			case swf_tag::kDefineShape2:
				st = shape(r, 2);
				break;
			case swf_tag::kDefineShape3:
				st = shape(r, 3);
				break;
			case swf_tag::kDefineShape4:
				st = shape(r, 4);
				break;
			case swf_tag::kPlaceObject:
			case swf_tag::kPlaceObject2:
			case swf_tag::kPlaceObject3:
			case swf_tag::kRemoveObject:
			case swf_tag::kRemoveObject2:
				st = display_event(p_code, r, r_tl);
				break;
			case swf_tag::kDefineText:
				m_.static_texts.push_back(r.u16());
				break;
			case swf_tag::kDoAction:
				r_tl.do_actions++;
				r_tl.action_bytes += p_len;
				m_.action_bytes += p_len;
				return Status::success();
			case swf_tag::kDoInitAction:
				if (p_len < 2) {
					return tag_fail(p_code, p_at, "shorter than its sprite id");
				}
				m_.do_init_actions++;
				m_.action_bytes += p_len - 2;
				return Status::success();
			case swf_tag::kDoAbc:
				m_.abc_bytes += p_len;
				return Status::success();
			case swf_tag::kDefineEditText:
				st = edit_text(r);
				break;
			case swf_tag::kDefineSprite:
				if (p_sprite) {
					return tag_fail(p_code, p_at, "a DefineSprite inside a sprite");
				}
				st = sprite(p_at, p_len);
				if (!st) {
					return st;
				}
				return Status::success();
			case swf_tag::kFrameLabel: {
				SwfFrameLabel label;
				const std::string_view name = r.cstr();
				if (r.failed()) {
					break;
				}
				BF3_TRY(charge_text(name, p_at));
				label.frame = r_tl.frames;
				label.name = std::string(name);
				r_tl.labels.push_back(std::move(label));
				return Status::success();
			}
			case swf_tag::kDefineFont2:
			case swf_tag::kDefineFont3:
				st = font(p_code, p_at, p_len);
				break;
			case swf_tag::kExportAssets:
				st = exports(r);
				break;
			case swf_tag::kImportAssets:
			case swf_tag::kImportAssets2:
				st = imports(p_code, r);
				break;
			case swf_tag::kDefineFontAlignZones:
				st = align_zones(r);
				break;
			case swf_tag::kCsmTextSettings:
				m_.csm_text_settings++;
				return Status::success();
			case swf_tag::kDefineScalingGrid: {
				SwfScalingGrid g;
				g.id = r.u16();
				g.splitter = r.rect();
				m_.scaling_grids.push_back(g);
				break;
			}
			case swf_tag::kDefineFontName: {
				SwfFontName n;
				n.font_id = r.u16();
				const std::string_view name = r.cstr();
				const std::string_view copyright = r.cstr();
				if (r.failed()) {
					break;
				}
				BF3_TRY(charge_text(name, p_at));
				BF3_TRY(charge_text(copyright, p_at));
				n.name = std::string(name);
				n.copyright = std::string(copyright);
				m_.font_names.push_back(std::move(n));
				break;
			}
			case swf_tag::kGfxExporterInfo:
				st = exporter_info(r);
				break;
			case swf_tag::kGfxDefineExternalImage:
			case swf_tag::kGfxDefineExternalImage2:
				st = external_image(p_code, r);
				break;
			case swf_tag::kGfxDefineSubImage: {
				SwfSubImage s;
				s.id = r.u16();
				s.image_id = r.u16();
				s.x1 = r.u16();
				s.y1 = r.u16();
				s.x2 = r.u16();
				s.y2 = r.u16();
				if (r.failed()) {
					break;
				}
				if (s.x2 < s.x1 || s.y2 < s.y1) {
					return tag_fail(p_code, p_at,
							"reversed rectangle (" + std::to_string(s.x1) + ", " + std::to_string(s.y1) + ", " + std::to_string(s.x2) + ", " +
									std::to_string(s.y2) + ")");
				}
				m_.sub_images.push_back(s);
				break;
			}
			default:
				if (swf_tag_name(p_code) == nullptr) {
					m_.unknown_tags++;
				}
				return Status::success();
		}
		if (!st) {
			return st;
		}
		if (r.failed()) {
			return tag_fail(p_code, p_at, "truncated (" + std::to_string(p_len) + " bytes)");
		}
		return Status::success();
	}

	Status sprite(size_t p_at, size_t p_len) {
		if (p_len < 4) {
			return tag_fail(swf_tag::kDefineSprite, p_at, "shorter than its header");
		}
		SwfSprite s;
		s.id = uint16_t(m_.body[p_at] | (m_.body[p_at + 1] << 8));
		s.timeline.declared_frames = uint16_t(m_.body[p_at + 2] | (m_.body[p_at + 3] << 8));
		BF3_TRY(timeline(p_at + 4, p_at + p_len, s.timeline, true));
		m_.sprites.push_back(std::move(s));
		return Status::success();
	}

	Status display_event(uint16_t p_code, SwfReader &r, SwfTimeline &r_tl) {
		if (++events_ > kSwfMaxDisplayEvents) {
			return tag_fail(p_code, r.abs_pos(), "more than " + std::to_string(kSwfMaxDisplayEvents) + " display events in the movie");
		}
		BF3_TRY(charge(kSwfDisplayEventUnits, r.abs_pos()));
		SwfDisplayEvent e;
		e.frame = r_tl.frames;
		switch (p_code) {
			case swf_tag::kPlaceObject: {
				e.tag_version = 1;
				e.op = SwfDisplayOp::Place;
				e.character = r.u16();
				e.depth = r.u16();
				e.has_matrix = true;
				e.matrix = r.matrix();
				if (!r.failed() && !r.at_end()) {
					e.has_color = true;
					e.color = r.cxform(false);
				}
				break;
			}
			case swf_tag::kRemoveObject:
				e.tag_version = 1;
				e.op = SwfDisplayOp::Remove;
				e.character = r.u16();
				e.depth = r.u16();
				break;
			case swf_tag::kRemoveObject2:
				e.tag_version = 2;
				e.op = SwfDisplayOp::Remove;
				e.depth = r.u16();
				break;
			default: {
				const bool v3 = p_code == swf_tag::kPlaceObject3;
				e.tag_version = v3 ? 3 : 2;
				const uint8_t f = r.u8();
				const uint8_t f2 = v3 ? r.u8() : 0;
				e.depth = r.u16();
				const bool has_char = (f & 0x02) != 0;
				const bool move = (f & 0x01) != 0;
				e.op = move ? (has_char ? SwfDisplayOp::Replace : SwfDisplayOp::Modify) : (has_char ? SwfDisplayOp::Place : SwfDisplayOp::Modify);
				if (v3 && ((f2 & 0x08) != 0 || ((f2 & 0x10) != 0 && has_char))) {
					const std::string_view cls = r.cstr();
					BF3_TRY(charge_text(cls, r.abs_pos()));
					e.class_name = std::string(cls);
				}
				if (has_char) {
					e.character = r.u16();
				}
				if ((f & 0x04) != 0) {
					e.has_matrix = true;
					e.matrix = r.matrix();
				}
				if ((f & 0x08) != 0) {
					e.has_color = true;
					e.color = r.cxform(true);
				}
				if ((f & 0x10) != 0) {
					e.has_ratio = true;
					e.ratio = r.u16();
				}
				if ((f & 0x20) != 0) {
					e.has_name = true;
					const std::string_view name = r.cstr();
					BF3_TRY(charge_text(name, r.abs_pos()));
					e.name = std::string(name);
				}
				if ((f & 0x40) != 0) {
					e.clip_depth = r.u16();
				}
				if (v3) {
					e.has_filters = (f2 & 0x01) != 0;
					if (e.has_filters) {
						skip_filters(r);
					}
					if ((f2 & 0x02) != 0 && !r.failed()) {
						e.blend_mode = r.u8();
					}
				}
				// Bitmap cache, visibility and clip actions follow inside the tag's length: skipped.
				break;
			}
		}
		if (r.failed()) {
			return Status::success(); // reported by tag() as a truncated tag
		}
		r_tl.events.push_back(std::move(e));
		return Status::success();
	}

	// PlaceObject3 FILTERLIST: fixed-size filters, the gradient ones by their colour count, convolution by its
	// matrix. An unknown filter id leaves the reader where it is (failed), so the blend mode is not read.
	void skip_filters(SwfReader &r) {
		const uint8_t n = r.u8();
		for (unsigned i = 0; i < n && !r.failed(); ++i) {
			const uint8_t id = r.u8();
			switch (id) {
				case 0: r.skip(23); break; // drop shadow
				case 1: r.skip(9); break; // blur
				case 2: r.skip(15); break; // glow
				case 3: r.skip(27); break; // bevel
				case 4:
				case 7: { // gradient glow / bevel
					const uint8_t colours = r.u8();
					r.skip(size_t(colours) * 5 + 19);
					break;
				}
				case 5: { // convolution
					const uint8_t mx = r.u8();
					const uint8_t my = r.u8();
					r.skip(8 + size_t(mx) * my * 4 + 5);
					break;
				}
				case 6: r.skip(80); break; // colour matrix
				default: r.fail(); break;
			}
		}
	}

	// FILLSTYLEARRAY / LINESTYLEARRAY: count x minimum record size checked against the bytes left first.
	Status fill_styles(SwfReader &r, int p_version, SwfShape &r_shape) {
		size_t n = r.u8();
		if (n == 0xff && p_version >= 2) {
			n = r.u16();
		}
		if (r.failed() || n * 3 > r.left()) {
			r.fail();
			return Status::success();
		}
		BF3_TRY(charge(n, r.abs_pos()));
		for (size_t i = 0; i < n && !r.failed(); ++i) {
			fill_style(r, p_version, r_shape);
		}
		return Status::success();
	}
	void fill_style(SwfReader &r, int p_version, SwfShape &r_shape) {
		const uint8_t type = r.u8();
		if (type == 0x00) {
			r.skip(p_version >= 3 ? 4 : 3);
			r_shape.solid_fills++;
		} else if (type == 0x10 || type == 0x12 || type == 0x13) {
			(void)r.matrix();
			const uint8_t g = r.u8();
			r.skip(size_t(g & 0x0f) * (p_version >= 3 ? 5 : 4));
			if (type == 0x13) {
				r.skip(2);
			}
			r_shape.gradient_fills++;
		} else if (type >= 0x40 && type <= 0x43) {
			SwfBitmapFill b;
			b.fill_type = type;
			b.bitmap_id = r.u16();
			b.matrix = r.matrix();
			if (!r.failed()) {
				r_shape.bitmap_fills.push_back(b);
			}
		} else {
			r.fail();
		}
	}
	Status line_styles(SwfReader &r, int p_version, SwfShape &r_shape) {
		size_t n = r.u8();
		if (n == 0xff) {
			n = r.u16();
		}
		if (r.failed() || n * 5 > r.left()) {
			r.fail();
			return Status::success();
		}
		BF3_TRY(charge(n, r.abs_pos()));
		r_shape.line_styles += uint32_t(n);
		for (size_t i = 0; i < n && !r.failed(); ++i) {
			r.skip(2); // width
			if (p_version == 4) {
				r.ub(2); // start cap
				const uint32_t join = r.ub(2);
				const uint32_t has_fill = r.ub(1);
				r.ub(11); // no h/v scale, pixel hinting, reserved, no close, end cap
				if (join == 2) {
					r.skip(2); // miter limit
				}
				if (has_fill != 0) {
					fill_style(r, p_version, r_shape);
				} else {
					r.skip(4);
				}
			} else {
				r.skip(p_version >= 3 ? 4 : 3);
			}
		}
		return Status::success();
	}

	Status shape(SwfReader &r, int p_version) {
		SwfShape s;
		s.version = uint8_t(p_version);
		s.id = r.u16();
		s.bounds = r.rect();
		if (p_version == 4) {
			(void)r.rect(); // edge bounds
			r.skip(1); // flags
		}
		BF3_TRY(fill_styles(r, p_version, s));
		BF3_TRY(line_styles(r, p_version, s));
		uint8_t nb = r.u8();
		unsigned fill_bits = nb >> 4;
		unsigned line_bits = nb & 0x0f;
		while (!r.failed()) {
			BF3_TRY(charge(1, r.abs_pos()));
			if (r.ub(1) == 0) {
				const uint32_t flags = r.ub(5);
				if (flags == 0) {
					break;
				}
				if ((flags & 0x01) != 0) {
					const unsigned n = r.ub(5);
					r.sb(n);
					r.sb(n);
				}
				if ((flags & 0x02) != 0) {
					r.ub(fill_bits);
				}
				if ((flags & 0x04) != 0) {
					r.ub(fill_bits);
				}
				if ((flags & 0x08) != 0) {
					r.ub(line_bits);
				}
				if ((flags & 0x10) != 0) {
					if (p_version < 2) {
						r.fail();
						break;
					}
					BF3_TRY(fill_styles(r, p_version, s));
					BF3_TRY(line_styles(r, p_version, s));
					nb = r.u8();
					fill_bits = nb >> 4;
					line_bits = nb & 0x0f;
				}
			} else {
				const bool straight = r.ub(1) != 0;
				const unsigned n = r.ub(4) + 2;
				if (straight) {
					if (r.ub(1) != 0) {
						r.sb(n);
						r.sb(n);
					} else {
						r.ub(1);
						r.sb(n);
					}
				} else {
					r.sb(n);
					r.sb(n);
					r.sb(n);
					r.sb(n);
				}
				s.edges++;
			}
		}
		r.align();
		if (!r.failed() && r.pos() != r.size()) {
			return tag_fail(swf_tag::kDefineShape, r.abs_pos(), "shape " + std::to_string(s.id) + " ends " + std::to_string(r.size() - r.pos()) + " bytes before its tag");
		}
		if (!r.failed()) {
			m_.shapes.push_back(std::move(s));
		}
		return Status::success();
	}

	Status edit_text(SwfReader &r) {
		SwfEditText t;
		t.id = r.u16();
		t.bounds = r.rect();
		const uint8_t f1 = r.u8();
		const uint8_t f2 = r.u8();
		t.flags = uint16_t((f1 << 8) | f2);
		if (t.has(SwfEditText::kHasFont)) {
			t.font_id = r.u16();
		}
		if (t.has(SwfEditText::kHasFontClass)) {
			const std::string_view cls = r.cstr();
			BF3_TRY(charge_text(cls, r.abs_pos()));
			t.font_class = std::string(cls);
		}
		if (t.has(SwfEditText::kHasFont) || t.has(SwfEditText::kHasFontClass)) {
			t.height = float(r.u16()) / 20.0f;
		}
		if (t.has(SwfEditText::kHasTextColor)) {
			t.color.r = r.u8();
			t.color.g = r.u8();
			t.color.b = r.u8();
			t.color.a = r.u8();
		}
		if (t.has(SwfEditText::kHasMaxLength)) {
			t.max_length = r.u16();
		}
		if (t.has(SwfEditText::kHasLayout)) {
			t.align = r.u8();
			t.left_margin = float(r.u16()) / 20.0f;
			t.right_margin = float(r.u16()) / 20.0f;
			t.indent = float(r.u16()) / 20.0f;
			t.leading = float(r.s16()) / 20.0f;
		}
		const std::string_view var = r.cstr();
		BF3_TRY(charge_text(var, r.abs_pos()));
		t.variable = std::string(var);
		if (t.has(SwfEditText::kHasText)) {
			const std::string_view text = r.cstr();
			BF3_TRY(charge_text(text, r.abs_pos()));
			t.initial_text = std::string(text);
		}
		if (!r.failed() && r.pos() != r.size()) {
			return tag_fail(swf_tag::kDefineEditText, r.abs_pos(),
					"edit text " + std::to_string(t.id) + " ends " + std::to_string(r.size() - r.pos()) + " bytes before its tag");
		}
		if (!r.failed()) {
			m_.edit_texts.push_back(std::move(t));
		}
		return Status::success();
	}

	Status font(uint16_t p_code, size_t p_at, size_t p_len) {
		SwfFontOutlines outlines;
		const std::span<const uint8_t> data = std::span<const uint8_t>(m_.body).subspan(p_at, p_len);
		Status st = detail::read_font_tag(data, p_code, false, budget_, outlines);
		if (!st) {
			return tag_fail(p_code, p_at, st.error().message);
		}
		SwfFontInfo info;
		info.id = outlines.id;
		info.tag_code = p_code;
		info.name = std::move(outlines.name);
		info.flags = data[2]; // read_font_tag succeeded, so the header is there
		info.language = outlines.language;
		info.glyph_count = uint16_t(outlines.codes.size());
		info.kind = outlines.kind;
		info.trailing_bytes = outlines.trailing_bytes;
		info.tag_offset = uint32_t(p_at);
		info.tag_length = uint32_t(p_len);
		m_.fonts.push_back(std::move(info));
		return Status::success();
	}

	Status exports(SwfReader &r) {
		const size_t n = r.u16();
		if (r.failed() || n * 3 > r.left()) {
			r.fail();
			return Status::success();
		}
		for (size_t i = 0; i < n && !r.failed(); ++i) {
			SwfExport e;
			e.id = r.u16();
			const std::string_view name = r.cstr();
			BF3_TRY(charge(1 + level::text_units(name.size()), r.abs_pos()));
			e.name = std::string(name);
			if (!r.failed()) {
				m_.exports.push_back(std::move(e));
			}
		}
		return Status::success();
	}

	Status imports(uint16_t p_code, SwfReader &r) {
		SwfImport im;
		im.tag_version = p_code == swf_tag::kImportAssets2 ? 2 : 1;
		const std::string_view url = r.cstr();
		BF3_TRY(charge_text(url, r.abs_pos()));
		im.url = std::string(url);
		if (im.tag_version == 2) {
			r.skip(2); // reserved: 1, 0
		}
		const size_t n = r.u16();
		if (r.failed() || n * 3 > r.left()) {
			r.fail();
			return Status::success();
		}
		for (size_t i = 0; i < n && !r.failed(); ++i) {
			SwfExport e;
			e.id = r.u16();
			const std::string_view name = r.cstr();
			BF3_TRY(charge(1 + level::text_units(name.size()), r.abs_pos()));
			e.name = std::string(name);
			im.symbols.push_back(std::move(e));
		}
		if (!r.failed()) {
			m_.imports.push_back(std::move(im));
		}
		return Status::success();
	}

	Status align_zones(SwfReader &r) {
		SwfFontAlignZones z;
		z.font_id = r.u16();
		z.csm_hint = uint8_t(r.u8() >> 6);
		while (!r.failed() && !r.at_end()) {
			BF3_TRY(charge(1, r.abs_pos()));
			const uint8_t n = r.u8();
			r.skip(size_t(n) * 4 + 1);
			z.glyphs++;
		}
		if (!r.failed()) {
			m_.font_align_zones.push_back(z);
		}
		return Status::success();
	}

	Status exporter_info(SwfReader &r) {
		SwfExporterInfo &x = m_.exporter;
		x.present = true;
		x.version = r.u16();
		if (x.version >= 0x10a) {
			x.flags = r.u32();
		}
		x.bitmap_format = r.u16();
		const std::string_view prefix = r.pstr8();
		const std::string_view name = r.pstr8();
		x.prefix = std::string(prefix);
		x.swf_name = std::string(name);
		x.extra_bytes = uint32_t(r.left());
		return Status::success();
	}

	Status external_image(uint16_t p_code, SwfReader &r) {
		SwfExternalImage im;
		im.tag_version = p_code == swf_tag::kGfxDefineExternalImage2 ? 2 : 1;
		im.image_id = r.u16();
		if (im.tag_version == 2) {
			im.field_2 = r.u16();
		}
		im.bitmap_format = r.u16();
		im.width = r.u16();
		im.height = r.u16();
		im.export_name = std::string(r.pstr8());
		im.file_name = std::string(r.pstr8());
		im.extra_bytes = uint32_t(r.left());
		if (!r.failed()) {
			m_.external_images.push_back(std::move(im));
		}
		return Status::success();
	}

	void finish() {
		auto by_id = [](const auto &a, const auto &b) { return a.id < b.id; };
		std::stable_sort(m_.sprites.begin(), m_.sprites.end(), by_id);
		std::stable_sort(m_.shapes.begin(), m_.shapes.end(), by_id);
		std::stable_sort(m_.sub_images.begin(), m_.sub_images.end(), by_id);
		std::stable_sort(m_.edit_texts.begin(), m_.edit_texts.end(), by_id);
		std::stable_sort(m_.fonts.begin(), m_.fonts.end(), by_id);
		std::stable_sort(m_.scaling_grids.begin(), m_.scaling_grids.end(), by_id);
		std::stable_sort(m_.static_texts.begin(), m_.static_texts.end());
		for (size_t code = 0; code < counts_.size(); ++code) {
			if (counts_[code] != 0) {
				m_.tag_counts.push_back({ uint16_t(code), counts_[code] });
			}
		}
		m_.budget_used = budget_.spent();
		m_.retained_bytes = retained();
	}

	size_t timeline_bytes(const SwfTimeline &p_tl) const {
		size_t b = vec_bytes(p_tl.labels) + vec_bytes(p_tl.events);
		for (const SwfFrameLabel &l : p_tl.labels) {
			b += str_bytes(l.name);
		}
		for (const SwfDisplayEvent &e : p_tl.events) {
			b += str_bytes(e.name) + str_bytes(e.class_name);
		}
		return b;
	}

	size_t retained() const {
		size_t b = sizeof(SwfMovie) + m_.body.capacity() + timeline_bytes(m_.root) + vec_bytes(m_.sprites);
		for (const SwfSprite &s : m_.sprites) {
			b += timeline_bytes(s.timeline);
		}
		b += vec_bytes(m_.exports) + vec_bytes(m_.imports) + vec_bytes(m_.external_images) + vec_bytes(m_.sub_images) + vec_bytes(m_.shapes) +
				vec_bytes(m_.edit_texts) + vec_bytes(m_.static_texts) + vec_bytes(m_.scaling_grids) + vec_bytes(m_.fonts) + vec_bytes(m_.font_align_zones) +
				vec_bytes(m_.font_names) + vec_bytes(m_.tag_counts);
		for (const SwfExport &e : m_.exports) {
			b += str_bytes(e.name);
		}
		for (const SwfImport &i : m_.imports) {
			b += str_bytes(i.url) + vec_bytes(i.symbols);
			for (const SwfExport &e : i.symbols) {
				b += str_bytes(e.name);
			}
		}
		for (const SwfShape &s : m_.shapes) {
			b += vec_bytes(s.bitmap_fills);
		}
		for (const SwfEditText &t : m_.edit_texts) {
			b += str_bytes(t.font_class) + str_bytes(t.variable) + str_bytes(t.initial_text);
		}
		for (const SwfFontInfo &f : m_.fonts) {
			b += str_bytes(f.name);
		}
		return b;
	}

	SwfMovie &m_;
	WorkBudget &budget_;
	std::vector<uint32_t> counts_;
	uint32_t tags_ = 0;
	uint32_t events_ = 0;
};

template <class V>
auto find_by_id(const V &p_v, uint16_t p_id) -> decltype(&p_v.front()) {
	auto it = std::lower_bound(p_v.begin(), p_v.end(), p_id, [](const auto &e, uint16_t id) { return e.id < id; });
	return (it != p_v.end() && it->id == p_id) ? &*it : nullptr;
}

} // namespace

const char *swf_tag_name(uint16_t p_code) {
	switch (p_code) {
		case swf_tag::kEnd: return "End";
		case swf_tag::kShowFrame: return "ShowFrame";
		case swf_tag::kDefineShape: return "DefineShape";
		case swf_tag::kPlaceObject: return "PlaceObject";
		case swf_tag::kRemoveObject: return "RemoveObject";
		case swf_tag::kSetBackgroundColor: return "SetBackgroundColor";
		case swf_tag::kDefineText: return "DefineText";
		case swf_tag::kDoAction: return "DoAction";
		case swf_tag::kDefineShape2: return "DefineShape2";
		case swf_tag::kPlaceObject2: return "PlaceObject2";
		case swf_tag::kRemoveObject2: return "RemoveObject2";
		case swf_tag::kDefineShape3: return "DefineShape3";
		case swf_tag::kDefineEditText: return "DefineEditText";
		case swf_tag::kDefineSprite: return "DefineSprite";
		case swf_tag::kFrameLabel: return "FrameLabel";
		case swf_tag::kDefineFont2: return "DefineFont2";
		case swf_tag::kExportAssets: return "ExportAssets";
		case swf_tag::kImportAssets: return "ImportAssets";
		case swf_tag::kDoInitAction: return "DoInitAction";
		case swf_tag::kFileAttributes: return "FileAttributes";
		case swf_tag::kPlaceObject3: return "PlaceObject3";
		case swf_tag::kImportAssets2: return "ImportAssets2";
		case swf_tag::kDefineFontAlignZones: return "DefineFontAlignZones";
		case swf_tag::kCsmTextSettings: return "CSMTextSettings";
		case swf_tag::kDefineFont3: return "DefineFont3";
		case swf_tag::kMetadata: return "Metadata";
		case swf_tag::kDefineScalingGrid: return "DefineScalingGrid";
		case swf_tag::kDoAbc: return "DoABC";
		case swf_tag::kDefineShape4: return "DefineShape4";
		case swf_tag::kDefineFontName: return "DefineFontName";
		case swf_tag::kGfxExporterInfo: return "GFX_ExporterInfo";
		case swf_tag::kGfxDefineExternalImage: return "GFX_DefineExternalImage";
		case swf_tag::kGfxFontTextureInfo: return "GFX_FontTextureInfo";
		case swf_tag::kGfxDefineExternalGradient: return "GFX_DefineExternalGradient";
		case swf_tag::kGfxDefineGradientMap: return "GFX_DefineGradientMap";
		case swf_tag::kGfxDefineCompactedFont: return "GFX_DefineCompactedFont";
		case swf_tag::kGfxDefineExternalSound: return "GFX_DefineExternalSound";
		case swf_tag::kGfxDefineExternalStreamSound: return "GFX_DefineExternalStreamSound";
		case swf_tag::kGfxDefineSubImage: return "GFX_DefineSubImage";
		case swf_tag::kGfxDefineExternalImage2: return "GFX_DefineExternalImage2";
		default: return nullptr;
	}
}

const char *swf_character_kind_name(SwfCharacterKind p_kind) {
	switch (p_kind) {
		case SwfCharacterKind::None: return "none";
		case SwfCharacterKind::Shape: return "shape";
		case SwfCharacterKind::Sprite: return "sprite";
		case SwfCharacterKind::EditText: return "edit_text";
		case SwfCharacterKind::StaticText: return "static_text";
		case SwfCharacterKind::Font: return "font";
		case SwfCharacterKind::SubImage: return "sub_image";
		case SwfCharacterKind::Imported: return "imported";
	}
	return "?";
}

const char *swf_font_kind_name(SwfFontKind p_kind) {
	switch (p_kind) {
		case SwfFontKind::Outlines: return "outlines";
		case SwfFontKind::NoGlyphs: return "no_glyphs";
		case SwfFontKind::CodesOnly: return "codes_only";
	}
	return "?";
}

Result<SwfMovie> SwfMovie::parse(std::span<const uint8_t> p_payload, WorkBudget *p_budget) {
	if (p_payload.size() < 8) {
		return make_error("swf: payload of " + std::to_string(p_payload.size()) + " bytes is shorter than the 8-byte header");
	}
	const bool cfx = p_payload[0] == 'C' && p_payload[1] == 'F' && p_payload[2] == 'X';
	const bool gfx = p_payload[0] == 'G' && p_payload[1] == 'F' && p_payload[2] == 'X';
	if (!cfx && !gfx) {
		return make_error("swf: signature is not CFX or GFX at 0");
	}
	SwfMovie m;
	m.compressed = cfx;
	m.version = p_payload[3];
	m.file_length = uint32_t(p_payload[4] | (p_payload[5] << 8) | (p_payload[6] << 16) | (uint32_t(p_payload[7]) << 24));
	if (m.file_length < 8) {
		return make_error("swf: declared length " + std::to_string(m.file_length) + " is shorter than the header at 4");
	}
	const size_t body_len = size_t(m.file_length) - 8;
	if (body_len > kSwfMaxBodyBytes) {
		return make_error("swf: declared body of " + std::to_string(body_len) + " bytes exceeds the " + std::to_string(kSwfMaxBodyBytes) + "-byte cap at 4");
	}
	std::unique_ptr<WorkBudget> local;
	WorkBudget *budget = p_budget;
	if (budget == nullptr) {
		local = std::make_unique<WorkBudget>(swf_default_budget_units(m.file_length));
		budget = local.get();
	}
	const std::span<const uint8_t> stored = p_payload.subspan(8);
	if (gfx) {
		if (stored.size() != body_len) {
			return make_error("swf: GFX body is " + std::to_string(stored.size()) + " bytes, the header declares " + std::to_string(body_len) + " at 8");
		}
		m.body.assign(stored.begin(), stored.end());
	} else {
		m.body.resize(body_len);
		size_t produced = 0;
		const libdeflate_result r = libdeflate_zlib_decompress(swf_decompressor(), stored.data(), stored.size(), m.body.data(), body_len, &produced);
		if (r == LIBDEFLATE_INSUFFICIENT_SPACE) {
			return make_error("swf: CFX body inflates past its declared " + std::to_string(body_len) + " bytes at 8");
		}
		if (r != LIBDEFLATE_SUCCESS) {
			return make_error("swf: CFX body is not a valid zlib stream at 8");
		}
		if (produced != body_len) {
			return make_error("swf: CFX body inflates to " + std::to_string(produced) + " bytes, the header declares " + std::to_string(body_len) + " at 8");
		}
	}
	SwfReader r(m.body, 8);
	m.stage = r.rect();
	const uint8_t rate_frac = r.u8();
	const uint8_t rate_int = r.u8();
	m.frame_rate = float(rate_int) + float(rate_frac) / 256.0f;
	m.frame_count = r.u16();
	if (r.failed()) {
		return make_error("swf: body of " + std::to_string(body_len) + " bytes is shorter than the stage, rate and frame count at 8");
	}
	m.root.declared_frames = m.frame_count;
	Parser parser(m, *budget);
	BF3_TRY(parser.run(r.pos()));
	return m;
}

const SwfSprite *SwfMovie::find_sprite(uint16_t p_id) const {
	return find_by_id(sprites, p_id);
}
const SwfShape *SwfMovie::find_shape(uint16_t p_id) const {
	return find_by_id(shapes, p_id);
}
const SwfSubImage *SwfMovie::find_sub_image(uint16_t p_id) const {
	return find_by_id(sub_images, p_id);
}
const SwfEditText *SwfMovie::find_edit_text(uint16_t p_id) const {
	return find_by_id(edit_texts, p_id);
}
const SwfFontInfo *SwfMovie::find_font(uint16_t p_id) const {
	return find_by_id(fonts, p_id);
}
const SwfExternalImage *SwfMovie::find_external_image(uint16_t p_image_id) const {
	for (const SwfExternalImage &e : external_images) {
		if (e.image_id == p_image_id) {
			return &e;
		}
	}
	return nullptr;
}
const SwfImport *SwfMovie::find_import(uint16_t p_id, const SwfExport **r_symbol) const {
	for (const SwfImport &im : imports) {
		for (const SwfExport &s : im.symbols) {
			if (s.id == p_id) {
				if (r_symbol != nullptr) {
					*r_symbol = &s;
				}
				return &im;
			}
		}
	}
	return nullptr;
}
SwfCharacterKind SwfMovie::character_kind(uint16_t p_id) const {
	if (find_shape(p_id) != nullptr) {
		return SwfCharacterKind::Shape;
	}
	if (find_sprite(p_id) != nullptr) {
		return SwfCharacterKind::Sprite;
	}
	if (find_edit_text(p_id) != nullptr) {
		return SwfCharacterKind::EditText;
	}
	if (std::binary_search(static_texts.begin(), static_texts.end(), p_id)) {
		return SwfCharacterKind::StaticText;
	}
	if (find_font(p_id) != nullptr) {
		return SwfCharacterKind::Font;
	}
	if (find_sub_image(p_id) != nullptr) {
		return SwfCharacterKind::SubImage;
	}
	if (find_import(p_id) != nullptr) {
		return SwfCharacterKind::Imported;
	}
	return SwfCharacterKind::None;
}
std::optional<uint16_t> SwfMovie::export_id(std::string_view p_name) const {
	for (const SwfExport &e : exports) {
		if (e.name == p_name) {
			return e.id;
		}
	}
	return std::nullopt;
}
std::string_view SwfMovie::export_name(uint16_t p_id) const {
	for (const SwfExport &e : exports) {
		if (e.id == p_id) {
			return e.name;
		}
	}
	return {};
}
std::span<const uint8_t> SwfMovie::font_tag(const SwfFontInfo &p_font) const {
	if (size_t(p_font.tag_offset) + p_font.tag_length > body.size()) {
		return {};
	}
	return std::span<const uint8_t>(body).subspan(p_font.tag_offset, p_font.tag_length);
}
uint32_t SwfMovie::tag_count(uint16_t p_code) const {
	auto it = std::lower_bound(tag_counts.begin(), tag_counts.end(), p_code, [](const SwfTagCount &c, uint16_t code) { return c.code < code; });
	return (it != tag_counts.end() && it->code == p_code) ? it->count : 0;
}

} // namespace bf3::core::ui
