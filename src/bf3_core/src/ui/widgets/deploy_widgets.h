#pragma once

#include "hud_widget_support.h"

namespace bf3::core::ui {

class HudSpawnTimer {
public:
	virtual ~HudSpawnTimer() = default;
	virtual void deploy_release() = 0;
	virtual bool deployable() const = 0;
	virtual double current_time() const = 0;
	virtual double start_time() const = 0;
	virtual bool can_spawn() const = 0;
	virtual int32_t current_frame() const = 0;
	virtual uint32_t deploys_fired() const = 0;
};
}
