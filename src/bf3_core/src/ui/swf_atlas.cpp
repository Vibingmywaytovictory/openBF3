#include "bf3/core/ui/swf_atlas.h"

#include "bf3/core/ui/swf_library.h"

#include <algorithm>
#include <map>
#include <set>
#include <string>

namespace bf3::core::ui {

namespace {

class Cutter {
public:
	Cutter(const SwfMovie &p_movie, std::string_view p_movie_name, WorkBudget &r_budget) :
			m_(p_movie), name_(p_movie_name), budget_(r_budget) {}

	Status run(SwfAtlasMap &r_map) {
		r_map.movie = std::string(name_);
		for (const SwfExternalImage &e : m_.external_images) {
			SwfAtlasImage im;
			im.image_id = e.image_id;
			im.file_name = e.file_name;
			im.texture_res = swf_atlas_texture_name(name_, e.file_name);
			im.width = e.width;
			im.height = e.height;
			r_map.images.push_back(std::move(im));
		}

		for (const SwfSprite &s : m_.sprites) {
			BF3_TRY(lists_of(s.id, s.timeline));
		}
		BF3_TRY(lists_of(kRootKey, m_.root));

		BF3_TRY(labels_of(kRootKey, m_.root, r_map));
		for (const SwfSprite &s : m_.sprites) {
			BF3_TRY(labels_of(s.id, s.timeline, r_map));
		}

		for (const SwfExport &e : m_.exports) {
			if (m_.find_sprite(e.id) == nullptr && m_.find_shape(e.id) == nullptr) {
				continue;
			}
			SwfAtlasRegion r;
			r.name = e.name;
			r.sprite = m_.find_sprite(e.id) != nullptr ? e.id : 0;
			BF3_TRY(append_character(e.id, SwfMatrix{}, 0, r.pieces));
			BF3_TRY(count_region(r));
			r_map.symbols.push_back(std::move(r));
		}

		for (const SwfSprite &s : m_.sprites) {
			const std::vector<std::vector<SwfDisplayEntry>> &lists = lists_[s.id];
			for (uint32_t f = 0; f < s.timeline.frames; ++f) {
				SwfAtlasRegion r;
				BF3_TRY(append_entries(lists[f], SwfMatrix{}, 1, r.pieces));
				if (r.pieces.empty()) {
					continue;
				}
				r.name = symbol_name(s.id) + "@" + std::to_string(f);
				r.sprite = s.id;
				r.frame = f;
				BF3_TRY(count_region(r));
				r_map.frames.push_back(std::move(r));
			}
		}

		for (const SwfSubImage &si : m_.sub_images) {
			SwfAtlasRegion r;
			r.name = "#" + std::to_string(si.id);
			r.pieces.push_back(piece_of(si, SwfMatrix{}));
			BF3_TRY(count_region(r));
			r_map.bare.push_back(std::move(r));
		}
		stats(r_map);
		return Status::success();
	}

private:
	static constexpr uint32_t kRootKey = 0x10000;
	enum class MemoState : uint8_t { None, Building, Done };
	struct Memo {
		MemoState state = MemoState::None;
		std::vector<SwfAtlasPiece> pieces;
	};

	Status charge(size_t p_units) {
		Status st = budget_.charge(p_units);
		if (!st) {
			return make_error("swf atlas " + std::string(name_) + ": work budget exhausted (" + st.error().message + ")");
		}
		return Status::success();
	}

	std::string symbol_name(uint16_t p_id) const {
		const std::string_view e = m_.export_name(p_id);
		return e.empty() ? "#" + std::to_string(p_id) : std::string(e);
	}

	Status lists_of(uint32_t p_id, const SwfTimeline &p_tl) {
		std::vector<uint32_t> frames;
		for (uint32_t f = 0; f < p_tl.frames; ++f) {
			frames.push_back(f);
		}
		if (p_tl.frames == 0) {
			frames.push_back(0);
		}
		for (const SwfFrameLabel &l : p_tl.labels) {
			if (l.frame >= p_tl.frames) {
				frames.push_back(l.frame);
			}
		}
		std::sort(frames.begin(), frames.end());
		frames.erase(std::unique(frames.begin(), frames.end()), frames.end());
		std::vector<std::vector<SwfDisplayEntry>> lists;
		Status st = display_lists_at(p_tl, frames, budget_, lists);
		if (!st) {
			return error_at("swf atlas " + std::string(name_) + " sprite " + std::to_string(p_id), st.error());
		}

		std::vector<std::vector<SwfDisplayEntry>> by_frame(size_t(p_tl.frames) + 1);
		for (size_t i = 0; i < frames.size(); ++i) {
			by_frame[frames[i]] = std::move(lists[i]);
		}
		lists_[p_id] = std::move(by_frame);
		return Status::success();
	}

	SwfAtlasPiece piece_of(const SwfSubImage &p_sub, const SwfMatrix &p_placement) const {
		SwfAtlasPiece p;
		p.sub_image = p_sub.id;
		p.image_id = p_sub.image_id;
		p.x1 = p_sub.x1;
		p.y1 = p_sub.y1;
		p.x2 = p_sub.x2;
		p.y2 = p_sub.y2;
		p.placement = p_placement;
		return p;
	}

	Status push(std::vector<SwfAtlasPiece> &r_out, const SwfAtlasPiece &p_piece) {
		BF3_TRY(charge(kSwfAtlasPieceUnits));
		if (r_out.size() >= kSwfAtlasMaxPiecesPerRegion) {
			return make_error("swf atlas " + std::string(name_) + ": more than " + std::to_string(kSwfAtlasMaxPiecesPerRegion) + " pieces in one region");
		}
		if (++total_pieces_ > kSwfAtlasMaxPieces) {
			return make_error("swf atlas " + std::string(name_) + ": more than " + std::to_string(kSwfAtlasMaxPieces) + " pieces in the map");
		}
		r_out.push_back(p_piece);
		return Status::success();
	}

	Status append_entries(const std::vector<SwfDisplayEntry> &p_entries, const SwfMatrix &p_parent, uint32_t p_depth, std::vector<SwfAtlasPiece> &r_out) {
		for (const SwfDisplayEntry &e : p_entries) {
			BF3_TRY(charge(kSwfAtlasPieceUnits));
			BF3_TRY(append_character(e.character, swf_concat(p_parent, e.matrix), p_depth, r_out));
		}
		return Status::success();
	}

	Status append_character(uint16_t p_id, const SwfMatrix &p_m, uint32_t p_depth, std::vector<SwfAtlasPiece> &r_out) {
		if (const SwfShape *shape = m_.find_shape(p_id)) {
			for (const SwfFillStyle &f : shape->fills) {
				const SwfSubImage *sub = f.kind == SwfFillKind::Bitmap ? m_.find_sub_image(f.bitmap_id) : nullptr;
				if (sub == nullptr) {
					continue;
				}
				BF3_TRY(push(r_out, piece_of(*sub, swf_concat(p_m, f.texel_to_shape()))));
			}
			return Status::success();
		}
		if (m_.find_sprite(p_id) == nullptr) {
			return Status::success();
		}
		const std::vector<SwfAtlasPiece> *frame0 = nullptr;
		BF3_TRY(sprite_frame0(p_id, p_depth, frame0));
		for (const SwfAtlasPiece &p : *frame0) {
			SwfAtlasPiece q = p;
			q.placement = swf_concat(p_m, p.placement);
			BF3_TRY(push(r_out, q));
		}
		return Status::success();
	}

	Status sprite_frame0(uint16_t p_id, uint32_t p_depth, const std::vector<SwfAtlasPiece> *&r_pieces) {
		Memo &memo = memo_[p_id];
		if (memo.state == MemoState::Done) {
			r_pieces = &memo.pieces;
			return Status::success();
		}
		if (memo.state == MemoState::Building) {
			return make_error("swf atlas " + std::string(name_) + ": sprite " + std::to_string(p_id) + " places itself (a cycle)");
		}
		if (p_depth > kSwfAtlasMaxNesting) {
			return make_error("swf atlas " + std::string(name_) + ": sprites nested deeper than " + std::to_string(kSwfAtlasMaxNesting));
		}
		memo.state = MemoState::Building;
		std::vector<SwfAtlasPiece> pieces;
		const auto it = lists_.find(p_id);
		if (it != lists_.end() && !it->second.empty()) {
			BF3_TRY(append_entries(it->second[0], SwfMatrix{}, p_depth + 1, pieces));
		}
		Memo &done = memo_[p_id];
		done.pieces = std::move(pieces);
		done.state = MemoState::Done;
		r_pieces = &done.pieces;
		return Status::success();
	}

	Status labels_of(uint32_t p_id, const SwfTimeline &p_tl, SwfAtlasMap &r_map) {
		const std::string symbol = p_id == kRootKey ? std::string("_root") : symbol_name(uint16_t(p_id));
		const std::vector<std::vector<SwfDisplayEntry>> &lists = lists_[p_id];
		for (const SwfFrameLabel &l : p_tl.labels) {
			SwfAtlasRegion r;
			r.name = symbol + "/" + l.name;
			r.sprite = p_id == kRootKey ? 0 : uint16_t(p_id);
			r.label = l.name;
			r.frame = l.frame;
			BF3_TRY(append_entries(lists[l.frame], SwfMatrix{}, 1, r.pieces));
			BF3_TRY(count_region(r));
			r_map.labels.push_back(std::move(r));
		}
		return Status::success();
	}

	Status count_region(const SwfAtlasRegion &p_region) { return charge(1 + p_region.name.size() / 16); }

	void stats(SwfAtlasMap &r_map) const {
		SwfAtlasStats &s = r_map.stats;
		s.sub_images = uint32_t(m_.sub_images.size());
		std::set<uint16_t> from_labels, from_symbols, from_frames;
		for (const SwfAtlasRegion &r : r_map.labels) {
			s.labels++;
			if (r.pieces.empty()) {
				s.labels_empty++;
			} else {
				s.labels_with_art++;
				(r.pieces.size() == 1 ? s.labels_one_piece : s.labels_several_pieces)++;
			}
			for (const SwfAtlasPiece &p : r.pieces) {
				from_labels.insert(p.sub_image);
			}
		}
		from_symbols = from_labels;
		for (const SwfAtlasRegion &r : r_map.symbols) {
			for (const SwfAtlasPiece &p : r.pieces) {
				from_symbols.insert(p.sub_image);
			}
		}
		for (const SwfAtlasRegion &r : r_map.frames) {
			for (const SwfAtlasPiece &p : r.pieces) {
				from_frames.insert(p.sub_image);
			}
		}
		s.sub_images_from_labels = uint32_t(from_labels.size());
		s.sub_images_from_symbols = uint32_t(from_symbols.size());
		s.sub_images_from_frames = uint32_t(from_frames.size());
		for (const SwfSubImage &si : m_.sub_images) {
			const SwfExternalImage *im = m_.find_external_image(si.image_id);
			if (im == nullptr) {
				s.pieces_without_image++;
			} else if (si.x2 > im->width || si.y2 > im->height) {
				s.rects_outside_atlas++;
			}
		}
	}

	const SwfMovie &m_;
	std::string_view name_;
	WorkBudget &budget_;
	std::map<uint32_t, std::vector<std::vector<SwfDisplayEntry>>> lists_;
	std::map<uint16_t, Memo> memo_;
	size_t total_pieces_ = 0;
};

}

const SwfAtlasRegion *SwfAtlasMap::find(std::string_view p_name) const {
	for (const std::vector<SwfAtlasRegion> *set : { &labels, &symbols, &frames, &bare }) {
		for (const SwfAtlasRegion &r : *set) {
			if (r.name == p_name) {
				return &r;
			}
		}
	}
	const SwfAtlasRegion *only = nullptr;
	std::set<uint16_t> sprites;
	for (const SwfAtlasRegion &r : labels) {
		if (r.label == p_name) {
			only = only == nullptr ? &r : only;
			sprites.insert(r.sprite);
		}
	}
	return sprites.size() == 1 ? only : nullptr;
}

Result<SwfAtlasMap> build_atlas_map(const SwfMovie &p_movie, std::string_view p_movie_name, WorkBudget &r_budget) {
	SwfAtlasMap map;
	Cutter cutter(p_movie, p_movie_name, r_budget);
	BF3_TRY(cutter.run(map));
	return map;
}

}
