#include "bf3/host/rest_check.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace bf3::host {

namespace {

void fail(RestCycleResult &r_result, const char *p_format, double p_a = 0.0, double p_b = 0.0) {
	if (!r_result.ok && r_result.failure[0] != '\0') {
		return;
	}
	r_result.ok = false;
	std::snprintf(r_result.failure.data(), r_result.failure.size(), p_format, p_a, p_b);
}

uint64_t add_seconds(uint64_t p_ns, double p_seconds) {
	return p_ns + uint64_t(std::max(0.0, p_seconds) * 1e9);
}

}

std::vector<RestCycleResult> judge_rest(std::span<const RestSample> p_samples, const RestCheckOptions &p_options) {
	std::vector<RestCycleResult> results;
	size_t i = 0;
	const size_t n = p_samples.size();
	while (i < n) {
		if (!p_samples[i].input_idle) {
			++i;
			continue;
		}

		const size_t begin = i;
		size_t end = i;
		while (end < n && p_samples[end].input_idle) {
			++end;
		}
		i = end;
		RestCycleResult r;
		r.ok = true;
		const RestSample &first = p_samples[begin];
		r.stop_tick = first.tick;
		r.stop_ns = first.time_ns;
		r.period_end_ns = p_samples[end - 1].time_ns;

		size_t rest = end;
		for (size_t k = begin; k < end; ++k) {
			if (p_samples[k].horizontal_speed <= p_options.rest_speed) {
				rest = k;
				break;
			}
		}
		const uint64_t max_stop_ns = add_seconds(r.stop_ns, p_options.max_stop_s);
		if (rest == end || p_samples[rest].time_ns > max_stop_ns) {
			fail(r, "no rest within %.2f s of the stop (creep, not rubber-banding)", p_options.max_stop_s);
			r.rest_ns = rest == end ? 0 : p_samples[rest].time_ns;
			results.push_back(r);
			continue;
		}
		r.rest_ns = p_samples[rest].time_ns;
		r.window_start_ns = add_seconds(r.rest_ns, p_options.settle_s);

		for (size_t k = begin; k < end; ++k) {
			const uint64_t before = k > 0 ? p_samples[k - 1].reconciliations : p_samples[k].reconciliations;
			if (p_samples[k].reconciliations <= before) {
				continue;
			}
			const RestSample &s = p_samples[k];
			const int64_t after_stop = int64_t(s.last_reconcile_tick) - int64_t(r.stop_tick);
			if (after_stop <= int64_t(p_options.edge_ticks)) {
				++r.edge_corrections;
				r.pushed = true;
				r.window_start_ns = std::max(r.window_start_ns, add_seconds(s.time_ns, p_options.settle_s));
			} else {
				fail(r, "a reconciliation at rest: snapshot tick stop + %.0f (edge allowance %.0f)", double(after_stop), double(p_options.edge_ticks));
			}
		}
		r.window_end_ns = add_seconds(r.window_start_ns, p_options.window_s);
		if (r.window_end_ns > r.period_end_ns) {
			fail(r, "the rest window does not fit the rest period (it ends %.2f s after the period, pushed %.0f)",
					double(r.window_end_ns - r.period_end_ns) / 1e9, r.pushed ? 1.0 : 0.0);
		}

		float lo[3] = { INFINITY, INFINITY, INFINITY };
		float hi[3] = { -INFINITY, -INFINITY, -INFINITY };
		size_t in_window = 0;
		for (size_t k = begin; k < end; ++k) {
			const RestSample &s = p_samples[k];
			if (s.time_ns < r.window_start_ns || s.time_ns > r.window_end_ns) {
				continue;
			}
			++in_window;
			if (k > begin && s.reconciliations > p_samples[k - 1].reconciliations && s.time_ns > r.window_start_ns) {
				++r.window_reconciliations;
			}
			for (size_t a = 0; a < 3; ++a) {
				lo[a] = std::min(lo[a], s.eye[a]);
				hi[a] = std::max(hi[a], s.eye[a]);
			}
			r.max_visual_offset_m = std::max(r.max_visual_offset_m, s.visual_offset_m);
		}
		if (in_window == 0) {
			fail(r, "no samples in the rest window");
		} else {
			for (size_t a = 0; a < 3; ++a) {
				r.eye_jitter_m = std::max(r.eye_jitter_m, hi[a] - lo[a]);
			}
		}
		if (r.window_reconciliations > 0) {
			fail(r, "%.0f reconciliation(s) inside the rest window", double(r.window_reconciliations));
		}
		if (in_window > 0 && !(r.eye_jitter_m < p_options.max_eye_jitter_m)) {
			fail(r, "eye jitter %.6f m in the window (bound %.6f m)", double(r.eye_jitter_m), double(p_options.max_eye_jitter_m));
		}
		if (r.max_visual_offset_m != 0.0f) {
			fail(r, "visual offset %.6f m in the window (must be exactly 0)", double(r.max_visual_offset_m));
		}
		results.push_back(r);
	}
	return results;
}

}
