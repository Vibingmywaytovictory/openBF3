#pragma once

#include "bf3/ai/knowledge/knowledge_context.h"

#include <array>
#include <span>

namespace bf3::ai {

class FireObject {
public:
	FireObject(const Vec3 &p_origin, const Vec3 &p_target, std::span<const float, kBattleStrengthCount> p_strengths,
			bool p_direct, sim::Tick p_time, float p_decay);

	bool is_within_radius(const Vec3 &p_position, float p_radius) const;
	float battle_strength(int p_type, const AiTimer &p_timer) const;
	bool is_direct() const { return direct_; }
	sim::Tick time() const { return time_; }
	const Vec3 &origin() const { return origin_; }
	const Vec3 &target() const { return target_; }

private:
	bool direct_ = false;
	std::array<float, kBattleStrengthCount> strengths_{};
	Vec3 origin_{};
	Vec3 target_{};
	sim::Tick time_ = 0;
	float decay_ = 0.0f;
};

class InformationManager;

class DangerObject {
public:
	virtual ~DangerObject() = default;
	virtual bool is_fire_object() const = 0;
	virtual bool is_information() const = 0;
	virtual bool position(Vec3 &r_position) const = 0;
	virtual const void *pointer() const = 0;
	bool operator==(const DangerObject &p_other) const;
};

class FireDanger final : public DangerObject {
public:
	FireDanger() = default;
	explicit FireDanger(const FireObject *p_fire) : fire_(p_fire) {}
	bool is_fire_object() const override { return true; }
	bool is_information() const override { return false; }
	bool position(Vec3 &r_position) const override;
	const void *pointer() const override { return fire_; }
	const FireObject *fire() const { return fire_; }

private:
	const FireObject *fire_ = nullptr;
};

class InfoDanger final : public DangerObject {
public:
	explicit InfoDanger(const InformationManager &p_information, uint32_t p_handle = kNoHandle) :
			information_(&p_information), handle_(p_handle) {}
	bool is_fire_object() const override { return false; }
	bool is_information() const override { return true; }
	bool position(Vec3 &r_position) const override;
	const void *pointer() const override;
	uint32_t handle() const { return handle_; }

private:
	const InformationManager *information_ = nullptr;
	uint32_t handle_ = kNoHandle;
};

}
