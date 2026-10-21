#include <doctest/doctest.h>

#include "bf3/core/bundle_manifest.h"
#include "bf3/core/db_object.h"
#include "bf3/core/delta_bundle.h"
#include "bf3/core/ebx.h"
#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/payload.h"
#include "bf3/core/sb_formats.h"
#include "bf3/core/superbundle_mount.h"
#include "test_support.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

using namespace bf3::core;

namespace {

using Bytes = std::vector<uint8_t>;

size_t fuzz_iters(size_t p_default) {
	char buf[32] = {};
	size_t len = 0;
	if (getenv_s(&len, buf, sizeof(buf), "OPENBF3_FUZZ_ITERS") == 0 && len > 1) {
		const long v = std::strtol(buf, nullptr, 10);
		if (v > 0) {
			return size_t(v);
		}
	}
	return p_default;
}

void mutate(Bytes &r_bytes, std::mt19937_64 &r_rng) {
	if (r_bytes.empty()) {
		return;
	}
	const unsigned mode = unsigned(r_rng() % 5);
	if (mode == 0) {
		r_bytes.resize(size_t(r_rng() % r_bytes.size()));
		return;
	}
	const unsigned count = 1 + unsigned(r_rng() % 8);
	for (unsigned i = 0; i < count; ++i) {

		const size_t at = (r_rng() % 3 == 0) ? size_t(r_rng() % std::min<size_t>(r_bytes.size(), 0x200)) : size_t(r_rng() % r_bytes.size());
		if (mode == 1) {
			r_bytes[at] ^= uint8_t(1u << (r_rng() % 8));
		} else if (mode == 2) {
			r_bytes[at] = uint8_t(r_rng());
		} else if (mode == 3 && at + 4 <= r_bytes.size()) {
			const uint32_t v = (r_rng() % 2) ? 0xFFFFFFFFu : uint32_t(r_rng());
			std::memcpy(r_bytes.data() + at, &v, 4);
		} else {
			r_bytes[at] = 0;
		}
	}
}

size_t walk(const EbxValue &p_value) {
	size_t n = 1;
	if (p_value.kind() == EbxValue::Kind::Struct) {
		for (const EbxField &f : p_value.as_struct()->fields()) {
			n += walk(f.value);
		}
	} else if (p_value.kind() == EbxValue::Kind::Array) {
		for (const EbxValue e : *p_value.as_array()) {
			n += walk(e);
		}
	}
	return n;
}

size_t walk(const EbxPartition &p_partition) {
	size_t n = 0;
	for (size_t i = 0; i < p_partition.instance_count(); ++i) {
		for (const EbxField &f : p_partition.instance(i)->fields()) {
			n += walk(f.value);
		}
	}
	return n;
}

}

TEST_SUITE("fuzz") {
	TEST_CASE("fuzz: mutated ebx partitions, stored payloads and toc/manifest bytes") {
		bf3::test::Mounted m;
		if (!bf3::test::open_or_skip(m)) {
			return;
		}
		const std::shared_ptr<const Install> install = m.install;
		Result<LevelMount> mount = install->mount_superbundle("Win32/Levels/MP_Subway/MP_Subway");
		REQUIRE_MESSAGE(mount.ok(), (mount.ok() ? std::string() : mount.error().message));
		REQUIRE(mount->load_all_bundles().ok());

		const size_t iters = fuzz_iters(250);
		std::mt19937_64 rng(0x0BF3F022u);
		const char *names[] = { "levels/mp_subway/mp_subway", "weapons/m16a4/m16a4", "characters/soldiers/mpsoldier" };
		size_t inputs = 0, parsed = 0, rejected = 0, values = 0;
		for (const char *name : names) {
			const Result<Bytes> plain = mount->read_ebx(name);
			REQUIRE_MESSAGE(plain.ok(), name);
			const Result<Bytes> stored = mount->read_stored(EntryRef::ebx(name));
			REQUIRE_MESSAGE(stored.ok(), name);
			{
				Result<EbxPartition> base = EbxPartition::parse(*plain);
				REQUIRE_MESSAGE(base.ok(), name);
				CHECK(walk(*base) > 0);
			}
			++inputs;
			for (size_t i = 0; i < iters; ++i) {
				Bytes b = *plain;
				mutate(b, rng);
				Result<EbxPartition> p = EbxPartition::parse(std::move(b));
				if (p.ok()) {
					++parsed;
					values += walk(*p);
				} else {
					++rejected;
					CHECK_FALSE(p.error().message.empty());
				}
				Bytes s = *stored;
				mutate(s, rng);
				(void)decode_blocks(s);
			}
		}
		CHECK(inputs == 3);
		CHECK(rejected > 0);
		std::printf("fuzz: ebx mutants %zu parsed (%zu values walked), %zu rejected\n", parsed, values, rejected);

		Result<Bytes> toc = install->read_whole("Data/Win32/Levels/MP_Subway/MP_Subway.toc");
		REQUIRE(toc.ok());
		Bytes plain = *toc;
		REQUIRE(deobfuscate(plain).ok());
		REQUIRE(DbDocument::parse(plain).ok());
		size_t db_ok = 0;
		for (size_t i = 0; i < iters * 2; ++i) {
			Bytes b = plain;
			mutate(b, rng);
			db_ok += DbDocument::parse(b).ok() ? 1 : 0;
			(void)parse_toc(b);
			(void)parse_binary_manifest(b, b.size());
			(void)parse_cas_manifest(b);
			(void)DeltaBundle::parse(b, 0, b.size());
			(void)parse_ebx_head(b);
			Bytes raw = *toc;
			mutate(raw, rng);
			(void)deobfuscate(raw);
		}
		std::printf("fuzz: toc mutants %zu of %zu still parse as DbObject\n", db_ok, iters * 2);
	}
}
