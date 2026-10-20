#include <doctest/doctest.h>

#include "test_support.h"
#include "ui2_test_support.h"

#include "iff_frame.h"

#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/kit_appearance.h"
#include "bf3/core/gameplay/kit_def.h"
#include "bf3/core/superbundle_mount.h"
#include "bf3/core/ui/nametags.h"
#include "bf3/host/client_session.h"
#include "bf3/net/protocol.h"
#include "bf3/render/soldier_assets.h"
#include "bf3/sim/soldier_state.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace ui = bf3::core::ui;
namespace gp = bf3::core::gameplay;
namespace host = bf3::host;
namespace net = bf3::net;
namespace gs = bf3::godot_side;
namespace t2 = bf3::test::ui2;
using bf3::core::EbxLibrary;
using bf3::core::EbxPartition;
using bf3::core::EbxResolver;
using bf3::core::Result;

namespace {

#define RC(p_expr) CHECK(rc_(bool(p_expr)))
#define RCF(p_expr) CHECK(rc_(!bool(p_expr)))

void row(const std::string &p_name, bool p_pass, const std::string &p_detail) {
	std::printf("IFF ROW %s %s %s\n", p_name.c_str(), p_pass ? "PASS" : "FAIL", p_detail.c_str());
	std::fflush(stdout);
}

std::string fmt(const char *p_format, double p_a = 0, double p_b = 0, double p_c = 0, double p_d = 0) {
	char b[256];
	std::snprintf(b, sizeof(b), p_format, p_a, p_b, p_c, p_d);
	return b;
}

ui::NametagSubject subject(uint8_t p_team, float p_distance, float p_crosshair = 1e9f, bool p_los = false) {
	ui::NametagSubject s;
	s.slot = 5;
	s.team = p_team;
	s.alive = true;
	s.in_front = true;
	s.distance_m = p_distance;
	s.crosshair_stage = p_crosshair;
	s.line_of_sight = p_los;
	return s;
}

constexpr uint64_t kNs0 = 1'000'000'000ull;

net::TimedOther sample(uint32_t p_tick, uint8_t p_slot, uint8_t p_team, float p_x, bool p_alive, bool p_firing, uint8_t p_generation = 1) {
	net::TimedOther t;
	t.server_tick = p_tick;
	t.soldier.slot = p_slot;
	t.soldier.generation = p_generation;
	t.soldier.position = { p_x, 0.0f, 10.0f };
	t.soldier.flags = p_alive ? uint8_t(bf3::sim::soldier_flags::kAlive) : uint8_t(0);
	t.soldier.weapon = 0;
	t.soldier.firing = p_firing;
	t.soldier.team = p_team;
	return t;
}

struct Recorded {
	host::PresentationFrame frame;
	uint64_t now_ns = kNs0;
};

void publish(Recorded &r_rec, uint32_t p_tick, const std::function<net::TimedOther(uint8_t, uint32_t)> &p_state) {
	host::PresentationFrame &f = r_rec.frame;
	f.valid = true;
	++f.serial;
	f.publish_ns = r_rec.now_ns;
	f.render_ticks_at_publish = double(p_tick) - 1.0;
	f.newest_tick = p_tick;
	f.own_team = 1;
	f.match_valid = true;
	f.other_count = 0;
	for (uint8_t slot = 1; slot < 8; ++slot) {
		host::OtherTrack &t = f.others[f.other_count++];
		t.slot = slot;
		t.count = 2;
		t.samples[0] = p_state(slot, p_tick - 1);
		t.samples[1] = p_state(slot, p_tick);
		t.track_newest_tick = p_tick;
	}
	f.player_count = 8;
	for (uint8_t slot = 0; slot < 8; ++slot) {
		net::PlayerRow &p = f.players[slot];
		p.slot = slot;
		p.team = slot < 4 ? 1 : 2;
		p.kit = slot % 4;
		p.bot = slot != 0;
		p.alive = true;
		const std::string name = "bot" + std::to_string(slot);
		p.name_bytes = uint8_t(name.size());
		std::memcpy(p.name.data(), name.data(), name.size());
	}
}

}

TEST_SUITE("pt1-iff-unit") {

TEST_CASE("the relation of a soldier to the viewer") {
	bool ok = true;
	auto rc_ = [&](bool p_c) { ok = ok && p_c; return p_c; };
	const ui::NametagViewer us{ 1, 0 };
	RC(ui::tag_relation(us, 1, 0) == ui::TagRelation::Friendly);
	RC(ui::tag_relation(us, 2, 0) == ui::TagRelation::Enemy);
	RC(ui::tag_relation(us, 0, 0) == ui::TagRelation::None);
	RC(ui::tag_relation(ui::NametagViewer{ 0, 0 }, 1, 0) == ui::TagRelation::None);

	RC(ui::tag_relation(ui::NametagViewer{ 1, 3 }, 1, 3) == ui::TagRelation::Squad);
	RC(ui::tag_relation(ui::NametagViewer{ 1, 3 }, 1, 2) == ui::TagRelation::Friendly);
	RC(ui::tag_relation(ui::NametagViewer{ 1, 0 }, 1, 0) == ui::TagRelation::Friendly);
	RC(ui::tag_relation(ui::NametagViewer{ 1, 3 }, 2, 3) == ui::TagRelation::Enemy);
	const ui::NametagStyle st;
	RC(ui::tag_tint(st, ui::TagRelation::Friendly) == st.friendly_tint);
	RC(ui::tag_tint(st, ui::TagRelation::Squad) == st.squad_tint);
	RC(ui::tag_tint(st, ui::TagRelation::Enemy) == st.enemy_tint);
	RC(ui::tag_tint(st, ui::TagRelation::None)[0] == 1.0f);
	row("relation", ok, "team 1 viewer: team 1 Friendly, team 2 Enemy, team 0 None; squads only when shared and non-zero");
}

TEST_CASE("the look-at radius runs from MaxLookAtThreshold at 0 m to MinLookAtThreshold at 50 m") {
	bool ok = true;
	auto rc_ = [&](bool p_c) { ok = ok && p_c; return p_c; };
	const ui::NametagStyle st;
	RC(ui::look_at_threshold(st, 0.0f) == doctest::Approx(240.0f));
	RC(ui::look_at_threshold(st, 25.0f) == doctest::Approx(140.0f));
	RC(ui::look_at_threshold(st, 50.0f) == doctest::Approx(40.0f));
	RC(ui::look_at_threshold(st, 500.0f) == doctest::Approx(40.0f));
	RC(ui::look_at_threshold(st, -3.0f) == doctest::Approx(240.0f));
	RC(ui::look_at_threshold(st, std::numeric_limits<float>::quiet_NaN()) == doctest::Approx(40.0f));
	ui::NametagStyle zero = st;
	zero.min_look_at_threshold_distance = 0.0f;
	RC(ui::look_at_threshold(zero, 1.0f) == doctest::Approx(40.0f));
	row("look_at_threshold", ok, fmt("%.0f / %.0f / %.0f / %.0f stage units at 0 / 25 / 50 / 500 m", ui::look_at_threshold(st, 0), ui::look_at_threshold(st, 25),
										   ui::look_at_threshold(st, 50), ui::look_at_threshold(st, 500)));
}

TEST_CASE("nametags: a teammate's icon always, the name within ShowLabelRange or when looked at") {
	bool ok = true;
	auto rc_ = [&](bool p_c) { ok = ok && p_c; return p_c; };
	const ui::NametagStyle st;
	const ui::NametagViewer us{ 1, 0 };

	ui::NametagDecision d = ui::decide_nametag(st, us, subject(1, 6.0f));
	RC(d.relation == ui::TagRelation::Friendly);
	RC(d.icon);
	RC(d.name);
	RCF(d.needs_line_of_sight);
	RC(d.alpha == doctest::Approx(1.0f - 6.0f / 10000.0f));
	RC(d.scale == doctest::Approx(1.0f));

	RC(ui::decide_nametag(st, us, subject(1, 15.0f)).name);
	d = ui::decide_nametag(st, us, subject(1, 35.0f));
	RC(d.icon);
	RCF(d.name);

	RC(ui::decide_nametag(st, us, subject(1, 35.0f, 99.0f)).name);
	RCF(ui::decide_nametag(st, us, subject(1, 35.0f, 101.0f)).name);

	RCF(ui::decide_nametag(st, us, subject(1, 35.0f, 0.0f, false)).needs_line_of_sight);
	RC(ui::decide_nametag(st, us, subject(1, 35.0f, 0.0f, false)).name);

	RC(ui::decide_nametag(st, us, subject(1, 900.0f)).icon);

	ui::NametagSubject dead = subject(1, 6.0f);
	dead.alive = false;
	RCF(ui::decide_nametag(st, us, dead).icon);
	ui::NametagSubject behind = subject(1, 6.0f);
	behind.in_front = false;
	RCF(ui::decide_nametag(st, us, behind).icon);
	RCF(ui::decide_nametag(st, us, subject(1, 10001.0f)).icon);
	row("nametag.friendly", ok, "6 m icon+name, 15 m icon+name, 35 m icon only (name when within 100 stage units of the crosshair), 900 m icon, dead/behind none");
}

TEST_CASE("nametags: an enemy shows nothing but its name while looked at in plain sight") {
	bool ok = true;
	auto rc_ = [&](bool p_c) { ok = ok && p_c; return p_c; };
	const ui::NametagStyle st;
	const ui::NametagViewer us{ 1, 0 };
	for (const float dist : { 6.0f, 15.0f, 35.0f }) {
		const ui::NametagDecision away = ui::decide_nametag(st, us, subject(2, dist));
		RC(away.relation == ui::TagRelation::Enemy);
		RCF(away.icon);
		RCF(away.name);
		RCF(away.needs_line_of_sight);
		const ui::NametagDecision hidden = ui::decide_nametag(st, us, subject(2, dist, 0.0f, false));
		RCF(hidden.icon);
		RCF(hidden.name);
		RC(hidden.needs_line_of_sight);
		const ui::NametagDecision seen = ui::decide_nametag(st, us, subject(2, dist, 0.0f, true));
		RCF(seen.icon);
		RC(seen.name);
	}

	RCF(ui::decide_nametag(st, us, subject(2, 35.0f, 150.0f, true)).name);

	const ui::NametagDecision unknown = ui::decide_nametag(st, us, subject(0, 6.0f, 0.0f, true));
	RC(unknown.relation == ui::TagRelation::None);
	RCF(unknown.icon);
	RCF(unknown.name);
	RCF(unknown.needs_line_of_sight);
	row("nametag.enemy", ok, "6 / 15 / 35 m: no icon; name only with the crosshair on it and a clear ray; team 0: nothing");
}

TEST_CASE("nametags: hostile numbers never tag and never trap") {
	bool ok = true;
	auto rc_ = [&](bool p_c) { ok = ok && p_c; return p_c; };
	const ui::NametagViewer us{ 1, 0 };
	const float nan = std::numeric_limits<float>::quiet_NaN();
	const float inf = std::numeric_limits<float>::infinity();
	ui::NametagStyle st;
	for (const float dist : { nan, inf, -inf, -1.0f }) {
		const ui::NametagDecision d = ui::decide_nametag(st, us, subject(1, dist, 0.0f, true));
		RCF(d.icon);
		RCF(d.name);
	}

	RCF(ui::decide_nametag(st, us, subject(2, 6.0f, nan, true)).name);

	st.fade_distance = 10.0f;
	st.fade_end_distance = 10.0f;
	st.max_far_fade = 0.25f;
	st.max_scale_mod = 1.0f;
	st.scale_distance = 0.0f;
	const ui::NametagDecision d = ui::decide_nametag(st, us, subject(1, 20.0f));
	RC(d.alpha == doctest::Approx(0.25f));
	RC(d.scale >= 0.0f);
	RC(d.scale <= 1.0f);
	row("nametag.hostile", ok, "NaN / inf / negative distances draw nothing; NaN crosshair = not looked at; degenerate fade and scale stay in [0, 1]");
}

TEST_CASE("the minimap: teammates while alive, an enemy only inside its fire-reveal window") {
	bool ok = true;
	auto rc_ = [&](bool p_c) { ok = ok && p_c; return p_c; };
	const ui::NametagViewer us{ 1, 0 };
	const ui::SpottingRules sp;
	RC(sp.fire_reveal_seconds() == doctest::Approx(3.15f));
	RC(ui::minimap_relation(us, 1, 0, true, 10.0, 0.0) == ui::TagRelation::Friendly);
	RC(ui::minimap_relation(us, 1, 0, false, 10.0, 0.0) == ui::TagRelation::None);
	RC(ui::minimap_relation(us, 2, 0, true, 10.0, 0.0) == ui::TagRelation::None);
	RC(ui::minimap_relation(us, 2, 0, true, 10.0, 10.0 + sp.fire_reveal_seconds()) == ui::TagRelation::Enemy);
	RC(ui::minimap_relation(us, 2, 0, true, 13.2, 13.15) == ui::TagRelation::None);
	RC(ui::minimap_relation(us, 2, 0, false, 10.0, 99.0) == ui::TagRelation::None);
	RC(ui::minimap_relation(us, 0, 0, true, 10.0, 99.0) == ui::TagRelation::None);
	RC(ui::minimap_relation(us, 2, 0, true, std::numeric_limits<double>::quiet_NaN(), 99.0) == ui::TagRelation::None);
	row("minimap.relation", ok, "teammates alive: Friendly; enemies: Enemy until shot + 3.15 s (ActiveSpottedTime 7 x SpotOnFireMultiplier 0.45), else hidden");
}

TEST_CASE("the kit's appearance by name") {
	bool ok = true;
	auto rc_ = [&](bool p_c) { ok = ok && p_c; return p_c; };
	struct Case {
		const char *kit, *appearance;
		bool match;
	};
	const Case cases[] = {
		{ "gameplay/kits/usassault", "persistence/unlocks/soldiers/visual/mp/us/mp_us_assault_appearance01", true },
		{ "gameplay/kits/usengineer", "persistence/unlocks/soldiers/visual/mp/us/mp_us_engi_appearance01", true },
		{ "gameplay/kits/ussupport", "persistence/unlocks/soldiers/visual/mp/us/mp_us_support_appearance01", true },
		{ "gameplay/kits/usrecon", "persistence/unlocks/soldiers/visual/mp/us/mp_us_recon_appearance01", true },
		{ "gameplay/kits/ruassault", "persistence/unlocks/soldiers/visual/mp/ru/mp_ru_assault_appearance01", true },
		{ "gameplay/kits/ruengineer", "persistence/unlocks/soldiers/visual/mp/ru/mp_ru_engi_appearance01", true },
		{ "gameplay/kits/rusupport", "persistence/unlocks/soldiers/visual/mp/ru/mp_ru_support_appearance01", true },
		{ "Gameplay/Kits/RURecon", "Persistence/Unlocks/Soldiers/Visual/MP/RU/MP_RU_Recon_Appearance02", true },

		{ "gameplay/kits/usassault", "persistence/unlocks/soldiers/visual/mp/ru/mp_ru_assault_appearance01", false },
		{ "gameplay/kits/usassault", "persistence/unlocks/soldiers/visual/mp/us/mp_us_engi_appearance01", false },
		{ "gameplay/kits/usengineer", "persistence/unlocks/soldiers/visual/mp/us/mp_us_assault_appearance01", false },
		{ "gameplay/kits/usassault", "mp_us_assault", false },
		{ "gameplay/kits/usassault", "sp_us_assault_appearance01", false },
		{ "gameplay/kits/usassault", "mp_us__appearance01", false },
		{ "gameplay/kits/us", "mp_us_assault_appearance01", false },
		{ "", "", false },
		{ "gameplay/kits/usassault", "mp_us_assault_head01", false },
	};
	size_t matched = 0;
	for (const Case &c : cases) {
		const bool got = gp::appearance_matches_kit(c.kit, c.appearance);
		CHECK_MESSAGE(rc_(got == c.match), c.kit, " ", c.appearance);
		matched += got == c.match ? 1 : 0;
	}
	row("kit_appearance.naming", ok, std::to_string(matched) + " / " + std::to_string(std::size(cases)) + " cases (8 kits, 9 negatives)");
}

TEST_CASE("recorded frames: teams, kits and names per soldier; the fire reveal; a death and a respawn") {
	bool ok = true;
	auto rc_ = [&](bool p_c) { ok = ok && p_c; return p_c; };
	Recorded rec;

	auto state = [](uint8_t p_slot, uint32_t p_tick) {
		const uint8_t team = p_slot == 7 ? uint8_t(0) : (p_slot < 4 ? uint8_t(1) : uint8_t(2));
		const bool firing = p_slot == 5 && p_tick == 110;
		const bool dead = p_slot == 6 && p_tick >= 120 && p_tick < 130;
		const uint8_t generation = p_slot == 6 && p_tick >= 130 ? uint8_t(2) : uint8_t(1);
		return sample(p_tick, p_slot, team, float(p_slot) * 2.0f, !dead, firing, generation);
	};
	gs::IffReveal reveal;
	const ui::SpottingRules sp;
	const ui::NametagViewer viewer{ 1, 0 };
	struct Seen {
		uint32_t tick;
		size_t friendly, enemy, hidden;
		double revealed_until_5;
	};
	std::vector<Seen> log;
	std::array<gs::IffSoldier, net::kMaxSlots> soldiers{};
	size_t m1_red = 0, m1_red_teammates = 0, drawn_total = 0;
	for (uint32_t tick = 100; tick <= 140; ++tick) {
		rec.now_ns = kNs0 + uint64_t(tick - 100) * 33'333'333ull;
		publish(rec, tick, state);
		const uint64_t draw_ns = rec.now_ns + 4'166'667ull;
		const size_t n = gs::iff_soldiers(rec.frame, draw_ns, soldiers);
		REQUIRE(rc_(n == 7));
		const double now_s = double(draw_ns) * 1e-9;
		reveal.update(std::span<const gs::IffSoldier>(soldiers.data(), n), now_s, sp.fire_reveal_seconds());
		std::array<gs::IffMinimapEntry, net::kMaxSlots> mm{};
		gs::IffMinimapCounts c;
		const size_t m = gs::iff_minimap(std::span<const gs::IffSoldier>(soldiers.data(), n), viewer, reveal, now_s, mm, &c);
		RC(m == c.friendly + c.enemy + c.squad);
		for (size_t i = 0; i < n; ++i) {
			const gs::IffSoldier &s = soldiers[i];
			RC(s.team == (s.slot < 4 ? 1 : 2));
			RC(s.kit == s.slot % 4);
			RC(s.bot);
			RC(s.name == "bot" + std::to_string(s.slot));
			RC(s.squad == 0);
		}

		for (size_t i = 0; i < m; ++i) {
			RC((mm[i].slot < 4) == (mm[i].relation == ui::TagRelation::Friendly));
		}
		log.push_back({ tick, c.friendly, c.enemy, c.hidden_enemies, reveal.revealed_until(5) });

		m1_red += n;
		for (size_t i = 0; i < n; ++i) {
			m1_red_teammates += soldiers[i].team == viewer.team ? 1 : 0;
		}
		drawn_total += m;
	}

	for (const Seen &s : log) {
		RC(s.friendly == 3);
	}

	size_t revealed_frames = 0;
	for (const Seen &s : log) {
		if (s.tick < 111) {
			RC(s.enemy == 0);
		}
		revealed_frames += s.enemy;
	}
	RC(reveal.reveals() == 1);
	RC(revealed_frames == 30);

	size_t dead_frames = 0;
	for (const Seen &s : log) {
		dead_frames += (s.hidden + s.enemy) == 3 ? 1 : 0;
	}
	RC(dead_frames == 10);
	row("frames.iff_soldiers", ok, "41 frames x 7 others: team from the sample (row for slot 7), kit, bot, name from the row");
	row("frames.reveal", ok,
			"1 shot -> " + std::to_string(revealed_frames) + " frames revealed (3.15 s at 30 Hz, clipped by the run); teammates 3 every frame; dead slot 6 drawn in no frame");

	RC(m1_red == 41 * 7);
	RC(m1_red_teammates == 41 * 3);
	RC(drawn_total == 41 * 3 + revealed_frames);
	row("frames.before_after", ok,
			"41 frames: the M1 loop drew " + std::to_string(m1_red) + " enemy arrows (" + std::to_string(m1_red_teammates) + " of them teammates); now " +
					std::to_string(drawn_total - revealed_frames) + " friendly + " + std::to_string(revealed_frames) + " revealed-enemy arrows, 0 teammates as enemies");

	host::PresentationFrame bad = rec.frame;
	bad.player_count = 255;
	bad.players[2].team = 3;
	bad.players[2].kit = 9;
	bad.players[2].name_bytes = 255;
	const size_t n = gs::iff_soldiers(bad, rec.now_ns, soldiers);
	RC(n == 7);
	for (size_t i = 0; i < n; ++i) {
		RC(soldiers[i].team <= 2);
		RC((soldiers[i].kit < 4 || soldiers[i].kit == gs::kIffNoKit));
		RC(soldiers[i].name.size() <= net::kMaxPlayerNameBytes);
	}
	bad.valid = false;
	RC(gs::iff_soldiers(bad, rec.now_ns, soldiers) == 0);
	RC(gs::iff_soldiers(rec.frame, rec.now_ns, std::span<gs::IffSoldier>()) == 0);
	gs::IffReveal r2;
	r2.update(std::span<const gs::IffSoldier>(soldiers.data(), 7), 1.0, std::numeric_limits<float>::quiet_NaN());
	RC(r2.revealed_until(200) == 0.0);
	row("frames.hostile", ok, "player_count 255, team 3, kit 9, name_bytes 255: clamped; invalid frame and empty output: 0 soldiers; NaN reveal: 0 s");
}

}

namespace {

constexpr const char *kLevel = "Win32/Levels/MP_Subway/MP_Subway";

struct Lib {
	std::unique_ptr<EbxLibrary> lib;
	std::string error;
};

const EbxLibrary *metro() {
	t2::InstallFixture *f = t2::install();
	if (f == nullptr) {
		return nullptr;
	}
	static Lib *l = [&] {
		Lib *out = new Lib();
		Result<std::unique_ptr<EbxLibrary>> opened = EbxLibrary::open(f->install, { kLevel });
		if (opened) {
			out->lib = std::move(*opened);
		} else {
			out->error = opened.error().message;
		}
		return out;
	}();
	REQUIRE_MESSAGE(l->lib != nullptr, l->error);
	return l->lib.get();
}

using Bytes = std::vector<uint8_t>;

void mutate(Bytes &r_bytes, std::mt19937_64 &r_rng) {
	if (r_bytes.empty()) {
		return;
	}
	const unsigned mode = unsigned(r_rng() % 6);
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
		} else if (mode == 4 && at + 4 <= r_bytes.size()) {
			const float f = (r_rng() % 3 == 0) ? std::nanf("") : (r_rng() % 2 ? 1e30f : -1.0f);
			std::memcpy(r_bytes.data() + at, &f, 4);
		} else {
			r_bytes[at] = 0;
		}
	}
}

class OverlayResolver final : public EbxResolver {
public:
	OverlayResolver(const EbxResolver &p_base, std::string p_name, const EbxPartition &p_partition) :
			base_(p_base), name_(std::move(p_name)), partition_(p_partition) {}
	const EbxPartition *partition(const bf3::core::Guid &p_guid) const override {
		return p_guid == partition_.guid() ? &partition_ : base_.partition(p_guid);
	}
	const EbxPartition *partition_by_name(std::string_view p_name) const override {
		return bf3::core::iequals_ascii(p_name, name_) ? &partition_ : base_.partition_by_name(p_name);
	}
	std::optional<std::string_view> name_of(const bf3::core::Guid &p_guid) const override {
		return p_guid == partition_.guid() ? std::optional<std::string_view>(name_) : base_.name_of(p_guid);
	}

private:
	const EbxResolver &base_;
	std::string name_;
	const EbxPartition &partition_;
};

struct Tally {
	size_t mutants = 0, parsed = 0, read_ok = 0;
};

Tally fuzz_partition(const EbxLibrary &p_lib, const std::string &p_name, uint64_t p_seed, size_t p_iters, const std::function<bool(const EbxResolver &)> &p_read) {
	Tally t;
	Result<std::vector<uint8_t>> bytes = p_lib.mount(0).read_ebx(p_name);
	REQUIRE_MESSAGE(bytes.ok(), p_name);
	{
		Result<EbxPartition> original = EbxPartition::parse(*bytes);
		REQUIRE(original.ok());
		const OverlayResolver overlay(p_lib, p_name, *original);
		REQUIRE_MESSAGE(p_read(overlay), p_name);
	}
	std::mt19937_64 rng(p_seed);
	for (size_t i = 0; i < p_iters; ++i) {
		Bytes m = *bytes;
		mutate(m, rng);
		++t.mutants;
		Result<EbxPartition> parsed = EbxPartition::parse(std::move(m));
		if (!parsed) {
			continue;
		}
		++t.parsed;
		const OverlayResolver overlay(p_lib, p_name, *parsed);
		t.read_ok += p_read(overlay) ? 1 : 0;
	}
	return t;
}

std::string tally_text(const std::string &p_what, const Tally &p_t) {
	return p_what + " " + std::to_string(p_t.mutants) + " mutants, " + std::to_string(p_t.parsed) + " parsed, " + std::to_string(p_t.read_ok) + " read";
}

}

TEST_SUITE("pt1-iff-install") {

TEST_CASE("UINametagCompData and the soldier's spotting rules read to their retail values") {
	const EbxLibrary *lib = metro();
	if (lib == nullptr) {
		return;
	}
	Result<ui::NametagStyle> st = ui::read_nametag_style(*lib);
	REQUIRE_MESSAGE(st.ok(), (st ? "" : st.error().message));
	const ui::NametagStyle d;
	CHECK(st->from_data);
	CHECK(st->clamped == 0);
	for (size_t i = 0; i < 4; ++i) {
		CHECK(st->friendly_tint[i] == doctest::Approx(d.friendly_tint[i]).epsilon(1e-4));
		CHECK(st->squad_tint[i] == doctest::Approx(d.squad_tint[i]).epsilon(1e-4));
		CHECK(st->enemy_tint[i] == doctest::Approx(d.enemy_tint[i]).epsilon(1e-4));
	}
	CHECK(st->draw_distance == d.draw_distance);
	CHECK(st->minimum_draw_distance == d.minimum_draw_distance);
	CHECK(st->fade_distance == d.fade_distance);
	CHECK(st->fade_end_distance == d.fade_end_distance);
	CHECK(st->max_far_fade == d.max_far_fade);
	CHECK(st->scale_distance == d.scale_distance);
	CHECK(st->max_scale_mod == d.max_scale_mod);
	CHECK(st->icon_size == d.icon_size);
	CHECK(st->vertical_offset == doctest::Approx(d.vertical_offset));
	CHECK(st->pixel_offset == d.pixel_offset);
	CHECK(st->min_look_at_threshold == d.min_look_at_threshold);
	CHECK(st->max_look_at_threshold == d.max_look_at_threshold);
	CHECK(st->min_look_at_threshold_distance == d.min_look_at_threshold_distance);
	CHECK(st->show_label_range == d.show_label_range);
	CHECK(st->name_font_size == d.name_font_size);
	CHECK(st->name_glow_size == d.name_glow_size);
	row("data.nametag_style", st->from_data && st->clamped == 0,
			fmt("FriendlyTint (%.3f, %.3f, %.3f), EnemyTint (%.3f, ...)", st->friendly_tint[0], st->friendly_tint[1], st->friendly_tint[2], st->enemy_tint[0]) +
					fmt(", ShowLabelRange %.0f m, IconSize %.0f, LookAt %.0f..%.0f", st->show_label_range, st->icon_size, st->min_look_at_threshold,
							st->max_look_at_threshold) +
					", clamped " + std::to_string(st->clamped));
	Result<ui::SpottingRules> sp = ui::read_spotting_rules(*lib);
	REQUIRE_MESSAGE(sp.ok(), (sp ? "" : sp.error().message));
	CHECK(sp->from_data);
	CHECK(sp->clamped == 0);
	CHECK(sp->active_spotted_time == doctest::Approx(7.0f));
	CHECK(sp->passive_spotted_time == doctest::Approx(1.5f));
	CHECK(sp->spot_on_fire_multiplier == doctest::Approx(0.45f));
	row("data.spotting", sp->from_data && sp->clamped == 0,
			fmt("ActiveSpottedTime %.2f, PassiveSpottedTime %.2f, SpotOnFireMultiplier %.2f -> fire reveal %.2f s", sp->active_spotted_time, sp->passive_spotted_time,
					sp->spot_on_fire_multiplier, sp->fire_reveal_seconds()));

	CHECK_FALSE(ui::read_nametag_style(*lib, nullptr, ui::kSoldierBlueprintPartition).ok());
	CHECK_FALSE(ui::read_spotting_rules(*lib, nullptr, "gameplay/teams/us").ok());
	CHECK_FALSE(ui::read_nametag_style(*lib, nullptr, "no/such/partition").ok());
}

TEST_CASE("the 8 kits wear 8 distinct appearances through their default camo") {
	const EbxLibrary *lib = metro();
	if (lib == nullptr) {
		return;
	}
	Result<gp::UnlockProgression> prog = gp::read_unlock_progression(*lib);
	REQUIRE(prog.ok());
	std::set<std::string> appearances;
	size_t per_team[2] = { 0, 0 }, ungated = 0, with_parts = 0;
	std::string list;
	std::string assault[2];
	const char *teams[2] = { "gameplay/teams/us", "gameplay/teams/ru" };
	for (size_t t = 0; t < 2; ++t) {
		const EbxPartition *team = lib->partition_by_name(teams[t]);
		REQUIRE(team != nullptr);
		Result<gp::TeamDef> td = gp::read_team_data(*lib, team->primary());
		REQUIRE(td.ok());
		CHECK(td->kits.size() == 4);
		for (const std::string &kit : td->kits) {
			Result<gp::KitAppearance> ka = gp::read_kit_appearance(*lib, kit);
			REQUIRE_MESSAGE(ka.ok(), kit, ": ", (ka ? "" : ka.error().message));
			appearances.insert(ka->appearance);
			++per_team[t];
			const gp::UnlockGate gate = prog->gate(ka->visual_unlock_guid);
			ungated += gate == gp::UnlockGate::Free ? 1 : 0;
			CHECK_MESSAGE(gate == gp::UnlockGate::Free, ka->visual_unlock, " gated by ", gp::unlock_gate_name(gate));

			const EbxPartition *app = lib->partition_by_name(ka->appearance);
			REQUIRE(app != nullptr);
			const auto parts = lib->follow_all(app->primary().data(), "LinkedTo");
			size_t linked = 0;
			for (const auto &p : parts) {
				linked += lib->follow_all(p.data(), "LinkedTo").empty() ? 0 : 1;
			}
			with_parts += (!parts.empty() && linked == parts.size()) ? 1 : 0;
			if (kit.find("assault") != std::string::npos) {
				assault[t] = ka->appearance;
			}
			list += " " + kit.substr(kit.find_last_of('/') + 1) + "->" + ka->appearance.substr(ka->appearance.find_last_of('/') + 1) + " (" +
					std::to_string(parts.size()) + " parts of " + std::to_string(ka->candidates) + " linked)";
		}
	}
	CHECK(appearances.size() == 8);
	CHECK(per_team[0] == 4);
	CHECK(per_team[1] == 4);
	CHECK(ungated == 8);
	CHECK(with_parts == 8);
	const bf3::render::SoldierSpec spec;
	CHECK(assault[0] == spec.teams[0].appearance);
	CHECK(assault[1] == spec.teams[1].appearance);
	row("data.kit_appearances", appearances.size() == 8 && ungated == 8 && with_parts == 8,
			std::to_string(appearances.size()) + " distinct, camo ungated " + std::to_string(ungated) + "/8, assault = S3 default:" + list);

	CHECK_FALSE(gp::read_kit_appearance(*lib, "gameplay/teams/us").ok());
	CHECK_FALSE(gp::read_kit_appearance(*lib, "no/such/kit").ok());
}

TEST_CASE("the crowd's soldier assets: every kit wears its own parts, team by team") {
	t2::InstallFixture *f = t2::install();
	if (f == nullptr) {
		return;
	}
	bf3::render::SoldierSpec spec;
	spec.teams[0].team_data = "gameplay/teams/us";
	spec.teams[1].team_data = "gameplay/teams/ru";
	const auto t0 = std::chrono::steady_clock::now();
	Result<std::unique_ptr<bf3::render::SoldierAssets>> assets = bf3::render::load_soldier_assets(f->reader, spec, nullptr, nullptr);
	REQUIRE_MESSAGE(assets.ok(), (assets ? "" : assets.error().message));
	const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	bool ok = true;
	std::string detail;
	for (size_t t = 0; t < 2; ++t) {
		const bf3::render::SoldierTeam &team = (*assets)->teams[t];
		std::set<std::vector<std::string>> looks;
		for (size_t k = 0; k < 4; ++k) {
			std::vector<std::string> worn;
			uint64_t vertices = 0;
			for (const bf3::render::SoldierPart &p : team.parts) {
				if ((p.kit_mask & (1u << k)) != 0 && !p.weapon) {
					worn.push_back(p.mesh + "|" + p.variation);
					vertices += p.prepared.vertices;
				}
			}
			ok = ok && !worn.empty() && !team.kit_appearances[k].empty();
			looks.insert(worn);
			const std::string app = team.kit_appearances[k];
			detail += " " + std::string(t == 0 ? "us" : "ru") + std::to_string(k) + "=" + app.substr(app.find_last_of('/') + 1) + " (" + std::to_string(worn.size()) +
					" parts, " + std::to_string(vertices) + " v)";
		}
		for (const bf3::render::SoldierPart &p : team.parts) {
			ok = ok && (!p.weapon || p.kit_mask == 0x0F) && p.kit_mask != 0;
		}
		CHECK_MESSAGE(looks.size() == 4, "team ", t, ": ", looks.size(), " distinct part sets");
		CHECK(team.binds.size() <= 256);
		CHECK(team.kit_appearances[0] == spec.teams[t].appearance);
		ok = ok && looks.size() == 4 && team.binds.size() <= 256 && team.kit_appearances[0] == spec.teams[t].appearance;
		detail += "; " + std::to_string(team.parts.size()) + " parts, " + std::to_string(team.binds.size()) + " binds;";
	}
	CHECK(ok);
	row("crowd.kit_parts", ok, fmt("load %.2f s, %.0f textures:", seconds, double((*assets)->textures.size())) + detail);
}

TEST_CASE("the readers under seeded mutation of their partitions") {
	const EbxLibrary *lib = metro();
	if (lib == nullptr) {
		return;
	}
	const auto t0 = std::chrono::steady_clock::now();
	const size_t iters = t2::fuzz_iters(250);
	const Tally a = fuzz_partition(*lib, std::string(ui::kNametagCompPartition), 21, iters, [](const EbxResolver &p_db) {
		gp::WorkBudget budget;
		return ui::read_nametag_style(p_db, &budget).ok();
	});
	const Tally b = fuzz_partition(*lib, std::string(ui::kSoldierBlueprintPartition), 22, std::max<size_t>(1, iters / 5), [](const EbxResolver &p_db) {
		gp::WorkBudget budget;
		return ui::read_spotting_rules(p_db, &budget).ok();
	});
	const Tally c = fuzz_partition(*lib, "gameplay/kits/usengineer", 23, iters, [](const EbxResolver &p_db) {
		gp::WorkBudget budget;
		return gp::read_kit_appearance(p_db, "gameplay/kits/usengineer", &budget).ok();
	});
	const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	CHECK(a.mutants == iters);
	row("fuzz", true, tally_text("uinametagcomp", a) + "; " + tally_text("mpsoldier", b) + "; " + tally_text("usengineer", c) + fmt(" in %.2f s", s));
}

TEST_CASE("install.unchanged") {
	t2::check_install_unchanged();
}

}
