#include <doctest/doctest.h>

#include "bf3/fx/effect_cache.h"
#include "bf3/fx/effect_compiler.h"
#include "bf3/fx/effect_def.h"
#include "bf3/fx/effect_library.h"
#include "bf3/fx/emitter_atlas.h"
#include "bf3/fx/fx_types.h"
#include "bf3/fx/impact_table.h"
#include "bf3/fx/simulator.h"

#include <cmath>
#include <cstdint>
#include <set>
#include <string>
#include <type_traits>

namespace fx = bf3::fx;

static_assert(fx::kProcessorTypeCount == 32);
static_assert(fx::kEvaluatorTypeCount == 10);
static_assert(sizeof(fx::ParticleVertex) == 80);
static_assert(sizeof(fx::ParticleLight) == 32);
static_assert(std::is_trivially_copyable_v<fx::ParticleVertex>);
static_assert(std::is_trivially_copyable_v<fx::ParticleLight>);
static_assert(std::is_trivially_copyable_v<fx::EvaluatorDef>);
static_assert(std::is_trivially_copyable_v<fx::ProcessorSlot>);
static_assert(std::is_trivially_copyable_v<fx::EmitterOutput>);
static_assert(fx::processor_stage(fx::ProcessorType::BaseEmitter) == fx::ProcessorStage::Structural);
static_assert(fx::processor_stage(fx::ProcessorType::Emitter) == fx::ProcessorStage::Structural);
static_assert(fx::processor_stage(fx::ProcessorType::SpawnRate) == fx::ProcessorStage::Emitter);
static_assert(fx::processor_stage(fx::ProcessorType::SpawnPosition) == fx::ProcessorStage::Spawn);
static_assert(fx::processor_stage(fx::ProcessorType::SpawnAnimation) == fx::ProcessorStage::Spawn);
static_assert(fx::processor_stage(fx::ProcessorType::UpdateAge) == fx::ProcessorStage::Update);
static_assert(fx::processor_stage(fx::ProcessorType::UpdateClipScale) == fx::ProcessorStage::Update);

static_assert(fx::ProcessorType::AirResistance < fx::ProcessorType::Gravity);
static_assert(fx::ProcessorType::Gravity < fx::ProcessorType::UpdateLinearVelocity);
static_assert(fx::ProcessorType::LocalForce < fx::ProcessorType::UpdateLinearVelocity);
static_assert(fx::ProcessorType::Turbulance < fx::ProcessorType::UpdateLinearVelocity);
static_assert(fx::ProcessorType::SpawnDirection < fx::ProcessorType::SpawnSpeed);

static_assert(std::char_traits<char>::length(fx::kFxEffectCacheKind) <= bf3::core::kCacheKindMaxLength);
static_assert(std::char_traits<char>::length(fx::kFxDepsCacheKind) <= bf3::core::kCacheKindMaxLength);

TEST_SUITE("fx-contract") {
	TEST_CASE("fx.contract: the processor and evaluator name tables round-trip and are distinct") {
		std::set<std::string> names;
		for (size_t i = 0; i < fx::kProcessorTypeCount; ++i) {
			const fx::ProcessorType t = fx::ProcessorType(i);
			const std::string_view name = fx::processor_type_name(t);
			REQUIRE(!name.empty());
			CHECK(name.size() > 4);
			CHECK(name.substr(name.size() - 4) == "Data");
			const std::optional<fx::ProcessorType> back = fx::processor_type_from_name(name);
			REQUIRE(back.has_value());
			CHECK(*back == t);
			names.insert(std::string(name));
		}
		CHECK(names.size() == 32);
		for (size_t i = 0; i < fx::kEvaluatorTypeCount; ++i) {
			const fx::EvaluatorType t = fx::EvaluatorType(i);
			const std::optional<fx::EvaluatorType> back = fx::evaluator_type_from_name(fx::evaluator_type_name(t));
			REQUIRE(back.has_value());
			CHECK(*back == t);
			names.insert(std::string(fx::evaluator_type_name(t)));
		}
		CHECK(names.size() == 42);
		CHECK(!fx::processor_type_from_name("UpdateCollisionData").has_value());
		CHECK(!fx::processor_type_from_name("spawnratedata").has_value());
		CHECK(!fx::evaluator_type_from_name("").has_value());
		CHECK(fx::processor_type_name(fx::ProcessorType(200)).empty());
	}

	TEST_CASE("fx.contract: the enumerator tables round-trip") {
		for (size_t i = 0; i < fx::kEvaluatorInputCount; ++i) {
			CHECK(fx::evaluator_input_from_name(fx::evaluator_input_name(fx::EvaluatorInput(i))) == fx::EvaluatorInput(i));
		}
		for (size_t i = 0; i < fx::kEmitterParameterCount; ++i) {
			CHECK(fx::emitter_parameter_from_name(fx::emitter_parameter_name(fx::EmitterParameter(i))) == fx::EmitterParameter(i));
		}
		for (size_t i = 0; i < fx::kEmittableTypeCount; ++i) {
			CHECK(fx::emittable_type_from_name(fx::emittable_type_name(fx::EmittableType(i))) == fx::EmittableType(i));
		}
		CHECK(fx::evaluator_input_from_name("EfNormTime") == fx::EvaluatorInput::NormTime);
		CHECK(fx::evaluator_input_from_name("EfEmitterNormTime") == fx::EvaluatorInput::EmitterNormTime);
		CHECK(fx::emitter_parameter_from_name("EmitterParameterDistance") == fx::EmitterParameter::Distance);
		CHECK(fx::emittable_type_from_name("ParticleMesh") == fx::EmittableType::ParticleMesh);
		CHECK(fx::polynomial_operation_from_name("Multiplication") == fx::PolynomialOperation::Multiplication);
		CHECK(fx::polynomial_operation_from_name("Addition") == fx::PolynomialOperation::Addition);
		CHECK(!fx::polynomial_operation_from_name("Subtraction").has_value());
	}

	TEST_CASE("fx.contract: G2 polynomial goldens (f(0) = w, f(1) = x+y+z+w, clamp then scale)") {

		fx::PolynomialDef p;
		p.coefficients = { 0.25f, -0.5f, 0.125f, 2.0f };
		p.min_clamp = -100;
		p.max_clamp = 100;
		CHECK(fx::evaluate_polynomial(p, 0.0f) == 2.0f);
		CHECK(fx::evaluate_polynomial(p, 1.0f) == 0.25f - 0.5f + 0.125f + 2.0f);
		CHECK(fx::evaluate_polynomial(p, 0.5f) == doctest::Approx(((0.25 * 0.5 - 0.5) * 0.5 + 0.125) * 0.5 + 2.0));

		fx::PolynomialDef cubic;
		cubic.coefficients = { 1, 0, 0, 0 };
		CHECK(fx::evaluate_polynomial(cubic, 0.5f) == 0.125f);

		fx::PolynomialDef alpha;
		alpha.coefficients = { 0.0f, 0.95294344f, -1.9477582f, 1.0f };
		CHECK(fx::evaluate_polynomial(alpha, 0.0f) == 1.0f);
		CHECK(fx::evaluate_polynomial(alpha, 1.0f) == doctest::Approx(0.0051852).epsilon(1e-4));
		CHECK(fx::evaluate_polynomial(alpha, 0.5f) == doctest::Approx(0.264435).epsilon(1e-4));

		fx::PolynomialDef smoke;
		smoke.coefficients = { 0.0f, 0.0f, -1.117318f, 1.1179048f };
		smoke.scale = 0.1f;
		CHECK(fx::evaluate_polynomial(smoke, 0.0f) == doctest::Approx(0.1f));
		CHECK(fx::evaluate_polynomial(smoke, 1.0f) == doctest::Approx(0.1f * 0.0005868f).epsilon(1e-2));

		fx::PolynomialDef level;
		level.coefficients = { 0.0f, 0.0f, 0.9804876f, 0.0050001144f };
		level.min_clamp = 0.9f;
		CHECK(fx::evaluate_polynomial(level, 0.0f) == 0.9f);
		CHECK(fx::evaluate_polynomial(level, 1.0f) == doctest::Approx(0.98548772f));

		fx::PolynomialDef constant;
		constant.coefficients = { 0.0f, 0.0f, 0.25f, 0.75f };
		constant.min_clamp = 1;
		constant.max_clamp = 1;
		CHECK(fx::evaluate_polynomial(constant, 0.3f) == 1.0f);
	}

	TEST_CASE("fx.contract: colour interpolation runs from Color1 (f = 0) to Color0 (f = 1)") {

		const fx::Vec4 c{ 0.0f, 0.0f, -41.20004f, 0.794f };
		const fx::Vec3 c0{ 125.0f, 42.417f, 8.716f };
		const fx::Vec3 c1{ 48.719f, 12.026f, 0.739f };
		const fx::Vec3 at0 = fx::evaluate_color_interp(c, c0, c1, 0.0f);
		CHECK(at0.x == doctest::Approx(48.719 + (125.0 - 48.719) * 0.794).epsilon(1e-5));
		const fx::Vec3 at1 = fx::evaluate_color_interp(c, c0, c1, 1.0f);
		CHECK(at1 == c1);
		const fx::Vec3 full = fx::evaluate_color_interp({ 0, 0, 0, 5 }, c0, c1, 0.5f);
		CHECK(full.x == doctest::Approx(c0.x));
		CHECK(full.y == doctest::Approx(c0.y));
		CHECK(full.z == doctest::Approx(c0.z));
	}

	TEST_CASE("fx.contract: counter-based random numbers are keyed, uniform and in [0, 1)") {
		CHECK(fx::fx_key(1, 2, 3, 4) == fx::fx_key(1, 2, 3, 4));
		CHECK(fx::fx_key(1, 2, 3, 4) != fx::fx_key(1, 2, 3, 5));
		CHECK(fx::fx_key(1, 2, 3, 4) != fx::fx_key(2, 2, 3, 4));
		CHECK(fx::fx_key(0, 1, 0, 0) != fx::fx_key(0, 0, 1, 0));
		double sum = 0;
		float lo = 1, hi = 0;
		const int n = 100000;
		for (int i = 0; i < n; ++i) {
			const float u = fx::fx_unit(fx::fx_key(42, 7, uint32_t(i), 3));
			REQUIRE(u >= 0.0f);
			REQUIRE(u < 1.0f);
			lo = std::fmin(lo, u);
			hi = std::fmax(hi, u);
			sum += u;
		}
		CHECK(sum / n == doctest::Approx(0.5).epsilon(0.01));
		CHECK(lo < 0.001f);
		CHECK(hi > 0.999f);
		CHECK(fx::fx_uniform(1.0f, -1.0f, 0.25f) == 0.5f);
		CHECK(fx::fx_uniform(0.8f, 1.0f, 0.0f) == 0.8f);
	}

	TEST_CASE("fx.contract: instance seeds separate emitters, parents and children (docs §6.8 key layout)") {
		const uint64_t start = fx::fx_harness_seed(1, 0);
		CHECK(start != fx::fx_harness_seed(1, 1));
		CHECK(start != fx::fx_harness_seed(2, 0));

		const uint64_t root0 = fx::fx_emitter_seed(start, 0, 0);
		CHECK(root0 != fx::fx_emitter_seed(start, 0, 1));
		CHECK(root0 != fx::fx_emitter_seed(start, 1, 0));

		const uint64_t child_a = fx::fx_child_seed(root0, 3, 0);
		const uint64_t child_b = fx::fx_child_seed(root0, 3, 1);
		const uint64_t child_c = fx::fx_child_seed(root0, 4, 0);
		CHECK(child_a != child_b);
		CHECK(child_a != child_c);
		CHECK(child_a != root0);

		const uint32_t slot = uint32_t(fx::ProcessorType::SpawnPosition);
		CHECK(fx::fx_key(child_a, 0, slot, 0) != fx::fx_key(child_b, 0, slot, 0));
		CHECK(fx::fx_key(child_a, 0, slot, 0) != fx::fx_key(root0, 0, slot, 0));
		CHECK(fx::kSlotLight >= fx::kProcessorTypeCount);
		CHECK(fx::kSlotTurbulence >= fx::kProcessorTypeCount);
		CHECK(fx::kSlotLight != fx::kSlotTurbulence);
	}

	TEST_CASE("fx.contract: default-constructed defs are the documented neutral values") {
		const fx::EffectDef effect;
		CHECK(effect.transform == fx::Transform{});
		const fx::EffectStart start;
		CHECK(start.start_age == 0.0f);
		const fx::TextureRef tex;
		CHECK(tex.frame_count == 1);
		CHECK(tex.frame_columns == 1);
		const fx::EvaluatorDef e;
		CHECK(e.type == fx::EvaluatorType::Polynomial);
		CHECK(e.parameter == fx::EmitterParameter::None);
		CHECK(e.polynomial.min_clamp == 0.0f);
		CHECK(e.polynomial.max_clamp == 1.0f);
		CHECK(e.polynomial.scale == 1.0f);
		const fx::EmitterDef d;
		for (const fx::ProcessorSlot &s : d.slots) {
			CHECK(!s.present);
			CHECK(s.evaluator == fx::kNoEvaluator);
		}
		const fx::ParticleVertex v;
		CHECK(v.alpha_max == 1.0f);
		CHECK(v.alpha_scale == 1.0f);
		const fx::EffectHandle h;
		CHECK(!h.valid());
	}
}
