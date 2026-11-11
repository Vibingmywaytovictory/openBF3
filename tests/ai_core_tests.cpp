#include <doctest/doctest.h>

#include "ai_knowledge_fakes.h"
#include "bf3/ai/behaviour/curves.h"
#include "bf3/ai/debug/draw_primitives.h"
#include "bf3/ai/knowledge/fire_object.h"
#include "bf3/ai/knowledge/information_factory.h"
#include "bf3/ai/knowledge/plug_ins.h"
#include "bf3/ai/knowledge/weapon_info.h"

#include <array>
#include <cmath>
#include <string>
#include <vector>

using namespace bf3;
using namespace bf3::ai;
using namespace bf3::ai::test;

namespace {

struct StepTimer final : AiTimer {
	sim::Tick tick = 0;
	sim::Tick game_tick() const override { return tick; }
	double tick_seconds() const override { return 1.0 / 60.0; }
	double update_time() const override { return double(tick) / 60.0; }
};

struct PositionLog final : InformationEnvironmentModifiers {
	std::vector<uint32_t> handles;
	void add_and_update_position(int, bool p_unit, uint32_t p_handle) override {
		CHECK(p_unit);
		handles.push_back(p_handle);
	}
};

struct RecordingRenderer final : DebugRenderer {
	std::vector<std::string> calls;
	void draw_line(const std::vector<Vec3> &p_points, uint32_t, float) override {
		calls.push_back("line " + std::to_string(p_points.size()));
	}
	void draw_circle(const Vec3 &, const Vec3 &, float p_radius, uint32_t, bool, int) override {
		calls.push_back("circle " + std::to_string(int(p_radius)));
	}
	void draw_screen_text(
			const Vec2 &, const std::vector<std::string> &p_lines, int, uint32_t, TextAlignX, TextAlignY) override {
		calls.push_back("text2d " + p_lines.front());
	}
	void draw_3d_text(
			const Vec3 &, const std::vector<std::string> &p_lines, int, uint32_t, TextAlignX, TextAlignY) override {
		calls.push_back("text3d " + p_lines.front());
	}
	void draw_triangle(const std::vector<Vec3> &p_vertices, const std::vector<uint32_t> &p_colours, bool) override {
		calls.push_back("triangle " + std::to_string(p_vertices.size()) + " " + std::to_string(p_colours.back()));
	}
};

constexpr std::array<float, kBattleStrengthCount> kStrengths = { 10.0f, 4.0f, 0.0f, 2.0f, 0.0f, 1.0f };

}

TEST_SUITE("ai-core") {
	TEST_CASE("FireObject: the XZ distance across the fire line, and strength decaying with game seconds") {
		const FireObject fire({ 0.0f, 0.0f, 0.0f }, { 0.0f, 5.0f, 100.0f }, kStrengths, true, 600, 1.0f);
		CHECK(fire.is_direct());
		CHECK(fire.is_within_radius({ 3.0f, 50.0f, 40.0f }, 3.0f));
		CHECK_FALSE(fire.is_within_radius({ -3.5f, 0.0f, 40.0f }, 3.0f));
		CHECK(fire.is_within_radius({ 0.5f, 0.0f, -400.0f }, 1.0f));
		StepTimer timer;
		timer.tick = 600;
		CHECK(fire.battle_strength(0, timer) == doctest::Approx(10.0f));
		timer.tick = 720;
		CHECK(fire.battle_strength(0, timer) == doctest::Approx(10.0f / 3.0f));
		CHECK(fire.battle_strength(1, timer) == doctest::Approx(4.0f / 3.0f));
		const FireObject indirect({ 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, kStrengths, false, 600, 0.1f);
		CHECK(indirect.battle_strength(0, timer) == doctest::Approx(10.0f / 1.2f));
		CHECK(indirect.is_within_radius({ 1000.0f, 0.0f, 1000.0f }, 0.0f));
	}

	TEST_CASE("DangerObject: a fire and an information compare by what they point at, never across kinds") {
		const FireObject a({ 1.0f, 2.0f, 3.0f }, { 4.0f, 5.0f, 6.0f }, kStrengths, true, 0, 1.0f);
		const FireObject b({ 1.0f, 2.0f, 3.0f }, { 4.0f, 5.0f, 6.0f }, kStrengths, true, 0, 1.0f);
		const FireDanger fa(&a);
		const FireDanger fa2(&a);
		const FireDanger fb(&b);
		const FireDanger none;
		CHECK(fa == fa2);
		CHECK_FALSE(fa == fb);
		CHECK_FALSE(none == none);
		Vec3 where{};
		CHECK(fa.position(where));
		CHECK(where.z == 3.0f);
		CHECK_FALSE(none.position(where));
		InformationManager information;
		const InfoDanger unknown(information);
		CHECK(unknown.pointer() == nullptr);
		CHECK_FALSE(unknown.position(where));
		CHECK_FALSE(unknown == fa);
		CHECK_FALSE(fa == unknown);
	}

	TEST_CASE("InformationKnown: an enemy as last seen, followed while in contact, its security fading over game "
			  "seconds") {
		Knowledge k;
		PositionLog log;
		KnowledgeContext context = k.make();
		context.environment = &log;
		const InformationFactory factory(context);
		FakeObject enemy;
		enemy.team = 2;
		enemy.primary = true;
		enemy.where = { 10.0f, 0.0f, 20.0f };
		enemy.transform.forward = { 1.0f, 0.0f, 0.0f };
		const uint32_t handle = factory.request_information_known(kNoHandle, &enemy, 1u << 21, 600, 10.0f, 0.5f, 30, 1);
		enemy.handles[1] = handle;
		REQUIRE(handle != kNoHandle);
		CHECK(factory.request_information_plug_in(handle, PlugInRequest::Physical, false));
		CHECK(factory.request_information_plug_in(handle, PlugInRequest::Armament, false));
		CHECK(factory.request_information_plug_in(handle, PlugInRequest::Unit, false));
		auto *const info = dynamic_cast<InformationKnown *>(k.information.information(handle));
		REQUIRE(info != nullptr);
		CHECK(info->information_sort() == 1);
		CHECK(info->is_of_type(InformationType::Soldier));
		CHECK_FALSE(info->is_of_type(InformationType::Mobile));
		CHECK(info->side() == 2);
		CHECK(info->owner_side() == 1);
		CHECK(info->time_stamp() == 600);
		CHECK_FALSE(info->has_contact());

		enemy.where = { 50.0f, 0.0f, 20.0f };
		enemy.transform.forward = { 0.0f, 0.0f, 1.0f };
		CHECK(info->position().x == 10.0f);
		CHECK(info->facing().x == 1.0f);
		info->add_contact();
		CHECK(info->has_contact());
		CHECK(info->position().x == 50.0f);
		CHECK(info->transformation().forward.z == 1.0f);
		CHECK(info->facing().x == 1.0f);
		CHECK(info->security() == 1.0f);
		info->remove_contact();
		enemy.where = { 90.0f, 0.0f, 20.0f };
		CHECK(info->position().x == 50.0f);
		CHECK(info->facing().z == 1.0f);

		k.timer.tick = 900;
		CHECK(info->security() == doctest::Approx(1.0f - SCurve::calculate(0.5f)));
		CHECK(info->security(600) == 1.0f);

		CHECK(info->set_time(900));
		CHECK(log.handles == std::vector<uint32_t>{ handle });
		CHECK(info->time() == 900);
		CHECK(info->time_stamp() == 900);
		CHECK(info->position().x == 90.0f);
		CHECK_FALSE(info->set_time(920));
		CHECK(info->time_stamp() == 920);
		CHECK(log.handles.size() == 1);

		k.timer.tick = 1520;
		CHECK(info->security() < 0.25f);
		CHECK(info->set_time(1520, 0.5f));
		CHECK(info->time() == 1520 - 12);
		CHECK(info->security() == doctest::Approx(1.0f - SCurve::calculate(0.02f)));
		CHECK_FALSE(info->set_time(1520, 0.5f));
	}

	TEST_CASE("known plug-ins: the physical and the weapons are snapshots, refreshed after a game second") {
		Knowledge k;
		FakeObject enemy;
		enemy.team = 2;
		enemy.body.hp = 40.0f;
		enemy.body.down = true;
		const uint32_t handle = k.factory.request_information_known(kNoHandle, &enemy, 0, 600, 10.0f, 0.5f, 30, 1);
		REQUIRE(k.factory.request_information_plug_in(handle, PlugInRequest::Physical, false));
		REQUIRE(k.factory.request_information_plug_in(handle, PlugInRequest::Armament, false));
		Information *const info = k.information.information(handle);
		IPIPhysical *const physical = info->physical();
		REQUIRE(dynamic_cast<IPIPhysicalKnown *>(physical) != nullptr);
		CHECK(physical->health() == 0.0f);
		physical->set_time();
		CHECK(physical->health() == 40.0f);
		CHECK(physical->man_down());
		CHECK(physical->mod() == doctest::Approx(SCurve::calculate(40.0f) * 0.5f + 0.5f));
		enemy.body.hp = 10.0f;
		CHECK(physical->health() == 40.0f);

		IPIArmament *const armament = info->armament();
		REQUIRE(dynamic_cast<IPIArmamentKnown *>(armament) != nullptr);
		const std::vector<uint32_t> &weapons = armament->weapons();
		REQUIRE(weapons.size() == 2);
		WeaponInfo *const rifle = k.weapons.information(weapons[0]);
		REQUIRE(dynamic_cast<WeaponInfoKnown *>(rifle) != nullptr);
		CHECK(rifle->ammo() == 30);
		CHECK(rifle->number_of_mags() == 3);
		enemy.rifle.rounds = 5;
		k.timer.tick = 660;
		armament->set_time(660);
		CHECK(rifle->ammo() == 30);
		CHECK(rifle->security() == doctest::Approx(1.0f - SCurve::calculate(0.2f)));
		k.timer.tick = 661;
		armament->set_time(661);
		CHECK(rifle->ammo() == 5);
		CHECK(rifle->shots_in_mag(0) == 5);
		CHECK(rifle->security() == 1.0f);
		armament->update_weapons();
		CHECK(k.weapons.live_count() == 2);
		CHECK(armament->weapon(1) != kNoHandle);
		CHECK(k.weapons.live_count() == 2);
		CHECK(k.information.free_information(handle));
		CHECK(k.weapons.live_count() == 0);
	}

	TEST_CASE("DPManager: primitives draw by kind in OpenBF2's order until their game-second lifetime runs out") {
		StepTimer timer;
		timer.tick = 60;
		DPManager manager(timer);
		RecordingRenderer renderer;
		DPStaticLine *const line =
				manager.create_static_line(2.0f, 0xFFFF0000u, { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, 1.0f);
		CHECK(line->points.size() == 3);
		manager.create_static_circle(1.0f, 0xFF00FF00u, { 0.0f, 0.0f, 0.0f }, 5.0f, { 0.0f, 1.0f, 0.0f }, false, 12);
		manager.create_static_2d_text(
				10.0f, 0xFFFFFF00u, { 10.0f, 10.0f }, { "screen" }, 5, TextAlignX::Left, TextAlignY::Top);
		manager.create_static_3d_text(
				10.0f, 0xFFFFFF00u, { 0.0f, 2.0f, 0.0f }, { "world" }, 5, TextAlignX::Centre, TextAlignY::Bottom);
		DPStaticPolygon *const polygon = manager.create_static_polygon(10.0f,
				{ { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, 1.0f } },
				{ 1u, 2u, 3u, 4u }, true);
		CHECK(polygon->triangle_count() == 2);
		CHECK(polygon->colour() == 1u);
		manager.create_static_triangle(
				10.0f, { { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 1.0f } }, { 7u, 8u, 9u }, false);
		CHECK(manager.size() == 6);
		manager.draw_all(renderer);
		CHECK(renderer.calls ==
				std::vector<std::string>{ "circle 5", "text2d screen", "text3d world", "triangle 3 9", "triangle 3 3",
						"triangle 3 4", "line 3" });

		timer.tick = 60 + 90;
		renderer.calls.clear();
		manager.draw_all(renderer);
		CHECK(manager.size() == 5);
		CHECK(renderer.calls.front() == "text2d screen");
		CHECK(manager.circle(line) == nullptr);
		REQUIRE(manager.line(line) == line);
		DrawPrimitive *handle = line;
		CHECK_FALSE(manager.delete_circle(handle));
		CHECK(handle == line);
		CHECK(manager.delete_line(handle));
		CHECK(handle == nullptr);
		CHECK(manager.size() == 4);
		timer.tick = 60 + 601;
		manager.nodraw_update();
		CHECK(manager.size() == 0);
		manager.create_static_line(1.0f, 1u, std::vector<Vec3>{ { 0.0f, 0.0f, 0.0f } }, 1.0f);
		manager.delete_all();
		CHECK(manager.size() == 0);
	}
}
