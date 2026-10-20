#include <doctest/doctest.h>

#include "bf3/audio/audio_analysis.h"
#include "bf3/audio/audio_check.h"
#include "bf3/audio/audio_config.h"
#include "bf3/audio/dsp.h"
#include "bf3/audio/mixer_core.h"
#include "bf3/audio/patch_instance.h"
#include "bf3/audio/wav_io.h"
#include "bf3/audio/weapon_sound.h"
#include "bf3/core/audio/sound_program.h"
#include "bf3/core/audio/sound_stream.h"
#include "bf3/core/audio/sound_types.h"
#include "bf3/core/audio/sound_wave.h"
#include "bf3/core/audio/wave_bank.h"
#include "bf3/core/ebx.h"

#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <type_traits>

namespace au = bf3::core::audio;
namespace ba = bf3::audio;

static_assert(au::kXasFrameBytes == 76 && au::kXasFrameSamples == 128);
static_assert(ba::kBlockFrames == 512 && ba::kMixRate == 48000);

static_assert(au::fourcc("SnP1") == 1399738417u);
static_assert(au::fourcc("Sen0") == 1399156272u);
static_assert(au::fourcc("Sub0") == 1400201776u);
static_assert(au::fourcc("LPB0") == 1280328240u);
static_assert(au::fourcc("Pn21") == 1349399089u);
static_assert(au::fourcc("HI20") == 1212756528u);

static_assert(au::kStoredConnectionNone == 0);

static_assert(ba::kM16CoreTapVoices.size() <= ba::kMaxTapVoices && ba::kM16NoiseTapVoices.size() <= ba::kMaxTapVoices);
static_assert(ba::kM16NoiseHpf.voice == 14 && ba::kM16NoiseHpf.plugin == 2 && ba::kM16CoreHpf.voice == 6 && ba::kM16CoreHpf.plugin == 1);

static_assert(std::is_trivially_copyable_v<ba::MixerCommand>);
static_assert(std::is_trivially_copyable_v<au::SoundPort>);
static_assert(std::is_trivially_copyable_v<au::SoundNode>);

TEST_SUITE("audio-contract") {

TEST_CASE("node type table: the census sets") {
	size_t patch = 0, a2 = 0, all = 0, children = 0;
	std::set<std::string> names;
	for (const au::SoundNodeTypeInfo &info : au::sound_node_types()) {
		CHECK(names.insert(std::string(info.ebx_name)).second);
		auto back = au::sound_node_type(info.ebx_name);
		REQUIRE(back.has_value());
		CHECK(*back == info.type);
		if (info.role == au::SoundNodeRole::Child) {
			++children;
			CHECK(info.a2);
			continue;
		}
		++all;
		if (info.role == au::SoundNodeRole::Patch) {
			++patch;
			if (info.a2) {
				++a2;
				CHECK(info.ebx_name.ends_with("NodeData"));
			}
		}
	}
	CHECK(patch == au::kPatchNodeTypes);
	CHECK(a2 == au::kA2NodeTypes);
	CHECK(all == au::kAllNodeTypes);
	CHECK(children == 8);
	CHECK(!au::sound_node_type("SoundPatchAsset").has_value());
	CHECK(au::sound_node_type_info(au::SoundNodeType::Sampler).option == "Loop");
	CHECK(au::sound_node_type_info(au::SoundNodeType::MultiCrossfader).children == "CrossfaderGroups");
}

TEST_CASE("enumerator names") {
	CHECK(au::simple_transform_op("SimpleTransformOperation_Multiply") == au::SimpleTransformOp::Multiply);
	CHECK(au::simple_transform_op("SimpleTransformOperation_ArcTangent") == au::SimpleTransformOp::ArcTangent);
	CHECK(au::simple_transform_op("SimpleTransformOperation_Nope") == au::SimpleTransformOp::Unknown);
	CHECK(au::simple_transform_op("Multiply") == au::SimpleTransformOp::Unknown);
}

TEST_CASE("seam hashes are ebx_name_hash of their names") {
	CHECK(bf3::core::ebx_name_hash("Fire") == ba::kHashFire);
	CHECK(bf3::core::ebx_name_hash("IsFirstPerson") == ba::kHashIsFirstPerson);
	CHECK(bf3::core::ebx_name_hash("Reload") == ba::kHashReload);
	CHECK(bf3::core::ebx_name_hash("AreaType") == ba::kHashAreaType);
}

TEST_CASE("tuning: defaults parse, bad input fails with the line") {
	CHECK(ba::audio_tuning_keys().size() == 16);
	auto empty = ba::parse_audio_tuning("");
	REQUIRE(empty.ok());
	CHECK(empty->window_top_db == 115.0f);
	auto set = ba::parse_audio_tuning("# c\n[output]\nlfe_fold_db = -3.5 # note\n\n[sampler]\nstep_crossfade_ms=1\n");
	REQUIRE(set.ok());
	CHECK(set->lfe_fold_db == -3.5f);
	CHECK(set->step_crossfade_ms == 1.0f);
	auto unknown = ba::parse_audio_tuning("[output]\nnope = 1\n");
	REQUIRE(!unknown.ok());
	CHECK(unknown.error().message.find("line 2") != std::string::npos);
	CHECK(!ba::parse_audio_tuning("[output]\nlfe_fold_db = 99\n").ok());
	CHECK(!ba::parse_audio_tuning("[output]\nlfe_fold_db = x\n").ok());
	CHECK(!ba::parse_audio_tuning("[output\n").ok());
	CHECK(!ba::parse_audio_tuning("lfe_fold_db = -3\n").ok());
}

TEST_CASE("tuning: the project's audio_tuning.cfg parses and sets every key") {
	std::ifstream file(OPENBF3_AUDIO_TUNING_CFG, std::ios::binary);
	REQUIRE(file.good());
	std::stringstream text;
	text << file.rdbuf();
	auto tuning = ba::parse_audio_tuning(text.str());
	REQUIRE_MESSAGE(tuning.ok(), (tuning.ok() ? std::string() : tuning.error().message));
	for (const ba::AudioTuningKey &key : ba::audio_tuning_keys()) {
		std::string line = std::string(key.key) + " =";
		CHECK_MESSAGE(text.str().find(line) != std::string::npos, key.key);
	}
}

}
