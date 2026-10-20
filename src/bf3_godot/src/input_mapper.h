#pragma once

#include "bf3/host/client_session.h"
#include "bf3/sim/command.h"

#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <array>
#include <cstdint>

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
	enum class M2aAction : uint8_t { Fire = 0, Ads, Reload, Primary, Sidearm, Grenade, Knife, FireMode, Kit, Team, Count };
	static constexpr size_t kM2aActionCount = size_t(M2aAction::Count);
	static const char *m2a_action_name(M2aAction p_action);

	static constexpr uint64_t kM2aReloadHoldNs = 100'000'000;
	void m2a_enable(bool p_on);
	bool m2a_enabled() const { return m2a_.enabled; }
	void m2a_set_weapons_allowed(bool p_allowed);
	bool m2a_weapons_allowed() const { return m2a_.allowed; }

	void m2a_set_slots(uint8_t p_active, uint8_t p_pending, uint16_t p_filled);

	void m2a_frame(const bf3::host::PresentationFrame &p_frame, uint64_t p_now_ns);

	void m2a_script_hold(M2aAction p_action, bool p_held);
	void m2a_script_press(M2aAction p_action);
	void m2a_script_select(uint8_t p_slot);
	bool m2a_held(M2aAction p_action) const { return size_t(p_action) < kM2aActionCount && m2a_.held[size_t(p_action)]; }
	uint32_t m2a_fire_presses() const { return m2a_.fire_presses; }
	uint8_t m2a_wanted_slot() const { return m2a_.wanted; }
	uint8_t m2a_fire_mode_counter() const { return m2a_.mode_counter; }
	uint32_t m2a_take_kit_presses();
	uint32_t m2a_take_team_presses();
	uint64_t m2a_swallowed_clicks() const { return m2a_.swallowed_clicks; }

private:
	bool m2a_handle_event(const godot::Ref<godot::InputEvent> &p_event);
	void m2a_sample(bf3::host::InputSample &r_sample) const;
	void m2a_select(uint8_t p_slot);
	void m2a_cycle(int p_direction);
	struct M2aInput {
		bool enabled = false;
		bool installed = false;
		bool allowed = false;
		bool swallow_fire = false;
		std::array<bool, kM2aActionCount> held{};
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
	} m2a_;

public:
	enum class M2bAction : uint8_t { Gadget = 0, Scoreboard, Deploy, Kit1, Kit2, Kit3, Kit4, Count };
	static constexpr size_t kM2bActionCount = size_t(M2bAction::Count);
	static const char *m2b_action_name(M2bAction p_action);
	void m2b_enable(bool p_on);
	bool m2b_enabled() const { return m2b_.enabled; }
	void m2b_set_spawn_screen(bool p_up);
	bool m2b_spawn_screen() const { return m2b_.spawn_screen; }
	bool m2b_scoreboard_held() const;
	int m2b_take_kit();
	uint32_t m2b_take_deploy_presses();

	void m2b_script_press(M2bAction p_action);
	void m2b_script_hold(M2bAction p_action, bool p_held);

private:
	bool m2b_handle_event(const godot::Ref<godot::InputEvent> &p_event);
	struct M2bInput {
		bool enabled = false;
		bool installed = false;
		bool spawn_screen = false;
		bool scoreboard_scripted = false;
		int kit = -1;
		uint32_t deploy_presses = 0;
		uint64_t consumed_clicks = 0;
	} m2b_;
};

}
