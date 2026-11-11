#pragma once

#include "bf3/ai/knowledge/ai_object.h"

#include <cstdint>
#include <list>
#include <utility>
#include <vector>

namespace bf3::ai {

class InformationManager;

struct WaypointBot {
	int side = 0;
	float smallest_radius = 0.0f;
	bool air_born = false;
};

class WaypointAreas {
public:
	virtual ~WaypointAreas() = default;
	virtual bool exists(int p_area) const = 0;
	virtual bool owned(int p_area, int p_side) const = 0;
	virtual bool inside(int p_area, const Vec3 &p_position) const = 0;
	virtual float size(int p_area, int p_side) const = 0;
	virtual void randomize_position(int p_area, int p_side, float p_fraction, Vec3 &r_position) const = 0;
};

enum class WaypointKind : uint8_t {
	CloseTo = 0,
	MoveTo = 1,
	Patrol = 2,
	AltitudeCloseTo = 5,
	AltitudeMoveTo = 6,
	AltitudePatrol = 7,
	Empty = 8
};

class WaypointList {
public:
	virtual ~WaypointList() = default;
	virtual float urgency(const Vec3 &p_position, const WaypointBot &p_bot) = 0;
	virtual bool goal_point(Vec3 &r_out) const = 0;
	virtual WaypointKind kind() const = 0;
	virtual float radius() const { return 0.0f; }

	void set_error(bool p_error) { error_ = p_error; }
	bool error() const { return error_; }
	bool changed() const { return changed_; }
	void clear_changed() { changed_ = false; }
	bool finished() const { return finished_; }
	float speed() const { return speed_; }
	void set_speed(float p_speed) { speed_ = p_speed; }

protected:
	bool changed_ = true;
	bool finished_ = false;
	bool error_ = false;
	float speed_ = -1.0f;
};

class WPEmpty final : public WaypointList {
public:
	explicit WPEmpty(const Vec3 &p_position) : position_(p_position) {}
	float urgency(const Vec3 &p_position, const WaypointBot &p_bot) override;
	bool goal_point(Vec3 &r_out) const override;
	WaypointKind kind() const override { return WaypointKind::Empty; }

private:
	Vec3 position_{};
};

class WPMoveTo final : public WaypointList {
public:
	using Goals = std::list<std::pair<Vec3, int>>;

	WPMoveTo(float p_radius, float p_step_radius, const Goals &p_goals, const WaypointAreas &p_areas);
	WPMoveTo(float p_radius, float p_step_radius, const std::vector<Vec3> &p_positions, int p_last_area,
			const WaypointAreas &p_areas);
	float urgency(const Vec3 &p_position, const WaypointBot &p_bot) override;
	bool goal_point(Vec3 &r_out) const override;
	WaypointKind kind() const override { return WaypointKind::MoveTo; }
	float radius() const override { return radius_; }
	void coords(std::vector<Vec3> &r_out) const;
	const Goals &goals() const { return goals_; }

private:
	float radius_ = 0.0f;
	float step_radius_ = 0.0f;
	Goals goals_;
	const WaypointAreas &areas_;
};

class WPCloseTo final : public WaypointList {
public:
	WPCloseTo(uint32_t p_target, float p_radius, const InformationManager &p_information);
	float urgency(const Vec3 &p_position, const WaypointBot &p_bot) override;
	bool goal_point(Vec3 &r_out) const override;
	WaypointKind kind() const override { return WaypointKind::CloseTo; }
	float radius() const override { return radius_; }
	float modify_max_speed(float p_speed) const;
	uint32_t target() const { return target_; }

private:
	uint32_t target_ = kNoHandle;
	float radius_ = 0.0f;
	float distance_sq_ = 25.0f;
	Vec3 goal_{};
	const InformationManager &information_;
};

class WPPatrol final : public WaypointList {
public:
	WPPatrol(float p_radius, const std::vector<Vec3> &p_points);
	float urgency(const Vec3 &p_position, const WaypointBot &p_bot) override;
	bool goal_point(Vec3 &r_out) const override;
	WaypointKind kind() const override { return WaypointKind::Patrol; }
	float radius() const override { return radius_; }
	void coords(std::vector<Vec3> &r_out) const;

private:
	float radius_ = 0.0f;
	std::list<Vec3> points_;
};

class WPAltitudeCloseTo final : public WaypointList {
public:
	WPAltitudeCloseTo(uint32_t p_target, float p_radius, float p_ceiling, float p_distance,
			const InformationManager &p_information);
	float urgency(const Vec3 &p_position, const WaypointBot &p_bot) override;
	bool goal_point(Vec3 &r_out) const override;
	WaypointKind kind() const override { return WaypointKind::AltitudeCloseTo; }
	float radius() const override { return radius_; }
	float distance() const { return distance_; }

private:
	uint32_t target_ = kNoHandle;
	float radius_ = 0.0f;
	float ceiling_ = 0.0f;
	float distance_ = 0.0f;
	const InformationManager &information_;
};

class WPAltitudeMoveTo final : public WaypointList {
public:
	WPAltitudeMoveTo(float p_radius, float p_height, float p_distance, const std::vector<Vec3> &p_positions);
	float urgency(const Vec3 &p_position, const WaypointBot &p_bot) override;
	bool goal_point(Vec3 &r_out) const override;
	WaypointKind kind() const override { return WaypointKind::AltitudeMoveTo; }
	float radius() const override { return radius_; }
	float distance() const { return distance_; }
	void coords(std::vector<Vec3> &r_out) const;

private:
	float radius_ = 0.0f;
	float height_ = 0.0f;
	float last_distance_sq_ = 1e9f;
	float distance_ = 0.0f;
	std::list<Vec3> positions_;
};

class WPAltitudePatrol final : public WaypointList {
public:
	WPAltitudePatrol(float p_radius, float p_height, float p_distance, const std::vector<Vec3> &p_positions);
	float urgency(const Vec3 &p_position, const WaypointBot &p_bot) override;
	bool goal_point(Vec3 &r_out) const override;
	WaypointKind kind() const override { return WaypointKind::AltitudePatrol; }
	float radius() const override { return radius_; }
	float distance() const { return distance_; }
	void coords(std::vector<Vec3> &r_out) const;

private:
	float radius_ = 0.0f;
	float height_ = 0.0f;
	float distance_ = 0.0f;
	std::list<Vec3> positions_;
};

}
