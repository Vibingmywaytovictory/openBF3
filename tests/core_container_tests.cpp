// Container layer against the retail install (read-only; skipped without an install). Asserts
// counts and hashes only, nothing from the install is stored. Reference numbers: the IW4L Rust
// readers (research/probes/tools/ebx-dump `list`) on the same install, patch v7 + Xpack1-5.
#include <doctest/doctest.h>

#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/payload.h"
#include "bf3/core/res_types.h"
#include "bf3/core/sha1_digest.h"
#include "bf3/core/superbundle_mount.h"
#include "bf3/core/thread_pool.h"
#include "test_support.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

using namespace bf3::core;

namespace {

struct Mounted {
	InstallReader reader;
	std::shared_ptr<Install> install;
};

bool open_or_skip(Mounted &r_m) {
	if (!r_m.reader.locate({})) {
		bf3::test::skip_install_case(r_m.reader.describe().c_str());
		return false;
	}
	Result<std::shared_ptr<Install>> install = Install::open(r_m.reader);
	REQUIRE_MESSAGE(install.ok(), (install.ok() ? std::string() : install.error().message));
	r_m.install = *install;
	return true;
}

bool listed(const std::string &p_superbundle) {
	const std::string l = to_lower_ascii(p_superbundle);
	return l.find("chunks") == std::string::npos && l.rfind("win32/loc/", 0) != 0;
}

LevelMount mount_all(const Install &p_install, const char *p_name) {
	Result<LevelMount> m = p_install.mount_superbundle(p_name);
	REQUIRE_MESSAGE(m.ok(), (m.ok() ? std::string() : m.error().message));
	const Status loaded = m->load_all_bundles();
	REQUIRE_MESSAGE(loaded.ok(), (loaded.ok() ? std::string() : loaded.error().message));
	return std::move(*m);
}

} // namespace

TEST_SUITE("install") {
	TEST_CASE("containers: packages, patched layout, merged CAS catalog") {
		Mounted m;
		if (!open_or_skip(m)) {
			return;
		}
		const Install &in = *m.install;
		CHECK(in.layout_path() == "Update/Patch/Data/layout.toc");
		CHECK(in.layout().superbundles.size() == 74);
		CHECK(in.layout().head == std::optional<int64_t>(138593));
		CHECK(in.layout().base == std::optional<int64_t>(69287));
		REQUIRE(in.packages().size() == 6);
		const Package &patch = in.packages().back();
		CHECK(patch.authoritative);
		CHECK(patch.version == std::optional<int64_t>(7));
		const int64_t required[] = { 2, 4, 5, 6, 7 };
		for (size_t i = 0; i < 5; ++i) {
			CHECK(in.packages()[i].name == "xpack" + std::to_string(i + 1));
			CHECK(in.packages()[i].require_version == std::optional<int64_t>(required[i]));
		}
		CHECK(in.cas().catalog_count() == 2);
		CHECK(in.cas().size() == 140232);
		CHECK(in.levels().size() == 29);
		CHECK(std::count_if(in.sites().begin(), in.sites().end(), [](const SuperbundleSite &s) { return s.delta; }) == 26);
		const SuperbundleSite *xp2 = in.site("win32/levels/xp2_palace/xp2_palace");
		REQUIRE(xp2 != nullptr);
		CHECK(xp2->delta);
		CHECK(xp2->base_toc == std::optional<std::string>("Update/Xpack2/Data/Win32/Levels/XP2_Palace/XP2_Palace.toc"));
		CHECK(xp2->patch_toc == std::optional<std::string>("Update/Patch/Data/Win32/Levels/XP2_Palace/XP2_Palace.toc"));
		const SuperbundleSite *xp5 = in.site("Win32/Levels/XP5_001/XP5_001");
		REQUIRE(xp5 != nullptr);
		CHECK(!xp5->patch_toc.has_value());
		CHECK(in.level("xp1_001")->expansion == std::optional<uint8_t>(1));
	}

	TEST_CASE("containers: every bundle manifest of every listed superbundle") {
		Mounted m;
		if (!open_or_skip(m)) {
			return;
		}
		const InstallSnapshot before = m.reader.snapshot();
		std::vector<std::string> names;
		for (const std::string &sb : m.install->superbundle_names()) {
			if (listed(sb)) {
				names.push_back(sb);
			}
		}
		CHECK(names.size() == 53);
		std::vector<Result<LevelMount>> mounts =
				parallel_map<Result<LevelMount>>(names.size(), [&](size_t i) { return m.install->mount_superbundle(names[i]); });
		struct Job {
			size_t sb, bundle;
		};
		std::vector<Job> jobs;
		size_t mount_errors = 0;
		for (size_t s = 0; s < mounts.size(); ++s) {
			if (!mounts[s]) {
				++mount_errors;
				MESSAGE(names[s], ": ", mounts[s].error().message);
				continue;
			}
			for (size_t b = 0; b < mounts[s]->bundle_count(); ++b) {
				jobs.push_back({ s, b });
			}
		}
		struct Count {
			bool ok = false;
			size_t ebx = 0, res = 0, chunks = 0;
			bool has_shaderdb = false;
		};
		const std::vector<Count> counts = parallel_map<Count>(jobs.size(), [&](size_t j) {
			Count c;
			Result<BundleManifest> manifest = mounts[jobs[j].sb]->read_manifest(jobs[j].bundle);
			if (!manifest) {
				return c;
			}
			c.ok = true;
			c.ebx = manifest->ebx.size();
			c.res = manifest->res.size();
			c.chunks = manifest->chunks.size();
			for (const ResEntry &r : manifest->res) {
				c.has_shaderdb |= r.name == "levels/mp_subway/mp_subway/shaderdb" && r.res_type == 0x36F3F2C0u && r.original_size == 28820786;
			}
			return c;
		});
		size_t errors = mount_errors, ebx = 0, res = 0, chunks = 0, shaderdb = 0;
		struct PerSb {
			size_t bundles = 0, ebx = 0, res = 0, chunks = 0;
		};
		std::vector<PerSb> per(names.size());
		for (size_t j = 0; j < jobs.size(); ++j) {
			const Count &c = counts[j];
			errors += c.ok ? 0 : 1;
			ebx += c.ebx;
			res += c.res;
			chunks += c.chunks;
			shaderdb += c.has_shaderdb ? 1 : 0;
			PerSb &p = per[jobs[j].sb];
			p.bundles++;
			p.ebx += c.ebx;
			p.res += c.res;
			p.chunks += c.chunks;
		}
		MESSAGE("bundles ", jobs.size(), ", ebx ", ebx, ", res ", res, ", chunks ", chunks, ", errors ", errors);
		CHECK(errors == 0);
		CHECK(jobs.size() == 1159);
		CHECK(ebx == 826908);
		CHECK(res == 266655);
		CHECK(chunks == 334072);
		CHECK(ebx + res + jobs.size() + 1 == 1094723); // rows of the `list` TSV incl. header
		CHECK(shaderdb == 1);
		auto expect = [&](const char *p_sb, size_t p_bundles, size_t p_ebx, size_t p_res, size_t p_chunks) {
			const auto it = std::find(names.begin(), names.end(), p_sb);
			REQUIRE(it != names.end());
			const PerSb &p = per[static_cast<size_t>(it - names.begin())];
			CHECK_MESSAGE((p.bundles == p_bundles && p.ebx == p_ebx && p.res == p_res && p.chunks == p_chunks), p_sb);
		};
		expect("Win32/Levels/MP_001/MP_001", 17, 21126, 6260, 7961); // patched CAS level
		expect("Win32/Levels/MP_Subway/MP_Subway", 20, 19523, 5811, 6684);
		expect("Win32/Levels/XP1_001/XP1_001", 16, 22610, 6408, 8215); // binary, full replacement
		expect("Win32/Levels/XP2_Factory/XP2_Factory", 14, 15151, 3901, 5314); // delta + base bundles
		expect("Win32/Levels/XP5_001/XP5_001", 19, 25354, 6949, 9422); // binary, base only
		expect("Win32/Globals", 2, 824, 9, 7);
		const SnapshotDiff diff = diff_snapshots(before, m.reader.snapshot());
		CHECK_MESSAGE(diff.clean(), diff.summary());
	}

	TEST_CASE("containers: CAS level index, res decode, toc chunk, catalog headers") {
		Mounted m;
		if (!open_or_skip(m)) {
			return;
		}
		const LevelMount level = mount_all(*m.install, "Win32/Levels/MP_Subway/MP_Subway");
		CHECK(level.bundle_count() == 20);
		CHECK(level.chunk_superbundles().size() == 5);
		CHECK(level.ebx_names().size() == 12524);
		CHECK(level.res_names().size() == 4219);
		CHECK(level.chunk_ids().size() == 4877);
		CHECK(level.toc_chunk_count() > 20000);

		const std::optional<ResInfo> info = level.res_info("Levels/MP_Subway/MP_Subway/ShaderDB");
		REQUIRE(info.has_value());
		CHECK(info->res_type == 0x36F3F2C0u);
		Result<std::pair<ResInfo, std::vector<uint8_t>>> shaderdb = level.read_res("levels/mp_subway/mp_subway/shaderdb");
		REQUIRE(shaderdb.ok());
		CHECK(shaderdb->second.size() == 28820786);
		const EntryRef ref = EntryRef::res("levels/mp_subway/mp_subway/shaderdb");
		Result<std::vector<uint8_t>> stored = level.read_stored(ref);
		REQUIRE(stored.ok());
		CHECK(sha1_of(*stored) == *level.entry_sha1(ref));

		// A toc-only chunk (Chunks0-2/MpChunks/SpChunks), framed.
		const Guid toc_chunk = *Guid::parse("54620b35-bdb6-a4f1-c73a-44712121a7a1");
		CHECK(!level.chunk_info(toc_chunk).has_value());
		CHECK(level.has_chunk(toc_chunk));
		Result<std::vector<uint8_t>> chunk = level.read_chunk(toc_chunk);
		REQUIRE(chunk.ok());
		CHECK(chunk->size() == 179792);
		CHECK(!level.read_chunk(*Guid::parse("00000000-0000-0000-0000-000000000001")).ok());

		// Every ebx is raw and names its own partition; the partition index has one entry each.
		Result<std::unordered_map<Guid, std::string, GuidHash>> partitions = level.ebx_partition_index();
		REQUIRE(partitions.ok());
		MESSAGE("MP_Subway partitions: ", partitions->size());
		CHECK(partitions->size() == 12524); // one partition per ebx name

		// The 32-byte cas_NN.cas header before a payload matches its catalog entry.
		const std::vector<std::string> ebx = level.ebx_names();
		size_t checked = 0, bad = 0;
		for (size_t i = 0; i < ebx.size() && checked < 300; i += 37) {
			const std::optional<Sha1> sha1 = level.entry_sha1(EntryRef::ebx(ebx[i]));
			if (sha1 && m.install->cas().contains(*sha1)) {
				++checked;
				bad += m.install->cas().check_entry_header(*sha1).ok() ? 0 : 1;
			}
		}
		CHECK(checked > 100);
		CHECK(bad == 0);
	}

	TEST_CASE("containers: delta level rebuilt through the run view") {
		Mounted m;
		if (!open_or_skip(m)) {
			return;
		}
		const LevelMount level = mount_all(*m.install, "Win32/Levels/XP2_Palace/XP2_Palace");
		CHECK(level.bundle_count() == 14);
		CHECK(level.chunk_superbundles().size() == 6); // + Win32/Xp2Chunks
		const std::vector<std::string> ebx = level.ebx_names();
		CHECK(ebx.size() == 11120);
		std::atomic<size_t> bad_sha1{ 0 }, bad_head{ 0 };
		parallel_for(ebx.size(), [&](size_t i) {
			const EntryRef ref = EntryRef::ebx(ebx[i]);
			Result<std::vector<uint8_t>> stored = level.read_stored(ref);
			if (!stored || sha1_of(*stored) != *level.entry_sha1(ref)) {
				++bad_sha1;
				return;
			}
			Result<EbxHead> head = parse_ebx_head(*stored);
			if (!head || head->file_len() != stored->size()) {
				++bad_head;
			}
		});
		CHECK(bad_sha1 == 0);
		CHECK(bad_head == 0);
	}

	TEST_CASE("containers: tail-only chunks of a binary level resolve to whole payloads") {
		Mounted m;
		if (!open_or_skip(m)) {
			return;
		}
		const LevelMount level = mount_all(*m.install, "Win32/Levels/XP1_001/XP1_001");
		std::vector<Guid> tails;
		for (const Guid &id : level.chunk_ids()) {
			if (level.chunk_info(id)->tail_only) {
				tails.push_back(id);
			}
		}
		CHECK(tails.size() == 1068);
		std::atomic<size_t> resolved{ 0 }, in_cas{ 0 }, bad{ 0 };
		parallel_for(tails.size(), [&](size_t i) {
			const ChunkInfo info = *level.chunk_info(tails[i]);
			in_cas += m.install->cas().contains(info.sha1) ? 1 : 0;
			Result<std::vector<uint8_t>> whole = level.read_chunk(tails[i]);
			if (!whole) {
				++bad;
				return;
			}
			// The tail decodes to the whole payload's decoded bytes from logicalOffset on.
			Result<std::vector<uint8_t>> tail_stored = level.read_stored(EntryRef::chunk(tails[i]));
			Result<std::vector<uint8_t>> tail = tail_stored ? decode_chunk(tails[i], *tail_stored) : tail_stored;
			const uint64_t from = info.range ? info.range->logical_offset : 0;
			if (!tail || from + tail->size() != whole->size() || !std::equal(tail->begin(), tail->end(), whole->begin() + static_cast<ptrdiff_t>(from))) {
				++bad;
				return;
			}
			++resolved;
		});
		MESSAGE("XP1_001 tails: ", tails.size(), ", whole copy in CAS: ", in_cas.load());
		CHECK(bad == 0);
		CHECK(resolved == tails.size());
		CHECK(in_cas == 1068);
	}
}
