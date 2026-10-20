#include <doctest/doctest.h>

#include "bf3/core/ebx.h"
#include "bf3/core/ebx_db.h"
#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/ui/text_census.h"
#include "bf3/core/ui/text_db.h"
#include "test_support.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <vector>

using namespace bf3::core;
using namespace bf3::core::ui;

namespace {

using Bytes = std::vector<uint8_t>;

void put32(Bytes &r_bytes, size_t p_at, uint32_t p_value) {
	for (int i = 0; i < 4; ++i) {
		r_bytes[p_at + i] = uint8_t(p_value >> (8 * i));
	}
}
uint32_t get32(const Bytes &p_bytes, size_t p_at) {
	uint32_t v = 0;
	std::memcpy(&v, p_bytes.data() + p_at, 4);
	return v;
}

Bytes make_histogram(size_t p_units, const std::vector<std::pair<size_t, uint16_t>> &p_set) {
	Bytes h(12 + 2 * p_units, 0);
	put32(h, 0, kTextHistogramMagic);
	put32(h, 4, uint32_t(h.size() - 8));
	put32(h, 8, uint32_t(p_set.size()));
	for (const auto &[index, unit] : p_set) {
		h[12 + 2 * index] = uint8_t(unit);
		h[12 + 2 * index + 1] = uint8_t(unit >> 8);
	}
	return h;
}

struct SynthEntry {
	uint32_t hash;
	Bytes encoded;
};

Bytes make_binary(const std::vector<SynthEntry> &p_entries, std::string_view p_tag = "Global") {
	const size_t tag_len = 16;
	const size_t table_at = 0x14 + tag_len;
	const size_t strings_at = table_at + 8 * p_entries.size();
	Bytes b(strings_at, 0);
	std::memcpy(b.data() + 0x14, p_tag.data(), p_tag.size());
	size_t offset = 0;
	for (size_t i = 0; i < p_entries.size(); ++i) {
		put32(b, table_at + 8 * i, p_entries[i].hash);
		put32(b, table_at + 8 * i + 4, uint32_t(offset));
		b.insert(b.end(), p_entries[i].encoded.begin(), p_entries[i].encoded.end());
		b.push_back(0);
		offset += p_entries[i].encoded.size() + 1;
	}
	put32(b, 0, kTextBinaryMagic);
	put32(b, 4, uint32_t(b.size() - 8));
	put32(b, 8, uint32_t(p_entries.size()));
	put32(b, 12, uint32_t(table_at - 8));
	put32(b, 16, uint32_t(strings_at - 8));
	return b;
}

Bytes ascii(std::string_view p_text) {
	return Bytes(p_text.begin(), p_text.end());
}

Bytes decode_histogram() {
	return make_histogram(512, { { 0x80, 0x00E9 }, { 0x81, 2 }, { 256 + 5, 0x20AC }, { 0x82, 0xD83D }, { 0x83, 0xDE00 }, { 0x84, 127 } });
}

std::string decode_one(const Bytes &p_encoded, size_t *r_errors = nullptr) {
	Result<TextDatabase> db = TextDatabase::parse(make_binary({ { 1, p_encoded } }), decode_histogram());
	REQUIRE_MESSAGE(db.ok(), (db.ok() ? std::string() : db.error().message));
	if (r_errors != nullptr) {
		*r_errors = db->decode_errors();
	}
	return std::string(*db->find(1));
}

std::string parse_error(const Bytes &p_binary, const Bytes &p_histogram) {
	Result<TextDatabase> db = TextDatabase::parse(p_binary, p_histogram);
	return db.ok() ? std::string() : db.error().message;
}

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

size_t walk(const TextDatabase &p_db) {
	size_t bytes = 0;
	for (size_t i = 0; i < p_db.size(); ++i) {
		bytes += p_db.text_at(i).size();
		if (i > 0) {
			CHECK(p_db.hash_at(i - 1) < p_db.hash_at(i));
		}
		REQUIRE(p_db.find(p_db.hash_at(i)).has_value());
	}
	return bytes;
}

struct InstallFixture {
	InstallReader reader;
	std::shared_ptr<const Install> install;
	bool open() {
		if (!reader.locate({})) {
			bf3::test::skip_install_case(reader.describe().c_str());
			return false;
		}
		Result<std::shared_ptr<Install>> opened = Install::open(reader);
		REQUIRE_MESSAGE(opened.ok(), (opened.ok() ? std::string() : opened.error().message));
		install = *opened;
		return true;
	}
};

}

TEST_SUITE("text-unit") {
	TEST_CASE("text: SID hash") {
		CHECK(text_hash("") == 0xFFFFFFFFu);
		CHECK(text_hash("A") == 0x20u);
		CHECK(text_hash("AB") == 0x20u * 33u + 66u);

		CHECK(text_hash("ID_H_US_B") == text_hash("ID_H_US_A") + 1);
		CHECK(text_hash("ID_H_US_A") == 0x34a54e7au);
		CHECK(text_hash("ID_P_WNAME_M16A4") == 0x0fd83dfau);

		CHECK(text_hash("id_h_us_a") != text_hash("ID_H_US_A"));
		CHECK(text_hash("ID_H_US_A") != ebx_name_hash("ID_H_US_A"));
	}

	TEST_CASE("text: decoding through the histogram") {
		size_t errors = 99;
		CHECK(decode_one(ascii("Hello, world"), &errors) == "Hello, world");
		CHECK(errors == 0);
		CHECK(decode_one({ 'M', 0x80, 'T' }, &errors) == "M\xC3\xA9T");
		CHECK(errors == 0);
		CHECK(decode_one({ 0x81, 0x85 }, &errors) == "\xE2\x82\xAC");
		CHECK(errors == 0);
		CHECK(decode_one({ 0x82, 0x83 }, &errors) == "\xF0\x9F\x98\x80");
		CHECK(errors == 0);
		CHECK(decode_one(Bytes{}, &errors).empty());
		CHECK(errors == 0);
	}

	TEST_CASE("text: malformed sequences are counted, never fatal") {
		size_t errors = 0;
		CHECK(decode_one({ 'a', 0x81 }, &errors) == "a");
		CHECK(errors == 1);
		CHECK(decode_one({ 0x81, 'x', 'y' }, &errors) == "y");
		CHECK(errors == 1);
		CHECK(decode_one({ 0x84, 0x90, 'z' }, &errors) == "z");
		CHECK(errors == 1);
		CHECK(decode_one({ 0x82, 'q' }, &errors) == "\xEF\xBF\xBDq");
		CHECK(errors == 1);
		CHECK(decode_one({ 0x83 }, &errors) == "\xEF\xBF\xBD");
		CHECK(errors == 1);
		CHECK(decode_one({ 0x82 }, &errors) == "\xEF\xBF\xBD");
		CHECK(errors == 1);
	}

	TEST_CASE("text: lookup, ordering, duplicates and the SID fallback") {
		const Bytes hist = make_histogram(256, {});
		std::vector<SynthEntry> entries = {
			{ text_hash("ID_B"), ascii("bee") },
			{ text_hash("ID_A"), ascii("ay") },
			{ text_hash("ID_M"), ascii("<MISSING>") },
			{ text_hash("ID_A"), ascii("second ay") },
			{ text_hash("ID_E"), ascii("") },
		};
		Result<TextDatabase> db = TextDatabase::parse(make_binary(entries, "Tag"), hist);
		REQUIRE(db.ok());
		CHECK(db->tag() == "Tag");
		CHECK(db->size() == 4);
		CHECK(db->duplicate_hashes() == 1);
		CHECK(db->decode_errors() == 0);
		CHECK(db->histogram_units() == 256);
		for (size_t i = 1; i < db->size(); ++i) {
			CHECK(db->hash_at(i - 1) < db->hash_at(i));
		}
		CHECK(db->find_sid("ID_A") == std::optional<std::string_view>("ay"));
		CHECK(db->find(text_hash("ID_B")) == std::optional<std::string_view>("bee"));
		CHECK_FALSE(db->find_sid("ID_Z").has_value());
		CHECK(db->find_sid("ID_E") == std::optional<std::string_view>(""));
		CHECK(db->resolves("ID_A"));
		CHECK(db->resolves("ID_E"));
		CHECK_FALSE(db->resolves("ID_M"));
		CHECK_FALSE(db->resolves("ID_Z"));
		CHECK(db->translate("ID_B") == "bee");
		CHECK(db->translate("ID_M") == "ID_M");
		CHECK(db->translate("ID_Z") == "ID_Z");
	}

	TEST_CASE("text: an empty database and an empty tag parse") {
		Result<TextDatabase> db = TextDatabase::parse(make_binary({}, ""), make_histogram(256, {}));
		REQUIRE(db.ok());
		CHECK(db->empty());
		CHECK(db->tag().empty());
		CHECK(db->translate("ID_X") == "ID_X");
	}

	TEST_CASE("text: header and table errors") {
		const Bytes hist = make_histogram(256, {});
		const Bytes good = make_binary({ { 7, ascii("seven") }, { 9, ascii("nine") } });
		REQUIRE(parse_error(good, hist).empty());

		Bytes b = good;
		put32(b, 0, 0x39001);
		CHECK(parse_error(b, hist).find("bad magic") != std::string::npos);
		b = good;
		put32(b, 4, get32(b, 4) + 1);
		CHECK(parse_error(b, hist).find("size field") != std::string::npos);
		b = Bytes(good.begin(), good.begin() + 0x10);
		CHECK(parse_error(b, hist).find("truncated") != std::string::npos);
		b = good;
		put32(b, 8, 0xFFFFFFFFu);
		CHECK(parse_error(b, hist).find("entry table") != std::string::npos);
		b = good;
		put32(b, 12, 0);
		CHECK(parse_error(b, hist).find("entry table") != std::string::npos);
		b = good;
		put32(b, 16, get32(b, 16) + 8);
		CHECK(parse_error(b, hist).find("string table offset") != std::string::npos);
		b = good;
		std::memset(b.data() + 0x14, 'x', 16);
		CHECK(parse_error(b, hist).find("tag") != std::string::npos);
		b = good;
		put32(b, 0x24 + 4, 0x7FFFFFF0u);
		CHECK(parse_error(b, hist).find("outside the chunk") != std::string::npos);
		b = good;
		b.back() = 'X';
		CHECK(parse_error(b, hist).find("NUL-terminated") != std::string::npos);
		CHECK(parse_error(Bytes{}, hist).find("truncated") != std::string::npos);
	}

	TEST_CASE("text: histogram errors") {
		const Bytes good = make_binary({ { 7, ascii("seven") } });
		Bytes h = make_histogram(256, {});
		put32(h, 0, kTextBinaryMagic);
		CHECK(parse_error(good, h).find("bad magic") != std::string::npos);
		h = make_histogram(255, {});
		CHECK(parse_error(good, h).find("code units") != std::string::npos);
		h = make_histogram(256, {});
		h.push_back(0);
		put32(h, 4, uint32_t(h.size() - 8));
		CHECK(parse_error(good, h).find("odd") != std::string::npos);
		h = make_histogram(256, {});
		put32(h, 4, 0);
		CHECK(parse_error(good, h).find("size field") != std::string::npos);
		CHECK(parse_error(good, Bytes(8, 0)).find("truncated") != std::string::npos);
	}

	TEST_CASE("text: overlapping strings cannot decode quadratically") {

		std::vector<SynthEntry> entries;
		entries.push_back({ 0, Bytes(4000, 'a') });
		Bytes b = make_binary(entries);
		const size_t n = 4000;
		const size_t table_at = 0x14 + 16;
		Bytes big(table_at + 8 * n, 0);
		std::memcpy(big.data(), b.data(), table_at);
		for (size_t i = 0; i < n; ++i) {
			put32(big, table_at + 8 * i, uint32_t(i));
			put32(big, table_at + 8 * i + 4, 0);
		}
		big.insert(big.end(), 4000, 'a');
		big.push_back(0);
		put32(big, 4, uint32_t(big.size() - 8));
		put32(big, 8, uint32_t(n));
		put32(big, 16, uint32_t(table_at + 8 * n - 8));
		CHECK(parse_error(big, make_histogram(256, {})).find("overlap") != std::string::npos);
	}

	TEST_CASE("text: synthetic mutants fail cleanly or parse fully") {
		std::vector<SynthEntry> entries;
		for (uint32_t i = 0; i < 64; ++i) {
			entries.push_back({ i * 7919u, { 'S', uint8_t('a' + i % 26), 0x80, 0x81, uint8_t(0x80 + i % 64), 0x82, 0x83 } });
		}
		const Bytes bin = make_binary(entries);
		const Bytes hist = decode_histogram();
		REQUIRE(TextDatabase::parse(bin, hist).ok());
		std::mt19937_64 rng(0x7E47DB01u);
		size_t parsed = 0, rejected = 0;
		for (size_t i = 0; i < fuzz_iters(2000); ++i) {
			Bytes b = bin;
			Bytes h = hist;
			mutate((i % 4 == 0) ? h : b, rng);
			Result<TextDatabase> db = TextDatabase::parse(b, h);
			if (db.ok()) {
				++parsed;
				walk(*db);
			} else {
				++rejected;
				CHECK_FALSE(db.error().message.empty());
			}
		}
		CHECK(parsed > 0);
		CHECK(rejected > 0);
	}

	TEST_CASE("text: SID strings") {
		CHECK(is_sid("ID_P_WNAME_M16A4"));
		CHECK(is_sid("ID_H_COMBAT_AREA_"));
		CHECK(is_sid("ID_"));
		CHECK_FALSE(is_sid("ID"));
		CHECK_FALSE(is_sid("id_p_wname"));
		CHECK_FALSE(is_sid("ID_P_WName"));
		CHECK_FALSE(is_sid("ID_P WNAME"));
		CHECK_FALSE(is_sid(" ID_P"));
		CHECK_FALSE(is_sid("XID_P"));
		CHECK(research_census_partitions().size() == 28);
	}
}

TEST_SUITE("text") {
	TEST_CASE("text: the retail database (us_loc) and the EBX SID census") {
		InstallFixture fx;
		if (!fx.open()) {
			return;
		}
		TextDatabaseSource source;
		Result<TextDatabase> db = open_text_database(fx.install, {}, &source);
		REQUIRE_MESSAGE(db.ok(), (db.ok() ? std::string() : db.error().message));

		CHECK(source.ref.partition == "localization/us_loc");
		CHECK(source.ref.language == "LanguageFormat_English");
		CHECK(iequals_ascii(source.superbundle, "Win32/Loc/en"));
		CHECK(source.ref.binary_chunk.to_string() == "50035522-22d3-b8c3-9aac-ce617deae923");
		CHECK(source.ref.binary_size == 599272);
		CHECK(source.ref.histogram_chunk.to_string() == "8ba57686-5a81-a229-726b-2d0f7ecb9b37");
		CHECK(source.ref.histogram_size == 6156);

		CHECK(db->size() == 15515);
		CHECK(db->tag() == "Global");
		CHECK(db->decode_errors() == 0);
		CHECK(db->duplicate_hashes() == 0);
		CHECK(text_hash("ID_H_US_A") == 0x34a54e7au);
		CHECK(text_hash("ID_H_US_B") == 0x34a54e7bu);
		CHECK(db->find(0x0fd83dfau) == std::optional<std::string_view>("M16A3"));
		CHECK(db->translate("ID_P_WNAME_M16A4") == "M16A3");
		CHECK(db->translate("ID_H_US_A") == "A");
		CHECK(db->translate("ID_H_US_B") == "B");
		CHECK(db->translate("ID_P_WNAME_AK74M") == "AK-74M");
		CHECK(db->translate("ID_M_LEVELNAME_MP_SUBWAY") == "OPERATION M\xC3\x89TRO");
		CHECK(db->translate("ID_P_VNAME_M1A2") == "ID_P_VNAME_M1A2");
		CHECK(walk(*db) > 0);

		Result<std::unique_ptr<EbxLibrary>> globals = EbxLibrary::open(fx.install, { "Win32/Globals" });
		REQUIRE(globals.ok());
		Result<std::vector<std::string>> listed = localized_text_partitions(**globals);
		REQUIRE(listed.ok());
		CHECK(listed->size() == 11);
		CHECK(listed->front() == "localization/us_loc");
		Result<TextDatabase> by_name = load_text_database(*fx.install, **globals, "localization/us_loc");
		REQUIRE(by_name.ok());
		CHECK(by_name->size() == db->size());

		Result<TextDatabase> german = load_text_database(*fx.install, **globals, "localization/ge_loc");
		CHECK_FALSE(german.ok());

		std::vector<std::string> sbs;
		for (const std::string_view sb : research_census_superbundles()) {
			sbs.emplace_back(sb);
		}
		Result<std::unique_ptr<EbxLibrary>> lib = EbxLibrary::open(fx.install, sbs);
		REQUIRE(lib.ok());
		const SidCensus census = sid_census(**lib, research_census_partitions(), *db);
		CHECK(census.missing_partitions.empty());
		CHECK(census.partitions == 28);
		CHECK(census.sids.size() == 1271);
		CHECK(census.resolved == 1247);
		CHECK(census.missing_marker + census.absent == 24);
		CHECK(census.absent == 24);
		MESSAGE("census: " << census.sids.size() << " SIDs, " << census.resolved << " resolve, " << census.absent << " absent, "
							<< census.missing_marker << " <MISSING>");
	}
}

TEST_SUITE("text-fuzz") {
	TEST_CASE("text: mutated retail chunks fail cleanly or parse fully") {
		InstallFixture fx;
		if (!fx.open()) {
			return;
		}
		Result<std::unique_ptr<EbxLibrary>> globals = EbxLibrary::open(fx.install, { "Win32/Globals" });
		REQUIRE(globals.ok());
		Result<TextDatabaseRef> ref = text_database_ref(**globals, "localization/us_loc");
		REQUIRE(ref.ok());
		Result<std::optional<Bytes>> bin = read_toc_chunk(*fx.install, "Win32/Loc/en", ref->binary_chunk);
		Result<std::optional<Bytes>> hist = read_toc_chunk(*fx.install, "Win32/Loc/en", ref->histogram_chunk);
		REQUIRE(bin.ok());
		REQUIRE(hist.ok());
		REQUIRE(bin->has_value());
		REQUIRE(hist->has_value());
		REQUIRE(TextDatabase::parse(**bin, **hist).ok());

		const size_t iters = fuzz_iters(300);
		std::mt19937_64 rng(0x0BF37E47u);
		size_t parsed = 0, rejected = 0, with_errors = 0, text_bytes = 0;
		for (size_t i = 0; i < iters; ++i) {
			Bytes b = **bin;
			Bytes h = **hist;
			if (i % 3 == 0) {
				mutate(h, rng);
			} else {
				mutate(b, rng);
			}
			Result<TextDatabase> db = TextDatabase::parse(b, h);
			if (db.ok()) {
				++parsed;
				with_errors += db->decode_errors() > 0 ? 1 : 0;
				text_bytes += walk(*db);
				(void)db->translate("ID_P_WNAME_M16A4");
			} else {
				++rejected;
				CHECK_FALSE(db.error().message.empty());
			}
		}
		CHECK(parsed > 0);
		CHECK(rejected > 0);
		std::printf("text fuzz: %zu mutants parsed (%zu with decode errors, %zu text bytes walked), %zu rejected\n", parsed, with_errors,
				text_bytes, rejected);
	}
}
