#pragma once

#include "bf3/core/ids.h"
#include "bf3/core/res/res_loader.h"
#include "bf3/core/result.h"
#include "bf3/core/ui/swf_movie.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core {
class Install;
}

namespace bf3::core::ui {

struct SwfMovieSource {
	std::string name;
	std::string superbundle;
	std::string bundle;
	Sha1 sha1;
	uint64_t size = 0;
	uint32_t holders = 0;
	uint32_t distinct_sha1s = 0;
};

std::vector<std::string> swf_superbundle_order(const Install &p_install);

std::string swf_movie_name_for_url(std::string_view p_url);

bool is_font_library_url(std::string_view p_url);

std::string swf_atlas_texture_name(std::string_view p_movie_name, std::string_view p_file_name);

struct SwfLibraryOptions {
	unsigned threads = 0;
};

class SwfLibrary {
public:

	static Result<SwfLibrary> open(std::shared_ptr<const Install> p_install, const SwfLibraryOptions &p_options = {});

	static SwfLibrary from_memory(std::vector<std::pair<std::string, std::vector<uint8_t>>> p_payloads);

	SwfLibrary();
	~SwfLibrary();
	SwfLibrary(SwfLibrary &&) noexcept;
	SwfLibrary &operator=(SwfLibrary &&) noexcept;

	const std::shared_ptr<const Install> &install() const;

	const std::vector<SwfMovieSource> &movies() const;

	const SwfMovieSource *find(std::string_view p_name) const;

	Result<std::vector<uint8_t>> read_payload(std::string_view p_name) const;

	Result<std::shared_ptr<const SwfMovie>> load(std::string_view p_name) const;

	Result<res::LoadedTexture> load_atlas_texture(std::string_view p_movie_name, const SwfExternalImage &p_image) const;

	Result<res::DxTexture> read_atlas_texture_header(std::string_view p_movie_name, const SwfExternalImage &p_image) const;

	double open_seconds() const;
	size_t superbundles_listed() const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

}
