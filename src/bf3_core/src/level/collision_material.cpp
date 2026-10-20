#include "bf3/core/level/collision_soup.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <utility>

namespace bf3::core::level {

namespace {

struct Word {
	std::string_view word;
	uint8_t surface;
};

constexpr Word kWords[] = {
	{ "concrete", surface::kConcrete },
	{ "cement", surface::kConcrete },
	{ "paintedmetal", surface::kPaintedMetal },
	{ "vehiclemetal", surface::kMetal },
	{ "metal", surface::kMetal },
	{ "glass", surface::kGlass },
	{ "window", surface::kGlass },
	{ "plaster", surface::kPlaster },
	{ "drywall", surface::kPlaster },
	{ "brick", surface::kBrick },
	{ "carpet", surface::kCarpet },
	{ "cloth", surface::kCloth },
	{ "fabric", surface::kCloth },
	{ "canvas", surface::kCloth },
	{ "cardboard", surface::kPaper },
	{ "paper", surface::kPaper },
	{ "plastic", surface::kPlastic },
	{ "rubber", surface::kRubber },
	{ "tire", surface::kRubber },
	{ "ceramic", surface::kCeramic },
	{ "porcelain", surface::kCeramic },
	{ "tile", surface::kCeramic },
	{ "asphalt", surface::kAsphalt },
	{ "tarmac", surface::kAsphalt },
	{ "gravel", surface::kGravel },
	{ "pebble", surface::kGravel },
	{ "grass", surface::kGrass },
	{ "foliage", surface::kFoliage },
	{ "leaves", surface::kFoliage },
	{ "vegetation", surface::kFoliage },
	{ "bush", surface::kFoliage },
	{ "hedge", surface::kFoliage },
	{ "tree", surface::kFoliage },
	{ "bark", surface::kBark },
	{ "wood", surface::kWood },
	{ "plank", surface::kWood },
	{ "water", surface::kWater },
	{ "slush", surface::kSlush },
	{ "snow", surface::kSnow },
	{ "ice", surface::kIce },
	{ "mud", surface::kMud },
	{ "dirt", surface::kDirt },
	{ "earth", surface::kDirt },
	{ "soil", surface::kDirt },
	{ "sand", surface::kSand },
	{ "rock", surface::kRock },
	{ "stone", surface::kRock },
	{ "soldier", surface::kFlesh },
	{ "flesh", surface::kFlesh },
	{ "cushion", surface::kCushion },
	{ "mattress", surface::kCushion },
	{ "fruit", surface::kFruit },
};

constexpr std::string_view kSurfaceNames[kSurfaceCount] = { "none", "bark", "brick", "carpet", "cloth", "concrete", "dirt", "flesh", "foliage", "glass",
	"grass", "gravel", "ice", "metal", "mud", "paper", "plaster", "rock", "sand", "snow", "water", "wood", "asphalt", "ceramic", "plastic", "rubber", "cushion",
	"fruit", "paintedmetal", "none", "slush", "none" };

constexpr int kMaxNameDepth = 4;

bool is_alpha(char p_c) {
	return (p_c >= 'a' && p_c <= 'z') || (p_c >= 'A' && p_c <= 'Z');
}

size_t weight(std::string_view p_name) {
	if (p_name.find("impacts/") != std::string_view::npos) {
		return 3;
	}
	if (p_name.find("generic_materials/") != std::string_view::npos || p_name.find("special_objects/") != std::string_view::npos) {
		return 2;
	}
	return 1;
}

Status collect_names(const EbxResolver &p_db, const EbxValue &p_value, int p_depth, WorkBudget &r_budget, std::vector<std::string_view> &r_out) {
	if (p_depth > kMaxNameDepth) {
		return Status::success();
	}
	BF3_TRY(r_budget.charge(1));
	switch (p_value.kind()) {
		case EbxValue::Kind::Ref: {
			const std::optional<EbxRef> ref = p_value.as_ref();
			if (ref && ref->kind == EbxRef::Kind::External) {
				if (const std::optional<std::string_view> name = p_db.name_of(ref->partition)) {
					BF3_TRY(charge_text(r_budget, *name));
					r_out.push_back(*name);
				}
			}
			break;
		}
		case EbxValue::Kind::Struct: {
			const std::optional<EbxStruct> s = p_value.as_struct();
			if (s) {
				for (const EbxField &field : s->fields()) {
					BF3_TRY(collect_names(p_db, field.value, p_depth + 1, r_budget, r_out));
				}
			}
			break;
		}
		case EbxValue::Kind::Array: {
			const OptionalEbxArray array = p_value.as_array();
			if (array) {
				BF3_TRY(r_budget.charge(array->size()));
				for (const EbxValue element : *array) {
					BF3_TRY(collect_names(p_db, element, p_depth + 1, r_budget, r_out));
				}
			}
			break;
		}
		default:
			break;
	}
	return Status::success();
}

}

std::string_view surface_name(uint8_t p_surface) {
	return p_surface < kSurfaceCount ? kSurfaceNames[p_surface] : "none";
}

std::string_view collision_body_name(uint8_t p_body) {
	switch (p_body) {
		case 0:
			return "collision";
		case 1:
			return "raycast";
		case 2:
			return "character";
		default:
			return "?";
	}
}

std::optional<uint8_t> classify_surface(std::string_view p_name) {
	std::string lower(p_name);
	for (char &c : lower) {
		if (c >= 'A' && c <= 'Z') {
			c = char(c - 'A' + 'a');
		}
	}
	std::vector<std::string_view> tokens;
	size_t at = 0;
	while (at < lower.size()) {
		while (at < lower.size() && !is_alpha(lower[at])) {
			++at;
		}
		const size_t start = at;
		while (at < lower.size() && is_alpha(lower[at])) {
			++at;
		}
		if (at > start) {
			tokens.emplace_back(lower.data() + start, at - start);
		}
	}
	for (const Word &w : kWords) {
		for (std::string_view token : tokens) {
			const bool hit = w.word.size() <= 4 ? token.starts_with(w.word) : token.find(w.word) != std::string_view::npos;
			if (hit) {
				return w.surface;
			}
		}
	}
	return std::nullopt;
}

size_t SurfaceMaterials::resolved_pairs() const {
	return size_t(std::count_if(pairs_.begin(), pairs_.end(), [](const std::optional<uint8_t> &p_pair) { return p_pair.has_value(); }));
}

SurfaceMaterials::Lookup SurfaceMaterials::lookup(uint32_t p_value, uint8_t &r_surface) const {
	const size_t pair = p_value & kMaterialPairMask;
	if (pair >= pairs_.size() || !pairs_[pair]) {
		return Lookup::Unknown;
	}
	const uint8_t material = *pairs_[pair];
	if (material >= index_map_.size()) {
		return Lookup::Unknown;
	}
	const uint32_t row = index_map_[material];
	if (row >= rows_.size()) {
		return Lookup::Unknown;
	}
	if (!rows_[row]) {
		return Lookup::Unnamed;
	}
	r_surface = *rows_[row];
	return Lookup::Surface;
}

Result<SurfaceMaterials> SurfaceMaterials::read(const EbxResolver &p_db, const EbxInstance &p_root, WorkBudget &r_budget) {
	BF3_TRY(r_budget.status());
	SurfaceMaterials out;
	const std::optional<EbxInstance> grid = p_db.follow(p_root.data(), "RuntimeMaterialGrid");
	if (!grid) {
		return out;
	}
	const EbxPartition &from = grid->partition();
	if (const std::optional<EbxValue> pairs = grid->get("MaterialPairs")) {
		const OptionalEbxArray array = pairs->as_array();
		if (array) {
			BF3_TRY(r_budget.charge(array->size()));
			out.pairs_.reserve(array->size());
			for (const EbxValue value : *array) {
				std::optional<uint8_t> material;
				const std::optional<EbxRef> ref = value.as_ref();
				if (ref) {
					if (const std::optional<EbxInstance> pair = p_db.resolve(from, *ref)) {
						const std::optional<EbxValue> index = pair->get("PhysicsMaterialIndex");
						const std::optional<int64_t> i = index ? index->as_i64() : std::nullopt;
						if (i) {
							material = uint8_t(uint64_t(*i));
						}
					}
				}
				out.pairs_.push_back(material);
			}
		}
	}
	if (const std::optional<EbxValue> map = grid->get("MaterialIndexMap")) {
		const OptionalEbxArray array = map->as_array();
		if (array) {
			BF3_TRY(r_budget.charge(array->size()));
			out.index_map_.reserve(array->size());
			for (const EbxValue value : *array) {
				out.index_map_.push_back(value.as_u32().value_or(0));
			}
		}
	}
	const std::optional<EbxValue> default_value = grid->get("DefaultMaterialIndex");
	const std::optional<uint32_t> default_row = default_value ? default_value->as_u32() : std::nullopt;
	const std::optional<EbxValue> rows = grid->get("InteractionGrid");
	const OptionalEbxArray row_array = rows ? rows->as_array() : OptionalEbxArray();
	if (!row_array) {
		return out;
	}
	BF3_TRY(r_budget.charge(row_array->size()));

	std::map<std::pair<const EbxPartition *, uint32_t>, std::vector<std::pair<uint8_t, size_t>>> voters;
	std::vector<std::string_view> names;
	uint32_t at = 0;
	for (const EbxValue row : *row_array) {
		const uint32_t index = at++;
		if (default_row && *default_row == index) {
			out.rows_.push_back(std::nullopt);
			continue;
		}
		std::array<size_t, kSurfaceCount> votes{};
		const std::optional<EbxValue> items = row.get("Items");
		const OptionalEbxArray item_array = items ? items->as_array() : OptionalEbxArray();
		if (item_array) {
			BF3_TRY(r_budget.charge(item_array->size()));
			for (const EbxValue item : *item_array) {
				for (std::string_view list : { std::string_view("PhysicsMaterialProperties"), std::string_view("PhysicsPropertyProperties") }) {
					const std::optional<EbxValue> relations = item.get(list);
					const OptionalEbxArray relation_array = relations ? relations->as_array() : OptionalEbxArray();
					if (!relation_array) {
						continue;
					}
					BF3_TRY(r_budget.charge(relation_array->size()));
					for (const EbxValue relation_value : *relation_array) {
						const std::optional<EbxRef> ref = relation_value.as_ref();
						if (!ref) {
							continue;
						}
						const std::optional<EbxInstance> relation = p_db.resolve(from, *ref);
						if (!relation) {
							continue;
						}
						const auto key = std::make_pair(&relation->partition(), relation->index());
						auto it = voters.find(key);
						if (it == voters.end()) {
							names.clear();
							for (const EbxField &field : relation->fields()) {
								BF3_TRY(collect_names(p_db, field.value, 0, r_budget, names));
							}
							std::vector<std::pair<uint8_t, size_t>> cast;
							for (std::string_view name : names) {
								if (const std::optional<uint8_t> s = classify_surface(name)) {
									cast.emplace_back(*s, weight(name));
								}
							}
							it = voters.emplace(key, std::move(cast)).first;
						}
						for (const auto &[s, w] : it->second) {
							votes[s] += w;
						}
					}
				}
			}
		}
		const size_t best = *std::max_element(votes.begin(), votes.end());
		std::optional<uint8_t> chosen;
		if (best > 0) {
			for (const Word &w : kWords) {
				if (votes[w.surface] == best) {
					chosen = w.surface;
					break;
				}
			}
		}
		out.rows_.push_back(chosen);
	}
	return out;
}

}
