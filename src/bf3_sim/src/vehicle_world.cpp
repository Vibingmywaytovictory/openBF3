#include "vehicle_world_internal.h"

#include "jolt_runtime.h"
#include "static_world_internal.h"

#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/OffsetCenterOfMassShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/Vehicle/WheeledVehicleController.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace bf3::sim {

namespace {

constexpr JPH::BroadPhaseLayer kStaticBroadPhase(0);
constexpr JPH::BroadPhaseLayer kMovingBroadPhase(1);
constexpr JPH::ObjectLayer kVehicleLayer = kVehicleObjectLayer;
constexpr uint32_t kObjectLayers = kVehicleObjectLayer + 1u;

class VehicleBroadPhaseLayers final : public JPH::BroadPhaseLayerInterface {
public:
	JPH::uint GetNumBroadPhaseLayers() const override { return 2; }
	JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer p_layer) const override {
		return p_layer < kStaticLayerCount ? kStaticBroadPhase : kMovingBroadPhase;
	}
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
	const char *GetBroadPhaseLayerName(JPH::BroadPhaseLayer p_layer) const override { return p_layer == kStaticBroadPhase ? "STATIC" : "MOVING"; }
#endif
};

class VehicleObjectVsBroadPhase final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
	bool ShouldCollide(JPH::ObjectLayer p_layer, JPH::BroadPhaseLayer) const override { return p_layer >= kStaticLayerCount && p_layer < kObjectLayers; }
};

class VehicleObjectPairs final : public JPH::ObjectLayerPairFilter {
public:
	bool ShouldCollide(JPH::ObjectLayer p_a, JPH::ObjectLayer p_b) const override {
		if (p_a == kVehicleLayer || p_b == kVehicleLayer) {
			const JPH::ObjectLayer other = p_a == kVehicleLayer ? p_b : p_a;
			return other == kVehicleLayer || other == JPH::ObjectLayer(StaticLayer::Collision);
		}
		return p_a >= kStaticLayerCount || p_b >= kStaticLayerCount;
	}
};

float combine_contact_friction(const JPH::Body &p_body1, const JPH::SubShapeID &, const JPH::Body &p_body2, const JPH::SubShapeID &) {
	if (p_body1.IsStatic()) {
		return p_body2.GetFriction();
	}
	if (p_body2.IsStatic()) {
		return p_body1.GetFriction();
	}
	return JPH::Sqrt(p_body1.GetFriction() * p_body2.GetFriction());
}

JPH::Vec3 to_jolt(const Vec3 &p_v) {
	return JPH::Vec3(p_v[0], p_v[1], p_v[2]);
}

Vec3 from_jolt(JPH::Vec3Arg p_v) {
	return { p_v.GetX(), p_v.GetY(), p_v.GetZ() };
}

bool finite_floats(const float *p_values, size_t p_count) {
	for (size_t i = 0; i < p_count; ++i) {
		if (!std::isfinite(p_values[i])) {
			return false;
		}
	}
	return true;
}

template <size_t N>
bool finite_array(const std::array<float, N> &p_values) {
	return finite_floats(p_values.data(), N);
}

namespace body_at {
constexpr size_t kActive = 0, kPosition = 1, kRotation = 13, kLinear = 29, kAngular = 41, kForce = 53, kTorque = 65, kSpheres = 77, kTimer = 125,
				 kAllowSleeping = 129;
}
static_assert(body_at::kAllowSleeping + 1 == kJoltBodyStreamBytes);

namespace constraint_at {
constexpr size_t kEnabled = 0, kController = 1, kWheels = 45, kWheelBytes = 68;
constexpr size_t kForward = 0, kRight = 4, kBrake = 8, kHandBrake = 12, kPreviousDt = 16, kRpm = 20, kGear = 24, kClutch = 28, kSwitch = 32, kRelease = 36,
				 kLatency = 40;
constexpr size_t kSpin = 0, kAngle = 4, kContactBody = 8, kContactPosition = 12, kContactNormal = 24, kContactLateral = 36, kSuspension = 48,
				 kLambdaSuspension = 52, kLambdaMaxUp = 56, kLambdaLongitudinal = 60, kLambdaLateral = 64;
}
static_assert(constraint_at::kWheels + constraint_at::kWheelBytes * 4 + 20 == jolt_vehicle_constraint_stream_bytes(4));

template <typename T>
void put(std::vector<uint8_t> &r_bytes, size_t p_at, const T &p_value) {
	std::memcpy(r_bytes.data() + p_at, &p_value, sizeof(T));
}

template <typename T>
T get(const std::vector<uint8_t> &p_bytes, size_t p_at) {
	T v;
	std::memcpy(&v, p_bytes.data() + p_at, sizeof(T));
	return v;
}

void put_bytes(std::vector<uint8_t> &r_bytes, size_t p_at, const void *p_data, size_t p_size) {
	std::memcpy(r_bytes.data() + p_at, p_data, p_size);
}

void get_bytes(const std::vector<uint8_t> &p_bytes, size_t p_at, void *r_data, size_t p_size) {
	std::memcpy(r_data, p_bytes.data() + p_at, p_size);
}

bool same_world_keys(const VehicleTuning &p_a, const VehicleTuning &p_b) {
	return std::memcmp(&p_a.gravity, &p_b.gravity, sizeof(float)) == 0 && std::memcmp(&p_a.ground_friction, &p_b.ground_friction, sizeof(float)) == 0 &&
			p_a.collision_steps == p_b.collision_steps && p_a.velocity_steps == p_b.velocity_steps && p_a.position_steps == p_b.position_steps;
}

float filter_axis(float p_current, float p_target, float p_in, float p_out) {
	const bool growing = (p_target > 0.0f && p_current >= 0.0f && p_target > p_current) || (p_target < 0.0f && p_current <= 0.0f && p_target < p_current);
	const float duration = growing ? p_in : p_out;
	if (!(duration > 0.0f)) {
		return p_target;
	}
	const float step = kTickSeconds / duration;
	if (p_target > p_current) {
		return std::min(p_current + step, p_target);
	}
	return std::max(p_current - step, p_target);
}

uint64_t fnv_bytes(uint64_t p_hash, const void *p_data, size_t p_size) {
	const unsigned char *b = static_cast<const unsigned char *>(p_data);
	for (size_t i = 0; i < p_size; ++i) {
		p_hash ^= b[i];
		p_hash *= 0x100000001B3ull;
	}
	return p_hash;
}

uint64_t snapshot_hash(const std::vector<uint8_t> &p_jolt, const std::vector<VehicleDriverState> &p_drivers) {
	uint64_t h = fnv_bytes(0xCBF29CE484222325ull, p_jolt.data(), p_jolt.size());
	for (const VehicleDriverState &d : p_drivers) {
		h = fnv_bytes(h, &d.steer_filter, sizeof(d.steer_filter));
		h = fnv_bytes(h, &d.throttle_filter, sizeof(d.throttle_filter));
		h = fnv_bytes(h, &d.brake_filter, sizeof(d.brake_filter));
		h = fnv_bytes(h, &d.stand_still_ticks, sizeof(d.stand_still_ticks));
		h = fnv_bytes(h, &d.reversing, sizeof(d.reversing));
	}
	return h;
}

JPH::EStateRecorderState jolt_section(VehicleSnapshotSection p_section) {
	switch (p_section) {
		case VehicleSnapshotSection::Global:
			return JPH::EStateRecorderState::Global;
		case VehicleSnapshotSection::Bodies:
			return JPH::EStateRecorderState::Bodies;
		case VehicleSnapshotSection::Contacts:
			return JPH::EStateRecorderState::Contacts;
		case VehicleSnapshotSection::Constraints:
			return JPH::EStateRecorderState::Constraints;
	}
	return JPH::EStateRecorderState::None;
}

}

void ByteStateRecorder::WriteBytes(const void *p_data, size_t p_size) {
	if (out_ == nullptr) {
		failed_ = true;
		return;
	}
	const uint8_t *b = static_cast<const uint8_t *>(p_data);
	out_->insert(out_->end(), b, b + p_size);
}

void ByteStateRecorder::ReadBytes(void *p_data, size_t p_size) {
	if (failed_ || in_.size() - at_ < p_size) {
		failed_ = true;
		std::memset(p_data, 0, p_size);
		return;
	}
	std::memcpy(p_data, in_.data() + at_, p_size);
	at_ += p_size;
}

void VehicleContactCounter::count(const JPH::Body &p_body1, const JPH::Body &p_body2, const JPH::ContactManifold &p_manifold) {
	if (contacts == nullptr) {
		return;
	}
	const JPH::Body *bodies[2] = { &p_body1, &p_body2 };
	for (size_t i = 0; i < 2; ++i) {
		const JPH::Body &self = *bodies[i];
		const JPH::Body &other = *bodies[1 - i];
		if (self.GetObjectLayer() != kVehicleLayer) {
			continue;
		}
		const uint32_t index = self.GetID().GetIndex();
		if (index < static_bodies || index - static_bodies >= contacts->size()) {
			continue;
		}
		VehicleContacts &c = (*contacts)[index - static_bodies];
		if (other.IsStatic()) {
			c.chassis_static_contacts = uint16_t(std::min<uint32_t>(c.chassis_static_contacts + 1u, 0xFFFFu));
		} else if (other.GetObjectLayer() == kVehicleLayer) {
			c.chassis_vehicle_contacts = uint16_t(std::min<uint32_t>(c.chassis_vehicle_contacts + 1u, 0xFFFFu));
		}
		c.max_penetration = std::max(c.max_penetration, p_manifold.mPenetrationDepth);
	}
}

bool VehicleSaveFilter::ShouldSaveBody(const JPH::Body &p_body) const {
	return !p_body.IsStatic();
}

std::vector<VehicleSlot> VehicleWorld::Impl::used_slots() const {
	std::vector<VehicleSlot> out;
	for (VehicleSlot s = 0; s < vehicles.size(); ++s) {
		if (vehicles[s].used) {
			out.push_back(s);
		}
	}
	return out;
}

void VehicleWorld::Impl::save_body_stream(VehicleSlot p_slot, std::vector<uint8_t> &r_bytes) const {
	r_bytes.clear();
	ByteStateRecorder recorder(r_bytes);
	const JPH::BodyLockRead lock(system->GetBodyLockInterfaceNoLock(), vehicles[p_slot].body);
	if (lock.Succeeded()) {
		system->SaveBodyState(lock.GetBody(), recorder);
	}
}

void VehicleWorld::Impl::save_constraint_stream(VehicleSlot p_slot, std::vector<uint8_t> &r_bytes) const {
	r_bytes.clear();
	ByteStateRecorder recorder(r_bytes);
	vehicles[p_slot].constraint->SaveState(recorder);
}

void VehicleWorld::Impl::drive(VehicleSlot p_slot, const VehicleCommand &p_command) {
	Vehicle &v = vehicles[p_slot];
	const VehicleModel &m = *v.model;
	VehicleDriverState &d = v.driver;

	const float t = float(std::max<int>(p_command.throttle, -127)) / 127.0f;
	const float b = float(p_command.brake) / 255.0f;
	const float s = float(std::max<int>(p_command.steer, -127)) / 127.0f;
	const bool hand_brake = (p_command.buttons & vehicle_buttons::kHandbrake) != 0;

	d.steer_filter = filter_axis(d.steer_filter, s, m.yaw_inertia_in, m.yaw_inertia_out);
	d.throttle_filter = filter_axis(d.throttle_filter, t, m.throttle_inertia_in, m.throttle_inertia_out);
	d.brake_filter = filter_axis(d.brake_filter, b, m.brake_inertia_in, m.brake_inertia_out);
	JPH::BodyInterface &bi = system->GetBodyInterfaceNoLock();
	const JPH::Vec3 velocity = bi.GetLinearVelocity(v.body);
	const JPH::Vec3 forward_axis = bi.GetRotation(v.body) * JPH::Vec3::sAxisZ();
	const float forward_speed = velocity.Dot(forward_axis);
	const float speed = velocity.Length();

	float forward = d.throttle_filter;
	float brake = d.brake_filter;
	if (forward < 0.0f && !m.allow_negative_throttle) {
		forward = 0.0f;
	}
	if (forward < 0.0f) {
		if (forward_speed < m.reverse_switch_speed) {
			d.reversing = 1;
		} else {
			brake = std::max(brake, -forward);
			forward = 0.0f;
			d.reversing = 0;
		}
	} else if (forward > 0.0f) {
		if (forward_speed > -m.reverse_switch_speed) {
			d.reversing = 0;
		} else {
			brake = std::max(brake, forward);
			forward = 0.0f;
		}
	}

	if (m.stand_still_brake && m.stand_still_ticks > 0) {
		if (p_command.throttle == 0 && speed < m.stand_still_speed) {
			if (d.stand_still_ticks < m.stand_still_ticks) {
				++d.stand_still_ticks;
			}
		} else {
			d.stand_still_ticks = 0;
		}
		if (d.stand_still_ticks >= m.stand_still_ticks) {
			brake = 1.0f;
		}
	}

	const float lock = vehicle_max_steer_angle(m, 0.0f);
	const float falloff = lock > 0.0f ? vehicle_max_steer_angle(m, forward_speed) / lock : 0.0f;
	const float right = d.steer_filter * falloff;

	if (p_command.throttle != 0 || p_command.brake != 0 || p_command.steer != 0 || hand_brake) {
		bi.ActivateBody(v.body);
	}

	static_cast<JPH::WheeledVehicleController *>(v.constraint->GetController())->SetDriverInput(forward, right, brake, hand_brake ? 1.0f : 0.0f);
	if (m.drag_coefficient > 0.0f && speed > 0.0f) {
		bi.AddForce(v.body, velocity * (-m.drag_coefficient * speed), JPH::EActivation::DontActivate);
	}
}

const char *vehicle_snapshot_section_name(VehicleSnapshotSection p_section) {
	switch (p_section) {
		case VehicleSnapshotSection::Global:
			return "global";
		case VehicleSnapshotSection::Bodies:
			return "bodies";
		case VehicleSnapshotSection::Contacts:
			return "contacts";
		case VehicleSnapshotSection::Constraints:
			return "constraints";
	}
	return "?";
}

VehicleWorld::VehicleWorld(std::unique_ptr<Impl> p_impl) :
		impl_(std::move(p_impl)) {}

VehicleWorld::~VehicleWorld() {
	Impl &w = *impl_;
	if (w.system) {
		JPH::BodyInterface &bi = w.system->GetBodyInterfaceNoLock();
		for (Impl::Vehicle &v : w.vehicles) {
			if (!v.used) {
				continue;
			}
			w.system->RemoveStepListener(v.constraint);
			w.system->RemoveConstraint(v.constraint);
			v.constraint = nullptr;
			bi.RemoveBody(v.body);
			bi.DestroyBody(v.body);
			v.used = false;
		}
		if (!w.statics.empty()) {
			bi.RemoveBodies(w.statics.data(), int(w.statics.size()));
			bi.DestroyBodies(w.statics.data(), int(w.statics.size()));
		}
	}
	w.statics.clear();
	w.system.reset();
	w.jobs.reset();
	w.temp.reset();
}

core::Result<std::unique_ptr<VehicleWorld>> VehicleWorld::create(std::shared_ptr<const StaticWorldShapes> p_shapes, const VehicleWorldOptions &p_options) {
	if (p_shapes == nullptr) {
		return core::make_error("vehicle world: no static shapes");
	}
	if (p_options.max_vehicles == 0 || p_options.max_vehicles > kMaxVehicleSlots) {
		return core::make_error("vehicle world: max_vehicles " + std::to_string(p_options.max_vehicles) + " outside 1.." + std::to_string(kMaxVehicleSlots));
	}
	if (p_options.temp_allocator_bytes < (64u << 10) || p_options.max_body_pairs < 64 || p_options.max_contact_constraints < 64) {
		return core::make_error("vehicle world: allocator or pair limits too small");
	}
	const std::vector<StaticBodySpec> specs = static_body_specs(p_shapes->impl());
	const size_t static_bodies = specs.size();
	const uint64_t max_bodies = uint64_t(static_bodies) + p_options.max_vehicles;
	if (max_bodies > (1u << 22)) {
		return core::make_error("vehicle world: " + std::to_string(max_bodies) + " bodies exceed Jolt's limit");
	}

	auto impl = std::make_unique<Impl>();
	Impl &w = *impl;
	w.runtime = jolt_runtime_acquire();
	w.shapes = std::move(p_shapes);
	w.options = p_options;
	w.broadphase_layers = std::make_unique<VehicleBroadPhaseLayers>();
	w.object_vs_broadphase = std::make_unique<VehicleObjectVsBroadPhase>();
	w.object_pairs = std::make_unique<VehicleObjectPairs>();
	w.vehicles.resize(p_options.max_vehicles);
	w.contacts.resize(p_options.max_vehicles);
	w.listener = std::make_unique<VehicleContactCounter>();
	w.listener->static_bodies = uint32_t(static_bodies);
	w.listener->contacts = &w.contacts;
	w.temp = std::make_unique<JPH::TempAllocatorImpl>(p_options.temp_allocator_bytes);
	w.jobs = std::make_unique<JPH::JobSystemSingleThreaded>(JPH::cMaxPhysicsJobs);
	w.system = std::make_unique<JPH::PhysicsSystem>();
	w.system->Init(JPH::uint(max_bodies), 0, p_options.max_body_pairs, p_options.max_contact_constraints, *w.broadphase_layers, *w.object_vs_broadphase,
			*w.object_pairs);
	JPH::PhysicsSettings settings = w.system->GetPhysicsSettings();
	settings.mNumVelocitySteps = p_options.tuning.velocity_steps;
	settings.mNumPositionSteps = p_options.tuning.position_steps;
	w.system->SetPhysicsSettings(settings);
	w.system->SetGravity(JPH::Vec3(0.0f, -p_options.tuning.gravity, 0.0f));
	w.system->SetCombineFriction(combine_contact_friction);
	w.system->SetContactListener(w.listener.get());

	JPH::BodyInterface &bi = w.system->GetBodyInterfaceNoLock();
	w.statics.reserve(static_bodies);
	for (const StaticBodySpec &spec : specs) {
		JPH::BodyCreationSettings body(spec.shape, JPH::RVec3(spec.position), spec.rotation, JPH::EMotionType::Static, JPH::ObjectLayer(spec.layer));
		body.mUserData = spec.user;
		JPH::Body *created = bi.CreateBody(body);
		if (created == nullptr) {
			return core::make_error("vehicle world: Jolt refused static body " + std::to_string(w.statics.size()));
		}
		w.statics.push_back(created->GetID());
	}
	if (!w.statics.empty()) {
		const JPH::BodyInterface::AddState state = bi.AddBodiesPrepare(w.statics.data(), int(w.statics.size()));
		bi.AddBodiesFinalize(w.statics.data(), int(w.statics.size()), state, JPH::EActivation::DontActivate);
	}
	w.system->OptimizeBroadPhase();
	return std::unique_ptr<VehicleWorld>(new VehicleWorld(std::move(impl)));
}

const std::shared_ptr<const StaticWorldShapes> &VehicleWorld::shapes() const {
	return impl_->shapes;
}

size_t VehicleWorld::static_body_count() const {
	return impl_->statics.size();
}

uint32_t VehicleWorld::static_body_id(size_t p_index) const {
	return p_index < impl_->statics.size() ? impl_->statics[p_index].GetIndexAndSequenceNumber() : kNoContactBody;
}

uint32_t VehicleWorld::vehicle_body_id(VehicleSlot p_slot) const {
	return JPH::BodyID(JPH::uint32(impl_->statics.size() + p_slot)).GetIndexAndSequenceNumber();
}

core::Status VehicleWorld::add_vehicle(VehicleSlot p_slot, std::shared_ptr<const VehicleModel> p_model, const VehiclePlacement &p_placement) {
	Impl &w = *impl_;
	const std::string where = "vehicle world: slot " + std::to_string(p_slot);
	if (p_slot >= w.vehicles.size()) {
		return core::make_error(where + " outside 0.." + std::to_string(w.vehicles.size() - 1));
	}
	if (w.vehicles[p_slot].used) {
		return core::make_error(where + " is in use");
	}
	if (p_model == nullptr || p_model->wheel_count == 0 || p_model->wheel_count > kMaxVehicleWheels || p_model->chassis.empty()) {
		return core::make_error(where + ": no usable model");
	}
	if (!same_world_keys(p_model->tuning, w.options.tuning)) {
		return core::make_error(where + ": the model was built with other world keys (gravity, ground friction, step counts) than the world");
	}
	const VehicleModel &m = *p_model;
	if (!finite_array(p_placement.position) || !finite_array(p_placement.rotation)) {
		return core::make_error(where + ": non-finite placement");
	}
	const JPH::Quat rotation = JPH::Quat(p_placement.rotation[0], p_placement.rotation[1], p_placement.rotation[2], p_placement.rotation[3]).Normalized();

	JPH::Array<JPH::RefConst<JPH::Shape>> hulls;
	for (size_t i = 0; i < m.chassis.size(); ++i) {
		JPH::Array<JPH::Vec3> points;
		points.reserve(m.chassis[i].points.size());
		for (const Vec3 &p : m.chassis[i].points) {
			points.push_back(to_jolt(p));
		}
		JPH::ConvexHullShapeSettings hull(points, m.chassis[i].convex_radius);
		JPH::ShapeSettings::ShapeResult r = hull.Create();
		if (r.HasError()) {
			return core::make_error(where + ": chassis piece " + std::to_string(i) + ": " + std::string(r.GetError().c_str()));
		}
		hulls.push_back(r.Get());
	}
	JPH::RefConst<JPH::Shape> inner;
	if (hulls.size() == 1) {
		inner = hulls[0];
	} else {
		JPH::StaticCompoundShapeSettings compound;
		for (const JPH::RefConst<JPH::Shape> &h : hulls) {
			compound.AddShape(JPH::Vec3::sZero(), JPH::Quat::sIdentity(), h.GetPtr());
		}
		JPH::ShapeSettings::ShapeResult r = compound.Create();
		if (r.HasError()) {
			return core::make_error(where + ": chassis compound: " + std::string(r.GetError().c_str()));
		}
		inner = r.Get();
	}
	JPH::OffsetCenterOfMassShapeSettings offset(to_jolt(m.center_of_mass) - inner->GetCenterOfMass(), inner.GetPtr());
	JPH::ShapeSettings::ShapeResult chassis = offset.Create();
	if (chassis.HasError()) {
		return core::make_error(where + ": chassis centre of mass: " + std::string(chassis.GetError().c_str()));
	}

	JPH::MassProperties mass = chassis.Get()->GetMassProperties();
	mass.ScaleToMass(m.mass);
	const JPH::Mat44 scale = JPH::Mat44::sScale(
			JPH::Vec3(std::sqrt(m.inertia_modifier[0]), std::sqrt(m.inertia_modifier[1]), std::sqrt(m.inertia_modifier[2])));
	mass.mInertia = scale * mass.mInertia * scale;

	JPH::BodyCreationSettings body(chassis.Get(), JPH::RVec3(to_jolt(p_placement.position)), rotation, JPH::EMotionType::Dynamic, kVehicleLayer);
	body.mOverrideMassProperties = JPH::EOverrideMassProperties::MassAndInertiaProvided;
	body.mMassPropertiesOverride = mass;
	body.mFriction = m.friction;
	body.mRestitution = m.restitution;
	body.mGravityFactor = m.gravity_factor;
	body.mAllowSleeping = true;
	body.mUserData = p_slot;
	JPH::BodyInterface &bi = w.system->GetBodyInterfaceNoLock();
	JPH::Body *created = bi.CreateBodyWithID(JPH::BodyID(JPH::uint32(w.statics.size() + p_slot)), body);
	if (created == nullptr) {
		return core::make_error(where + ": Jolt refused the chassis body");
	}

	JPH::VehicleConstraintSettings vehicle;
	vehicle.mUp = JPH::Vec3::sAxisY();
	vehicle.mForward = JPH::Vec3::sAxisZ();
	vehicle.mMaxPitchRollAngle = m.max_pitch_roll_angle;
	for (size_t i = 0; i < m.wheel_count; ++i) {
		const VehicleWheelModel &wm = m.wheels[i];
		JPH::WheelSettingsWV *wheel = new JPH::WheelSettingsWV;
		wheel->mPosition = to_jolt(wm.attachment);
		wheel->mSuspensionForcePoint = to_jolt(wm.data_centre);
		wheel->mSuspensionDirection = -JPH::Vec3::sAxisY();
		wheel->mSteeringAxis = JPH::Vec3::sAxisY();
		wheel->mWheelUp = JPH::Vec3::sAxisY();
		wheel->mWheelForward = JPH::Vec3::sAxisZ();
		wheel->mSuspensionMinLength = wm.suspension_min;
		wheel->mSuspensionMaxLength = wm.suspension_max;
		wheel->mSuspensionPreloadLength = 0.0f;
		wheel->mSuspensionSpring = JPH::SpringSettings(JPH::ESpringMode::StiffnessAndDamping, wm.spring_stiffness, wm.spring_damping);
		wheel->mRadius = wm.radius;
		wheel->mWidth = wm.width;
		wheel->mInertia = wm.inertia;
		wheel->mAngularDamping = wm.angular_damping;
		wheel->mMaxSteerAngle = wm.max_steer_angle;
		wheel->mMaxBrakeTorque = wm.max_brake_torque;
		wheel->mMaxHandBrakeTorque = wm.max_hand_brake_torque;
		for (JPH::LinearCurve::Point &p : wheel->mLongitudinalFriction.mPoints) {
			p.mY *= m.tuning.longitudinal_grip;
		}
		for (JPH::LinearCurve::Point &p : wheel->mLateralFriction.mPoints) {
			p.mY *= m.tuning.lateral_grip;
		}
		vehicle.mWheels.push_back(wheel);
	}
	JPH::WheeledVehicleControllerSettings *controller = new JPH::WheeledVehicleControllerSettings;
	controller->mEngine.mMaxTorque = m.engine_max_torque;
	controller->mEngine.mMinRPM = m.engine_min_rpm;
	controller->mEngine.mMaxRPM = m.engine_max_rpm;
	controller->mEngine.mNormalizedTorque.Clear();
	for (size_t i = 0; i < m.torque_points; ++i) {
		controller->mEngine.mNormalizedTorque.AddPoint(m.torque_curve[i][0], m.torque_curve[i][1]);
	}
	controller->mEngine.mInertia = m.engine_inertia;
	controller->mEngine.mAngularDamping = m.engine_angular_damping;
	controller->mTransmission.mMode = JPH::ETransmissionMode::Auto;
	controller->mTransmission.mGearRatios.clear();
	for (size_t i = 0; i < m.forward_gears; ++i) {
		controller->mTransmission.mGearRatios.push_back(m.forward_ratios[i]);
	}
	controller->mTransmission.mReverseGearRatios.clear();
	for (size_t i = 0; i < m.reverse_gears; ++i) {
		controller->mTransmission.mReverseGearRatios.push_back(m.reverse_ratios[i]);
	}
	controller->mTransmission.mSwitchTime = m.switch_time;
	controller->mTransmission.mClutchReleaseTime = m.clutch_release_time;
	controller->mTransmission.mSwitchLatency = m.switch_latency;
	controller->mTransmission.mShiftUpRPM = m.shift_up_rpm;
	controller->mTransmission.mShiftDownRPM = m.shift_down_rpm;
	controller->mTransmission.mClutchStrength = m.clutch_strength;
	for (size_t i = 0; i < m.differentials; ++i) {
		JPH::VehicleDifferentialSettings diff;
		diff.mLeftWheel = m.differential_wheels[i][0];
		diff.mRightWheel = m.differential_wheels[i][1];
		diff.mDifferentialRatio = m.differential_ratio[i];
		diff.mLeftRightSplit = 0.5f;
		diff.mEngineTorqueRatio = m.differential_torque_ratio[i];
		controller->mDifferentials.push_back(diff);
	}
	controller->mDifferentialLimitedSlipRatio = m.differential_limited_slip;
	vehicle.mController = controller;
	for (size_t i = 0; i < m.anti_roll_bars; ++i) {
		JPH::VehicleAntiRollBar bar;
		bar.mLeftWheel = m.anti_roll_wheels[i][0];
		bar.mRightWheel = m.anti_roll_wheels[i][1];
		bar.mStiffness = m.anti_roll_stiffness[i];
		vehicle.mAntiRollBars.push_back(bar);
	}

	Impl::Vehicle &v = w.vehicles[p_slot];
	v.constraint = new JPH::VehicleConstraint(*created, vehicle);
	switch (m.tester) {
		case WheelCollisionTester::Cylinder:
			v.tester = new JPH::VehicleCollisionTesterCastCylinder(kVehicleLayer);
			break;
		case WheelCollisionTester::Sphere:
			v.tester = new JPH::VehicleCollisionTesterCastSphere(kVehicleLayer, 0.5f * m.wheels[0].width);
			break;
		case WheelCollisionTester::Ray:
			v.tester = new JPH::VehicleCollisionTesterRay(kVehicleLayer);
			break;
	}
	v.constraint->SetVehicleCollisionTester(v.tester);

	const float ground = w.options.tuning.ground_friction;
	v.constraint->SetCombineFriction([ground](JPH::uint, float &r_longitudinal, float &r_lateral, const JPH::Body &p_body, const JPH::SubShapeID &) {
		if (p_body.IsStatic()) {
			r_longitudinal *= ground;
			r_lateral *= ground;
			return;
		}
		const float f = p_body.GetFriction();
		r_longitudinal = JPH::Sqrt(r_longitudinal * f);
		r_lateral = JPH::Sqrt(r_lateral * f);
	});
	bi.AddBody(created->GetID(), JPH::EActivation::Activate);
	w.system->AddConstraint(v.constraint);
	w.system->AddStepListener(v.constraint);
	v.used = true;
	v.model = std::move(p_model);
	v.body = created->GetID();
	v.driver = VehicleDriverState();
	w.contacts[p_slot] = VehicleContacts();

	VehicleState clean = state(p_slot);
	for (size_t i = 0; i < clean.wheel_count; ++i) {
		clean.wheels[i].contact_body = kNoContactBody;
		clean.wheels[i].contact_position = {};
		clean.wheels[i].contact_normal = {};
		clean.wheels[i].contact_lateral = {};
	}
	return restore_vehicle_state(p_slot, clean, vehicle_restore::kWheelContacts, ContactCachePolicy::Keep);
}

bool VehicleWorld::has_vehicle(VehicleSlot p_slot) const {
	return p_slot < impl_->vehicles.size() && impl_->vehicles[p_slot].used;
}

core::Status VehicleWorld::step(std::span<const VehicleSlot> p_slots, std::span<const VehicleCommand> p_commands) {
	Impl &w = *impl_;
	if (p_slots.size() != p_commands.size()) {
		return core::make_error("vehicle world step: " + std::to_string(p_slots.size()) + " slots and " + std::to_string(p_commands.size()) + " commands");
	}
	std::vector<VehicleCommand> commands(w.vehicles.size());
	std::vector<uint8_t> named(w.vehicles.size(), 0);
	for (size_t i = 0; i < p_slots.size(); ++i) {
		const VehicleSlot s = p_slots[i];
		if (!has_vehicle(s)) {
			return core::make_error("vehicle world step: no vehicle in slot " + std::to_string(s));
		}
		if (named[s] != 0) {
			return core::make_error("vehicle world step: slot " + std::to_string(s) + " named twice");
		}
		named[s] = 1;
		commands[s] = p_commands[i];
	}
	for (VehicleContacts &c : w.contacts) {
		c = VehicleContacts();
	}

	for (VehicleSlot s = 0; s < w.vehicles.size(); ++s) {
		if (w.vehicles[s].used) {
			w.drive(s, commands[s]);
		}
	}
	const JPH::EPhysicsUpdateError error = w.system->Update(kTickSeconds, int(w.options.tuning.collision_steps), w.temp.get(), w.jobs.get());
	for (VehicleSlot s = 0; s < w.vehicles.size(); ++s) {
		const Impl::Vehicle &v = w.vehicles[s];
		if (!v.used) {
			continue;
		}
		uint8_t in_contact = 0;
		for (const JPH::Wheel *wheel : v.constraint->GetWheels()) {
			in_contact += wheel->HasContact() ? 1 : 0;
		}
		w.contacts[s].wheels_in_contact = in_contact;
	}
	++w.tick;
	if (error != JPH::EPhysicsUpdateError::None) {
		return core::make_error("vehicle world step: Jolt update error " + std::to_string(uint32_t(error)) + " (a contact cache is full)");
	}
	return core::Status::success();
}

Tick VehicleWorld::tick() const {
	return impl_->tick;
}

VehicleState VehicleWorld::state(VehicleSlot p_slot) const {
	const Impl &w = *impl_;
	VehicleState out;
	out.tick = w.tick;
	if (!has_vehicle(p_slot)) {
		return out;
	}
	const Impl::Vehicle &v = w.vehicles[p_slot];
	std::vector<uint8_t> body;
	w.save_body_stream(p_slot, body);
	std::vector<uint8_t> constraint;
	w.save_constraint_stream(p_slot, constraint);
	const size_t wheels = v.constraint->GetWheels().size();
	if (body.size() != kJoltBodyStreamBytes || constraint.size() != jolt_vehicle_constraint_stream_bytes(wheels) || wheels > kMaxVehicleWheels) {
		return out;
	}
	VehicleBodyState &b = out.body;
	b.active = get<uint8_t>(body, body_at::kActive) != 0 ? 1 : 0;
	get_bytes(body, body_at::kPosition, b.com_position.data(), 12);
	get_bytes(body, body_at::kRotation, b.rotation.data(), 16);
	get_bytes(body, body_at::kLinear, b.linear_velocity.data(), 12);
	get_bytes(body, body_at::kAngular, b.angular_velocity.data(), 12);
	get_bytes(body, body_at::kForce, b.force.data(), 12);
	get_bytes(body, body_at::kTorque, b.torque.data(), 12);
	get_bytes(body, body_at::kSpheres, b.sleep_test_spheres.data(), 48);
	b.sleep_test_timer = get<float>(body, body_at::kTimer);
	b.allow_sleeping = get<uint8_t>(body, body_at::kAllowSleeping) != 0 ? 1 : 0;
	{
		const JPH::BodyLockRead lock(w.system->GetBodyLockInterfaceNoLock(), v.body);
		if (lock.Succeeded()) {
			b.origin = from_jolt(JPH::Vec3(lock.GetBody().GetPosition()));
		}
	}
	out.constraint_enabled = get<uint8_t>(constraint, constraint_at::kEnabled) != 0 ? 1 : 0;
	VehicleControllerState &c = out.controller;
	const size_t ctl = constraint_at::kController;
	c.forward_input = get<float>(constraint, ctl + constraint_at::kForward);
	c.right_input = get<float>(constraint, ctl + constraint_at::kRight);
	c.brake_input = get<float>(constraint, ctl + constraint_at::kBrake);
	c.hand_brake_input = get<float>(constraint, ctl + constraint_at::kHandBrake);
	c.previous_delta_time = get<float>(constraint, ctl + constraint_at::kPreviousDt);
	c.engine_rpm = get<float>(constraint, ctl + constraint_at::kRpm);
	c.gear = get<int32_t>(constraint, ctl + constraint_at::kGear);
	c.clutch = get<float>(constraint, ctl + constraint_at::kClutch);
	c.gear_switch_time_left = get<float>(constraint, ctl + constraint_at::kSwitch);
	c.clutch_release_time_left = get<float>(constraint, ctl + constraint_at::kRelease);
	c.gear_switch_latency_time_left = get<float>(constraint, ctl + constraint_at::kLatency);
	out.wheel_count = uint8_t(wheels);
	for (size_t i = 0; i < wheels; ++i) {
		const size_t at = constraint_at::kWheels + i * constraint_at::kWheelBytes;
		VehicleWheelState &ws = out.wheels[i];
		ws.angular_velocity = get<float>(constraint, at + constraint_at::kSpin);
		ws.angle = get<float>(constraint, at + constraint_at::kAngle);
		ws.contact_body = get<uint32_t>(constraint, at + constraint_at::kContactBody);
		get_bytes(constraint, at + constraint_at::kContactPosition, ws.contact_position.data(), 12);
		get_bytes(constraint, at + constraint_at::kContactNormal, ws.contact_normal.data(), 12);
		get_bytes(constraint, at + constraint_at::kContactLateral, ws.contact_lateral.data(), 12);
		ws.suspension_length = get<float>(constraint, at + constraint_at::kSuspension);
		ws.lambda_suspension = get<float>(constraint, at + constraint_at::kLambdaSuspension);
		ws.lambda_suspension_max_up = get<float>(constraint, at + constraint_at::kLambdaMaxUp);
		ws.lambda_longitudinal = get<float>(constraint, at + constraint_at::kLambdaLongitudinal);
		ws.lambda_lateral = get<float>(constraint, at + constraint_at::kLambdaLateral);
		const JPH::Wheel *wheel = v.constraint->GetWheel(JPH::uint(i));
		ws.steer_angle = wheel->GetSteerAngle();
		ws.in_contact = wheel->HasContact() ? 1 : 0;
	}
	const size_t tail = constraint_at::kWheels + wheels * constraint_at::kWheelBytes;
	get_bytes(constraint, tail, out.pitch_roll_axis.data(), 12);
	out.pitch_roll_lambda = get<float>(constraint, tail + 12);
	out.current_step = get<uint32_t>(constraint, tail + 16);
	out.driver = v.driver;
	return out;
}

VehicleContacts VehicleWorld::contacts(VehicleSlot p_slot) const {
	return p_slot < impl_->contacts.size() ? impl_->contacts[p_slot] : VehicleContacts();
}

void VehicleWorld::save(VehicleWorldSnapshot &r_snapshot) const {
	const Impl &w = *impl_;
	r_snapshot.tick = w.tick;
	r_snapshot.jolt.clear();
	ByteStateRecorder recorder(r_snapshot.jolt);
	w.system->SaveState(recorder, JPH::EStateRecorderState::All, &w.save_filter);
	r_snapshot.drivers.clear();
	for (const Impl::Vehicle &v : w.vehicles) {
		if (v.used) {
			r_snapshot.drivers.push_back(v.driver);
		}
	}
	r_snapshot.hash = snapshot_hash(r_snapshot.jolt, r_snapshot.drivers);
}

void VehicleWorld::save_section(VehicleSnapshotSection p_section, std::vector<uint8_t> &r_bytes) const {
	const Impl &w = *impl_;
	r_bytes.clear();
	ByteStateRecorder recorder(r_bytes);
	w.system->SaveState(recorder, jolt_section(p_section), &w.save_filter);
}

core::Status VehicleWorld::restore(const VehicleWorldSnapshot &p_snapshot) {
	Impl &w = *impl_;
	const std::vector<VehicleSlot> slots = w.used_slots();
	if (p_snapshot.drivers.size() != slots.size()) {
		return core::make_error("vehicle world restore: the snapshot has " + std::to_string(p_snapshot.drivers.size()) + " vehicles, the world " +
				std::to_string(slots.size()));
	}

	const std::vector<uint8_t> &j = p_snapshot.jolt;
	constexpr size_t kHead = 1 + 4 + 12;
	constexpr size_t kBodyRecord = 4 + kJoltBodyStreamBytes;
	if (j.size() < kHead + 4 + slots.size() * kBodyRecord || j[0] != uint8_t(JPH::EStateRecorderState::All)) {
		return core::make_error("vehicle world restore: the stream is " + std::to_string(j.size()) + " bytes, too short or not a full save");
	}
	uint32_t bodies = 0;
	std::memcpy(&bodies, j.data() + kHead, 4);
	if (bodies != slots.size()) {
		return core::make_error("vehicle world restore: the stream has " + std::to_string(bodies) + " bodies, the world " + std::to_string(slots.size()) +
				" vehicles");
	}
	for (size_t i = 0; i < slots.size(); ++i) {
		uint32_t id = 0;
		std::memcpy(&id, j.data() + kHead + 4 + i * kBodyRecord, 4);
		if (id != w.vehicles[slots[i]].body.GetIndexAndSequenceNumber()) {
			return core::make_error("vehicle world restore: body " + std::to_string(i) + " of the stream is not slot " + std::to_string(slots[i]) +
					"'s vehicle (the slot lists differ)");
		}
	}

	std::vector<uint8_t> backup;
	{
		ByteStateRecorder out(backup);
		w.system->SaveState(out, JPH::EStateRecorderState::All, &w.save_filter);
	}
	ByteStateRecorder in{ std::span<const uint8_t>(j) };
	const bool ok = w.system->RestoreState(in, &w.save_filter) && !in.IsFailed() && in.consumed() == j.size();
	if (!ok) {
		ByteStateRecorder back{ std::span<const uint8_t>(backup) };
		const bool restored = w.system->RestoreState(back, &w.save_filter) && !back.IsFailed();
		return core::make_error(std::string("vehicle world restore: Jolt refused the stream") + (restored ? "; the world is unchanged" : "; the backup failed too"));
	}
	for (size_t i = 0; i < slots.size(); ++i) {
		w.vehicles[slots[i]].driver = p_snapshot.drivers[i];
	}
	w.tick = p_snapshot.tick;
	return core::Status::success();
}

core::Status VehicleWorld::restore_vehicle_state(VehicleSlot p_slot, const VehicleState &p_state, uint32_t p_groups, ContactCachePolicy p_contacts) {
	Impl &w = *impl_;
	namespace g = vehicle_restore;
	const std::string where = "vehicle world restore_vehicle_state: slot " + std::to_string(p_slot);
	if (!has_vehicle(p_slot)) {
		return core::make_error(where + " has no vehicle");
	}
	Impl::Vehicle &v = w.vehicles[p_slot];
	const VehicleModel &m = *v.model;
	if (p_state.wheel_count != m.wheel_count || p_state.wheel_count != v.constraint->GetWheels().size()) {
		return core::make_error(where + ": " + std::to_string(p_state.wheel_count) + " wheels, the model has " + std::to_string(m.wheel_count));
	}
	if ((p_groups & ~g::kAll) != 0) {
		return core::make_error(where + ": unknown restore groups");
	}

	const VehicleBodyState &b = p_state.body;
	if ((p_groups & g::kBody) != 0) {
		if (!finite_array(b.com_position) || !finite_array(b.rotation) || !finite_array(b.linear_velocity) || !finite_array(b.angular_velocity) || b.active > 1) {
			return core::make_error(where + ": non-finite body state");
		}
		for (const float c : b.com_position) {
			if (std::fabs(c) > 1e6f) {
				return core::make_error(where + ": position outside 1e6 m");
			}
		}
		const double n = double(b.rotation[0]) * b.rotation[0] + double(b.rotation[1]) * b.rotation[1] + double(b.rotation[2]) * b.rotation[2] +
				double(b.rotation[3]) * b.rotation[3];
		if (!(n >= 1.0 - 1e-3 && n <= 1.0 + 1e-3)) {
			return core::make_error(where + ": rotation squared norm " + std::to_string(n) + " outside 1 +- 1e-3");
		}
	}
	if ((p_groups & g::kBodySleep) != 0) {
		bool ok = finite_array(b.force) && finite_array(b.torque) && std::isfinite(b.sleep_test_timer) && b.allow_sleeping <= 1;
		for (const VehicleSleepSphere &s : b.sleep_test_spheres) {
			ok = ok && finite_array(s.center) && std::isfinite(s.radius);
		}
		if (!ok) {
			return core::make_error(where + ": non-finite sleep state");
		}
	}
	const VehicleControllerState &c = p_state.controller;
	if ((p_groups & g::kController) != 0) {
		const float f[] = { c.previous_delta_time, c.engine_rpm, c.clutch, c.gear_switch_time_left, c.clutch_release_time_left, c.gear_switch_latency_time_left };
		if (!finite_floats(f, sizeof(f) / sizeof(f[0]))) {
			return core::make_error(where + ": non-finite controller state");
		}
		if (c.gear < -int32_t(m.reverse_gears) || c.gear > int32_t(m.forward_gears)) {
			return core::make_error(where + ": gear " + std::to_string(c.gear) + " outside the model's range");
		}
	}
	if ((p_groups & g::kControllerInputs) != 0) {
		const float f[] = { c.forward_input, c.right_input, c.brake_input, c.hand_brake_input };
		if (!finite_floats(f, 4)) {
			return core::make_error(where + ": non-finite controller inputs");
		}
	}
	for (size_t i = 0; i < p_state.wheel_count; ++i) {
		const VehicleWheelState &ws = p_state.wheels[i];
		if ((p_groups & g::kWheels) != 0) {
			const float f[] = { ws.angular_velocity, ws.angle, ws.suspension_length };
			if (!finite_floats(f, 3)) {
				return core::make_error(where + ": wheel " + std::to_string(i) + " non-finite");
			}
		}
		if ((p_groups & g::kWheelContacts) != 0) {
			if (!finite_array(ws.contact_position) || !finite_array(ws.contact_normal) || !finite_array(ws.contact_lateral)) {
				return core::make_error(where + ": wheel " + std::to_string(i) + " contact non-finite");
			}
			if (ws.contact_body != kNoContactBody) {
				const JPH::BodyID id(ws.contact_body);
				const JPH::BodyLockRead lock(w.system->GetBodyLockInterfaceNoLock(), id);
				if (!lock.Succeeded()) {
					return core::make_error(where + ": wheel " + std::to_string(i) + " contact body " + std::to_string(ws.contact_body) + " is not in this world");
				}
			}
		}
		if ((p_groups & g::kLambdas) != 0) {
			const float f[] = { ws.lambda_suspension, ws.lambda_suspension_max_up, ws.lambda_longitudinal, ws.lambda_lateral };
			if (!finite_floats(f, 4)) {
				return core::make_error(where + ": wheel " + std::to_string(i) + " lambdas non-finite");
			}
		}
	}
	if ((p_groups & g::kPitchRoll) != 0 && (!finite_array(p_state.pitch_roll_axis) || !std::isfinite(p_state.pitch_roll_lambda))) {
		return core::make_error(where + ": non-finite pitch-roll state");
	}
	if ((p_groups & g::kStep) != 0 && p_state.constraint_enabled > 1) {
		return core::make_error(where + ": constraint_enabled above 1");
	}
	const VehicleDriverState &d = p_state.driver;
	if ((p_groups & g::kDriver) != 0 &&
			(!std::isfinite(d.steer_filter) || !std::isfinite(d.throttle_filter) || !std::isfinite(d.brake_filter) || d.reversing > 1)) {
		return core::make_error(where + ": non-finite driver state");
	}

	std::vector<uint8_t> body;
	w.save_body_stream(p_slot, body);
	std::vector<uint8_t> constraint;
	w.save_constraint_stream(p_slot, constraint);
	const size_t wheels = p_state.wheel_count;
	if (body.size() != kJoltBodyStreamBytes || constraint.size() != jolt_vehicle_constraint_stream_bytes(wheels)) {
		return core::make_error(where + ": Jolt's streams are " + std::to_string(body.size()) + " and " + std::to_string(constraint.size()) +
				" bytes, expected " + std::to_string(kJoltBodyStreamBytes) + " and " + std::to_string(jolt_vehicle_constraint_stream_bytes(wheels)));
	}
	if ((p_groups & g::kBody) != 0) {
		put<uint8_t>(body, body_at::kActive, b.active);
		put_bytes(body, body_at::kPosition, b.com_position.data(), 12);
		put_bytes(body, body_at::kRotation, b.rotation.data(), 16);
		put_bytes(body, body_at::kLinear, b.linear_velocity.data(), 12);
		put_bytes(body, body_at::kAngular, b.angular_velocity.data(), 12);
	}
	if ((p_groups & g::kBodySleep) != 0) {
		put_bytes(body, body_at::kForce, b.force.data(), 12);
		put_bytes(body, body_at::kTorque, b.torque.data(), 12);
		put_bytes(body, body_at::kSpheres, b.sleep_test_spheres.data(), 48);
		put<float>(body, body_at::kTimer, b.sleep_test_timer);
		put<uint8_t>(body, body_at::kAllowSleeping, b.allow_sleeping);
	}
	const size_t ctl = constraint_at::kController;
	if ((p_groups & g::kControllerInputs) != 0) {
		put<float>(constraint, ctl + constraint_at::kForward, c.forward_input);
		put<float>(constraint, ctl + constraint_at::kRight, c.right_input);
		put<float>(constraint, ctl + constraint_at::kBrake, c.brake_input);
		put<float>(constraint, ctl + constraint_at::kHandBrake, c.hand_brake_input);
	}
	if ((p_groups & g::kController) != 0) {
		put<float>(constraint, ctl + constraint_at::kPreviousDt, c.previous_delta_time);
		put<float>(constraint, ctl + constraint_at::kRpm, c.engine_rpm);
		put<int32_t>(constraint, ctl + constraint_at::kGear, c.gear);
		put<float>(constraint, ctl + constraint_at::kClutch, c.clutch);
		put<float>(constraint, ctl + constraint_at::kSwitch, c.gear_switch_time_left);
		put<float>(constraint, ctl + constraint_at::kRelease, c.clutch_release_time_left);
		put<float>(constraint, ctl + constraint_at::kLatency, c.gear_switch_latency_time_left);
	}
	for (size_t i = 0; i < wheels; ++i) {
		const size_t at = constraint_at::kWheels + i * constraint_at::kWheelBytes;
		const VehicleWheelState &ws = p_state.wheels[i];
		if ((p_groups & g::kWheels) != 0) {
			put<float>(constraint, at + constraint_at::kSpin, ws.angular_velocity);
			put<float>(constraint, at + constraint_at::kAngle, ws.angle);
			put<float>(constraint, at + constraint_at::kSuspension, ws.suspension_length);
		}
		if ((p_groups & g::kWheelContacts) != 0) {
			put<uint32_t>(constraint, at + constraint_at::kContactBody, ws.contact_body);
			put_bytes(constraint, at + constraint_at::kContactPosition, ws.contact_position.data(), 12);
			put_bytes(constraint, at + constraint_at::kContactNormal, ws.contact_normal.data(), 12);
			put_bytes(constraint, at + constraint_at::kContactLateral, ws.contact_lateral.data(), 12);
		}
		if ((p_groups & g::kLambdas) != 0) {
			put<float>(constraint, at + constraint_at::kLambdaSuspension, ws.lambda_suspension);
			put<float>(constraint, at + constraint_at::kLambdaMaxUp, ws.lambda_suspension_max_up);
			put<float>(constraint, at + constraint_at::kLambdaLongitudinal, ws.lambda_longitudinal);
			put<float>(constraint, at + constraint_at::kLambdaLateral, ws.lambda_lateral);
		}
	}
	const size_t tail = constraint_at::kWheels + wheels * constraint_at::kWheelBytes;
	if ((p_groups & g::kPitchRoll) != 0) {
		put_bytes(constraint, tail, p_state.pitch_roll_axis.data(), 12);
		put<float>(constraint, tail + 12, p_state.pitch_roll_lambda);
	}
	if ((p_groups & g::kStep) != 0) {
		put<uint8_t>(constraint, constraint_at::kEnabled, p_state.constraint_enabled);
		put<uint32_t>(constraint, tail + 16, p_state.current_step);
	}
	{
		JPH::BodyLockWrite lock(w.system->GetBodyLockInterfaceNoLock(), v.body);
		if (!lock.Succeeded()) {
			return core::make_error(where + ": the chassis body is gone");
		}
		ByteStateRecorder in{ std::span<const uint8_t>(body) };
		w.system->RestoreBodyState(lock.GetBody(), in);
	}
	{
		ByteStateRecorder in{ std::span<const uint8_t>(constraint) };
		v.constraint->RestoreState(in);
	}
	if ((p_groups & g::kDriver) != 0) {
		v.driver = d;
	}
	if (p_contacts == ContactCachePolicy::Invalidate) {
		w.system->GetBodyInterfaceNoLock().InvalidateContactCache(v.body);
	}
	return core::Status::success();
}

uint64_t VehicleWorld::state_hash() const {
	VehicleWorldSnapshot s;
	save(s);
	return s.hash;
}

namespace {

void add_quad(std::vector<StaticTriangle> &r_out, const Vec3 &p_0, const Vec3 &p_1, const Vec3 &p_2, const Vec3 &p_3) {
	StaticTriangle a;
	a.v = { p_0, p_1, p_2 };
	StaticTriangle b;
	b.v = { p_0, p_2, p_3 };
	r_out.push_back(a);
	r_out.push_back(b);
}

void add_box(std::vector<StaticTriangle> &r_out, const Vec3 &p_min, const Vec3 &p_max) {
	const float x0 = p_min[0], y0 = p_min[1], z0 = p_min[2], x1 = p_max[0], y1 = p_max[1], z1 = p_max[2];
	add_quad(r_out, { x0, y1, z0 }, { x0, y1, z1 }, { x1, y1, z1 }, { x1, y1, z0 });
	add_quad(r_out, { x0, y0, z0 }, { x0, y1, z0 }, { x1, y1, z0 }, { x1, y0, z0 });
	add_quad(r_out, { x1, y0, z1 }, { x1, y1, z1 }, { x0, y1, z1 }, { x0, y0, z1 });
	add_quad(r_out, { x0, y0, z1 }, { x0, y1, z1 }, { x0, y1, z0 }, { x0, y0, z0 });
	add_quad(r_out, { x1, y0, z0 }, { x1, y1, z0 }, { x1, y1, z1 }, { x1, y0, z1 });
}

void add_ridge(std::vector<StaticTriangle> &r_out, float p_x0, float p_x1, float p_z0, float p_z1, float p_z2, float p_z3, float p_y, float p_h) {
	const float top = p_y + p_h;
	add_quad(r_out, { p_x0, p_y, p_z0 }, { p_x0, top, p_z1 }, { p_x1, top, p_z1 }, { p_x1, p_y, p_z0 });
	if (p_z2 > p_z1) {
		add_quad(r_out, { p_x0, top, p_z1 }, { p_x0, top, p_z2 }, { p_x1, top, p_z2 }, { p_x1, top, p_z1 });
	}
	add_quad(r_out, { p_x0, top, p_z2 }, { p_x0, p_y, p_z3 }, { p_x1, p_y, p_z3 }, { p_x1, top, p_z2 });
}

}

core::Result<std::shared_ptr<const StaticWorldShapes>> build_synthetic_vehicle_ground(const SyntheticVehicleGround &p_ground) {
	if (!std::isfinite(p_ground.half_size) || !std::isfinite(p_ground.y) || p_ground.half_size < 64.0f || p_ground.half_size > 8192.0f) {
		return core::make_error("synthetic vehicle ground: half size outside 64..8192 m");
	}
	std::vector<StaticTriangle> t;
	const float h = p_ground.half_size;
	const float y = p_ground.y;
	for (float x = -h; x < h; x += kStaticTileSize) {
		for (float z = -h; z < h; z += kStaticTileSize) {
			const float x1 = std::min(x + kStaticTileSize, h), z1 = std::min(z + kStaticTileSize, h);
			add_quad(t, { x, y, z }, { x, y, z1 }, { x1, y, z1 }, { x1, y, z });
		}
	}
	if (p_ground.features && h >= 450.0f) {
		namespace sg = synthetic_ground;

		add_box(t, { sg::kWallX0, y, sg::kWallZ }, { sg::kWallX1, y + sg::kWallHeight, sg::kWallZ + sg::kWallThickness });

		for (size_t i = 0; i < sg::kBumpCount; ++i) {
			const float z0 = sg::kBumpZ0 + float(i) * sg::kBumpSpacing;
			add_ridge(t, sg::kBumpLineX - sg::kBumpHalfWidth, sg::kBumpLineX + sg::kBumpHalfWidth, z0, z0 + 0.5f * sg::kBumpLength, z0 + 0.5f * sg::kBumpLength,
					z0 + sg::kBumpLength, y, sg::kBumpHeight);
		}
		add_ridge(t, sg::kBumpLineX - sg::kBumpHalfWidth, sg::kBumpLineX + sg::kBumpHalfWidth, sg::kKickerZ0, sg::kKickerZ0 + sg::kKickerRamp,
				sg::kKickerZ0 + sg::kKickerRamp + sg::kKickerTop, sg::kKickerZ0 + 2.0f * sg::kKickerRamp + sg::kKickerTop, y, sg::kKickerHeight);

		const float rise = (sg::kSlopeX1 - sg::kSlopeX0) * sg::kSlopeTan;
		add_quad(t, { sg::kSlopeX0, y, sg::kSlopeZ0 }, { sg::kSlopeX0, y, sg::kSlopeZ1 }, { sg::kSlopeX1, y + rise, sg::kSlopeZ1 },
				{ sg::kSlopeX1, y + rise, sg::kSlopeZ0 });
	}
	return build_static_world(std::span<const StaticTriangle>(t));
}

}
