#include <doctest/doctest.h>

#include "bf3/core/cache/cache_key.h"
#include "bf3/core/cache/digest.h"
#include "bf3/core/cache/disk_cache.h"
#include "test_support.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace bf3::core;

namespace {

struct Row {
	std::string kind;
	Sha1Digest sha1;
	uint64_t stored_size = 0;
	uint64_t decoded_size = 0;
	std::string meta_hex;
};

bool read_file(const fs::path &p_path, std::vector<uint8_t> &r_out) {
	r_out.clear();
	FILE *f = nullptr;
	if (_wfopen_s(&f, p_path.c_str(), L"rb") != 0 || f == nullptr) {
		return false;
	}
	std::fseek(f, 0, SEEK_END);
	const long long n = _ftelli64(f);
	std::fseek(f, 0, SEEK_SET);
	r_out.resize(static_cast<size_t>(n));
	const bool ok = n == 0 || std::fread(r_out.data(), 1, r_out.size(), f) == r_out.size();
	std::fclose(f);
	return ok;
}

std::vector<std::string> split_tabs(const std::string &p_line) {
	std::vector<std::string> out;
	size_t at = 0;
	for (;;) {
		const size_t tab = p_line.find('\t', at);
		out.push_back(p_line.substr(at, tab == std::string::npos ? std::string::npos : tab - at));
		if (tab == std::string::npos) {
			return out;
		}
		at = tab + 1;
	}
}

bool load_index(const fs::path &p_dir, std::vector<Row> &r_rows) {
	std::vector<uint8_t> bytes;
	if (!read_file(p_dir / L"index.tsv", bytes)) {
		return false;
	}
	std::string text(bytes.begin(), bytes.end());
	size_t at = text.find('\n');
	while (at != std::string::npos && at + 1 < text.size()) {
		const size_t end = text.find('\n', at + 1);
		std::string line = text.substr(at + 1, end == std::string::npos ? std::string::npos : end - at - 1);
		at = end;
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		const std::vector<std::string> f = split_tabs(line);
		if (f.size() != 7) {
			continue;
		}
		const std::optional<Sha1Digest> sha1 = Sha1Digest::from_hex(f[2]);
		if (!sha1) {
			return false;
		}
		Row r;
		r.kind = f[0];
		r.sha1 = *sha1;
		r.stored_size = std::strtoull(f[3].c_str(), nullptr, 10);
		r.decoded_size = std::strtoull(f[4].c_str(), nullptr, 10);
		r.meta_hex = f[6];
		r_rows.push_back(std::move(r));
	}
	return true;
}

std::vector<uint8_t> parse_meta(const std::string &p_hex) {
	std::vector<uint8_t> out;
	for (size_t i = 0; i + 1 < p_hex.size(); i += 3) {
		out.push_back(static_cast<uint8_t>(std::strtoul(p_hex.substr(i, 2).c_str(), nullptr, 16)));
	}
	return out;
}

CacheKey key_of(const Row &p_row) {
	CacheKeyInputs in;
	in.kind = p_row.kind == "res" ? "raw-res" : "raw-chunk";
	in.sources = { p_row.sha1 };
	in.converter_version = 1;
	in.engine = "cache-data-test";
	in.format_version = 1;
	return *make_cache_key(in);
}

double seconds_since(std::chrono::steady_clock::time_point p_t0) {
	return std::chrono::duration<double>(std::chrono::steady_clock::now() - p_t0).count();
}

}

TEST_SUITE("cache_data") {
	TEST_CASE("cache data: manifest SHA1s and a round trip of real payloads") {
		wchar_t env[1024];
		const DWORD env_len = GetEnvironmentVariableW(L"OPENBF3_CACHE_TESTDATA", env, 1024);
		if (env_len == 0 || env_len >= 1024) {
			bf3::test::skip_install_case("OPENBF3_CACHE_TESTDATA not set");
			return;
		}
		const fs::path dir(env);
		std::vector<Row> rows;
		REQUIRE(load_index(dir, rows));
		REQUIRE(!rows.empty());

		auto t0 = std::chrono::steady_clock::now();
		uint64_t stored_bytes = 0;
		size_t sha1_ok = 0;
		std::vector<uint8_t> buf;
		for (const Row &r : rows) {
			REQUIRE(read_file(dir / L"stored" / (r.sha1.hex() + ".bin"), buf));
			CHECK(buf.size() == r.stored_size);
			stored_bytes += buf.size();
			sha1_ok += sha1_of(buf) == r.sha1 ? 1 : 0;
		}
		const double t_sha1 = seconds_since(t0);
		CHECK(sha1_ok == rows.size());
		MESSAGE("entries ", rows.size(), ", stored ", stored_bytes, " B, SHA-1 matches ", sha1_ok, ", ", t_sha1, " s (with file reads)");

		wchar_t tmp[MAX_PATH];
		GetTempPathW(MAX_PATH, tmp);
		const fs::path base = fs::path(tmp) / (L"openbf3-cache-data-" + std::to_wstring(GetCurrentProcessId()));
		const fs::path fake_install = base / L"install";
		std::error_code ec;
		fs::create_directories(fake_install, ec);
		CacheOptions o;
		o.root = base / L"cache";
		o.guard.add_protected_root(fake_install);
		{

			wchar_t install[MAX_PATH];
			const DWORD n = GetEnvironmentVariableW(L"OPENBF3_INSTALL", install, MAX_PATH);
			o.guard.add_protected_root(n > 0 && n < MAX_PATH ? fs::path(install) : fs::path(L"C:\\Program Files\\EA Games\\Battlefield 3"));
		}
		Cache cache;
		std::string why;
		REQUIRE(cache.open(o, &why) == CacheStatus::Ok);

		std::vector<Sha256Digest> expect(rows.size());
		std::atomic<size_t> next{ 0 };
		std::atomic<uint64_t> put_bytes{ 0 };
		std::atomic<int> failures{ 0 };
		t0 = std::chrono::steady_clock::now();
		std::vector<std::thread> threads;
		for (int ti = 0; ti < 8; ++ti) {
			threads.emplace_back([&] {
				std::vector<uint8_t> data;
				for (size_t i = next++; i < rows.size(); i = next++) {
					const Row &r = rows[i];
					if (!read_file(dir / L"payload" / (r.sha1.hex() + ".bin"), data) || data.size() != r.decoded_size) {
						failures.fetch_add(1);
						continue;
					}
					if (r.kind == "res") {
						const std::vector<uint8_t> meta = parse_meta(r.meta_hex);
						data.insert(data.begin(), meta.begin(), meta.end());
					}
					expect[i] = sha256_of(data);
					const PutResult p = cache.put(key_of(r), data);
					if (p.status != CacheStatus::Ok) {
						failures.fetch_add(1);
					}
					put_bytes += data.size();
				}
			});
		}
		for (std::thread &th : threads) {
			th.join();
		}
		const double t_put = seconds_since(t0);
		CHECK(failures.load() == 0);
		cache.close();

		t0 = std::chrono::steady_clock::now();
		REQUIRE(cache.open(o, &why) == CacheStatus::Ok);
		const double t_open = seconds_since(t0);
		CHECK(cache.stats().entries == rows.size());
		t0 = std::chrono::steady_clock::now();
		next = 0;
		threads.clear();
		for (int ti = 0; ti < 8; ++ti) {
			threads.emplace_back([&] {
				for (size_t i = next++; i < rows.size(); i = next++) {
					GetResult g = cache.get(key_of(rows[i]));
					if (g.status != CacheStatus::Ok || sha256_of(g.entry.payload()) != expect[i]) {
						failures.fetch_add(1);
					}
				}
			});
		}
		for (std::thread &th : threads) {
			th.join();
		}
		const double t_get = seconds_since(t0);
		CHECK(failures.load() == 0);
		const CacheStats s = cache.stats();
		CHECK(s.verified == rows.size());
		MESSAGE("put ", put_bytes.load(), " B in ", t_put, " s; reopen ", t_open, " s; get+verify ", t_get, " s; cache bytes ", s.bytes);
		cache.close();
		struct Cleanup {
			fs::path p;
			~Cleanup() {
				std::error_code e;
				fs::remove_all(p, e);
			}
		} cleanup{ base };
	}
}
