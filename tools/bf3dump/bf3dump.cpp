// bf3dump: read-only inspection of the BF3 install through bf3_core.
// Origin: the commands and output formats follow the ebx-dump probe
// (research/probes/tools/ebx-dump/src/main.rs), which drives IW4L's Rust readers.
//
// The install is located like the game does (--install=<dir>, OPENBF3_INSTALL, default path) and
// is only read; output goes to the paths given on the command line (refused inside the install).
//
// Commands
//   list <out.tsv>             every ebx/res entry (and the chunk count) of every bundle of every
//                              non-chunk, non-Loc superbundle, columns
//                              superbundle bundle kind name res_type original_size
//                              (same rows, order and bytes as the ebx-dump probe's `list`)
//   chunk <sb> <guid>...       chunk presence, decoded size, first 48 bytes (probe `chunk` format)
//   reshead <sb> <res>...      res type, meta, decoded size, first 64 bytes (probe `reshead` format)
//   chunks <sb>                the bundle chunk index of a superbundle: id, sha1, size, tail, range
//   verify <sb[,sb..]|@all>    load every bundle; check stored SHA1s and decode every ebx/res/chunk
//   info                       packages, layout, catalogs, superbundle sites
// EBX (same semantics, output and JSON as the probe's commands; <sb> may be "a,b,.." or @all-mp,
// the first superbundle that holds a partition wins):
//   find <sb> <needle>...      partitions whose lowercase name contains a needle:
//                              name, primary type, superbundle, bundle (sorted by name)
//   types <sb> <Type>...       every instance of the exact types: type, partition, instance GUID
//   dump <sb> <out_dir> <depth> <follow> <root>...
//                              one JSON per root: the root partition and the breadth-first closure of
//                              referenced partitions whose name contains one of the comma-separated
//                              <follow> substrings ("*" = all, "-" = none) up to <depth>; fx/ and
//                              sound/ are never followed unless "*"; mesh/texture/shader/sound asset
//                              partitions are listed but not expanded
//   ebxload <sb> [--walk]      bulk-parse every partition (timing, failures, bytes); --walk also
//                              decodes every value of every instance
//   ebxload <sb> --lookups     (H hygiene) then get() by name of every field of every instance, 3 passes
//   numcheck                   self-check of the probe's JSON number text (no install needed)
// Weapons (bf3_core gameplay readers; README "Weapon tables"):
//   weapon <partition> [--unlock <u_..>]... [--level <sb>] [--name]
//                              one resolved WeaponDef (a variant or a blueprint) with the level's
//                              body multipliers for its projectile; --name adds the localised name
//   weapons [--level <sb>]     one TSV row per SoldierWeaponUnlockAsset under weapons/
// Text database (text_commands.cpp): text <sid-or-hash>... | text --census [<sb-spec>] | text --tsv <out.tsv>
//   level <LEVEL> <MODE> [--legacy-mvdb]  canonical placement listing of one level x mode
//                              (level_commands.cpp; parity with research/probes/tools/level-oracle)
//   levelmodes <LEVEL>         the modes a level offers
//   collision <LEVEL> <MODE>   the collision soup: placements, tiles, hashes (collision_commands.cpp;
//                              parity with research/probes/tools/level-oracle collision)
//   prepare <LEVEL> <MODE> [--cache=<dir>] [--threads=N]
//                              placed meshes + weapon variants into the derived-data cache (prepare_commands.cpp)
// M1 T1 (world):
//   shapes <LEVEL> <MODE> [--cache=<dir> | --no-cache] [--threads=N] [--no-raycast] [--tiles]
//                              the static world's Jolt shapes through the cache (shapes_commands.cpp); `prepare
//                              ... --shapes` adds them to a prepare run
//   spawns <LEVEL> <MODE>      M1 T2: spawn points and character spawners, TSV (spawns_commands.cpp)
// L1 look:
//   looks <LEVEL> <MODE> [--materials] [--shader NAME]  the drawn-material census, M1 mapping against L1 (look_commands.cpp)
// S1 (server scale): nav bake|stats|components|path|probe and bots (nav_commands.h)
// S2 vehicle:
//   vehicledrive <LEVEL> <MODE> [<blueprint>]   the drive def and the vehicle model (vehicle_commands.cpp)
//   vehicle-replay <LEVEL> <MODE> [options]     courses and gates C / D / G1 / G1r / G2x (G2 reported), --handling, --sweep,
//                                               --hash-out (gate X), --log-out / --log-in (vehicle_commands.cpp)
// UI ui1 (docs/design/ui-swf-fonts.md §12.4; swf_commands.cpp):
//   swf census | movie <name> | labels <name> | layout <name> | hud | fonts
//                              SwfMovie symbol tables, atlas label maps, HUD positions, UI fonts
// UI ui2 (ui_commands.cpp; docs/design/ui-minimap-icons.md §15):
//   minimap <LEVEL> [MODE] | minimap --census   UIMinimapData, combat areas, objectives / the all-level census
//   hud-icons | item-icons [LEVEL] | hud-sources [--unbound]   the hud3dicons atlas, _small item icons, DataSources
// M2a weapons (docs/design/m2a-weapons.md §10; weapon_sim_commands.cpp):
//   weapon-sim <partition> | --all | --check   the weapon simulation on resolved defs (timing, drop, shots to kill)
//   range <partition> [...]    the shooting range on a level's static world (--perf: W11's load)
// M2a VFX1:
//   fx census | compile | simulate | check | bench | atlas | impacts   effects (fx_commands.cpp; docs/design/m2a-vfx1.md §7.7)
//
// Command semantics and output formats follow the ebx-dump probe
// (research/probes/tools/ebx-dump), which is built on the IW4L readers that bf3_core ports.
#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/payload.h"
#include "bf3/core/res_types.h"
#include "bf3/core/sb_formats.h"
#include "bf3/core/sha1_digest.h"
#include "bf3/core/superbundle_mount.h"
#include "bf3/core/thread_pool.h"
#include "ebx_commands.h"
#include "weapon_commands.h"
#include "text_commands.h"
#include "gameplay_commands.h"
#include "json_number.h"
#include "level_commands.h"
#include "collision_commands.h"
#include "prepare_commands.h"
#include "shapes_commands.h" // M1 T1
// M1 T2: spawn points and spawners of a level x mode.
#include "spawns_commands.h"
#include "logic_commands.h" // S4 logic
// L1 look: the drawn-material census.
#include "look_commands.h"
// S1: the navmesh and the scale bench.
#include "nav_commands.h"
// S2 vehicle: vehicledrive, vehicle-replay.
#include "vehicle_commands.h"
// UI ui1: the SwfMovie, atlas, layout and UI font commands.
#include "swf_commands.h"
// UI ui2: minimap, HUD icon and HUD source commands.
#include "ui_commands.h"
// M2a weapons: weapon-sim and range.
#include "weapon_sim_commands.h"
// M2a VFX1: bf3dump fx (effect compiler, simulator, atlas, impacts).
#include "fx_commands.h"
// M2a lc: lagcomp (the capsule pose table, E18 (a) deviation, E13 rewind rows).
#include "lagcomp_commands.h"

#include <fcntl.h>
#include <io.h>
#include <windows.h>

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

using namespace bf3::core;

namespace {

std::string narrow(const wchar_t *p_text) {
	const int len = static_cast<int>(wcslen(p_text));
	const int need = WideCharToMultiByte(CP_UTF8, 0, p_text, len, nullptr, 0, nullptr, nullptr);
	std::string out(static_cast<size_t>(need), '\0');
	WideCharToMultiByte(CP_UTF8, 0, p_text, len, out.data(), need, nullptr, nullptr);
	return out;
}

std::wstring widen(std::string_view p_text) {
	const int need = MultiByteToWideChar(CP_UTF8, 0, p_text.data(), static_cast<int>(p_text.size()), nullptr, 0);
	std::wstring out(static_cast<size_t>(need), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, p_text.data(), static_cast<int>(p_text.size()), out.data(), need);
	return out;
}

double seconds_since(std::chrono::steady_clock::time_point p_start) {
	return std::chrono::duration<double>(std::chrono::steady_clock::now() - p_start).count();
}

std::string hex_spaced(const uint8_t *p_bytes, size_t p_count) {
	std::string out;
	for (size_t i = 0; i < p_count; ++i) {
		char b[4];
		std::snprintf(b, sizeof(b), i ? " %02x" : "%02x", p_bytes[i]);
		out += b;
	}
	return out;
}

void append_u64(std::string &r_out, uint64_t p_value) {
	char buf[24];
	const auto res = std::to_chars(buf, buf + sizeof(buf), p_value);
	r_out.append(buf, res.ptr);
}

struct Context {
	InstallReader reader;
	std::shared_ptr<Install> install;
};

bool open_install(const std::string &p_install_dir, Context &r_ctx) {
	InstallLocateOptions options;
	options.cmdline_path = p_install_dir;
	if (!r_ctx.reader.locate(options)) {
		std::fprintf(stderr, "install not usable: %s\n", r_ctx.reader.describe().c_str());
		for (const std::string &p : r_ctx.reader.status().problems) {
			std::fprintf(stderr, "  %s\n", p.c_str());
		}
		return false;
	}
	Result<std::shared_ptr<Install>> install = Install::open(r_ctx.reader);
	if (!install) {
		std::fprintf(stderr, "open install: %s\n", install.error().message.c_str());
		return false;
	}
	r_ctx.install = *install;
	return true;
}

// Opens an output file after the write guard has cleared it (never inside the install).
FILE *open_output(const Context &p_ctx, const std::string &p_path) {
	const std::filesystem::path path(widen(p_path));
	const PathGuard guard = p_ctx.reader.write_guard();
	const PathGuard::Verdict verdict = guard.check_write_target(path);
	if (verdict != PathGuard::Verdict::Allowed) {
		std::fprintf(stderr, "refusing to write %s: %s\n", p_path.c_str(), PathGuard::verdict_name(verdict));
		return nullptr;
	}
	FILE *f = nullptr;
	if (_wfopen_s(&f, to_extended_length(path).c_str(), L"wb") != 0 || f == nullptr) {
		std::fprintf(stderr, "cannot create %s\n", p_path.c_str());
	}
	return f;
}

// --- list ---------------------------------------------------------------------------------------

int cmd_list(Context &p_ctx, const std::string &p_out) {
	const auto start = std::chrono::steady_clock::now();
	FILE *out = open_output(p_ctx, p_out);
	if (out == nullptr) {
		return 1;
	}
	std::vector<std::string> names;
	for (const std::string &sb : p_ctx.install->superbundle_names()) {
		const std::string l = to_lower_ascii(sb);
		if (l.find("chunks") != std::string::npos || l.rfind("win32/loc/", 0) == 0) {
			continue;
		}
		names.push_back(sb);
	}
	// Mount every superbundle, then read every bundle manifest, both in parallel; write in order.
	std::vector<Result<LevelMount>> mounts =
			parallel_map<Result<LevelMount>>(names.size(), [&](size_t i) { return p_ctx.install->mount_superbundle(names[i]); });
	struct Job {
		size_t sb;
		size_t bundle;
	};
	std::vector<Job> jobs;
	for (size_t s = 0; s < mounts.size(); ++s) {
		if (mounts[s]) {
			for (size_t b = 0; b < mounts[s]->bundle_count(); ++b) {
				jobs.push_back({ s, b });
			}
		}
	}
	struct Text {
		std::string rows;
		size_t entries = 0;
		std::string error;
	};
	std::vector<Text> texts = parallel_map<Text>(jobs.size(), [&](size_t j) {
		const LevelMount &m = *mounts[jobs[j].sb];
		const std::string &sb = names[jobs[j].sb];
		const std::string &bundle = m.bundle_name(jobs[j].bundle);
		Text t;
		Result<BundleManifest> manifest = m.read_manifest(jobs[j].bundle);
		if (!manifest) {
			t.error = sb + " " + bundle + ": " + manifest.error().message;
			return t;
		}
		std::string prefix = sb + "\t" + bundle + "\t";
		t.rows.reserve((manifest->ebx.size() + manifest->res.size() + 1) * (prefix.size() + 80));
		for (const EbxEntry &e : manifest->ebx) {
			t.rows += prefix;
			t.rows += "ebx\t";
			t.rows += e.name;
			t.rows += "\t\t";
			append_u64(t.rows, e.original_size);
			t.rows += '\n';
		}
		for (const ResEntry &e : manifest->res) {
			t.rows += prefix;
			t.rows += "res\t";
			t.rows += e.name;
			t.rows += '\t';
			t.rows += res_type_name(e.res_type);
			t.rows += '\t';
			append_u64(t.rows, e.original_size);
			t.rows += '\n';
		}
		t.rows += prefix;
		t.rows += "chunks\t";
		append_u64(t.rows, manifest->chunks.size());
		t.rows += "\t\t\n";
		t.entries = manifest->ebx.size() + manifest->res.size();
		return t;
	});

	static const char kHeader[] = "superbundle\tbundle\tkind\tname\tres_type\toriginal_size\n";
	std::fwrite(kHeader, 1, sizeof(kHeader) - 1, out);
	size_t job = 0, rows = 0, errors = 0;
	for (size_t s = 0; s < names.size(); ++s) {
		if (!mounts[s]) {
			std::fprintf(stderr, "%s: %s\n", names[s].c_str(), mounts[s].error().message.c_str());
			++errors;
			continue;
		}
		size_t n = 0;
		for (size_t b = 0; b < mounts[s]->bundle_count(); ++b, ++job) {
			const Text &t = texts[job];
			if (!t.error.empty()) {
				std::fprintf(stderr, "%s\n", t.error.c_str());
				++errors;
				continue;
			}
			std::fwrite(t.rows.data(), 1, t.rows.size(), out);
			n += t.entries;
		}
		rows += n;
		std::fprintf(stderr, "%s: %zu entries\n", names[s].c_str(), n);
	}
	const bool write_ok = std::fflush(out) == 0 && !std::ferror(out);
	std::fclose(out);
	std::fprintf(stderr, "list: %zu superbundles, %zu bundles, %zu entries, %zu errors, %.3f s\n", names.size(), jobs.size(), rows, errors,
			seconds_since(start));
	return (errors == 0 && write_ok) ? 0 : 1;
}

// --- chunk / reshead ----------------------------------------------------------------------------

Result<LevelMount> mount_loaded(Context &p_ctx, const std::string &p_sb) {
	Result<LevelMount> m = p_ctx.install->mount_superbundle(p_sb);
	if (!m) {
		return m;
	}
	Status st = m->load_all_bundles();
	if (!st) {
		return error_at("load bundles " + p_sb, st.error());
	}
	return m;
}

int cmd_chunk(Context &p_ctx, const std::string &p_sb, const std::vector<std::string> &p_ids) {
	Result<LevelMount> m = mount_loaded(p_ctx, p_sb);
	if (!m) {
		std::fprintf(stderr, "mount %s: %s\n", p_sb.c_str(), m.error().message.c_str());
		return 1;
	}
	for (const std::string &id : p_ids) {
		const std::optional<Guid> g = Guid::parse(id);
		if (!g) {
			std::printf("%s\tbad guid\n", id.c_str());
			continue;
		}
		const bool in_index = m->chunk_info(*g).has_value();
		Result<std::vector<uint8_t>> bytes = m->read_chunk(*g);
		if (bytes) {
			std::printf("%s\tin_bundle_index=%s\tdecoded=%zu\thead=%s\n", id.c_str(), in_index ? "true" : "false", bytes->size(),
					hex_spaced(bytes->data(), std::min<size_t>(bytes->size(), 48)).c_str());
		} else {
			std::printf("%s\tin_bundle_index=%s\terror=%s\n", id.c_str(), in_index ? "true" : "false", bytes.error().message.c_str());
		}
	}
	return 0;
}

int cmd_reshead(Context &p_ctx, const std::string &p_sb, const std::vector<std::string> &p_names) {
	Result<LevelMount> m = mount_loaded(p_ctx, p_sb);
	if (!m) {
		std::fprintf(stderr, "mount %s: %s\n", p_sb.c_str(), m.error().message.c_str());
		return 1;
	}
	for (const std::string &name : p_names) {
		Result<std::pair<ResInfo, std::vector<uint8_t>>> r = m->read_res(name);
		if (r) {
			const auto &[info, bytes] = *r;
			std::printf("%s\t%s\tmeta=%s\tlen=%zu\thead=%s\n", name.c_str(), res_type_name(info.res_type).c_str(),
					hex_spaced(info.res_meta.data(), 16).c_str(), bytes.size(), hex_spaced(bytes.data(), std::min<size_t>(bytes.size(), 64)).c_str());
		} else {
			std::printf("%s\terror=%s\n", name.c_str(), r.error().message.c_str());
		}
	}
	return 0;
}

int cmd_chunks(Context &p_ctx, const std::string &p_sb) {
	Result<LevelMount> m = mount_loaded(p_ctx, p_sb);
	if (!m) {
		std::fprintf(stderr, "mount %s: %s\n", p_sb.c_str(), m.error().message.c_str());
		return 1;
	}
	std::printf("id\tsha1\tstored_size\ttail_only\trange\n");
	for (const Guid &id : m->chunk_ids()) {
		const std::optional<ChunkInfo> info = m->chunk_info(id);
		std::string range = "-";
		if (info->range) {
			range = std::to_string(info->range->start) + ".." + std::to_string(info->range->end) + "@" + std::to_string(info->range->logical_offset);
		}
		std::printf("%s\t%s\t%llu\t%d\t%s\n", id.to_string().c_str(), info->sha1.to_string().c_str(),
				static_cast<unsigned long long>(info->stored_size), info->tail_only ? 1 : 0, range.c_str());
	}
	std::fprintf(stderr, "%s: %zu bundle chunks, %zu toc chunks (%zu chunk superbundles)\n", p_sb.c_str(), m->chunk_ids().size(),
			m->toc_chunk_count(), m->chunk_superbundles().size());
	return 0;
}

// --- verify -------------------------------------------------------------------------------------

int cmd_verify(Context &p_ctx, const std::string &p_spec) {
	std::vector<std::string> sbs;
	if (p_spec == "@all") {
		for (const std::string &sb : p_ctx.install->superbundle_names()) {
			const std::string l = to_lower_ascii(sb);
			if (l.find("chunks") == std::string::npos && l.rfind("win32/loc/", 0) != 0) {
				sbs.push_back(sb);
			}
		}
	} else {
		size_t pos = 0;
		while (pos <= p_spec.size()) {
			const size_t comma = std::min(p_spec.find(',', pos), p_spec.size());
			sbs.push_back(p_spec.substr(pos, comma - pos));
			pos = comma + 1;
		}
	}
	int failed_sbs = 0;
	for (const std::string &sb : sbs) {
		const auto start = std::chrono::steady_clock::now();
		Result<LevelMount> m = mount_loaded(p_ctx, sb);
		if (!m) {
			std::printf("%s\tmount error: %s\n", sb.c_str(), m.error().message.c_str());
			++failed_sbs;
			continue;
		}
		const std::vector<std::string> ebx = m->ebx_names();
		const std::vector<std::string> res = m->res_names();
		const std::vector<Guid> chunks = m->chunk_ids();
		std::atomic<size_t> bad_sha1{ 0 }, bad_decode{ 0 }, bad_ebx_size{ 0 }, tails{ 0 };
		std::atomic<uint64_t> decoded_bytes{ 0 };
		std::mutex report_mutex;
		std::vector<std::string> report;
		auto note = [&](std::string p_line) {
			std::lock_guard lock(report_mutex);
			if (report.size() < 20) {
				report.push_back(std::move(p_line));
			}
		};
		auto check_sha1 = [&](const EntryRef &p_entry, const std::string &p_what) {
			Result<std::vector<uint8_t>> stored = m->read_stored(p_entry);
			const std::optional<Sha1> want = m->entry_sha1(p_entry);
			if (!stored || !want || sha1_of(*stored) != *want) {
				++bad_sha1;
				note(p_what + ": stored sha1 " + (stored ? "differs" : stored.error().message));
			}
		};
		const size_t total = ebx.size() + res.size() + chunks.size();
		parallel_for(total, [&](size_t i) {
			if (i < ebx.size()) {
				check_sha1(EntryRef::ebx(ebx[i]), "ebx " + ebx[i]);
				Result<std::vector<uint8_t>> b = m->read_ebx(ebx[i]);
				if (!b) {
					++bad_decode;
					note("ebx " + ebx[i] + ": " + b.error().message);
					return;
				}
				decoded_bytes += b->size();
				Result<EbxHead> h = parse_ebx_head(*b);
				if (!h || h->file_len() != b->size()) {
					++bad_ebx_size;
					note("ebx " + ebx[i] + ": header size differs from the payload");
				}
				return;
			}
			i -= ebx.size();
			if (i < res.size()) {
				check_sha1(EntryRef::res(res[i]), "res " + res[i]);
				Result<std::pair<ResInfo, std::vector<uint8_t>>> r = m->read_res(res[i]);
				if (!r) {
					++bad_decode;
					note("res " + res[i] + ": " + r.error().message);
					return;
				}
				decoded_bytes += r->second.size();
				return;
			}
			i -= res.size();
			const Guid &id = chunks[i];
			const std::optional<ChunkInfo> info = m->chunk_info(id);
			if (info && !info->tail_only) {
				check_sha1(EntryRef::chunk(id), "chunk " + id.to_string());
			} else {
				++tails;
			}
			Result<std::vector<uint8_t>> b = m->read_chunk(id);
			if (!b) {
				++bad_decode;
				note("chunk " + id.to_string() + ": " + b.error().message);
				return;
			}
			decoded_bytes += b->size();
		});
		const bool ok = bad_sha1 == 0 && bad_decode == 0 && bad_ebx_size == 0;
		std::printf("%s\tbundles=%zu\tebx=%zu\tres=%zu\tchunks=%zu\ttail_only=%zu\tdecoded_mb=%.1f\tbad_sha1=%zu\tbad_decode=%zu\tbad_ebx_size=%zu\t%.2f s\t%s\n",
				sb.c_str(), m->bundle_count(), ebx.size(), res.size(), chunks.size(), tails.load(), decoded_bytes.load() / 1048576.0,
				bad_sha1.load(), bad_decode.load(), bad_ebx_size.load(), seconds_since(start), ok ? "OK" : "FAIL");
		for (const std::string &line : report) {
			std::printf("  %s\n", line.c_str());
		}
		std::fflush(stdout);
		failed_sbs += ok ? 0 : 1;
	}
	return failed_sbs == 0 ? 0 : 1;
}

// --- info ---------------------------------------------------------------------------------------

int cmd_info(Context &p_ctx) {
	const Install &in = *p_ctx.install;
	std::printf("root\t%s\n", p_ctx.reader.status().root_utf8.c_str());
	std::printf("layout\t%s\tsuperbundles=%zu\thead=%lld\tbase=%lld\n", in.layout_path().c_str(), in.layout().superbundles.size(),
			static_cast<long long>(in.layout().head.value_or(-1)), static_cast<long long>(in.layout().base.value_or(-1)));
	for (const Package &p : in.packages()) {
		std::printf("package\t%s\t%s\tauthoritative=%d\tversion=%lld\trequire=%lld\tmount_order=%lld\n", p.name.c_str(), p.data_dir.c_str(),
				p.authoritative ? 1 : 0, static_cast<long long>(p.version.value_or(-1)), static_cast<long long>(p.require_version.value_or(-1)),
				static_cast<long long>(p.mount_order));
	}
	std::printf("cas\tcatalogs=%zu\tentries=%zu\n", in.cas().catalog_count(), in.cas().size());
	for (const SuperbundleSite &s : in.sites()) {
		std::printf("site\t%s\tdelta=%d\tbase=%s\tpatch=%s\n", s.name.c_str(), s.delta ? 1 : 0, s.base_toc ? s.base_toc->c_str() : "-",
				s.patch_toc ? s.patch_toc->c_str() : "-");
	}
	std::printf("levels\t%zu\n", in.levels().size());
	return 0;
}

void usage() {
	std::fprintf(stderr,
			"usage: bf3dump [--install=<dir>] <command> ...\n"
			"  list <out.tsv>\n"
			"  chunk <sb> <guid>...\n"
			"  reshead <sb> <res>...\n"
			"  chunks <sb>\n"
			"  verify <sb[,sb..]|@all>\n"
			"  info\n"
			"  find <sb[,sb..]|@all-mp> <needle>...\n"
			"  types <sb[,sb..]|@all-mp> <Type>...\n"
			"  dump <sb[,sb..]|@all-mp> <out_dir> <depth> <follow> <root>...\n"
			"  ebxload <sb[,sb..]|@all-mp> [--walk]\n"
			"  ebxload <sb[,sb..]|@all-mp> --lookups\n" // H hygiene
			"  numcheck\n"
			"  weapon <partition> [--unlock <u_..>]... [--level <sb>] [--name]\n"
			"  weapons [--level <sb>]\n"
			"  text <sid-or-hash>... | text --census [<sb-spec>] | text --tsv <out.tsv>\n"
			"  level <LEVEL> <MODE> [--legacy-mvdb]\n"
			"  levelmodes <LEVEL>\n"
			"  collision <LEVEL> <MODE>\n"
			"  prepare <LEVEL> <MODE> [--cache=<dir>] [--threads=N] [--no-weapons] [--no-meshes] [--shapes]\n"
			"  shapes <LEVEL> <MODE> [--cache=<dir> | --no-cache] [--threads=N] [--no-raycast] [--tiles]\n");
	std::fprintf(stderr, "%s", bf3dump::gameplay_usage());
	std::fprintf(stderr, "  spawns <LEVEL> <MODE>\n"); // M1 T2
	std::fprintf(stderr, "%s", bf3dump::logic_usage()); // S4 logic
	std::fprintf(stderr, "  looks <LEVEL> <MODE> [--materials] [--shader NAME]...\n"); // L1 look
	std::fprintf(stderr, "%s", bf3dump::vehicle_usage()); // S2 vehicle
	std::fprintf(stderr, "%s", bf3dump::swf_usage()); // UI ui1
	std::fprintf(stderr, "%s", bf3dump::weapon_sim_usage()); // M2a weapons
	std::fprintf(stderr, "%s", bf3dump::fx_usage()); // M2a VFX1
	std::fprintf(stderr, "%s", bf3dump::lagcomp_usage()); // M2a lc
}

} // namespace

int wmain(int argc, wchar_t **argv) {
	std::vector<std::string> args;
	std::string install_dir;
	for (int i = 1; i < argc; ++i) {
		std::string a = narrow(argv[i]);
		if (a.rfind("--install=", 0) == 0) {
			install_dir = a.substr(10);
		} else {
			args.push_back(std::move(a));
		}
	}
	if (args.empty()) {
		usage();
		return 2;
	}
	// Plain LF line ends on stdout, as the probe (Rust) writes them.
	_setmode(_fileno(stdout), _O_BINARY);
	if (args[0] == "numcheck" && args.size() == 1) {
		// The probe's JSON number text rules on synthetic values; needs no install.
		const int failed = bf3dump::json_number_selfcheck();
		std::fprintf(stderr, "numcheck: %s\n", failed == 0 ? "PASS" : "FAIL");
		return failed == 0 ? 0 : 1;
	}
	const auto start = std::chrono::steady_clock::now();
	Context ctx;
	if (!open_install(install_dir, ctx)) {
		return 77; // no install: the same "skipped" code as the doctest suites
	}
	std::fprintf(stderr, "install opened in %.3f s (%zu superbundles, %zu CAS entries)\n", seconds_since(start), ctx.install->sites().size(),
			ctx.install->cas().size());
	const std::string &cmd = args[0];
	const std::vector<std::string> rest(args.begin() + 1, args.end());
	if (cmd == "list" && rest.size() == 1) {
		return cmd_list(ctx, rest[0]);
	}
	if (cmd == "chunk" && rest.size() >= 2) {
		return cmd_chunk(ctx, rest[0], std::vector<std::string>(rest.begin() + 1, rest.end()));
	}
	if (cmd == "reshead" && rest.size() >= 2) {
		return cmd_reshead(ctx, rest[0], std::vector<std::string>(rest.begin() + 1, rest.end()));
	}
	if (cmd == "chunks" && rest.size() == 1) {
		return cmd_chunks(ctx, rest[0]);
	}
	if (cmd == "verify" && rest.size() == 1) {
		return cmd_verify(ctx, rest[0]);
	}
	if (cmd == "info" && rest.empty()) {
		return cmd_info(ctx);
	}
	const bf3dump::EbxContext ebx_ctx{ &ctx.reader, ctx.install };
	if (cmd == "find" && rest.size() >= 2) {
		return bf3dump::cmd_find(ebx_ctx, rest[0], std::vector<std::string>(rest.begin() + 1, rest.end()));
	}
	if (cmd == "types" && rest.size() >= 2) {
		return bf3dump::cmd_types(ebx_ctx, rest[0], std::vector<std::string>(rest.begin() + 1, rest.end()));
	}
	if (cmd == "dump" && rest.size() >= 5) {
		size_t depth = 0;
		const auto res = std::from_chars(rest[2].data(), rest[2].data() + rest[2].size(), depth);
		if (res.ec != std::errc() || res.ptr != rest[2].data() + rest[2].size()) {
			std::fprintf(stderr, "depth: not a number: %s\n", rest[2].c_str());
			return 2;
		}
		return bf3dump::cmd_dump(ebx_ctx, rest[0], rest[1], depth, rest[3], std::vector<std::string>(rest.begin() + 4, rest.end()));
	}
	if (cmd == "ebxload" && (rest.size() == 1 || (rest.size() == 2 && rest[1] == "--walk"))) {
		return bf3dump::cmd_ebxload(ebx_ctx, rest[0], rest.size() == 2);
	}
	// H hygiene: times get() by name of every field of every instance (the EBX lookup tables).
	if (cmd == "ebxload" && rest.size() == 2 && rest[1] == "--lookups") {
		return bf3dump::cmd_ebxload(ebx_ctx, rest[0], false, true);
	}
	if (cmd == "weapon" && !rest.empty()) {
		return bf3dump::cmd_weapon(ebx_ctx, rest);
	}
	if (cmd == "weapons") {
		return bf3dump::cmd_weapons(ebx_ctx, rest);
	}
	if (cmd == "text" && rest.size() >= 1 && rest[0] == "--census" && rest.size() <= 2) {
		return bf3dump::cmd_text_census(ebx_ctx, rest.size() == 2 ? rest[1] : std::string());
	}
	if (cmd == "text" && rest.size() == 2 && rest[0] == "--tsv") {
		return bf3dump::cmd_text_tsv(ebx_ctx, rest[1]);
	}
	if (cmd == "text" && !rest.empty() && rest[0].rfind("--", 0) != 0) {
		return bf3dump::cmd_text_lookup(ebx_ctx, rest);
	}
	if (cmd == "level" && (rest.size() == 2 || (rest.size() == 3 && rest[2] == "--legacy-mvdb"))) {
		return bf3dump::cmd_level(ctx.install, rest[0], rest[1], rest.size() == 3);
	}
	if (cmd == "prepare") {
		return bf3dump::cmd_prepare(ctx.reader, ctx.install, rest);
	}
	if (cmd == "levelmodes" && rest.size() == 1) {
		return bf3dump::cmd_level_modes(ctx.install, rest[0]);
	}
	if (const int rc = bf3dump::dispatch_gameplay(ebx_ctx, cmd, rest); rc >= 0) {
		return rc;
	}
	if (cmd == "collision" && rest.size() == 2) {
		return bf3dump::cmd_collision(ctx.install, rest[0], rest[1]);
	}
	// M1 T1 (world): the static world's shapes.
	if (cmd == "shapes") {
		return bf3dump::cmd_shapes(ctx.reader, ctx.install, rest);
	}
	// M1 T2: spawns <LEVEL> <MODE> (spawns_commands.cpp; docs/design/m1.md §10.1).
	if (cmd == "spawns" && rest.size() == 2) {
		return bf3dump::cmd_spawns(ctx.install, rest[0], rest[1]);
	}
	// S4 logic: logic <sub> ... (logic_commands.cpp; docs/design/s4.md §9.4).
	if (cmd == "logic") {
		return bf3dump::cmd_logic(ctx.reader, ctx.install, rest);
	}
	// L1 look: looks <LEVEL> <MODE> [--materials] [--shader NAME] (look_commands.cpp).
	if (cmd == "looks") {
		return bf3dump::cmd_looks(ctx.install, rest);
	}
	// S1: nav ... and bots ... (nav_commands.cpp; docs/design/s1.md §12).
	if (cmd == "nav") {
		return bf3dump::cmd_nav(ctx.reader, ctx.install, rest);
	}
	if (cmd == "bots") {
		return bf3dump::cmd_bots(ctx.reader, ctx.install, rest);
	}
	// S2 vehicle: vehicledrive / vehicle-replay (vehicle_commands.cpp; docs/design/s2.md §1, §8).
	if (cmd == "vehicledrive") {
		return bf3dump::cmd_vehicledrive(ctx.install, rest);
	}
	if (cmd == "vehicle-replay") {
		return bf3dump::cmd_vehicle_replay(ctx.install, rest);
	}
	// UI ui1: swf <census|movie|labels|layout|hud|fonts> (swf_commands.cpp); 2 = usage.
	if (cmd == "swf") {
		if (const int rc = bf3dump::cmd_swf(ctx.reader, ctx.install, rest); rc != 2) {
			return rc;
		}
	}
	// UI ui2: minimap, hud-icons, item-icons, hud-sources (ui_commands.cpp).
	if (const int rc = bf3dump::dispatch_ui2(ebx_ctx, cmd, rest); rc >= 0) {
		return rc;
	}
	// M2a weapons: weapon-sim and range (weapon_sim_commands.cpp; docs/design/m2a-weapons.md §10).
	if (cmd == "weapon-sim") {
		return bf3dump::cmd_weapon_sim(ebx_ctx, rest);
	}
	if (cmd == "range") {
		return bf3dump::cmd_range(ebx_ctx, rest);
	}
	// M2a VFX1: fx <subcommand> (fx_commands.cpp; docs/design/m2a-vfx1.md §7.7).
	if (cmd == "fx") {
		return bf3dump::cmd_fx(ebx_ctx, rest);
	}
	// M2a lc: lagcomp <LEVEL> <MODE> (lagcomp_commands.cpp; docs/design/m2a-lc.md).
	if (cmd == "lagcomp") {
		return bf3dump::cmd_lagcomp(ebx_ctx, rest);
	}
	usage();
	return 2;
}
