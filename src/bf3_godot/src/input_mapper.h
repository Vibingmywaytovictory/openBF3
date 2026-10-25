#pragma once

#include "bf3/host/client_session.h"
#include "bf3/sim/command.h"

#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <godot_cpp/classes/global_constants.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace bf3::godot_side {

enum class InputAction : uint8_t {
	Forward = 0,
	Back,
	Left,
	Right,
	Sprint,
	Jump,
	Crouch,
	Prone,
	FreeCamera,
	Up,
	Down,
	Count,
};
inline constexpr size_t kInputActionCount = size_t(InputAction::Count);

const char *input_action_name(InputAction p_action);

std::string ui_idk_of_key(godot::Key p_key);
std::string_view ui_idb_of_button(godot::MouseButton p_button);

struct ViewAngles {
	float yaw = 0.0f;
	float pitch = 0.0f;
};

class InputMapper {
public:

	static float pitch_limit();
	static float clamp_pitch(float p_pitch);

	void install_actions();

	void handle_event(const godot::Ref<godot::InputEvent> &p_event);

	void poll_devices();
	void set_capture(bool p_capture);
	bool captured() const { return captured_; }

	void set_scripted(bool p_scripted);
	bool scripted() const { return scripted_; }
	void script_hold(InputAction p_action, bool p_held);
	void script_press(InputAction p_action);
	void script_view(float p_yaw, float p_pitch);
	void release_all();

	void adopt(const bf3::host::PresentationFrame &p_frame);

	void reset_life();

	void set_requested_pose(bf3::sim::Pose p_pose) { pose_ = p_pose; }
	bf3::sim::Pose requested_pose() const { return pose_; }

	bf3::host::InputSample sample() const;
	const ViewAngles &view() const { return view_; }
	const ViewAngles &free_view() const { return free_view_; }
	bool free_camera() const { return free_camera_; }
	void set_free_camera(bool p_on);
	bool held(InputAction p_action) const { return held_[size_t(p_action)]; }
	uint32_t adopted_life() const { return life_; }
	uint64_t adoptions() const { return adoptions_; }

	godot::Vector3 free_move() const;
	float free_speed_factor() const { return held(InputAction::Sprint) ? 6.0f : 1.0f; }

	float mouse_sensitivity = 0.0022f;

private:
	void press(InputAction p_action);
	void look(float p_dx, float p_dy);

	std::array<bool, kInputActionCount> held_{};
	ViewAngles view_;
	ViewAngles free_view_;
	bf3::sim::Pose pose_ = bf3::sim::Pose::Stand;
	uint32_t jump_presses_ = 0;
	uint32_t life_ = 0;
	uint64_t adoptions_ = 0;
	bool free_camera_ = false;
	bool scripted_ = false;
	bool captured_ = false;
	bool actions_installed_ = false;

public:
	enum class WeaponAction : uint8_t { Fire = 0, Ads, Reload, Primary, Sidearm, Grenade, Knife, FireMode, Kit, Team, Count };
	static constexpr size_t kWeaponActionCount = size_t(WeaponAction::Count);
	static const char *weapon_action_name(WeaponAction p_action);

	static constexpr uint64_t kReloadHoldNs = 100'000'000;
	void enable_weapons(bool p_on);
	bool weapons_enabled() const { return weapon_.enabled; }
	void set_weapons_allowed(bool p_allowed);
	bool weapons_allowed() const { return weapon_.allowed; }

	void set_weapon_slots(uint8_t p_active, uint8_t p_pending, uint16_t p_selectable);
	static constexpr uint8_t kQuickThrowSlot = 6;
	void set_active_weapon(bf3::sim::weapons::WeaponPhase p_phase, int32_t p_magazine, int32_t p_reserve);
	bool quick_throwing() const { return weapon_.quick; }

	void weapon_frame(const bf3::host::PresentationFrame &p_frame, uint64_t p_now_ns);

	void script_hold(WeaponAction p_action, bool p_held);
	void script_press(WeaponAction p_action);
	void script_select(uint8_t p_slot);
	bool held(WeaponAction p_action) const { return size_t(p_action) < kWeaponActionCount && weapon_.held[size_t(p_action)]; }
	uint32_t fire_presses() const { return weapon_.fire_presses; }
	uint8_t wanted_slot() const { return weapon_.wanted; }
	uint8_t fire_mode_counter() const { return weapon_.mode_counter; }
	uint32_t take_kit_presses();
	uint32_t take_team_presses();
	uint64_t swallowed_clicks() const { return weapon_.swallowed_clicks; }

private:
	bool weapon_handle_event(const godot::Ref<godot::InputEvent> &p_event);
	void weapon_sample(bf3::host::InputSample &r_sample) const;
	bool slot_selectable(uint8_t p_slot) const;
	void select_weapon(uint8_t p_slot);
	void cycle_weapon(int p_direction);
	struct WeaponKeys {
		bool enabled = false;
		bool installed = false;
		bool allowed = false;
		bool swallow_fire = false;
		std::array<bool, kWeaponActionCount> held{};
		uint32_t fire_presses = 0;
		uint8_t wanted = 0xFF;
		uint8_t mode_counter = 0;
		uint8_t active = 0, pending = 0xFF;
		uint16_t filled = 0;
		uint32_t kit_presses = 0, team_presses = 0;
		uint64_t reload_until_ns = 0;
		uint64_t now_ns = 0;
		uint32_t life = 0;
		uint64_t swallowed_clicks = 0;
		const bf3::host::PresentationFrame *frame = nullptr;
		bool quick = false, quick_held = false;
		uint8_t quick_back = 0xFF;
	} weapon_;

public:
	enum class TdmAction : uint8_t { Gadget = 0, Scoreboard, Deploy, Kit1, Kit2, Kit3, Kit4, Count };
	static constexpr size_t kTdmActionCount = size_t(TdmAction::Count);
	static const char *tdm_action_name(TdmAction p_action);
	void enable_tdm(bool p_on);
	bool tdm_enabled() const { return tdm_.enabled; }
	void set_spawn_screen(bool p_up);
	bool spawn_screen_up() const { return tdm_.spawn_screen; }
	bool scoreboard_held() const;
	int take_kit();
	uint32_t take_deploy_presses();

	void script_press(TdmAction p_action);
	void script_hold(TdmAction p_action, bool p_held);

private:
	bool tdm_handle_event(const godot::Ref<godot::InputEvent> &p_event);
	struct TdmKeys {
		bool enabled = false;
		bool installed = false;
		bool spawn_screen = false;
		bool scoreboard_scripted = false;
		int kit = -1;
		uint32_t deploy_presses = 0;
		uint64_t consumed_clicks = 0;
	} tdm_;
};

}
