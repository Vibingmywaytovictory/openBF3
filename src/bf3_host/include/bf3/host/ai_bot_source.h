#pragma once

#include "bf3/ai/bot/bot_main.h"
#include "bf3/host/game_server.h"
#include "bf3/host/tdm_brain.h"
#include "bf3/sim/sim_types.h"

#include <cstdint>
#include <memory>

namespace bf3::host {

struct TdmShared;

TdmBotActivity tdm_activity(ai::InfantryActivity p_activity);

class AiBotSource final : public CommandSource {
public:
	AiBotSource(std::shared_ptr<TdmShared> p_shared, uint32_t p_index);
	~AiBotSource() override;
	AiBotSource(const AiBotSource &) = delete;
	AiBotSource &operator=(const AiBotSource &) = delete;

	sim::Command command(sim::Tick p_tick, const sim::SoldierState &p_state) override;
	void set_slot(uint8_t p_slot);

	const ai::BotMain &bot() const;
	ai::BotMain &bot();
	ai::SquadBot &squad_bot();

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

}
