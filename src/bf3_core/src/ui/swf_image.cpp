#include "bf3/core/ui/swf_image.h"

#include "bf3/core/res/dx_texture.h"
#include "bf3/core/res/res_loader.h"
#include "bf3/core/util/text.h"

#include <algorithm>

namespace bf3::core::ui {

std::string swf_image_res_name(std::string_view p_url) {
	std::string name = to_lower_ascii(p_url);
	if (name.starts_with(kSwfImageScheme)) {
		name.erase(0, kSwfImageScheme.size());
	}
	std::replace(name.begin(), name.end(), '\\', '/');
	return name;
}

Result<LevelMount> mount_swf_images(const Install &p_install) {
	Result<LevelMount> m = p_install.mount_superbundle(kSwfImageSuperbundle);
	if (!m) {
		return error_at("swf images " + std::string(kSwfImageSuperbundle), m.error());
	}
	BF3_TRY(m->load_all_bundles());
	return m;
}

SwfImageInfo read_swf_image_info(const LevelMount &p_mount, std::string_view p_res_name) {
	SwfImageInfo out;
	out.status = SwfImageStatus::Missing;
	Result<res::RawRes> raw = res::read_raw_res(p_mount, p_res_name);
	if (!raw) {
		out.error = raw.error().message;
		return out;
	}
	res::DxTexture tex;
	const res::ResError e = res::DxTexture::parse(raw->bytes(), tex);
	if (!e.ok()) {
		out.error = "res " + std::string(p_res_name) + ": " + e.message();
		return out;
	}
	if (tex.kind != res::TextureKind::Texture2D || tex.faces() != 1 || tex.width == 0 || tex.height == 0 || tex.width > kSwfImageMaxSide ||
			tex.height > kSwfImageMaxSide) {
		out.error = "res " + std::string(p_res_name) + ": not a 2D image up to " + std::to_string(kSwfImageMaxSide) + " texels";
		return out;
	}
	out.status = SwfImageStatus::Ready;
	out.width = tex.width;
	out.height = tex.height;
	return out;
}

SwfMountImages::SwfMountImages(LevelMount p_mount) :
		mount_(std::move(p_mount)) {}

SwfImageInfo SwfMountImages::request(std::string_view p_res_name) {
	++requests_;
	const auto it = cache_.find(p_res_name);
	if (it != cache_.end()) {
		return it->second;
	}
	SwfImageInfo info = read_swf_image_info(mount_, p_res_name);
	cache_.emplace(std::string(p_res_name), info);
	return info;
}

}
