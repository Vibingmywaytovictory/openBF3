#pragma once

#include "bf3/ai/knowledge/ai_object.h"

#include <array>
#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <vector>

namespace bf3::ai {

class Information;
class InformationManager;

class InformationGridPredicate {
public:
	virtual ~InformationGridPredicate() = default;
	virtual bool include_information(const Information &p_information) const = 0;
};

class UnitPredicate final : public InformationGridPredicate {
public:
	bool include_information(const Information &p_information) const override;
};

struct GridPlane {
	Vec3 normal{};
	float d = 0.0f;
};

struct GridFrustum {
	std::array<Vec3, 8> corners{};
	std::array<GridPlane, 6> planes{};
};

class AIInformationGrid {
public:
	struct InfoInfo {
		float x = 0.0f;
		float y = 0.0f;
		float radius = 0.0f;
		bool visited = false;
	};

	struct GridCellElem {
		uint32_t id = 0;
		InfoInfo *info = nullptr;
	};

	using Cell = std::list<GridCellElem>;

	AIInformationGrid(
			const InformationManager &p_information, int p_side, int p_dim_x, int p_dim_z, Vec2 p_min, Vec2 p_max);

	void add_info(uint32_t p_id);
	void remove_info(uint32_t p_id);
	bool safe_remove_info(uint32_t p_id, const InformationGridPredicate *p_remove);
	bool update_position(uint32_t p_id);
	void add_and_update_position(uint32_t p_id);

	void information_within_circle(std::vector<uint32_t> &r_out, const Vec2 &p_centre, float p_radius,
			const InformationGridPredicate *p_keep) const;
	void information_within_frustum(
			std::vector<uint32_t> &r_out, const GridFrustum &p_frustum, const InformationGridPredicate *p_keep) const;
	void information_within_rectangle(std::vector<uint32_t> &r_out, const Vec2 &p_min, const Vec2 &p_max,
			const InformationGridPredicate *p_keep) const;
	void all_information(std::vector<uint32_t> &r_out, const InformationGridPredicate *p_keep,
			const InformationGridPredicate *p_cull);
	void all_information(std::vector<uint32_t> &r_out, const InformationGridPredicate *p_keep) const;

	int side() const { return side_; }
	size_t size() const { return infos_.size(); }

private:
	void min_max(int &r_min_x, int &r_max_x, int &r_min_y, int &r_max_y, const Vec2 &p_min, const Vec2 &p_max) const;
	void add_element_to_cells(uint32_t p_id, InfoInfo *p_info);
	void remove_element_from_cells(uint32_t p_id, const InfoInfo &p_info);
	void update_element_in_cells(uint32_t p_id, const InfoInfo &p_old, InfoInfo *p_now);
	void erase_cell_element(int p_cell, uint32_t p_id);
	template <class Test>
	void visit_cells(std::vector<uint32_t> &r_out, int p_min_x, int p_max_x, int p_min_y, int p_max_y,
			const InformationGridPredicate *p_keep, Test p_test) const;
	InfoInfo *make_info(const Information &p_information) const;

	const InformationManager &information_;
	int side_ = 0;
	int dim_x_ = 1;
	int dim_z_ = 1;
	Vec2 min_{};
	Vec2 max_{};
	std::vector<Cell> cells_;
	std::map<uint32_t, std::unique_ptr<InfoInfo>> infos_;
	mutable std::vector<InfoInfo *> visited_;
};

}
