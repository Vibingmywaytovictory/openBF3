#include "bf3/ai/knowledge/information_grid.h"

#include "bf3/ai/knowledge/information_factory.h"
#include "bf3/ai/knowledge/plug_ins.h"

#include <algorithm>
#include <cmath>

namespace bf3::ai {

namespace {

int cell_index(float p_offset, int p_dim, float p_extent) {
	if (!(p_extent > 0.0f) || std::isnan(p_offset)) {
		return 0;
	}
	return int(std::clamp(p_offset * float(p_dim) / p_extent, 0.0f, float(p_dim - 1)));
}

}

bool UnitPredicate::include_information(const Information &p_information) const {
	return p_information.unit() != nullptr;
}

AIInformationGrid::AIInformationGrid(
		const InformationManager &p_information, int p_side, int p_dim_x, int p_dim_z, Vec2 p_min, Vec2 p_max) :
		information_(p_information), side_(p_side), dim_x_(std::max(p_dim_x, 1)), dim_z_(std::max(p_dim_z, 1)),
		min_(p_min), max_(p_max) {
	cells_.resize(size_t(dim_x_) * size_t(dim_z_) + 1);
}

AIInformationGrid::InfoInfo *AIInformationGrid::make_info(const Information &p_information) const {
	const IPIPhysical *const physical = p_information.physical();
	const Vec3 &position = p_information.position();
	InfoInfo *const info = new InfoInfo;
	info->x = position.x;
	info->y = position.z;
	info->radius = physical != nullptr ? physical->radius() : 0.0f;
	info->visited = false;
	return info;
}

void AIInformationGrid::add_info(uint32_t p_id) {
	const Information *const information = information_.information(p_id);
	if (information == nullptr) {
		return;
	}
	const auto found = infos_.find(p_id);
	if (found == infos_.end()) {
		InfoInfo *const info = make_info(*information);
		add_element_to_cells(p_id, info);
		infos_.emplace(p_id, std::unique_ptr<InfoInfo>(info));
		return;
	}
	InfoInfo *const info = found->second.get();
	const InfoInfo old = *info;
	info->x = information->position().x;
	info->y = information->position().z;
	update_element_in_cells(p_id, old, info);
}

void AIInformationGrid::remove_info(uint32_t p_id) {
	const auto found = infos_.find(p_id);
	if (found == infos_.end()) {
		return;
	}
	remove_element_from_cells(p_id, *found->second);
	infos_.erase(found);
}

bool AIInformationGrid::safe_remove_info(uint32_t p_id, const InformationGridPredicate *p_remove) {
	const auto found = infos_.find(p_id);
	if (found == infos_.end()) {
		return true;
	}
	const Information *const information = information_.information(p_id);
	if (information != nullptr && p_remove != nullptr && !p_remove->include_information(*information)) {
		return false;
	}
	remove_element_from_cells(p_id, *found->second);
	infos_.erase(found);
	return true;
}

bool AIInformationGrid::update_position(uint32_t p_id) {
	const Information *const information = information_.information(p_id);
	const auto found = infos_.find(p_id);
	if (information == nullptr || found == infos_.end()) {
		return false;
	}
	InfoInfo *const info = found->second.get();
	const Vec3 &position = information->position();
	const float dx = info->x - position.x;
	const float dy = info->y - position.z;
	if (dx * dx + dy * dy >= 1.0f) {
		const InfoInfo old = *info;
		info->x = position.x;
		info->y = position.z;
		update_element_in_cells(p_id, old, info);
		return true;
	}
	return false;
}

void AIInformationGrid::add_and_update_position(uint32_t p_id) {
	add_info(p_id);
}

void AIInformationGrid::min_max(
		int &r_min_x, int &r_max_x, int &r_min_y, int &r_max_y, const Vec2 &p_min, const Vec2 &p_max) const {
	const float extent_x = max_.x - min_.x;
	const float extent_y = max_.y - min_.y;
	r_min_x = cell_index(p_min.x - min_.x, dim_x_, extent_x);
	r_max_x = cell_index(p_max.x - min_.x, dim_x_, extent_x);
	r_min_y = cell_index(p_min.y - min_.y, dim_z_, extent_y);
	r_max_y = cell_index(p_max.y - min_.y, dim_z_, extent_y);
}

template <class Test>
void AIInformationGrid::visit_cells(std::vector<uint32_t> &r_out, int p_min_x, int p_max_x, int p_min_y, int p_max_y,
		const InformationGridPredicate *p_keep, Test p_test) const {
	for (int y = p_min_y; y <= p_max_y; ++y) {
		for (int x = p_min_x; x <= p_max_x; ++x) {
			for (const GridCellElem &element : cells_[size_t(x + dim_x_ * y)]) {
				InfoInfo *const info = element.info;
				if (info->visited) {
					continue;
				}
				info->visited = true;
				visited_.push_back(info);
				const Information *const information = information_.information(element.id);
				if (information != nullptr && (p_keep == nullptr || p_keep->include_information(*information)) &&
						p_test(*info, *information)) {
					r_out.push_back(element.id);
				}
			}
		}
	}
	for (InfoInfo *const info : visited_) {
		info->visited = false;
	}
	visited_.resize(0);
}

void AIInformationGrid::information_within_circle(std::vector<uint32_t> &r_out, const Vec2 &p_centre, float p_radius,
		const InformationGridPredicate *p_keep) const {
	int min_x = 0, max_x = 0, min_y = 0, max_y = 0;
	min_max(min_x, max_x, min_y, max_y, { p_centre.x - p_radius, p_centre.y - p_radius },
			{ p_centre.x + p_radius, p_centre.y + p_radius });
	visit_cells(r_out, min_x, max_x, min_y, max_y, p_keep, [&](const InfoInfo &p_info, const Information &) {
		const float reach = p_radius + p_info.radius;
		const float dx = p_centre.x - p_info.x;
		const float dy = p_centre.y - p_info.y;
		return reach * reach > dx * dx + dy * dy;
	});
}

void AIInformationGrid::information_within_frustum(
		std::vector<uint32_t> &r_out, const GridFrustum &p_frustum, const InformationGridPredicate *p_keep) const {
	Vec2 lo{ p_frustum.corners[0].x, p_frustum.corners[0].z };
	Vec2 hi = lo;
	for (size_t corner = 1; corner < p_frustum.corners.size(); ++corner) {
		const Vec3 &c = p_frustum.corners[corner];
		if (c.x < lo.x) {
			lo.x = c.x;
		} else if (c.x > hi.x) {
			hi.x = c.x;
		}
		if (c.z < lo.y) {
			lo.y = c.z;
		} else if (c.z > hi.y) {
			hi.y = c.z;
		}
	}
	int min_x = 0, max_x = 0, min_y = 0, max_y = 0;
	min_max(min_x, max_x, min_y, max_y, lo, hi);
	visit_cells(
			r_out, min_x, max_x, min_y, max_y, p_keep, [&](const InfoInfo &p_info, const Information &p_information) {
				const Vec3 &p = p_information.position();
				for (const GridPlane &plane : p_frustum.planes) {
					if (plane.normal.x * p.x + plane.normal.y * p.y + plane.normal.z * p.z + plane.d > p_info.radius) {
						return false;
					}
				}
				return true;
			});
}

void AIInformationGrid::information_within_rectangle(std::vector<uint32_t> &r_out, const Vec2 &p_min, const Vec2 &p_max,
		const InformationGridPredicate *p_keep) const {
	int min_x = 0, max_x = 0, min_y = 0, max_y = 0;
	min_max(min_x, max_x, min_y, max_y, p_min, p_max);
	visit_cells(r_out, min_x, max_x, min_y, max_y, p_keep, [&](const InfoInfo &p_info, const Information &) {
		return p_info.x - p_info.radius <= p_max.x && p_min.x <= p_info.x + p_info.radius &&
				p_info.y - p_info.radius <= p_max.y && p_min.y <= p_info.y + p_info.radius;
	});
}

void AIInformationGrid::all_information(
		std::vector<uint32_t> &r_out, const InformationGridPredicate *p_keep, const InformationGridPredicate *p_cull) {
	auto it = infos_.begin();
	while (it != infos_.end()) {
		if (p_keep == nullptr && p_cull == nullptr) {
			r_out.push_back(it->first);
			++it;
			continue;
		}
		const Information *const information = information_.information(it->first);
		if (information == nullptr || (p_cull != nullptr && p_cull->include_information(*information))) {
			remove_element_from_cells(it->first, *it->second);
			it = infos_.erase(it);
			continue;
		}
		if (p_keep == nullptr || p_keep->include_information(*information)) {
			r_out.push_back(it->first);
		}
		++it;
	}
}

void AIInformationGrid::all_information(std::vector<uint32_t> &r_out, const InformationGridPredicate *p_keep) const {
	for (const auto &[id, info] : infos_) {
		const Information *const information = information_.information(id);
		if (information == nullptr) {
			continue;
		}
		if (p_keep != nullptr && !p_keep->include_information(*information)) {
			continue;
		}
		r_out.push_back(id);
	}
}

void AIInformationGrid::update_element_in_cells(uint32_t p_id, const InfoInfo &p_old, InfoInfo *p_now) {
	int old_min_x = 0, old_max_x = 0, old_min_y = 0, old_max_y = 0;
	min_max(old_min_x, old_max_x, old_min_y, old_max_y, { p_old.x - p_old.radius, p_old.y - p_old.radius },
			{ p_old.x + p_old.radius, p_old.y + p_old.radius });
	int new_min_x = 0, new_max_x = 0, new_min_y = 0, new_max_y = 0;
	min_max(new_min_x, new_max_x, new_min_y, new_max_y, { p_now->x - p_now->radius, p_now->y - p_now->radius },
			{ p_now->x + p_now->radius, p_now->y + p_now->radius });
	if (new_min_x == old_min_x && new_max_x == old_max_x && new_min_y == old_min_y && new_max_y == old_max_y) {
		return;
	}
	for (int y = old_min_y; y <= old_max_y; ++y) {
		for (int x = old_min_x; x <= old_max_x; ++x) {
			erase_cell_element(x + dim_x_ * y, p_id);
		}
	}
	for (int y = new_min_y; y <= new_max_y; ++y) {
		for (int x = new_min_x; x <= new_max_x; ++x) {
			Cell &cell = cells_[size_t(x + dim_x_ * y)];
			cell.insert(cell.begin(), GridCellElem{ p_id, p_now });
		}
	}
}

void AIInformationGrid::add_element_to_cells(uint32_t p_id, InfoInfo *p_info) {
	int min_x = 0, max_x = 0, min_y = 0, max_y = 0;
	min_max(min_x, max_x, min_y, max_y, { p_info->x - p_info->radius, p_info->y - p_info->radius },
			{ p_info->x + p_info->radius, p_info->y + p_info->radius });
	for (int y = min_y; y <= max_y; ++y) {
		for (int x = min_x; x <= max_x; ++x) {
			Cell &cell = cells_[size_t(x + dim_x_ * y)];
			cell.insert(cell.begin(), GridCellElem{ p_id, p_info });
		}
	}
}

void AIInformationGrid::remove_element_from_cells(uint32_t p_id, const InfoInfo &p_info) {
	int min_x = 0, max_x = 0, min_y = 0, max_y = 0;
	min_max(min_x, max_x, min_y, max_y, { p_info.x - p_info.radius, p_info.y - p_info.radius },
			{ p_info.x + p_info.radius, p_info.y + p_info.radius });
	for (int y = min_y; y <= max_y; ++y) {
		for (int x = min_x; x <= max_x; ++x) {
			erase_cell_element(x + dim_x_ * y, p_id);
		}
	}
}

void AIInformationGrid::erase_cell_element(int p_cell, uint32_t p_id) {
	Cell &cell = cells_[size_t(p_cell)];
	const auto found =
			std::find_if(cell.begin(), cell.end(), [&](const GridCellElem &p_element) { return p_element.id == p_id; });
	if (found != cell.end()) {
		cell.erase(found);
	}
}

}
