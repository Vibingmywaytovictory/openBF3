#pragma once

#include "bf3/core/install.h"
#include "bf3/core/result.h"
#include "bf3/core/superbundle_mount.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>

namespace bf3::core::ui {

inline constexpr std::string_view kSwfImageScheme = "img://";
inline constexpr std::string_view kSwfImageSuperbundle = "Win32/UI";
inline constexpr uint32_t kSwfImageMaxSide = 4096;

std::string swf_image_res_name(std::string_view p_url);

enum class SwfImageStatus : uint8_t {
	Loading,
	Ready,
	Missing,
};

struct SwfImageInfo {
	SwfImageStatus status = SwfImageStatus::Loading;
	uint32_t width = 0;
	uint32_t height = 0;
	std::string error;
};

class SwfImageSource {
public:
	virtual ~SwfImageSource() = default;
	virtual SwfImageInfo request(std::string_view p_res_name) = 0;
};

Result<LevelMount> mount_swf_images(const Install &p_install);

SwfImageInfo read_swf_image_info(const LevelMount &p_mount, std::string_view p_res_name);

class SwfMountImages final : public SwfImageSource {
public:
	explicit SwfMountImages(LevelMount p_mount);
	SwfImageInfo request(std::string_view p_res_name) override;
	uint32_t requests() const { return requests_; }

private:
	LevelMount mount_;
	std::map<std::string, SwfImageInfo, std::less<>> cache_;
	uint32_t requests_ = 0;
};

}
