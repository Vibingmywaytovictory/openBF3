#include "bf3/core/ui/swf_layout.h"

#include <algorithm>
#include <array>
#include <optional>
#include <string>

namespace bf3::core::ui {

namespace {

constexpr std::array<std::string_view, 14> kHudScreens = {
	"hudmpscreen",
	"hudscreen",
	"hudconquestscreen",
	"hudtdmscreen",
	"hudsdmscreen",
	"hudrushscreen",
	"hudctfscreen",
	"hudgmscreen",
	"hudmatchpreroundscreen",
	"hudpreroundscreen",
	"killscreen",
	"spawnscreenpc",
	"spectatorscreen",
	"weaponcommonscreen",
};

std::string movie_key(std::string_view p_name) {
	std::string key = to_lower_ascii(p_name);
	for (char &c : key) {
		c = c == '\\' ? '/' : c;
	}
	if (key.find('/') == std::string::npos) {
		key = "ui/assets/" + key;
	}
	return key;
}

std::string symbol_of(const SwfMovie &p_movie, uint16_t p_id) {
	const std::string_view e = p_movie.export_name(p_id);
	return e.empty() ? "#" + std::to_string(p_id) : std::string(e);
}

class LayoutBuilder {
public:
	LayoutBuilder(const SwfLibrary &p_movies, const SwfLayoutOptions &p_options, WorkBudget &r_budget) :
			movies_(p_movies), options_(p_options), budget_(r_budget) {}

	Status charge(size_t p_units) {
		Status st = budget_.charge(p_units);
		if (!st) {
			return make_error("swf layout: work budget exhausted (" + st.error().message + ")");
		}
		return Status::success();
	}

	Status frame0(const SwfTimeline &p_tl, std::vector<SwfDisplayEntry> &r_entries) {
		const uint32_t frame = 0;
		std::vector<std::vector<SwfDisplayEntry>> lists;
		BF3_TRY(display_lists_at(p_tl, std::span<const uint32_t>(&frame, 1), budget_, lists));
		r_entries = std::move(lists[0]);
		return Status::success();
	}

	Status children(const std::shared_ptr<const SwfMovie> &p_movie, const std::string &p_name, const SwfTimeline &p_tl, uint32_t p_depth, SwfLayoutNode &r_parent) {
		std::vector<SwfDisplayEntry> entries;
		BF3_TRY(frame0(p_tl, entries));
		for (const SwfDisplayEntry &e : entries) {
			SwfLayoutNode child;
			child.instance = std::string(e.name);
			child.depth = e.depth;
			child.local = e.matrix;
			child.stage = swf_concat(r_parent.stage, e.matrix);
			BF3_TRY(node(p_movie, p_name, e.character, p_depth, child));
			r_parent.children.push_back(std::move(child));
		}
		return Status::success();
	}

	Status node(const std::shared_ptr<const SwfMovie> &p_movie, const std::string &p_name, uint16_t p_id, uint32_t p_depth, SwfLayoutNode &r_node) {
		if (++nodes_ > kSwfLayoutMaxNodes) {
			return make_error("swf layout: more than " + std::to_string(kSwfLayoutMaxNodes) + " nodes");
		}
		BF3_TRY(charge(1 + r_node.instance.size() / 16));
		const SwfMovie &m = *p_movie;
		r_node.movie = p_name;
		r_node.symbol = symbol_of(m, p_id);
		if (m.find_shape(p_id) != nullptr) {
			r_node.kind = SwfLayoutKind::Shape;
			return Status::success();
		}
		if (const SwfEditText *t = m.find_edit_text(p_id)) {
			r_node.kind = SwfLayoutKind::EditText;
			SwfLayoutText &x = r_node.text;
			if (!t->font_class.empty()) {
				x.font = t->font_class;
			} else if (t->has(SwfEditText::kHasFont)) {
				const SwfExport *sym = nullptr;
				if (const SwfFontInfo *f = m.find_font(t->font_id)) {
					x.font = f->name;
				} else if (m.find_import(t->font_id, &sym) != nullptr && sym != nullptr) {
					x.font = sym->name;
				}
			}
			x.size = t->height;
			x.color = t->color;
			x.align = t->align;
			x.bounds = t->bounds;
			x.initial_text = t->initial_text;
			x.html = t->has(SwfEditText::kHtml);
			BF3_TRY(charge(1 + (x.font.size() + x.initial_text.size()) / 16));
			return Status::success();
		}
		if (std::binary_search(m.static_texts.begin(), m.static_texts.end(), p_id)) {
			r_node.kind = SwfLayoutKind::StaticText;
			return Status::success();
		}
		if (const SwfSprite *s = m.find_sprite(p_id)) {
			r_node.kind = SwfLayoutKind::Sprite;
			for (const SwfScalingGrid &g : m.scaling_grids) {
				if (g.id == p_id) {
					r_node.has_scaling_grid = true;
					r_node.scaling_grid = g.splitter;
				}
			}
			if (p_depth + 1 < options_.max_depth) {
				BF3_TRY(children(p_movie, p_name, s->timeline, p_depth + 1, r_node));
			}
			return Status::success();
		}
		const SwfExport *sym = nullptr;
		if (const SwfImport *im = m.find_import(p_id, &sym); im != nullptr && sym != nullptr) {
			r_node.kind = SwfLayoutKind::Import;
			r_node.symbol = sym->name;
			const std::string target = swf_movie_name_for_url(im->url);
			if (!options_.follow_imports || target.empty() || p_depth + 1 >= options_.max_depth || movies_.find(target) == nullptr) {
				return Status::success();
			}
			Result<std::shared_ptr<const SwfMovie>> other = movies_.load(target);
			if (!other) {
				return Status::success();
			}
			const std::optional<uint16_t> id = (*other)->export_id(sym->name);
			if (!id) {
				return Status::success();
			}
			--nodes_;
			return node(*other, target, *id, p_depth + 1, r_node);
		}
		r_node.kind = SwfLayoutKind::Other;
		return Status::success();
	}

private:
	const SwfLibrary &movies_;
	const SwfLayoutOptions &options_;
	WorkBudget &budget_;
	size_t nodes_ = 0;
};

}

const char *hud_anchor_name(HudAnchor p_anchor) {
	switch (p_anchor) {
		case HudAnchor::TopLeft: return "top_left";
		case HudAnchor::Top: return "top";
		case HudAnchor::TopRight: return "top_right";
		case HudAnchor::Left: return "left";
		case HudAnchor::Centre: return "centre";
		case HudAnchor::Right: return "right";
		case HudAnchor::BottomLeft: return "bottom_left";
		case HudAnchor::Bottom: return "bottom";
		case HudAnchor::BottomRight: return "bottom_right";
	}
	return "?";
}

HudAnchor hud_anchor_for(float p_x, float p_y, const SwfRect &p_stage) {
	const float w = p_stage.width();
	const float h = p_stage.height();
	const int col = p_x < p_stage.x_min + w / 3.0f ? 0 : (p_x > p_stage.x_min + 2.0f * w / 3.0f ? 2 : 1);
	const int row = p_y < p_stage.y_min + h / 3.0f ? 0 : (p_y > p_stage.y_min + 2.0f * h / 3.0f ? 2 : 1);
	return HudAnchor(row * 3 + col);
}

std::span<const std::string_view> hud_screen_movies() {
	return kHudScreens;
}

Result<SwfLayoutNode> read_layout(const SwfLibrary &p_movies, std::string_view p_movie, const SwfLayoutOptions &p_options, WorkBudget &r_budget) {
	const std::string name = movie_key(p_movie);
	Result<std::shared_ptr<const SwfMovie>> movie = p_movies.load(name);
	if (!movie) {
		return movie.error();
	}
	LayoutBuilder b(p_movies, p_options, r_budget);
	SwfLayoutNode root;
	root.kind = SwfLayoutKind::Sprite;
	root.movie = name;
	root.symbol = "_root";
	if (p_options.max_depth > 0) {
		Status st = b.children(*movie, name, (*movie)->root, 0, root);
		if (!st) {
			return error_at("swf layout " + name, st.error());
		}
	}
	return root;
}

Result<std::vector<HudPlacement>> read_hud_placements(const SwfLibrary &p_movies, std::string_view p_movie, WorkBudget &r_budget) {
	const std::string name = movie_key(p_movie);
	Result<std::shared_ptr<const SwfMovie>> loaded = p_movies.load(name);
	if (!loaded) {
		return loaded.error();
	}
	const SwfMovie &m = **loaded;
	std::vector<HudPlacement> out;
	const uint32_t frame = 0;
	std::vector<std::vector<SwfDisplayEntry>> lists;
	BF3_TRY(display_lists_at(m.root, std::span<const uint32_t>(&frame, 1), r_budget, lists));

	const SwfDisplayEntry *screen = nullptr;
	for (const SwfDisplayEntry &e : lists[0]) {
		if (m.find_sprite(e.character) != nullptr && (screen == nullptr || e.name == "instance1")) {
			if (screen == nullptr || e.name == "instance1") {
				screen = &e;
			}
		}
	}
	if (screen == nullptr) {
		return out;
	}
	const SwfSprite *sprite = m.find_sprite(screen->character);
	std::vector<std::vector<SwfDisplayEntry>> inner;
	BF3_TRY(display_lists_at(sprite->timeline, std::span<const uint32_t>(&frame, 1), r_budget, inner));
	for (const SwfDisplayEntry &e : inner[0]) {
		BF3_TRY(r_budget.charge(1 + e.name.size() / 16));
		HudPlacement p;
		p.movie = name;
		p.stage = m.stage;
		p.screen_symbol = symbol_of(m, screen->character);
		p.instance = std::string(e.name);
		const SwfExport *sym = nullptr;
		if (const SwfImport *im = m.find_import(e.character, &sym); im != nullptr && sym != nullptr) {
			p.widget = sym->name;
			p.widget_movie = swf_movie_name_for_url(im->url);
		} else {
			p.widget = symbol_of(m, e.character);
			p.widget_movie = name;
		}
		p.depth = e.depth;
		p.matrix = swf_concat(screen->matrix, e.matrix);
		p.anchor = hud_anchor_for(p.matrix.tx, p.matrix.ty, m.stage);
		const int col = int(p.anchor) % 3;
		const int row = int(p.anchor) / 3;
		const float ax = col == 0 ? m.stage.x_min : (col == 1 ? (m.stage.x_min + m.stage.x_max) / 2.0f : m.stage.x_max);
		const float ay = row == 0 ? m.stage.y_min : (row == 1 ? (m.stage.y_min + m.stage.y_max) / 2.0f : m.stage.y_max);
		p.anchor_dx = p.matrix.tx - ax;
		p.anchor_dy = p.matrix.ty - ay;
		out.push_back(std::move(p));
	}
	return out;
}

}
