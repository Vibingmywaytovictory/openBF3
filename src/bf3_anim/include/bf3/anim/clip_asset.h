#pragma once
// Loading one Ant clip for playback: the static library (s_BasicAssets) once, then a package and a clip of
// it, decoded and bound to its controller and rig (bf3_core ant/). Godot-free; the Godot viewmodel loader and
// the tests share it.
//
//   std::unique_ptr<AntLibrary> lib = *load_ant_library(source);                // ~22 MB parse
//   LoadedClip reload = *load_clip(source, *lib, "m9", "ReloadEmpty Anim");
//   Skeleton rig = *skeleton_from_rig(*reload.binding.rig);
//   ClipTracks tracks = *bind_clip_tracks(reload.binding, reload.clip, rig);

#include "bf3/core/ant/ant_clip.h"
#include "bf3/core/ant/ant_library.h"
#include "bf3/core/ant/ant_package.h"
#include "bf3/core/ant/ant_source.h"
#include "bf3/core/result.h"

#include <memory>
#include <string_view>

namespace bf3::anim {

// The library over the source's Static package (s_BasicAssets).
core::Result<std::unique_ptr<core::ant::AntLibrary>> load_ant_library(const core::ant::AntPackageSource &p_source);
// Reads and parses one package by its short name (e.g. "m9").
core::Result<std::unique_ptr<core::ant::AntPackage>> load_ant_package(const core::ant::AntPackageSource &p_source, std::string_view p_short_name);

struct LoadedClip {
	std::unique_ptr<core::ant::AntPackage> package; // owns the bytes binding.clip points into
	core::ant::ClipBinding binding; // rig points into the library (keep the library alive)
	core::ant::DecodedClip clip;
};

// Decodes and binds the clip named p_clip ("__name", e.g. "ReloadEmpty Anim") of p_package.
core::Result<LoadedClip> load_clip(std::unique_ptr<core::ant::AntPackage> p_package, const core::ant::AntLibrary &p_library, std::string_view p_clip);
core::Result<LoadedClip> load_clip(const core::ant::AntPackageSource &p_source, const core::ant::AntLibrary &p_library, std::string_view p_package,
		std::string_view p_clip);

} // namespace bf3::anim
