// Display lists of a SWF timeline (bf3/core/ui/swf_movie.h display_lists_at; docs/design/ui-swf-fonts.md §5.1, §9):
// the timeline's PlaceObject / RemoveObject events replayed once, in tag order, with a snapshot taken whenever the
// replay passes a requested frame. Cost O(events + sum of the snapshot sizes); every replayed event charges one unit
// and every copied entry kSwfAtlasPieceUnits-like 4 units, before the work is done.
#include "bf3/core/ui/swf_movie.h"

#include <algorithm>
#include <map>
#include <numeric>
#include <string>

namespace bf3::core::ui {

namespace {
constexpr size_t kCopiedEntryUnits = 4;
} // namespace

Status display_lists_at(const SwfTimeline &p_timeline, std::span<const uint32_t> p_frames, WorkBudget &r_budget,
		std::vector<std::vector<SwfDisplayEntry>> &r_lists) {
	r_lists.clear();
	for (const uint32_t f : p_frames) {
		if (f > p_timeline.frames) {
			return make_error("swf: display list of frame " + std::to_string(f) + " requested on a timeline of " + std::to_string(p_timeline.frames) +
					" frames");
		}
	}
	BF3_TRY(r_budget.charge(1 + p_frames.size()));
	std::vector<size_t> order(p_frames.size());
	std::iota(order.begin(), order.end(), size_t(0));
	std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return p_frames[a] < p_frames[b]; });
	r_lists.resize(p_frames.size());

	std::map<uint16_t, SwfDisplayEntry> list;
	size_t next = 0; // next event to replay
	const std::vector<SwfDisplayEvent> &events = p_timeline.events;
	auto apply = [&](const SwfDisplayEvent &e) {
		switch (e.op) {
			case SwfDisplayOp::Place: {
				SwfDisplayEntry entry;
				entry.depth = e.depth;
				entry.character = e.character;
				entry.matrix = e.has_matrix ? e.matrix : SwfMatrix{};
				entry.color = e.has_color ? e.color : SwfColorTransform{};
				entry.clip_depth = e.clip_depth;
				entry.name = e.name;
				list[e.depth] = entry;
				break;
			}
			case SwfDisplayOp::Modify:
			case SwfDisplayOp::Replace: {
				auto it = list.find(e.depth);
				if (it == list.end()) {
					break; // nothing at that depth: Flash ignores the move
				}
				SwfDisplayEntry &entry = it->second;
				if (e.op == SwfDisplayOp::Replace) {
					entry.character = e.character;
				}
				if (e.has_matrix) {
					entry.matrix = e.matrix;
				}
				if (e.has_color) {
					entry.color = e.color;
				}
				if (e.clip_depth != 0) {
					entry.clip_depth = e.clip_depth;
				}
				if (e.has_name) {
					entry.name = e.name;
				}
				break;
			}
			case SwfDisplayOp::Remove:
				list.erase(e.depth);
				break;
		}
	};
	size_t i = 0;
	while (i < order.size()) {
		const uint32_t frame = p_frames[order[i]];
		while (next < events.size() && events[next].frame <= frame) {
			BF3_TRY(r_budget.charge(1));
			apply(events[next]);
			++next;
		}
		// Every request for this frame gets its own copy.
		BF3_TRY(r_budget.charge(kCopiedEntryUnits * list.size()));
		std::vector<SwfDisplayEntry> snapshot;
		snapshot.reserve(list.size());
		for (const auto &[depth, entry] : list) {
			snapshot.push_back(entry);
		}
		size_t j = i + 1;
		while (j < order.size() && p_frames[order[j]] == frame) {
			BF3_TRY(r_budget.charge(kCopiedEntryUnits * snapshot.size()));
			r_lists[order[j]] = snapshot;
			++j;
		}
		r_lists[order[i]] = std::move(snapshot);
		i = j;
	}
	return Status::success();
}

} // namespace bf3::core::ui
