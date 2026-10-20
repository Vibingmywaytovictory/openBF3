#pragma once

#include "bf3/core/ebx.h"
#include "bf3/core/ebx_db.h"
#include "bf3/core/ids.h"
#include "bf3/fx/effect_def.h"
#include "bf3/fx/simulator.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fxt {

using bf3::core::Guid;

struct Val {
	uint32_t word = 0;
	std::vector<std::pair<std::string, Val>> fields;
};
using Vals = std::vector<std::pair<std::string, Val>>;

inline Val f32(float p_v) {
	return Val{ std::bit_cast<uint32_t>(p_v), {} };
}
inline Val i32(int64_t p_v) {
	return Val{ uint32_t(int32_t(p_v)), {} };
}
inline Val flag(bool p_v) {
	return Val{ p_v ? 1u : 0u, {} };
}
inline Val raw(uint32_t p_word) {
	return Val{ p_word, {} };
}
inline Val st(Vals p_fields) {
	return Val{ 0, std::move(p_fields) };
}
inline Val v2(float p_x, float p_y) {
	return st({ { "x", f32(p_x) }, { "y", f32(p_y) } });
}
inline Val v3(float p_x, float p_y, float p_z) {
	return st({ { "x", f32(p_x) }, { "y", f32(p_y) }, { "z", f32(p_z) } });
}
inline Val v4(float p_x, float p_y, float p_z, float p_w) {
	return st({ { "x", f32(p_x) }, { "y", f32(p_y) }, { "z", f32(p_z) }, { "w", f32(p_w) } });
}
inline Val transform(float p_tx = 0, float p_ty = 0, float p_tz = 0) {
	return st({ { "right", v3(1, 0, 0) }, { "up", v3(0, 1, 0) }, { "forward", v3(0, 0, 1) }, { "trans", v3(p_tx, p_ty, p_tz) } });
}
inline void set(Vals &r_vals, const std::string &p_name, Val p_v) {
	for (auto &[n, v] : r_vals) {
		if (n == p_name) {
			v = std::move(p_v);
			return;
		}
	}
	r_vals.emplace_back(p_name, std::move(p_v));
}

struct Field {
	std::string name;
	char kind = 'i';
	std::string elem;
};
struct Type {
	std::string name;
	std::vector<Field> fields;
	bool value_type = false;
	std::string base;
	bool enum_type = false;
};

inline const std::vector<Type> &type_lib() {
	static const std::vector<Type> lib = [] {
		std::vector<Type> t;
		t.push_back({ "Vec2", { { "x", 'f' }, { "y", 'f' } }, true });
		t.push_back({ "Vec3", { { "x", 'f' }, { "y", 'f' }, { "z", 'f' } }, true });
		t.push_back({ "Vec4", { { "x", 'f' }, { "y", 'f' }, { "z", 'f' }, { "w", 'f' } }, true });
		t.push_back({ "LinearTransform", { { "right", 'S', "Vec3" }, { "up", 'S', "Vec3" }, { "forward", 'S', "Vec3" }, { "trans", 'S', "Vec3" } }, true });
		t.push_back({ "EvaluatorInputEnum", { { "EfNormTime" }, { "EfEmitterNormTime" }, { "EfOne" }, { "EfVelocity" } }, false, {}, true });
		t.push_back({ "EmitterParameterEnum",
				{ { "EmitterParameterNone" }, { "EmitterParameterDistance" }, { "EmitterParameterVec" }, { "EmitterParameterVecAverage" },
						{ "EmitterParameter1" }, { "EmitterParameter2" }, { "EmitterParameter3" }, { "EmitterParameterBogus" } },
				false, {}, true });
		t.push_back({ "EmittableTypeEnum",
				{ { "ScreenAlignedQuad" }, { "DirectionAlignedQuad" }, { "WorldAlignedQuad" }, { "Quad" }, { "ParticleMesh" }, { "Trail" }, { "Ribbon" } },
				false, {}, true });
		t.push_back({ "PolynomialOperationEnum", { { "Multiplication" }, { "Addition" }, { "Division" } }, false, {}, true });
		t.push_back({ "PolynomialTerm", { { "Coefficients", 'S', "Vec4" }, { "MinClamp", 'f' }, { "MaxClamp", 'f' }, { "ScaleValue", 'f' } }, true });
		t.push_back({ "EmitterTextureInfo",
				{ { "TextureName", 's' }, { "NormalmapName", 's' }, { "EnableAnimation", 'b' }, { "AnimationFrameCount", 'f' },
						{ "AnimationFrameColumnCount", 'f' } },
				true });
		t.push_back({ "EmitterTextureAtlasInfo", { { "TextureName", 's' }, { "MinUv", 'S', "Vec2" }, { "MaxUv", 'S', "Vec2" } }, true });

		t.push_back({ "EmitterTemplateData",
				{ { "EmittableType", 'e', "EmittableTypeEnum" }, { "Lifetime", 'f' }, { "LifetimeFrameCount", 'i' }, { "MaxCount", 'i' },
						{ "TimeScale", 'f' }, { "LocalSpace", 'b' }, { "FollowSpawnSource", 'b' }, { "ActAsPointLight", 'b' }, { "Emissive", 'b' },
						{ "Opaque", 'b' }, { "ForceFullRes", 'b' }, { "ForceNiceSorting", 'b' }, { "KillParticlesWithEmitter", 'b' },
						{ "RepeatParticleSpawning", 'b' }, { "TransparencySunShadowEnable", 'b' }, { "ExclusionVolumeCullEnable", 'b' },
						{ "Mesh", 'r' }, { "PointLightRadius", 'f' }, { "PointLightColor", 'S', "Vec3" }, { "PointLightIntensity", 'S', "Vec4" },
						{ "PointLightMinClamp", 'f' }, { "PointLightMaxClamp", 'f' }, { "PointLightRandomIntensityMin", 'f' },
						{ "PointLightRandomIntensityMax", 'f' }, { "PointLightPivot", 'S', "Vec3" }, { "MaxSpawnDistance", 'f' },
						{ "VisibleAfterDistance", 'f' }, { "MeshCullingDistance", 'f' }, { "MinScreenArea", 'f' }, { "ParticleCullingFactor", 'f' },
						{ "DistanceScaleNearValue", 'f' }, { "DistanceScaleFarValue", 'f' }, { "DistanceScaleLength", 'f' },
						{ "SoftParticlesFadeDistanceMultiplier", 'f' }, { "LightMultiplier", 'f' }, { "LightWrapAroundFactor", 'f' },
						{ "VertexPixelLightingBlendFactor", 'f' }, { "GlobalLocalNormalBlendFactor", 'f' }, { "ZOcclusionLookup", 'F' } } });
		t.push_back({ "EmitterDocument", { { "Name", 's' }, { "RootProcessor", 'r' }, { "TemplateData", 'r' } } });
		t.push_back({ "ProcessorData",
				{ { "Enable", 'b' }, { "EvaluatorInput", 'e', "EvaluatorInputEnum" }, { "DefaultValues", 'S', "Vec4" }, { "Pre", 'r' },
						{ "NextProcessor", 'r' } } });
		const auto proc = [&](const char *p_name, std::vector<Field> p_fields) { t.push_back({ p_name, std::move(p_fields), false, "ProcessorData" }); };
		proc("BaseEmitterData", { { "EmitterAsset", 'r' } });
		proc("EmitterData", { { "EmitterAssets", 'a' } });
		proc("SpawnRateData", { { "SpawnRate", 'f' }, { "DistributeOverTime", 'b' }, { "DistributeOverDistance", 'b' }, { "TrailSegmentLength", 'f' } });
		proc("SpawnPositionData", {});
		proc("SpawnDirectionData", { { "DirectionFromEmitterOrigin", 'f' }, { "InheritSpeedAndDirectionFromEmitter", 'b' } });
		proc("SpawnSpeedData", { { "Speed", 'f' } });
		proc("SpawnSizeData", { { "Size", 'f' } });
		proc("SpawnRotationData", { { "Rotation", 'f' } });
		proc("SpawnRotationSpeedData", { { "RotationSpeed", 'f' } });
		proc("SpawnOrientationData", {});
		proc("SpawnAnimationFrameData", { { "AnimationFrame", 'i' } });
		proc("SpawnAnimationData", { { "AnimationSpeed", 'f' } });
		proc("UpdateAgeData", { { "Lifetime", 'f' } });
		proc("UpdateCameraProximityData", { { "ForwardOffset", 'f' }, { "Size", 'S', "Vec3" } });
		proc("AirResistanceData", { { "DragFactor", 'f' } });
		proc("GravityData", { { "Gravity", 'f' } });
		proc("LocalForceData", { { "LocalForce", 'S', "Vec3" } });
		proc("TurbulanceData", { { "Intensity", 'f' } });
		proc("UpdateLinearVelocityData", {});
		proc("UpdateRotationData", {});
		proc("UpdateSizeData", { { "Pivot", 'S', "Vec2" } });
		proc("UpdateSizeXData", {});
		proc("UpdateSizeYData", {});
		proc("UpdateSizeZData", {});
		proc("UpdateColorData", { { "Color", 'S', "Vec3" } });
		proc("UpdateTransparencyData", { { "CullThreshold", 'f' } });
		proc("UpdateAlphaLevelMinData", { { "MinLevel", 'f' } });
		proc("UpdateAlphaLevelMaxData", { { "MaxLevel", 'f' } });
		proc("UpdateAlphaLevelScaleData", { { "Exponent", 'f' } });
		proc("UpdateTextureCoordsData", { { "TextureBlendValue", 'f' }, { "TextureInfo1", 'S', "EmitterTextureInfo" } });
		proc("UpdateOrientationData", {});
		proc("UpdateClipScaleData", { { "Lookup", 'I' } });
		proc("MysteryProcessorData", {});

		t.push_back({ "EvaluatorData", { { "EmitterParameter", 'e', "EmitterParameterEnum" } } });
		const auto eval = [&](const char *p_name, std::vector<Field> p_fields) { t.push_back({ p_name, std::move(p_fields), false, "EvaluatorData" }); };
		eval("PolynomialData", { { "Coefficients", 'S', "Vec4" }, { "MinClamp", 'f' }, { "MaxClamp", 'f' }, { "ScaleValue", 'f' } });
		eval("PolynomialColorInterpData", { { "Coefficients", 'S', "Vec4" }, { "Color0", 'S', "Vec3" }, { "Color1", 'S', "Vec3" } });
		eval("PolynomialOperatorData",
				{ { "FirstOperand", 'S', "PolynomialTerm" }, { "SecondOperand", 'S', "PolynomialTerm" }, { "Operation", 'e', "PolynomialOperationEnum" },
						{ "MinClampResult", 'f' }, { "MaxClampResult", 'f' } });
		eval("RandomEvaluatorData", { { "Min", 'f' }, { "Max", 'f' } });
		eval("RandomXYZEvaluatorData", { { "MinX", 'f' }, { "MinY", 'f' }, { "MinZ", 'f' }, { "MaxX", 'f' }, { "MaxY", 'f' }, { "MaxZ", 'f' } });
		eval("BoxEvaluatorData", { { "Dimensions", 'S', "Vec3" }, { "Pivot", 'S', "Vec3" } });
		eval("SphereEvaluatorData", { { "Radius", 'f' }, { "Scale", 'S', "Vec3" }, { "Pivot", 'S', "Vec3" } });
		eval("RotateVectorData", { { "Angle", 'f' }, { "InputAffectsPhi", 'b' }, { "RotateWithinPlane", 'b' } });
		eval("CameraProximityEvaluatorData",
				{ { "ForwardOffset", 'f' }, { "InnerRadius", 'f' }, { "InnerRadiusDirection", 'S', "Vec3" }, { "Offset", 'S', "Vec3" },
						{ "Size", 'S', "Vec3" } });
		eval("SampleTextureData",
				{ { "ColorIntensityMin", 'S', "Vec3" }, { "ColorIntensityMax", 'S', "Vec3" }, { "TextureDimensions", 'S', "Vec2" },
						{ "TextureOriginU", 'f' }, { "TextureOriginV", 'f' }, { "GradientData", 'v', "Vec4" } });

		t.push_back({ "EffectBlueprint", { { "Object", 'r' } } });
		t.push_back({ "EffectEntityData",
				{ { "Transform", 'S', "LinearTransform" }, { "CullDistance", 'f' }, { "StartDelay", 'f' }, { "LowEndPc", 'b' }, { "MediumPc", 'b' },
						{ "HighEndPc", 'b' }, { "MaxInstanceCount", 'i' }, { "ResetInstanceWhenStarted", 'b' }, { "Components", 'a' } } });
		t.push_back({ "EffectComponentData",
				{ { "Transform", 'S', "LinearTransform" }, { "StartDelay", 'f' }, { "CullDistance", 'f' }, { "LowEndPc", 'b' }, { "MediumPc", 'b' },
						{ "HighEndPc", 'b' } } });
		t.push_back({ "EmitterEntityData",
				{ { "Emitter", 'r' }, { "SpawnProbability", 'f' }, { "MaxInstanceCount", 'i' }, { "ResetInstanceWhenStarted", 'b' } }, false,
				"EffectComponentData" });
		t.push_back({ "SoundEffectEntityData", { { "Sound", 'r' } }, false, "EffectComponentData" });
		t.push_back({ "VisualEnvironmentEffectEntityData", { { "VisualEnvironment", 'r' } }, false, "EffectComponentData" });
		t.push_back({ "TargetAsset", {} });

		t.push_back({ "EmitterSystemAsset", { { "BaseAtlasTexture", 'r' }, { "NormalAtlasTexture", 'r' }, { "TextureInfos", 'v', "EmitterTextureAtlasInfo" } } });
		t.push_back({ "MaterialRelationPropertyPair", { { "PhysicsMaterialProperties", 'a' } }, true });
		t.push_back({ "MaterialInteractionGridRow", { { "Items", 'v', "MaterialRelationPropertyPair" } }, true });
		t.push_back({ "MaterialGridData",
				{ { "MaterialIndexMap", 'I' }, { "DefaultMaterialIndex", 'i' }, { "InteractionGrid", 'v', "MaterialInteractionGridRow" },
						{ "DefaultMaterial", 'r' }, { "MaterialPairs", 'a' } } });
		t.push_back({ "MaterialRelationEffectData",
				{ { "ImpactEffectMaxSpreadAngle", 'f' }, { "MirrorImpactDirection", 'b' }, { "ImpactEffects", 'a' }, { "ImpactDebris", 'a' } } });
		t.push_back({ "MaterialRelationDecalData", { { "Decal", 'r' } } });
		t.push_back({ "MaterialRelationSoundData", { { "ImpactSound", 'r' } } });
		return t;
	}();
	return lib;
}

struct Array {
	std::string elem;
	std::vector<uint32_t> words;
	std::vector<Vals> structs;
};

struct Instance {
	std::string type;
	Vals values;
};

inline Guid guid(uint32_t p_seed, uint32_t p_index) {
	Guid g{};
	std::memcpy(g.bytes.data(), &p_index, 4);
	std::memcpy(g.bytes.data() + 4, &p_seed, 4);
	g.bytes[15] = 0xfe;
	return g;
}
inline Guid partition_guid(uint32_t p_seed) {
	return guid(p_seed, 0xFFFFFFFFu);
}

struct Partition {
	uint32_t seed = 1;
	std::vector<Instance> instances;
	std::vector<Array> arrays;
	std::vector<std::string> strings;
	std::vector<std::pair<uint32_t, uint32_t>> imports;

	uint32_t add(Instance p_instance) {
		instances.push_back(std::move(p_instance));
		return uint32_t(instances.size() - 1);
	}
	Val ref(uint32_t p_instance) const { return raw(p_instance + 1); }
	Val ext(uint32_t p_seed, uint32_t p_instance) {
		for (size_t i = 0; i < imports.size(); ++i) {
			if (imports[i] == std::make_pair(p_seed, p_instance)) {
				return raw(0x80000000u | uint32_t(i));
			}
		}
		imports.emplace_back(p_seed, p_instance);
		return raw(0x80000000u | uint32_t(imports.size() - 1));
	}
	Val str(std::string p_text) {
		strings.push_back(std::move(p_text));
		return raw(uint32_t(strings.size() - 1));
	}
	Val array(Array p_array) {
		arrays.push_back(std::move(p_array));
		return raw(uint32_t(arrays.size() - 1));
	}
	std::vector<uint8_t> build() const;
};

namespace detail {

inline void put32(std::vector<uint8_t> &r_out, size_t p_at, uint32_t p_value) {
	std::memcpy(r_out.data() + p_at, &p_value, 4);
}

}

inline std::vector<uint8_t> Partition::build() const {
	namespace code = bf3::core::ebx_code;
	using detail::put32;
	const std::vector<Type> &types = type_lib();
	const auto type_at = [&](const std::string &p_name) -> uint32_t {
		for (uint32_t i = 0; i < types.size(); ++i) {
			if (types[i].name == p_name) {
				return i;
			}
		}
		return UINT32_MAX;
	};

	std::vector<std::string> elems{ "", "*f", "*i" };
	std::vector<std::string> names{ "DataContainer", "$", "array", "member" };
	const auto add_name = [&](const std::string &p_name) {
		if (std::find(names.begin(), names.end(), p_name) == names.end()) {
			names.push_back(p_name);
		}
	};
	for (const Type &t : types) {
		add_name(t.name);
		for (const Field &f : t.fields) {
			add_name(f.name);
			if (f.kind == 'v' && std::find(elems.begin(), elems.end(), f.elem) == elems.end()) {
				elems.push_back(f.elem);
			}
		}
	}
	std::string name_table;
	for (const std::string &n : names) {
		name_table += n;
		name_table += '\0';
	}

	std::vector<uint32_t> sizes(types.size(), 0);
	std::vector<std::vector<uint32_t>> offsets(types.size());
	const auto field_size = [&](const Field &p_f) -> uint32_t { return p_f.kind == 'S' ? sizes[type_at(p_f.elem)] : 4u; };
	for (size_t i = 0; i < types.size(); ++i) {
		const Type &t = types[i];
		if (t.enum_type) {
			sizes[i] = 4;
			continue;
		}
		uint32_t at = t.value_type ? 0 : (t.base.empty() ? 8 : sizes[type_at(t.base)]);
		for (const Field &f : t.fields) {
			offsets[i].push_back(at);
			at += field_size(f);
		}
		sizes[i] = std::max<uint32_t>(at, 4);
	}
	const auto array_complex = [&](const std::string &p_elem) {
		return uint16_t(1 + types.size() + (std::find(elems.begin(), elems.end(), p_elem) - elems.begin()));
	};
	struct F {
		std::string name;
		uint8_t code;
		uint16_t ref;
		uint32_t offset;
	};
	struct C {
		std::string name;
		uint32_t field_start;
		uint8_t count;
		uint8_t code;
		uint16_t size;
	};
	std::vector<F> fields;
	std::vector<C> complexes{ { "DataContainer", 0, 0, code::kClass, 8 } };
	for (size_t i = 0; i < types.size(); ++i) {
		const Type &t = types[i];
		const uint32_t start = uint32_t(fields.size());
		if (t.enum_type) {
			for (size_t k = 0; k < t.fields.size(); ++k) {
				fields.push_back({ t.fields[k].name, code::kVoid, 0, uint32_t(k + 1) });
			}
			complexes.push_back({ t.name, start, uint8_t(fields.size() - start), code::kEnum, 4 });
			continue;
		}
		if (!t.value_type) {
			fields.push_back({ "$", code::kVoid, uint16_t(t.base.empty() ? 0 : 1 + type_at(t.base)), 0 });
		}
		for (size_t k = 0; k < t.fields.size(); ++k) {
			const Field &f = t.fields[k];
			uint8_t c = code::kInt32;
			uint16_t ref = 0;
			switch (f.kind) {
				case 'b':
					c = code::kBoolean;
					break;
				case 'f':
					c = code::kFloat32;
					break;
				case 'r':
					c = code::kClass;
					break;
				case 's':
					c = code::kCString;
					break;
				case 'S':
					c = code::kValueType;
					ref = uint16_t(1 + type_at(f.elem));
					break;
				case 'e':
					c = code::kEnum;
					ref = uint16_t(1 + type_at(f.elem));
					break;
				case 'a':
					c = code::kArray;
					ref = array_complex("");
					break;
				case 'F':
					c = code::kArray;
					ref = array_complex("*f");
					break;
				case 'I':
					c = code::kArray;
					ref = array_complex("*i");
					break;
				case 'v':
					c = code::kArray;
					ref = array_complex(f.elem);
					break;
				default:
					break;
			}
			fields.push_back({ f.name, c, ref, offsets[i][k] });
		}
		complexes.push_back({ t.name, start, uint8_t(fields.size() - start), t.value_type ? code::kValueType : code::kClass, uint16_t(sizes[i]) });
	}
	for (const std::string &e : elems) {
		const uint32_t start = uint32_t(fields.size());
		const uint8_t c = e.empty() ? code::kClass : e == "*f" ? code::kFloat32 : e == "*i" ? code::kInt32 : code::kValueType;
		fields.push_back({ "member", c, uint16_t(c == code::kValueType ? 1 + type_at(e) : 0), 0 });
		complexes.push_back({ "array", start, 1, code::kArray, 4 });
	}

	std::vector<Array> all = arrays;
	const size_t empty_at = all.size();
	for (const std::string &e : elems) {
		all.push_back(Array{ e, {}, {} });
	}
	std::string string_table;
	std::vector<uint32_t> string_at;
	for (const std::string &s : strings) {
		string_at.push_back(uint32_t(string_table.size()));
		string_table += s;
		string_table += '\0';
	}
	const auto align16 = [](size_t p_at) { return (p_at + 15) & ~size_t(15); };
	const size_t names_at = 80 + imports.size() * 32;
	const size_t fields_at = names_at + name_table.size();
	const size_t complexes_at = align16(fields_at + fields.size() * 16);
	const size_t instance_repeaters_at = complexes_at + complexes.size() * 16;
	const size_t array_repeaters_at = align16(instance_repeaters_at + instances.size() * 12);
	const size_t meta_size = align16(array_repeaters_at + all.size() * 12);
	const size_t strings_at = meta_size;
	const size_t instances_at = strings_at + string_table.size();
	size_t instance_payload = 0;
	for (const Instance &i : instances) {
		instance_payload += 16 + sizes[type_at(i.type)];
	}
	const size_t arrays_at = instances_at + instance_payload;
	const auto stride = [&](const Array &p_a) -> size_t { return (p_a.elem.empty() || p_a.elem[0] == '*') ? 4 : sizes[type_at(p_a.elem)]; };
	const auto count_of = [](const Array &p_a) { return (p_a.elem.empty() || p_a.elem[0] == '*') ? p_a.words.size() : p_a.structs.size(); };
	size_t array_bytes = 0;
	for (const Array &a : all) {
		array_bytes += 4 + count_of(a) * stride(a);
	}
	const size_t total = arrays_at + array_bytes;
	std::vector<uint8_t> b(total, 0);
	put32(b, 0x00, 0x0FB2D1CE);
	put32(b, 0x04, uint32_t(meta_size));
	put32(b, 0x08, uint32_t(total - meta_size));
	put32(b, 0x0C, uint32_t(imports.size()));
	put32(b, 0x14, uint32_t(instances.size()));
	put32(b, 0x18, uint32_t(complexes.size()));
	put32(b, 0x1C, uint32_t(fields.size()));
	put32(b, 0x20, uint32_t(name_table.size()));
	put32(b, 0x24, uint32_t(string_table.size()));
	put32(b, 0x28, uint32_t(all.size()));
	put32(b, 0x2C, uint32_t(instance_payload));
	const Guid pg = partition_guid(seed);
	const Guid primary = guid(seed, 0);
	std::memcpy(b.data() + 0x30, pg.bytes.data(), 16);
	std::memcpy(b.data() + 0x40, primary.bytes.data(), 16);
	for (size_t i = 0; i < imports.size(); ++i) {
		const Guid p = partition_guid(imports[i].first);
		const Guid g = guid(imports[i].first, imports[i].second);
		std::memcpy(b.data() + 80 + i * 32, p.bytes.data(), 16);
		std::memcpy(b.data() + 80 + i * 32 + 16, g.bytes.data(), 16);
	}
	std::memcpy(b.data() + names_at, name_table.data(), name_table.size());
	for (size_t i = 0; i < fields.size(); ++i) {
		const size_t at = fields_at + i * 16;
		put32(b, at, bf3::core::ebx_name_hash(fields[i].name));
		const uint16_t flags = uint16_t(fields[i].code << 4);
		std::memcpy(b.data() + at + 4, &flags, 2);
		std::memcpy(b.data() + at + 6, &fields[i].ref, 2);
		put32(b, at + 8, fields[i].offset);
	}
	for (size_t i = 0; i < complexes.size(); ++i) {
		const size_t at = complexes_at + i * 16;
		put32(b, at, bf3::core::ebx_name_hash(complexes[i].name));
		put32(b, at + 4, complexes[i].field_start);
		b[at + 8] = complexes[i].count;
		b[at + 9] = 4;
		const uint16_t flags = uint16_t(complexes[i].code << 4);
		std::memcpy(b.data() + at + 10, &flags, 2);
		std::memcpy(b.data() + at + 12, &complexes[i].size, 2);
	}
	std::memcpy(b.data() + strings_at, string_table.data(), string_table.size());
	std::function<void(uint32_t, const Vals &, size_t)> write = [&](uint32_t p_type, const Vals &p_vals, size_t p_at) {
		const Type &t = types[p_type];
		if (!t.value_type && !t.base.empty()) {
			write(type_at(t.base), p_vals, p_at);
		}
		for (size_t k = 0; k < t.fields.size(); ++k) {
			const Field &f = t.fields[k];
			const size_t at = p_at + offsets[p_type][k];
			const Val *v = nullptr;
			for (const auto &[n, val] : p_vals) {
				if (n == f.name) {
					v = &val;
				}
			}
			switch (f.kind) {
				case 's':
					put32(b, at, v != nullptr ? string_at[v->word] : 0xFFFFFFFFu);
					break;
				case 'a':
				case 'F':
				case 'I':
				case 'v': {
					const std::string elem = f.kind == 'a' ? std::string() : f.kind == 'F' ? std::string("*f") : f.kind == 'I' ? std::string("*i") : f.elem;
					put32(b, at, v != nullptr ? v->word : uint32_t(empty_at + (std::find(elems.begin(), elems.end(), elem) - elems.begin())));
					break;
				}
				case 'b':
					b[at] = uint8_t(v != nullptr && v->word != 0);
					break;
				case 'S':
					if (v != nullptr) {
						write(type_at(f.elem), v->fields, at);
					}
					break;
				default:
					put32(b, at, v != nullptr ? v->word : 0);
					break;
			}
		}
	};
	size_t at = instances_at;
	for (size_t i = 0; i < instances.size(); ++i) {
		const uint32_t type = type_at(instances[i].type);
		put32(b, instance_repeaters_at + i * 12 + 4, 1);
		put32(b, instance_repeaters_at + i * 12 + 8, 1 + type);
		const Guid g = guid(seed, uint32_t(i));
		std::memcpy(b.data() + at, g.bytes.data(), 16);
		write(type, instances[i].values, at + 16);
		at += 16 + sizes[type];
	}
	size_t array_at = 0;
	for (size_t i = 0; i < all.size(); ++i) {
		const Array &a = all[i];
		const size_t count = count_of(a);
		const size_t rep = array_repeaters_at + i * 12;
		put32(b, rep, uint32_t(array_at + 4));
		put32(b, rep + 4, uint32_t(count));
		put32(b, rep + 8, array_complex(a.elem));
		put32(b, arrays_at + array_at, uint32_t(count));
		for (size_t k = 0; k < count; ++k) {
			const size_t element_at = arrays_at + array_at + 4 + k * stride(a);
			if (a.elem.empty() || a.elem[0] == '*') {
				put32(b, element_at, a.words[k]);
			} else {
				write(type_at(a.elem), a.structs[k], element_at);
			}
		}
		array_at += 4 + count * stride(a);
	}
	return b;
}

struct DocBuilder {
	Partition p;
	uint32_t last = UINT32_MAX;

	explicit DocBuilder(uint32_t p_seed, uint32_t p_max_count = 16, float p_lifetime = 0, float p_time_scale = 1) {
		p.seed = p_seed;
		p.add({ "EmitterTemplateData",
				{ { "EmittableType", raw(1) }, { "Lifetime", f32(p_lifetime) }, { "MaxCount", i32(p_max_count) }, { "TimeScale", f32(p_time_scale) },
						{ "PointLightRandomIntensityMin", f32(1) }, { "PointLightRandomIntensityMax", f32(1) },
						{ "PointLightMaxClamp", f32(1) } } });
		p.add({ "EmitterDocument", { { "TemplateData", p.ref(0) } } });
	}
	Vals &tmpl() { return p.instances[0].values; }

	uint32_t proc(const std::string &p_type, Vals p_values = {}, uint32_t p_input = 1, Val p_defaults = v4(1, 1, 1, 1), bool p_enable = true) {
		set(p_values, "Enable", flag(p_enable));
		set(p_values, "EvaluatorInput", raw(p_input));
		set(p_values, "DefaultValues", std::move(p_defaults));
		const uint32_t i = p.add({ p_type, std::move(p_values) });
		if (last == UINT32_MAX) {
			set(p.instances[1].values, "RootProcessor", p.ref(i));
		} else {
			set(p.instances[last].values, "NextProcessor", p.ref(i));
		}
		last = i;
		return i;
	}

	uint32_t pre(uint32_t p_proc, const std::string &p_type, Vals p_values, uint32_t p_parameter = 1) {
		set(p_values, "EmitterParameter", raw(p_parameter));
		const uint32_t e = p.add({ p_type, std::move(p_values) });
		set(p.instances[p_proc].values, "Pre", p.ref(e));
		return e;
	}
	static Vals poly(float p_x, float p_y, float p_z, float p_w, float p_min = 0, float p_max = 1, float p_scale = 1) {
		return { { "Coefficients", v4(p_x, p_y, p_z, p_w) }, { "MinClamp", f32(p_min) }, { "MaxClamp", f32(p_max) }, { "ScaleValue", f32(p_scale) } };
	}
};

struct EffectBuilder {
	Partition p;
	std::vector<uint32_t> components;

	explicit EffectBuilder(uint32_t p_seed, uint32_t p_max_instances = 1) {
		p.seed = p_seed;
		p.add({ "EffectBlueprint", { { "Object", raw(2) } } });
		p.add({ "EffectEntityData",
				{ { "Transform", transform() }, { "LowEndPc", flag(true) }, { "MediumPc", flag(true) }, { "HighEndPc", flag(true) },
						{ "MaxInstanceCount", i32(p_max_instances) }, { "ResetInstanceWhenStarted", flag(true) } } });
	}
	Vals &entity() { return p.instances[1].values; }
	static Vals component_base() {
		return { { "Transform", transform() }, { "LowEndPc", flag(true) }, { "MediumPc", flag(true) }, { "HighEndPc", flag(true) } };
	}

	uint32_t emitter(uint32_t p_doc_seed, float p_probability = 1, Vals p_extra = {}) {
		Vals v = component_base();
		set(v, "Emitter", p.ext(p_doc_seed, 1));
		set(v, "SpawnProbability", f32(p_probability));
		set(v, "MaxInstanceCount", i32(1));
		set(v, "ResetInstanceWhenStarted", flag(true));
		for (auto &[n, x] : p_extra) {
			set(v, n, x);
		}
		return component({ "EmitterEntityData", std::move(v) });
	}
	uint32_t sound(uint32_t p_target_seed) {
		Vals v = component_base();
		set(v, "Sound", p.ext(p_target_seed, 0));
		return component({ "SoundEffectEntityData", std::move(v) });
	}
	uint32_t component(Instance p_instance) {
		const uint32_t i = p.add(std::move(p_instance));
		components.push_back(i);
		return i;
	}

	Partition &done() {
		Array a;
		for (const uint32_t c : components) {
			a.words.push_back(c + 1);
		}
		set(p.instances[1].values, "Components", p.array(std::move(a)));
		return p;
	}
};

inline Partition target(uint32_t p_seed) {
	Partition p;
	p.seed = p_seed;
	p.add({ "TargetAsset", {} });
	return p;
}

class MemResolver final : public bf3::core::EbxResolver {
public:
	bool set(const std::string &p_name, const Partition &p_partition) { return set_bytes(p_name, p_partition.build()); }
	bool set_bytes(const std::string &p_name, std::vector<uint8_t> p_bytes) {
		auto db = std::make_unique<bf3::core::EbxDb>();
		const bf3::core::Result<bool> r = db->parse_insert(p_name, std::move(p_bytes));
		if (!r.ok()) {
			return false;
		}
		dbs_[p_name] = std::move(db);
		++versions_[p_name];
		return true;
	}
	void remove(const std::string &p_name) {
		dbs_.erase(p_name);
		++versions_[p_name];
	}
	const bf3::core::EbxPartition *partition(const Guid &p_guid) const override {
		for (const auto &[n, db] : dbs_) {
			if (const bf3::core::EbxPartition *p = db->partition(p_guid)) {
				return p;
			}
		}
		return nullptr;
	}
	const bf3::core::EbxPartition *partition_by_name(std::string_view p_name) const override {
		const auto it = dbs_.find(std::string(p_name));
		return it == dbs_.end() ? nullptr : it->second->partition_by_name(p_name);
	}
	std::optional<std::string_view> name_of(const Guid &p_guid) const override {
		for (const auto &[n, db] : dbs_) {
			if (db->partition(p_guid) != nullptr) {
				return std::string_view(n);
			}
		}
		return std::nullopt;
	}

	std::optional<bf3::core::Sha1> digest(std::string_view p_name) const {
		if (!dbs_.contains(std::string(p_name))) {
			return std::nullopt;
		}
		bf3::core::Sha1 s{};
		const auto it = versions_.find(std::string(p_name));
		const uint64_t v = it == versions_.end() ? 0 : it->second;
		const uint64_t h = std::hash<std::string>{}(std::string(p_name));
		std::memcpy(s.bytes.data(), &v, 8);
		std::memcpy(s.bytes.data() + 8, &h, 8);
		return s;
	}

private:
	std::map<std::string, std::unique_ptr<bf3::core::EbxDb>> dbs_;
	std::map<std::string, uint64_t> versions_;
};

inline bf3::fx::EmitterDef emitter_def(const std::string &p_name, uint32_t p_max_count, float p_rate, float p_life, float p_window = 0) {
	using namespace bf3::fx;
	EmitterDef e;
	e.name = p_name;
	e.tmpl.max_count = p_max_count;
	e.tmpl.lifetime = p_window;
	e.slots[size_t(ProcessorType::SpawnRate)].present = true;
	e.params.spawn_rate = p_rate;
	e.params.distribute_over_time = true;
	if (p_life > 0) {
		e.slots[size_t(ProcessorType::UpdateAge)].present = true;
		e.params.particle_lifetime = p_life;
	}
	e.slots[size_t(ProcessorType::UpdateLinearVelocity)].present = true;
	return e;
}

inline uint64_t particles_bound(const bf3::fx::EffectDef &p_def, uint32_t p_emitter) {
	const bf3::fx::EmitterDef &e = p_def.emitters[p_emitter];
	uint64_t children = 0;
	for (const uint32_t c : e.children) {
		children += particles_bound(p_def, c);
	}
	return uint64_t(e.tmpl.max_count) * (1 + children);
}

inline std::shared_ptr<const bf3::fx::EffectDef> effect_def(const std::string &p_name, std::vector<bf3::fx::EmitterDef> p_emitters,
		std::vector<uint32_t> p_roots = { 0 }, uint16_t p_max_instances = 1, bool p_reset = true) {
	using namespace bf3::fx;
	auto d = std::make_shared<EffectDef>();
	d->name = p_name;
	d->max_instance_count = p_max_instances;
	d->reset_instance_when_started = p_reset;
	d->emitters = std::move(p_emitters);
	for (const uint32_t r : p_roots) {
		EffectComponentDef c;
		c.kind = ComponentKind::Emitter;
		c.emitter = r;
		d->components.push_back(c);
	}
	uint64_t total = 0;
	for (const uint32_t r : p_roots) {
		total += particles_bound(*d, r);
	}
	d->max_particles = uint32_t(total);
	return d;
}

}
