#include "hud_killfeed.h"

#include "bf3/host/hud_feed.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace bf3::godot_side {

using namespace godot;
namespace ui = bf3::core::ui;

namespace {

void draw_headshot(CanvasItem &p_item, Vector2 p_c, float p_size, const Color &p_colour) {
	const float r = 0.5f * p_size;
	p_item.draw_arc(p_c, r * 0.8f, 0.0f, 6.2831853f, 20, p_colour, std::max(1.0f, p_size / 8.0f), true);
	p_item.draw_circle(p_c, std::max(1.0f, r * 0.25f), p_colour);
}

}

void Bf3TdmKillFeed::update_screen() {
	const ui::HudModel &m = ctx_->model;
	const bool any = m.messages.kill_message.newest_serial != 0;
	if (!any) {
		ctx_->records.kill_feed_lines = 0;
	}
	show_if(any);
}

void Bf3TdmKillFeed::draw_screen() {
	TdmHudContext &c = *ctx_;
	const ui::HudModel &m = c.model;
	TdmHudRecords &rec = c.records;
	const ui::HudKillFeed &feed = m.messages.kill_message;
	const double now = TdmHudContext::now_s();
	const float s = tdm_scale(*this);
	const TdmPlacement p = c.placement(kTdmKillFeedPlacement);
	const Ref<Font> font = c.font(TdmFont::Text);
	const int32_t size = tdm_px(kKillFeedNameSize, s);
	const float line_h = kKillFeedLineHeight * s;
	const float gap = kKillFeedGap * s;
	const Vector2 icon_size(kKillFeedIconWidth * s, kKillFeedIconHeight * s);
	const float ascent = font.is_valid() ? float(font->get_ascent(size)) : float(size);
	const bool covered = m.team.scoreboard_enabled;
	uint8_t drawn = 0;
	rec.kill_feed_newest_serial = feed.newest_serial;

	for (size_t k = 0; k < feed.lines.size(); ++k) {
		if (feed.newest_serial < k + 1) {
			break;
		}
		const uint32_t serial = feed.newest_serial - uint32_t(k);
		const ui::HudKill &line = feed.lines[serial % feed.lines.size()];
		if (line.serial != serial) {
			continue;
		}
		Seen &seen = seen_[serial % seen_.size()];
		if (seen.serial != serial) {
			seen.serial = serial;
			seen.first_s = now;
			++rec.kill_feed_lines_started;
		}
		const double age = now - seen.first_s;
		if (age >= host::kKillFeedSeconds || covered) {
			continue;
		}
		const float alpha = age > host::kKillFeedSeconds - kKillFeedFadeSeconds ? float((host::kKillFeedSeconds - age) / kKillFeedFadeSeconds) : 1.0f;
		const std::string killer = line.suicide ? std::string() : tdm_player_name(m, line.killer);
		const std::string victim = tdm_player_name(m, line.victim);
		const Ref<Texture2D> icon = c.weapon_icon(line.item_id);

		const float wk = killer.empty() ? 0.0f : tdm_text_width(font, killer, size) + gap;
		const float wi = icon.is_valid() ? icon_size.x + gap : 0.0f;
		const float wh = line.headshot ? icon_size.y + gap : 0.0f;
		const float wv = tdm_text_width(font, victim, size);
		const float total = wk + wi + wh + wv;
		const Vector2 right = tdm_at(*this, p.anchor, p.x - kKillFeedRightInset, p.y + kKillFeedTop);
		const float top = right.y + float(drawn) * line_h;
		float x = right.x - total;
		const bool own = line.killer == m.local_player || line.victim == m.local_player;
		if (own && m.local_player != ui::kHudNoPlayer) {
			Color tint = line.victim == m.local_player ? kTdmEnemyColour : kTdmLocalColour;
			tint.a = 0.22f * alpha;
			draw_rect(Rect2(x - gap, top - 0.1f * line_h, total + 2.0f * gap, line_h * 0.95f), tint, true);
		}
		const float baseline = top + 0.5f * line_h + 0.5f * ascent - 0.1f * line_h;
		if (!killer.empty()) {
			Color col = tdm_player_colour(m, line.killer);
			col.a = alpha;
			x += tdm_draw_text(*this, font, Vector2(x, baseline), killer, size, col) + gap;
		}
		if (icon.is_valid()) {
			const float iy = top + 0.5f * line_h - 0.5f * icon_size.y - 0.1f * line_h;
			draw_texture_rect(icon, Rect2(Vector2(x, iy), icon_size), false, Color(1.0f, 1.0f, 1.0f, alpha));
			x += wi;
		}
		if (line.headshot) {
			Color col = kTdmTextColour;
			col.a = alpha;
			draw_headshot(*this, Vector2(x + 0.5f * icon_size.y, top + 0.4f * line_h), icon_size.y, col);
			x += wh;
		}
		Color vcol = tdm_player_colour(m, line.victim);
		vcol.a = alpha;
		tdm_draw_text(*this, font, Vector2(x, baseline), victim, size, vcol);
		++drawn;
	}
	rec.kill_feed_lines = drawn;
}

}
