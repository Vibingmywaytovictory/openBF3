#include "bf3/anim/clip_asset.h"

#include <string>
#include <utility>
#include <vector>

namespace bf3::anim {

using namespace core::ant;

core::Result<std::unique_ptr<AntPackage>> load_ant_package(const AntPackageSource &p_source, std::string_view p_short_name) {
	const AntPackageInfo *info = p_source.find(p_short_name);
	if (info == nullptr) {
		return core::make_error("ant: no package " + std::string(p_short_name));
	}
	std::vector<uint8_t> bytes;
	std::string error;
	if (!p_source.read(*info, bytes, error)) {
		return core::make_error("ant: read " + info->partition + ": " + error);
	}
	auto p = std::make_unique<AntPackage>();
	if (!p->parse(info->short_name, std::move(bytes), error)) {
		return core::make_error("ant: parse " + info->partition + ": " + error);
	}
	return p;
}

core::Result<std::unique_ptr<AntLibrary>> load_ant_library(const AntPackageSource &p_source) {
	for (const AntPackageInfo &info : p_source.packages()) {
		if (info.packaging != AntPackagingType::Static) {
			continue;
		}
		core::Result<std::unique_ptr<AntPackage>> p = load_ant_package(p_source, info.short_name);
		if (!p) {
			return p.error();
		}
		auto lib = std::make_unique<AntLibrary>();
		const AntResult r = lib->set_static(std::move(*p));
		if (!r.ok()) {
			return core::make_error("ant: static package " + info.short_name + ": " + r.message);
		}
		return lib;
	}
	return core::make_error("ant: the source has no Static package (s_BasicAssets)");
}

core::Result<LoadedClip> load_clip(std::unique_ptr<AntPackage> p_package, const AntLibrary &p_library, std::string_view p_clip) {
	const std::string where = "ant: " + p_package->name() + " \"" + std::string(p_clip) + "\"";
	const GdStruct clip = AntLibrary::find_clip(*p_package, p_clip);
	if (!clip.valid()) {
		return core::make_error(where + ": no such clip");
	}
	LoadedClip out;
	AntResult r = decode_clip(clip, out.clip);
	if (!r.ok()) {
		return core::make_error(where + ": decode: " + r.message);
	}
	r = p_library.bind(*p_package, clip, out.binding);
	if (!r.ok()) {
		return core::make_error(where + ": bind: " + r.message);
	}
	if (out.binding.rig == nullptr) {
		return core::make_error(where + ": no rig");
	}
	out.package = std::move(p_package);
	return out;
}

core::Result<LoadedClip> load_clip(const AntPackageSource &p_source, const AntLibrary &p_library, std::string_view p_package, std::string_view p_clip) {
	core::Result<std::unique_ptr<AntPackage>> p = load_ant_package(p_source, p_package);
	if (!p) {
		return p.error();
	}
	return load_clip(std::move(*p), p_library, p_clip);
}

} // namespace bf3::anim
