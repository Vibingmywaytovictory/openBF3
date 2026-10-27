#include "deploy_widgets.h"

namespace bf3::core::ui {

namespace {

class SpawnTimerClass final : public HudSpawnTimer, public HudWidgetController {
public:
	void on_clip_load(HudWidgetHost &r_host) override {
		host_ = &r_host;
		r_host.self().set_visible(false);
		set_start_state(r_host);
		r_host.clip("buttonGlow").goto_and_stop("off");
	}

	void initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) override {
		setup_refresh_data(*this, r_host, p_init.data);
		r_host.self().set_visible(true);
		update_active_vis(r_host);
	}

	void update(HudWidgetHost &r_host, std::string_view p_key, const As2Value &p_value) override {
		if (p_key == "Time") {
			update_time(r_host, p_value);
		} else if (p_key == "CanSpawn") {
			update_can_spawn(r_host, p_value);
		} else if (p_key == "AutoDeployEnabled" && p_value.defined()) {
			auto_deploy_enabled_ = p_value.truthy();
		}
	}

	void bind(const HudModel &p_model, const HudClientState &, HudData &r_out) const override {
		r_out.push_back({ "Time", As2Value::of(hud_spawn_timer_seconds(p_model.spawn_logic.time_to_respawn)) });
		r_out.push_back({ "CanSpawn", As2Value::of(p_model.spawn_points.can_spawn), false });
	}

	std::optional<float> tween_property(HudWidgetHost &, std::string_view p_name) override {
		if (p_name == kNewFrame) {
			return float(curr_frame_);
		}
		return std::nullopt;
	}

	void set_tween_property(HudWidgetHost &r_host, std::string_view p_name, float p_value) override {
		if (p_name == kNewFrame) {
			curr_frame_ = int32_t(as2_round(p_value));
			r_host.clip("innerWhite").goto_and_stop(curr_frame_ + 1);
		}
	}

	bool mouse_move(HudWidgetHost &r_host, const HudMouse &p_mouse) override {
		if (p_mouse.target != r_host.clip("btnDeploy").path()) {
			return false;
		}
		if (p_mouse.kind == HudMouseKind::RollOver) {
			if (deployable_) {
				r_host.clip("deploy").goto_and_play("over");
			}
			r_host.clip("timerButton").goto_and_stop("over");
			return true;
		}
		if (p_mouse.kind == HudMouseKind::RollOut) {
			if (deployable_) {
				r_host.clip("deploy").goto_and_stop("up");
			}
			r_host.clip("timerButton").goto_and_stop("up");
			return true;
		}
		return false;
	}

	bool mouse_press(HudWidgetHost &r_host, const HudMouse &p_mouse) override {
		if (p_mouse.target != r_host.clip("btnDeploy").path()) {
			return false;
		}
		if (deployable_) {
			r_host.clip("deploy").goto_and_stop("over");
		}
		r_host.clip("timerButton").goto_and_stop("press");
		return true;
	}

	bool mouse_release(HudWidgetHost &r_host, const HudMouse &p_mouse) override {
		if (p_mouse.target != r_host.clip("btnDeploy").path() || p_mouse.kind != HudMouseKind::Release) {
			return false;
		}
		deploy_release(r_host);
		return true;
	}

	void deploy_release() override {
		if (host_ != nullptr) {
			deploy_release(*host_);
		}
	}

	bool deployable() const override { return deployable_; }
	double current_time() const override { return current_time_; }
	double start_time() const override { return start_time_; }
	bool can_spawn() const override { return can_spawn_; }
	int32_t current_frame() const override { return curr_frame_; }
	uint32_t deploys_fired() const override { return deploys_fired_; }

private:
	void set_start_state(HudWidgetHost &r_host) {
		r_host.clip("countdown.textCountdown").set_text(as2_number_string(kStartCount));
		const As2Clip deploy_text = r_host.clip("deploy.textDeploy.txt");
		deploy_text.set_text(r_host.translate("ID_M_SPAWN_DEPLOY"));
		deploy_text.set_text_fit(SwfTextFit::Fit);
		r_host.clip("deployStatus.textNormal.txt").set_text(r_host.translate("ID_M_SPAWN_TIME_TO_DEPLOY"));
		r_host.clip("deployStatus.textAuto.txt").set_text(r_host.translate("ID_M_SPAWN_TIME_TO_AUTODEPLOY"));
		r_host.clip("arrowsDeploy").set_visible(false);
		r_host.clip("innerWhite").set_visible(false);
		r_host.clip("deployStatus").set_visible(false);
		r_host.clip("countdown").set_visible(false);
		r_host.set_mouse_target(r_host.clip("btnDeploy"), true);
	}

	void deploy_release(HudWidgetHost &r_host) {
		r_host.clip("timerButton").goto_and_stop("over");
		if (!deployable_) {
			if (current_time_ > 0.0) {
				r_host.clip("glow").goto_and_play("auto");
				r_host.clip("deployStatus").goto_and_play("in_auto");
				deployable_ = true;
			} else if (can_spawn_) {
				fire_deploy(r_host);
			}
		} else {
			deployable_ = false;
			r_host.clip("glow").goto_and_play("normal");
			r_host.clip("deployStatus").goto_and_play("in_normal");
		}
	}

	void fire_deploy(HudWidgetHost &r_host) {
		++deploys_fired_;
		r_host.fire_event(kDeployEvent, As2Value::of(deployable_));
	}

	void update_active_vis(HudWidgetHost &r_host) const {
		r_host.self().set_alpha(current_time_ > 0.0 || !can_spawn_ ? kDimAlpha : 100.0f);
	}

	void update_time(HudWidgetHost &r_host, const As2Value &p_value) {
		if (!p_value.defined()) {
			return;
		}
		r_host.self().set_visible(true);
		current_time_ = p_value.to_number();
		if (current_time_ > start_time_) {
			start_time_ = current_time_;
		}
		r_host.remove_tweens(r_host.self());
		r_host.add_custom_tween(kNewFrame, kNewFrameEnd, current_time_, HudTweenEase::Linear);
		if (current_time_ > 0.0) {
			r_host.clip("deploy").set_visible(false);
			r_host.clip("deployStatus").set_visible(true);
			r_host.clip("countdown").set_visible(true);
			r_host.clip("countdown.textCountdown").set_text(as2_number_string(current_time_));
			r_host.clip("buttonGlow").goto_and_play("on");
			if (current_time_ <= kLightBarSeconds) {
				goto_and_stop_number(r_host.clip("lightbars"), current_time_ + 2.0);
			}
		} else {
			r_host.clip("deployStatus").set_visible(false);
			r_host.clip("deploy").set_visible(true);
			r_host.clip("lightbars").set_visible(false);
			r_host.clip("deploy").goto_and_play("over");
			r_host.clip("buttonGlow").goto_and_play("deploy");
			r_host.clip("glow").goto_and_play("auto");
			r_host.clip("countdown.textCountdown").set_text("");
			if (deployable_ || (can_spawn_ && auto_deploy_enabled_)) {
				fire_deploy(r_host);
			}
		}
		update_active_vis(r_host);
	}

	void update_can_spawn(HudWidgetHost &r_host, const As2Value &p_value) {
		if (!p_value.defined()) {
			return;
		}
		can_spawn_ = p_value.truthy();
		if (!can_spawn_) {
			deployable_ = false;
			r_host.clip("glow").goto_and_play("normal");
			r_host.clip("deployStatus").goto_and_play("in_normal");
			update_active_vis(r_host);
		} else {
			update_time(r_host, As2Value::of(current_time_));
		}
	}

	static constexpr std::string_view kNewFrame = "newFrame";
	static constexpr float kNewFrameEnd = 360.0f;
	static constexpr double kStartCount = 20.0;
	static constexpr double kLightBarSeconds = 5.0;
	static constexpr float kDimAlpha = 20.0f;
	static constexpr int32_t kDeployEvent = 0;
	HudWidgetHost *host_ = nullptr;
	double start_time_ = 0.0;
	double current_time_ = 0.0;
	int32_t curr_frame_ = 0;
	bool deployable_ = false;
	bool can_spawn_ = true;
	bool auto_deploy_enabled_ = false;
	uint32_t deploys_fired_ = 0;
};

}

int32_t hud_spawn_timer_seconds(float p_time_to_respawn) {
	return p_time_to_respawn > 0.0f ? int32_t(std::ceil(p_time_to_respawn)) : 0;
}

void register_hud_widgets_spawn_timer(HudWidgetRegistry &r_registry) {
	r_registry.add("Widget.SpawnTimer.SpawnTimer", [] { return std::make_unique<SpawnTimerClass>(); });
}

}
