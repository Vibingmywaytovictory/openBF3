# 04 — BF3 vehicles as playable, networked systems (Godot 4.8, C++, Jolt)

Written 2026-10-10. Scope: wheeled, tracked, helicopters, jets, boats, stationary and gadget vehicles as a C++ gameplay
system: physics route, per-family models mapped to BF3 data, combat and damage, seats/cameras/turrets/HUD/rendering/audio,
networking, class design and an ordered plan. The BF3 install is read in place and never modified; every value below was
read from the retail files through a read-only probe (no file in the install, in `C:\Users\olku\iw4L` or in the reference
folders was written).

**Tags.** **[verified]** = I read it myself in the retail data, in source code or in the API dump; **[report claim]** =
stated by a source I did not re-check; **[inferred]** = my reading. **Names** are the EBX type-descriptor spellings
(PascalCase, including Frostbite's typos such as `HorisontalMinEffectVelocity`, `LattitudeFrictionScale`,
`DisabledStrenghModifier`); `C:\Users\olku\VU-Docs\types\fb\<Type>.yaml` lists the same fields with a lower-case initial
(`MaxHealth` = `maxHealth`) [verified: `VehicleHealthZoneData.yaml:16-25`, `WheelConfigData.yaml:54,103`,
`PropellerEngineConfigData.yaml:28,41,103`]. Units: metres, seconds; Frostbite transforms taken verbatim (handoff §5).

**Evidence.** `S` = `C:\Users\olku\AppData\Local\Temp\claude\C--Users-olku-BF3-reimplementation\192b4390-9a90-436b-abe1-9117ba9cb3e5\scratchpad\vehicles`.
* `S\probe\src\main.rs`: a dumper built (target dir in `S`) against the IW4L crates `asset_transport`, `ebx_bf3`,
  `level_bf3` as path dependencies (read-only); it loads one level×mode and prints whole partitions with every field and
  reference, or every `VehicleSpawnReferenceObjectData` of the traversal.
* Dumps: `S\out\mp007_vehicles.txt` (MP_007 ConquestLarge0: AH-1Z, Mi-28, F/A-18, Su-35, M1A2, T-90, VDV, Growler, TOW2,
  Kornet, C-RAM, Pantsir, LAV-AD, Tunguska), `mp001_vehicles.txt` (Humvee, LAV-25, BMP-2, Vodnik), `mp017_vehicles.txt`
  (RHIB, AAV-7A1, Venom, C-RAM carrier), `xp1_vehicles.txt` (F-35B, DPV, skid loader), `xp3_vehicles.txt` (quad, AC-130,
  M1128, 2S25, Ka-60, BM-23), `xp5_vehicles.txt` (KLR650, Humvee ASRAD, Vodnik Phoenix), `mp007rush_vehicles.txt`
  (A-10, Su-25), `mp013_vehicles.txt` (Z-11W), `gadget_vehicles.txt` (M224, SOFLAM), `mp007_weapons.txt` + summary
  `mp007_weapons_sum.txt` (36 `vehicles/common/weapondata/*` partitions), `health_all.txt` (health/damage fields of 37
  vehicles), `mp007_grid_full.txt` + `mp007_matcontainer.txt` + `matnames.txt` (material grid), `spawns_all.tsv`
  (2,048 vehicle spawns over every level×mode).
* Godot source at the exact build: `S\godot-src` = github.com/godotengine/godot commit
  `c971f93e7e76b0ef919bf6009e7b868bea04db7f` (2026-09-29, the dev7 build's hash, resolved through the GitHub API), sparse
  checkout of `modules/jolt_physics`, `thirdparty/jolt_physics`, `scene/3d/physics`, `servers`, `main`. API facts from
  `C:\Users\olku\BF3 reimplementation\research\godot-api\extension_api.json` (header: `4.8.0 dev7 official`, single
  precision) [verified].

---

## 0. Decisions this report recommends (summary)

1. **Physics route: our own Jolt inside the C++ gameplay library, not Godot's PhysicsServer3D.** Link Jolt (pin the same
   5.6.0 commit Godot ships, or newer) built with `CROSS_PLATFORM_DETERMINISTIC`, own the `JPH::PhysicsSystem`s, step them on
   the server-tick thread, and use `JPH::VehicleConstraint` with Jolt's wheeled/tracked/motorcycle controllers first and a
   BF3-specific `JPH::VehicleController` subclass where fidelity needs it. Godot's physics server stays empty (Jolt or
   Dummy). Reasons: Godot exposes no way to step a space, snapshot/restore it, or reach Jolt's vehicle classes from
   GDExtension (§1.1–1.3); an engine module could, but costs a custom engine build per Godot release (§1.4).
2. **All gameplay collision lives in that one world** (static level, soldiers as `JPH::CharacterVirtual`, vehicles,
   projectile traces, destruction pieces); cosmetic debris in a client-only `PhysicsSystem` that shares the same immutable
   `JPH::Shape` objects, so static collision is held once. This is a cross-topic decision (soldier movement, destruction):
   soldiers and vehicles must collide in the same world.
3. **Vehicles are data-driven from `VehicleEntityData` and nothing else**: one generic vehicle entity whose components
   (chassis, wheels, tracks, engines, rotors, wings/flaps, float physics, turrets, seats, cameras, weapons, health zones)
   come from the blueprint's component tree. The TV missile, SOFLAM, MAV, EOD bot and M224 are vehicles in BF3's data too
   (§2.7, §3.2) [verified].
4. **Damage uses BF3's own tables**: `VehicleHealthZoneData` per zone, `DisabledDamageThreshold`/`DecayDelay`/`DecayRate`,
   `RegenerationDelay`/`RegenerationRate`, and the level `MaterialGridData` cell
   `[projectile PhysicsPropertyIndex][vehicle PhysicsMaterialIndex]` → `MaterialRelationDamageData.DamageProtectionMultiplier`
   (tank shell → MBT 0.9, rocket → MBT 2.0, small-arms `BulletDamage` → every vehicle 0) [verified data, formula inferred].
5. **Networking: server-authoritative, driver-predicted with rollback in a small client prediction world** (static shapes
   + own vehicle dynamic + others kinematic), reconcile on error > ε with visual smoothing; passengers/gunners ride the
   interpolated vehicle with locally predicted turret aim; lag compensation for hits on vehicles. Do not count on bit-exact
   replay as IW4L had; design for small corrections.
6. **Order:** Humvee (MP_001 Grand Bazaar) → seats/turret/HMG/damage → listen-server prediction → T-90/M1A2 → AH-1Z →
   F/A-18 → RHIB/amphibious → TOW/Kornet/C-RAM/Pantsir → bots in vehicles → remaining 49-blueprint roster (§6.2).

---

## 1. Physics route on Godot 4.8 + Jolt

### 1.1 What the 4.8 dev7 API exposes [verified in `extension_api.json`]

| need | exposed? | detail |
|---|---|---|
| per-body custom force integration | yes | `RigidBody3D._integrate_forces` (virtual), `set_use_custom_integrator`; `PhysicsServer3D.body_set_force_integration_callback(RID, Callable, Variant)`, `body_set_omit_force_integration`, `body_set_state_sync_callback`, `body_get_direct_state` |
| direct body state | yes | `PhysicsDirectBodyState3D`: velocities, `apply_force/impulse/torque`, `get_inverse_inertia_tensor`, contacts (`get_contact_*`), `get_step`, `get_space_state` |
| queries | yes | `PhysicsDirectSpaceState3D.intersect_ray/point/shape`, `cast_motion`, `collide_shape`, `get_rest_info`, plus allocation-free `*_into` variants with result objects `PhysicsIntersectRayResult3D`, `PhysicsCastMotionResult3D`, `PhysicsCollideShapeResult3D`, `PhysicsGetRestInfoResult3D`, `PhysicsIntersectShapeResult3D`, `PhysicsIntersectPointResult3D` |
| sweep a body | yes | `PhysicsServer3D.body_test_motion(RID, PhysicsTestMotionParameters3D, PhysicsTestMotionResult3D)` |
| per-body gravity scale, damping | yes | `BODY_PARAM_GRAVITY_SCALE`, `_LINEAR_DAMP`, `_ANGULAR_DAMP`, `BODY_PARAM_CENTER_OF_MASS`, `_INERTIA` |
| **step a space / the server manually** | **no** | `PhysicsServer3D` has no `step`, `space_step`, `flush_queries` methods; `_step`, `_sync`, `_flush_queries` exist only as virtuals of `PhysicsServer3DExtension` (for *implementing* a server) |
| **save/restore a space** | **no** | nothing comparable to Jolt `SaveState/RestoreState` |
| **Jolt classes** | **no** | no `Jolt*` class in the dump; Jolt is the internal implementation |
| VehicleBody3D / VehicleWheel3D | yes | `VehicleBody3D`: `set_engine_force/brake/steering` only; `VehicleWheel3D`: radius, suspension rest length/travel/stiffness/max force, damping compression/relaxation, traction/steering flags, `friction_slip`, `roll_influence`, `get_rpm`, `get_skidinfo` |
| physics tick | yes | `Engine.set_physics_ticks_per_second` (project default 60), `set_max_physics_steps_per_frame` (8), `get_physics_interpolation_fraction` |
| alternatives | yes | `PhysicsServer3DManager.register_server/set_default_server`; a `"Dummy"` 3D server is registered (`servers/register_server_types.cpp:373`) |

### 1.2 How Godot's Jolt integration steps (source at c971f93e7) [verified]

* Main loop order per physics tick: `PhysicsServer3D::sync()` → `flush_queries()` → scene-tree `physics_process` →
  `end_sync()` → `step(physics_step)` (`main/main.cpp:5017-5062`). Force-integration callbacks run inside
  `flush_queries` via `JoltSpace3D::call_queries` → `JoltBody3D::call_queries` (`jolt_physics_server_3d.cpp:1665-1673`,
  `objects/jolt_body_3d.cpp:1076-1093`), i.e. once per tick, before the next step.
* `JoltSpace3D::step` calls `physics_system->Update(p_step, 1, temp_allocator, job_system)` — one collision step per tick,
  whole space at once (`spaces/jolt_space_3d.cpp:190-222`). Velocity/position iterations from project settings (defaults
  10/2, `jolt_project_settings.cpp:36-37`).
* Godot sets Jolt's gravity to zero (`jolt_space_3d.cpp:138`) and adds gravity as a force per active body in `pre_step`,
  iterating `GetActiveBodiesUnsafe` (`jolt_space_3d.cpp:78-85`, `jolt_body_3d.cpp:147-166`); damping is applied by Godot
  before integration, not by Jolt (`jolt_body_3d.cpp:154-163`).
* Internals an engine module *could* reach: `JoltPhysicsServer3D::get_singleton()` (`jolt_physics_server_3d.h:126`),
  `get_space(RID)` (`:444`), `JoltSpace3D::get_physics_system()` (`spaces/jolt_space_3d.h:113`),
  `JoltObject3D::get_jolt_body()/get_jolt_id()` (`objects/jolt_object_3d.h:120-121`), `JoltShape3D::get_jolt_ref()`
  (`shapes/jolt_shape_3d.h:87`). All public, all C++-internal (not in the extension API).
* Jolt version 5.6.0 (`thirdparty/README.md:512-515`); **the vehicle sources are compiled into Godot**
  (`MotorcycleController`, `TrackedVehicleController`, `VehicleAntiRollBar`, `VehicleCollisionTester`, `VehicleConstraint`,
  `VehicleController`, `VehicleDifferential`, `VehicleEngine`, `VehicleTrack`, `VehicleTransmission`, `Wheel`,
  `WheeledVehicleController` — `modules/jolt_physics/SCsub:121-132`) but unreachable from GDExtension.
* SCsub defines only `JPH_ENABLE_ASSERTS` (dev builds), `JPH_DEBUG_RENDERER` (editor), `JPH_DOUBLE_PRECISION` (double
  builds) — **not `JPH_CROSS_PLATFORM_DETERMINISTIC`** (`SCsub:150-158`).
* `VehicleBody3D` is a port of Bullet's `btRaycastVehicle` (`scene/3d/physics/vehicle_body_3d.cpp:38`
  `btVehicleJacobianEntry`, one `intersect_ray` per wheel at `:457-485`, `_update_suspension` `:545`, `_update_friction`
  `:729`): one ray per wheel, linear spring + compression/relaxation damping, a single `friction_slip` coefficient,
  `roll_influence` hack, no engine/gearbox/torque curve/differential/tracks/anti-roll bars.

### 1.3 Jolt determinism and rollback (primary source) 

Jolt's `Docs/Architecture.md` "Deterministic Simulation": deterministic provided the state-modifying APIs are called in
exactly the same order and the same binary runs; cross-platform determinism needs the `CROSS_PLATFORM_DETERMINISTIC`
option (~8 % slower) plus precise FP settings (MSVC `/fp:precise`, clang `-ffp-contract=off`) and no standard trig;
listener callbacks run multithreaded (order not deterministic); broad-phase queries and `GetActiveBodies` order are not
deterministic; BodyIDs must match. "Rolling back a simulation": `SaveState` every frame, `RestoreState` frame N; it records
only state the step modifies, so bodies/constraints must not be added or removed in between, and BodyIDs must be kept
(https://github.com/jrouwe/JoltPhysics/blob/master/Docs/Architecture.md) [verified, read 2026-10-10]. In 5.6.0
`PhysicsSystem::SaveState(StateRecorder&, EStateRecorderState, const StateRecorderFilter*)` takes a filter
(`Jolt/Physics/PhysicsSystem.h:165-168`) and `VehicleConstraint`/`WheeledVehicleController` implement
`SaveState/RestoreState` (`VehicleConstraint.h:199-200`, `WheeledVehicleController.h:171-172`) [verified].

What Godot's integration does to these conditions [inferred from §1.2]: Godot's Jolt is not built cross-platform
deterministic; bodies are added through pending lists; gravity/forces are applied per active body in an unordered loop
(each body's own force sum, so order-independent); and above all the client and server *worlds differ* (different bodies,
different add order). So Godot-side Jolt gives at best same-binary determinism of an identical world, which a networked
client never has, and no way to snapshot/replay a space from an extension.

### 1.4 The four options compared

| criterion | (a) custom raycast vehicle over `PhysicsServer3D` (GDExtension) | (b) engine module into Godot's `JPH::PhysicsSystem` | (c) own Jolt linked in the GDExtension / gameplay library | (d) tuned `VehicleBody3D` |
|---|---|---|---|---|
| reach | rigid body in Godot's space; forces in the force-integration callback; wheel rays with `intersect_ray_into`, chassis sweeps with `body_test_motion` | `JPH::VehicleConstraint` on the `JoltBody3D`'s `JPH::Body`, added to the space's system; can also build a private `PhysicsSystem` sharing Godot's `JPH::Shape`s via `get_jolt_ref()` | everything Jolt has: `VehicleConstraint` (wheeled/tracked/motorcycle), custom `VehicleController` subclasses (all hooks virtual: `ConstructWheel`, `PreCollide`, `PostCollide`, `SolveLongitudinalAndLateralConstraints`, `SaveState`… `VehicleController.h:30-81`), collision testers ray/sphere/cylinder (`VehicleCollisionTester.h:84-140`), `SaveState/RestoreState`, several `PhysicsSystem`s sharing refcounted shapes | Bullet raycast vehicle |
| fidelity to BF3 data (Radius, Spring, gearbox, torque curve, Pacejka K, tracks) | anything you write; contact-rich behaviour (pushing, stacking) from Jolt | Jolt's models + subclasses | same as (b) | low: no torque curve, gears, diff, tracks, anti-roll, per-curve friction |
| replay for prediction | **no** (cannot step or restore a space; the vehicle's own state can be replayed only if the chassis is not a Jolt rigid body → becomes IW4L-style kinematic integrator) | yes, in a private system (engine-side) | **yes** | no |
| determinism | same-binary at best, worlds differ | same-binary (no `CROSS_PLATFORM_DETERMINISTIC` in Godot's build) | cross-platform with the define | none to speak of |
| threading / tick | stepped by Godot's main loop at `physics_ticks_per_second`; server tick tied to frames (the IW4L lesson says run it on its own thread) | same as (a) for Godot's space; own system can step anywhere | own thread, own rate, headless server possible without Godot | as (a) |
| static-world memory | once (Godot's space) | once (share shapes) | once if Godot's physics stays empty; ×2 if Godot physics is also fed the level | once |
| CPU, ~40 vehicles | per-wheel GDExtension calls (~320 ray queries per tick at 8 wheels/vehicle) — cheap but each crosses the binding | native | native; Jolt's `VehicleStressTest` sample drives 225 four-wheel ray-tested vehicles (`Samples/Tests/Vehicle/VehicleStressTest.cpp`, 15×15 loop) [verified via GitHub] | native but node per wheel |
| effort / risk | medium; fights Jolt for vehicle–vehicle contacts | **custom Godot build (editor, templates, server) for every 4.8.x/4.9 update**; breaks the official-build workflow | medium: CMake/SCons for Jolt in the extension, our own debug draw, static-world building in Jolt (we write the Havok→shape converter anyway) | low effort, dead end |

### 1.5 Recommendation: (c), our own Jolt as the gameplay physics world

* Build Jolt from source inside the `bf3` (Godot-independent) library with `CROSS_PLATFORM_DETERMINISTIC`, `/fp:precise`,
  same commit on every platform; pin it in the repo. The GDExtension links the library statically. Windows DLLs have their
  own symbol namespace, so Godot's internal Jolt and ours do not clash [inferred]; on Linux build with
  `-fvisibility=hidden` and a version script so the `.so` exports only the GDExtension entry point [inferred mitigation].
* One **`StaticWorld`** of immutable `JPH::Shape`s (terrain `HeightFieldShape`, per-object `MeshShape`/`ConvexHullShape`
  compounds, water volumes) built once per level (cached outside the install under `user://`, keyed by content digest).
  `PhysicsSystem`s: `ServerWorld` (authoritative), per local player a `PredictionWorld` (static bodies + the predicted
  vehicle dynamic + nearby entities kinematic), client `CosmeticWorld` (debris/bangers). Bodies are per system, shapes are
  shared (Jolt shapes are refcounted, `RefConst<Shape>`) [inferred from Jolt's design; verify memory with Caspian].
* Godot's `PhysicsServer3D`: unused. Keep Jolt selected (harmless when empty) or switch the project to `"Dummy"`
  [verified registered]. Navigation baking takes the collision triangles as source geometry, not physics bodies
  [inferred].
* Fallback if (c) is rejected: (a) with an IW4L-style kinematic vehicle integrator (`crates/vehicle_iw4`) that only
  *queries* Godot's Jolt — replayable and bit-exact, but vehicle-vs-world contacts are hand-written (probe spheres) and
  vehicles do not push debris.
* CPU budget [inferred]: 40 vehicles × ≤ 8 physics wheels (tracks: only `wptNormal` "Suspension" wheels, 3–5 per side) =
  ~320 ray/cylinder casts per 60 Hz step plus 40 constraint solves; Jolt's stress sample suggests well under 1 ms per step
  on one core. Measure on Caspian CQL (14 vehicle types, 39 spawns) in step 2 of the plan.

---

## 2. Per-family models mapped to BF3 data

### 2.0 Common chassis (`VehicleEntityData` → `ChassisComponentData`/`VehicleComponentData` → `VehicleConfigData`)

| BF3 field | value examples [verified] | use |
|---|---|---|
| `BodyMass` | Humvee/Vodnik 1500, buggies/quad/DPV 800, KLR650 250, RHIB 1000, skid loader 2500, tanks/IFV/AA/helis/stationary 5000, jets/AC-130 10000 | Jolt `MotionProperties` mass |
| `CenterOfMass`, `CenterOfMassHandlingOffset` | Humvee (0,0.6,0.2)/(0,0,0); T-90 (0,1,0)/(0,−0.42,−0.2); AH-1Z (0,−2,0)/(0,2,0); Mi-28 (0,−2.8,0)/(0,2.8,0) | CoM offset (`OffsetCenterOfMassShape`); "handling offset" semantics unknown (helicopters: cancels the CoM shift for handling forces?) [inferred] |
| `InertiaModifier` | Humvee (1.49,1.51,1.5), M1A2 (0.665,1.735,1.39), AH-1Z (1.19,2.325,1.17), F/A-18 (0.762,0.852,0.825) | scale the shape-derived inertia diagonal |
| `GravityModifier` | wheeled 1.85, buggies 1.65, tanks/IFV 1.6, quad 1.3, RHIB 1.25, KLR650 1.2, air/stationary 1.0, TV missile 0.1 | Jolt per-body `GravityFactor` (do **not** drop it as IW4L did) |
| `MotionDamping` → `MotionDampingData` {`Pitch`,`Yaw`,`Roll`,`Linear`,`LinearModifier`} | AH-1Z 0.2/0.08/0.2; jets 0.5/0.35/0.65; KLR650 0.99/0.5/1.9 | per-axis angular damping in body space [inferred] |
| `AeroDynamicPhysics` → `AeroDynamicPhysicsData.BodyDrag` (+ offsets) | helis (0.035,0.08,0.0042); jets (0.035,0.08,0.00375); TV missile (0.5,0.5,0) | per-axis quadratic drag in body space (x side, y vertical, z forward) [inferred] |
| `CoefficientOfAirFriction`, `AirDensity`, `AirDragArea` | 0.5 / 1.225 / 9 everywhere | ½ρC·A·v² ground-vehicle drag |
| `Stabilizer` → `StabilizerData` | M1A2/AAV `YawStrength 0.5, AdvancedYaw true`; Mi-28 has one | yaw stabilisation torque [inferred] |
| `ConstantForce[]` `ConstantForceData` | jets: `FCCriticalDamaged`, `FTTorque` (0,0,500000) `STBody` | spin a critically damaged jet (roll torque) |
| `Input` `VehicleInputData` | deadzones and `*InertiaIn/OutDuration`: Humvee yaw 0.23/0.85 s; jets throttle 3 s; AH-1Z throttle 1.2 s | input smoothing **inside the deterministic step** (part of predicted state) |
| `AntiRollBars {Front, Rear}` → `AntiRollBar {Stiffness, TorqueActivationLimit}` | Humvee 1.2 / 0.5; buggies present | Jolt `VehicleAntiRollBar` |
| `UseGearbox`, `UseTurnAroundForce`, `UseDownForce(WheelFactor)`, `DownForceBaseFactor` 8, `DownForceWheelFactor` 40, `UseMotorcycleControl`, `UseStandStillBrake/Sleep` | Humvee/buggies `UseTurnAroundForce true` | flags of the ground model |
| `FloatPhysics` → `BoatFloatPhysicsData` | RHIB, AAV, LAV-25, LAV-AD, BMP-2, Vodnik(s) | buoyancy (§2.5) |
| `MotorbikePhysics` → `MotorbikeData` | KLR650 | lean/wheelie (§2.6) |

`VehicleEntityData.UsePrediction = true` on every vehicle dumped [verified] — Frostbite predicted vehicles on the client
(semantics inferred). Spawn origin = spawn point; aircraft sit on their gear (AH-1Z origin 82 in above ground) (handoff
04 §4.1) [report claim].

### 2.1 Wheeled (Humvee, Vodnik, LAV-25, BTR-90, M1128, VDV buggy, Growler, DPV, quad, Humvee ASRAD, Vodnik Phoenix, XP4 Humvee/Vodnik/Van modified, BM-23, HIMARS)

Data per vehicle (wheel = `WheelComponentData.Config` → `WheelConfigData`; `Spring` = `SpringData {Length, Stiffness,
Damping}`; friction = `LongitudinalFrictionForceMaxFactor/LateralFrictionForceMaxFactor`; `DrivingType`/`SteeringType`)
[verified, `S\out\*_vehicles.txt` via `S\scripts\wheels.py`]:

| vehicle | engine `RpmCurvePoints` → `TorqueCurvePoints`, `RpmMax`, `EnginePowerMultiplier` | `ForwardGearRatios` / `ForwardGearSpeeds` / `TransmissionEfficiency` | wheels: `Radius`, Spring L/K/D, `Mass`, `DiffGearRatio`, friction lon/lat |
|---|---|---|---|
| HumveeArmored | 500–4500 → 250,450,400,285,185,85,30; 5000; 1 | 3,1.8,1.7,1.65,1.4 / 250,350,600,700,900 / 1 | 0.46, 0.4/0.5/0.06, 12–15 kg, 3.8, front 3.5/8, rear 5/7 |
| GAZ-3937 Vodnik | 285,400,350,200,120,85,30; 5000; 1 | as Humvee | 0.55, 0.35/0.66/0.06–0.085, 3.0, 10/8.5 and 3.5/8 |
| LAV-25 / LAV-AD / M1128 | 500,1100,1500,1750,2000 → 400,475,275,180,100; 1600; 5 | 1.85,1.65 / 30,100 / 1 | 0.5, 0.3/0.66/0.15, 50 kg, 2.0, 10/10 (8 wheels, front 4 steer) |
| VDV buggy | 250,500,425,285,185,85,30 (peak 1000 rpm); 5000; 1; `Boost.ForwardStrength 4` | 3,2.5,2,1.65,1.2 / 20,35,55,75,90 / 1.2 | 0.34, 0.35/0.62/0.06–0.1, 3.8/2.8, 3.5/5 and 5/7 |
| Growler ITV / DPV | 250,500,450,300,185,85,30; 5000 | 6,4.2,2,1.65,1.4 / 25,35,60,70,90 | 0.40 / 0.38, 0.35–0.4/0.62, 3.8 |
| QuadBike | 300,250,300,80 @ 500–3000; 3000; 2 | 3,1 / 30,200 | 0.315, 0.6/0.75/0.2 |
| STAR 1466 (BM-23) | 400,465,525,525,250; 1500; 5 | 1.85,1.65 / 30,100 | 0.57, 6 wheels |
| KLR650 | 1000–10000 → 20…48…20; 10000; 3.8 | 1.7,1.4,1.136,0.9,0.7 / 15,30,43,50,60 | 0.367, 0.4/0.72/0.3, 8 kg, 18.35, 200/400 front 600/450 rear |

Shared per-wheel fields seen on every ground vehicle: `LateralPosK 5.002381`, `LateralNegK 0.053606`,
`LongitudinalPosK 5.39649`, `LongitudinalNegK 0.176533`, `AlignMomScale 0.126552` (identical on Humvee and M1A2 — a
global Pacejka-like default), `BrakeForce 4000`, `RollingResistance{Internal}BaseFactor 15`,
`RollingResistanceVelocityFactor 150`, `EngineBrake*`, `TransmissionLoss` Vec2 curve (Humvee (0,0)(50,10)(60,20)(100,20)),
`SteeringSensitivity[]` `{SteeringSensitivity, Velocity}` (Humvee 0.25@0 → 0.12@66), `SlopeGripMin/MaxAngle` 40/90,
`*SlipCondition`, `FrictionMoment*`, `SphereCollision {ExtraRadius 1.2, Enabled false}`, `Spring.ProgressiveStartRatio`
0.35–0.4 and `ProgressiveExponent` 1.5, `AttachOffsetY` −0.22/−0.25, `DisabledStrenghModifier` 0.3/0.35 [verified].

Model [inferred]: `JPH::VehicleConstraint` + `WheeledVehicleController`, one `WheelSettingsWV` per `WheelComponentData`
with `PhysicsType wptNormal` (`wptFollow`/`wptStatic` are visual only). Mapping: `Radius`→`mRadius`, `Width`→`mWidth`,
`Spring.Length`→`mSuspensionMaxLength−mSuspensionMinLength`, `AttachOffsetY`→attachment point, `SteeringType 1`→
`mMaxSteerAngle` (from `SteeringSensitivity` × a max angle to tune), `DrivingType≠0`→differential membership, engine
curve → `VehicleEngineSettings.mNormalizedTorque` (`LinearCurve`, x = rpm/`RpmMax`) with `mMaxTorque` = peak ×
`EnginePowerMultiplier`, gears → `VehicleTransmissionSettings.mGearRatios`, `DiffGearRatio` → `VehicleDifferentialSettings`,
`BrakeForce`→`mMaxBrakeTorque` (× radius), friction max factors and the K terms → `mLongitudinalFriction`/`mLateralFriction`
curves (friction vs slip), `GravityModifier`→body gravity factor, anti-roll bars → `VehicleAntiRollBar`. Spring
`Stiffness`/`Damping` are dimensionless BF3 factors (IW4L: "raw BF3 factors", handoff 04 §4.1) — map to Jolt
`SpringSettings` (frequency, damping ratio) by fitting the static sag the IW4L sim found right (7/16 of travel at
Stiffness 0.5) [report claim → tune]. `ForwardGearSpeeds` semantics never understood (Humvee 250…900 vs tanks 30,100 vs
jets 45…130) — treat as shift thresholds to tune. If Jolt's tyre model cannot match BF3 feel, subclass
`WheeledVehicleController` (as Jolt's own `MotorcycleController` does) and implement BF3's force model there.

IW4L's measured feel to aim for: Humvee 68 km/h at 6 s, brake ~10 m/s², BF3-consistent top-speed hint 107 km/h (from the
torque curve) [report claim]. The VDV buggy fell through Caspian's terrain at the US base in several IW4L runs (never
diagnosed) — test every ground spawn against the heightfield in step 1 [report claim].

### 2.2 Tracked (M1A2, T-90, BMP-2, 2S25 Sprut-SD, Tunguska, AAV-7A1, skid loader)

Data [verified]: `TrackWheelComponentData` per road wheel (M1A2 18, T-90/BMP/Tunguska 16, 2S25 18, AAV 22, skid loader 4),
`SteeringType 2`, `TrackComponentData {Mesh (SkinnedMeshAsset), Skeleton, ScrollFactor}` (M1A2 −0.475, T-90 0.11),
`TrackedTurnSpeedLimit`/`TrackedForwardSpeedLimit` (M1A2 2/1.5). M1A2: `CombustionEngineConfigData` 285,300,285,225,100 @
500–2000, `RpmMax 1600`, `EnginePowerMultiplier 2.2`, `Boost.ForwardStrength 1.12, CrawlStrength 0.68,
AccelerationScale 1.5`; gearbox 1.5,1.3 / 30,100, efficiency 0.75; wheel `Radius 0.4`, spring 0.4/0.5/0.1, `Mass 35`,
`DiffGearRatio 4.88`, friction 10/2, `HandBrakeForce 4000`; `StabilizerData YawStrength 0.5`. T-90 (= BMP-2 and Tunguska
engine): 425,350,275,180,100, multiplier 2, gearbox 1.08,1.25 / 30,200, efficiency 0.85, radius 0.47; 2S25 multiplier 3.

Model [inferred]: `TrackedVehicleController` with two `VehicleTrackSettings` (one per `TrackComponentData`; `mWheels` =
its track wheels, `mDrivenWheel` = the sprocket), only `wptNormal` wheels as physics wheels; steering by `mLeftRatio`/
`mRightRatio`. IW4L's lesson: an honest track-force differential barely turned the hull because lateral tyre grip
cancelled it; a yaw-rate governor (target ±50°/s halved toward top speed, lateral grip −80 % while turning) gave T-90
65° and BMP-2 62° pivots in 2 s [report claim, handoff 04 §4.2] — reproduce with Jolt first, then with
`TrackedTurnSpeedLimit` as the governor's limit [inferred]. IW4L's top-speed hints came out M1A2 38 km/h vs T-90 53 km/h
although BF3 balances them — the gearbox/efficiency/`Boost` differences above explain part of it [inferred]; calibrate both
against reference measurements.

### 2.3 Helicopters (AH-1Z, Mi-28, UH-1Y Venom, Ka-60, AH-6J, Z-11W)

Data [verified, AH-1Z unless noted]: two `EngineComponentData` with `PropellerEngineConfigData` and a child
`RotorComponentData` each.
* Main: `PropellerType_Rotor`, `RotorConfig` → `RotorParameters`, `RpmCurvePoints [0,10000]`/`TorqueCurvePoints [0,50]`,
  `InternalAccelerationFactor 2.55`, `InternalDeaccelerationFactor 0.35`, `ForceMagnitudeMultiplier 2`,
  `LiftForceSpringConstant 40000`, `LiftForceDampingConstant 20000`, `CyclicInputScaleRoll/Pitch 0.2`,
  `CollectiveInputIdle 0.9`, `CollectiveThrottleInputScale 1.1`, `CollectiveBrakeInputScale 1.1`,
  `HorisontalMinEffectVelocity 20`, `HorisontalMinEffectMod 0.2`, `EnableNewHelicopter true`, `PitchStrength 20`,
  `PitchLimit 0.5`, `PitchFromVel 20`, `RollStrength 25`, `BankingStrength 40`, `BankingLimit 10`,
  `PitchUpWhenBankStrength 6`, `PitchUpWhenBankLimit 2`, `GravityMod 0.1`, `SPForward/Sideways/VerticalStrength 30/10/10`
  (`SPAllowed false`).
* `RotorParameters` (AH-1Z = Venom = Ka-60): `CyclicInputScaleRoll 0.18`, `CyclicInputScalePitch 0.85`,
  `CollectiveThrottleInputScale 2.5`, `CollectiveBrakeInputScale 2`, `CollectiveInputIdle 0.99`,
  `HorizontalForceModifier 1.2`, `AngleOfAttack` Vec2 curve (0,0.1)(10,0.25)(25,0.65)(75,1)(100,1.8)(140,3.5)(200,3.5),
  `CyclicFadeOutOffset 0.25`, `AdditionalGravityModifier 8`. Z-11W (scout): roll 0.25, pitch 1, collective 1.12,
  `AngleOfAttack` (1.94,0.1)(25,0.5)(50,1.5)(80,2)(120,2), `MotionDamping` linear 1.5.
* Tail: `PropellerType_Regular`, `ForceMagnitudeInputType FMITYaw`, `ForceMagnitudeMultiplier 75`, torque [0,500],
  engine component at (0.011,0.677,−9.089).
* `RotorComponentData`: main `LowRpmModel {PartIndex 10}`, `HighRpmModel {PartIndex 11}`, `RotationAxis raY`,
  `RotationMultiplier 0.008`, `ChangeModelRpm 10000`, `TriggerGroundEffectHeight 80`, `BlowEffect null`; tail parts
  15/16, `raX`, `HighRpmModel.RotationRpm 1500`, multiplier 0.1, `ChangeModelRpm 1000`, `CriticallyDamagedRotationForce −12`.
  Main engine at (0,0.347,0), rotor at (0,−0.347,0) in it (hub at the vehicle origin).
* Chassis: `BodyMass 5000`, `GravityModifier 1`, `GearboxConfigData` neutral-only, 4–5 skid "wheels" (`DrivingType 0`,
  `Radius 0.2–0.4`, friction 100/200), 3 `WingComponentData` (`WingPhysicsData` Lift 5.3/18/5.3, Drag 2.65/10/0).

Model [inferred]: (1) rotor rpm = first-order spool toward `CollectiveInputIdle`-scaled target using
`InternalAcceleration/DeaccelerationFactor`; (2) lift along body up = spring-damper on the commanded vertical speed
(`LiftForceSpringConstant`/`DampingConstant`) — this matches IW4L's working "collective commands a climb rate"
(`collectiveInputIdle 0.99` < 1 explains BF3's slight sink with the collective released, which IW4L noted) ; (3) cyclic
pitch/roll torques from `CyclicInputScale*` × `PitchStrength`/`RollStrength`, auto-levelling within
`PitchLimit`/`BankingLimit`, `PitchFromVel`; (4) horizontal force from tilt × `HorizontalForceModifier`, with
`AngleOfAttack` (x likely speed in km/h, y a multiplier) shaping it; (5) tail rotor yaw = `ForceMagnitudeMultiplier` ×
yaw input; (6) wings add weathervaning; (7) `MotionDamping` + `BodyDrag`. Start from IW4L's tuned controller (spool 3 s,
climb +15/−18 m/s, pitch 90°/s, roll 120°/s, pedals 70°/s, weathervane above 7.6 m/s, 159 km/h at 25° nose-down, 209 km/h
at 36°, dive 250 km/h, 35° bank turns 12°/s; `7-GAPS-PART/aircraft.md` "Validation evidence") [report claim, offline probe
only, never playtested], then swap its constants for the BF3 fields one at a time against reference flights. Rotor hub =
`RotorComponentData` transform, radius from the blade part bounds — not the model half-width IW4L used [verified data].
Rotor strike: BF3 data has no instant-kill rule (IW4L invented one); use rotor-disc contact as a hull impact through
`VelocityDamageThreshold 20` / `VelocityDamageMagnifier 5` [inferred]. Helicopters: health 500/zone (Venom/Ka-60 750),
`VehicleLockableInfo HeatSignature 10, LowAltitudeHeight 25` (§3.2).

### 2.4 Jets (F/A-18E, Su-35BM, F-35B, A-10, Su-25TM; `*_SpawnInAir` for Air Superiority; AC-130/C-130 gunships)

Data [verified, F/A-18 unless noted]: `JetEngineConfigData {RpmCurvePoints [0,250,500,750,1000], TorqueCurvePoints
[0,5,15,50,150], RpmMax 1000, EnginePowerMultiplier 1.4, InternalAccelerationFactor 0.65, InternalDeaccelerationFactor
0.5, Boost {ForwardStrength 1.5 (A-10/Su-25 1.0), DissipationTime 9999, RecoveryTime 1, CrawlStrength 0.78},
DirectionVectorIndex 2, ForceMagnitudeMultiplier 270, MaxVelocity 0, IsTurnable false, PowerFadeOutRange (1000,1500)
(A-10/Su-25 (1e6,1e6)), Position (0,0,−5)}`; `BodyMass 10000`; `VehicleInputData` deadzones 0.2 and throttle inertia 3 s;
`MotionDampingData` 0.5/0.35/0.65; `AeroDynamicPhysicsData.BodyDrag (0.035,0.08,0.00375)`; 5 `WingComponentData` and 12
(Su-35 6) `FlapComponentData {RotationAxis RALeft|RAUp, RotationScale}`:

| `WingPhysicsData` | Lift | FlapLift | Drag/FlapDrag | `BaseAngleOfAttack` | `InputForFlap` | `LiftCoefficient` (Curve2D, x = AoA °) | `MaxFlapAngleScaleFactor` (Curve2D) |
|---|---|---|---|---|---|---|---|
| #531 main | 20 | 0 | 1/0 | 0 | 0 | (−28,−10)…(−0.75,0)(0.75,0)…(28,10) | — |
| #530 | 0 | 4 | 1/0.4 | 0 | 0 | peak (24,6), (48,5), (72,0) — stall shape | (0,1)(250,1.75)(305,1.7)(320,1.05)(350,0.9)(400,0.52)(500,0.4) |
| #529 | 0 | 4.2 | 1/0.2 | 0 | 1 | (−28,−5)…(28,5) | (0,1)(250,1.85)(300,0.5)…(800,0.25) |
| #527/#528 | 25 | ∓2.5 | 2.5/0.4 | 3 | 2 | peak ±(18,9), (72,0) | (0,1)(200,1.25)(250,1.2)(285,0.85)(325,0.65)(400,0.35) |

`LandingGearLogicData`: retract at height 5 / velocity 251, deploy at height 20 / velocity 250 / angle 35. F-35B adds
`RotorParameters` (`ReverseThrottle true`, `CollectiveInputIdle 0.6`, `EnableHorisontalMinEffect true`,
`HorisontalMinEffectVelocity 200`) and two `PropellerEngineConfigData` (the VTOL mode). Health 500/zone, disabled at 170,
regeneration 18/s after 10 s, `VelocityDamageThreshold 50`, `VelocityDamageMagnifier 100`, `WaterDamage 250`.

Model [inferred]: thrust = `ForceMagnitudeMultiplier` × engine torque(rpm) × `EnginePowerMultiplier` along body forward,
afterburner = `Boost.ForwardStrength` while boosting (no depletion: `DissipationTime 9999`), idle = `CrawlStrength`,
thrust faded over `PowerFadeOutRange` (altitude ceiling, by its 1000–1500 values; A-10/Su-25 effectively off);
per wing: lift = Lift × `LiftCoefficient`(AoA) × v², flap lift = FlapLift × input(`InputForFlap` 0 pitch, 1 yaw,
2 roll — from the RAUp rudder flaps and the ± aileron pair) × `MaxFlapAngleScaleFactor`(speed). **The control-authority
curves peak at 250–305 and collapse by 320** (x read as km/h) — this *is* BF3's famous turning sweet spot that players put at
310–315 km/h (community: forums.anandtech.com BF3 thread; EA's BF4 jet tips name 315) [report claim]; the landing-gear
velocities 250/251 support km/h units [inferred]. Stall = AoA beyond the `LiftCoefficient` peak (24°) at low speed; there
is no explicit stall-speed field [verified absence in the five wings]. The units of `MaxVelocity` and the speed axis must be
checked by flying in VU/BF3 and comparing the HUD speed with the curve knee.

AC-130/C-130 (`UIHudIcon_Gunship`, `BodyMass 10000`, 4 rotors, no wheels, health 2000, AC-130 `VelocityDamageThreshold
600`) fly scripted orbits in BF3 [report claim]; treat as path-following vehicles with gunner seats.

### 2.5 Boats and amphibious (RHIB, RHIB_XP3; amphibious AAV-7A1, LAV-25/LAV-AD, BMP-2, Vodnik, Vodnik Phoenix)

`BoatFloatPhysicsData` [verified]:

| field | RHIB | AAV-7A1 | LAV-25 | BMP-2 | Vodnik |
|---|---|---|---|---|---|
| `Density` / `FilledDensity` | 0.5 / 1.2 | 0.5 / 1.2 | same | same | same |
| `WaterResistanceAxisMod` | (1,1,2) | (1,1,1) | (1,1,1) | (1,1,1) | (1,1,1) |
| `WaterFrictionAxisMod` | (8,1,0.58) | (8,1,0.25) | (8,1,0.25) | (8,1,0.25) | (8,1,0.25) |
| `SubSurfaceSplits`, `Depth` | 4, 0 | 10, 1.5 | 10, 1.5 | 10, 1 | 10, 1.5 |
| `FrontCurveDegree`/`SideCurveDegree` | 3.2/2.8 | 3/2 | 3/2 | 3/2 | 3/2 |
| `NonEngineSteer`, `WaterDampeningMod`, `LiftModifier`, `AngularDampening`, `FrontRatio` | 25, 1.65, 0.2, 0.4, 0.5 | 3.5, 2, 0.35, 0.01, 0.6 | 3.5, 2, 0.5, 0.01, 0.6 | 3.5, 2, 0.9, 0.01, 0.6 | 3.5, 8, 0.5, 0.01, 0.3 |

Propulsion: `JetEngineConfigData {IsWaterJetEngine true, IsTurnable true, AngleInputYMultiplier −25 (RHIB) / −11 (LAV,
AAV) / −8 (BMP) / −5 (Vodnik), Position ≈(0,0.2,−2.5), RHIB ForceMagnitudeMultiplier 10 EnginePowerMultiplier 0.65,
amphibians MaxVelocity 32 EnginePowerMultiplier 7–17}`, RHIB gearbox 5,3.2,2.3 / 20,60,90 [verified]. RHIB health
350/zone, `GravityModifier 1.25`.

Model [inferred]: sample the hull box in a `SubSurfaceSplits`² grid, buoyancy per sample = submerged volume × water density
× g / `Density`-ratio, hull shape from `Front/SideCurveDegree`, anisotropic drag `WaterFrictionAxisMod` (strong lateral,
weak forward), planing lift `LiftModifier` × v², steering by turning the water-jet thrust vector by
`AngleInputYMultiplier` degrees; `FilledDensity 1.2` > 1 when wrecked → sinks. Water surface = the level's
`WaterEntityData` triangle mesh (handoff 04 §3.1) as a Jolt sensor plus a height query; implement as a
`JPH::PhysicsStepListener` that applies forces (Jolt has a `Body::ApplyBuoyancyImpulse` helper usable per sample)
[inferred]. Amphibious vehicles switch from wheel/track drive to water-jet when the engine is submerged
(`EngineComponentData.OutputIsEngineInWater`) [inferred].

### 2.6 Motorbike, quad, DPV, skid loader

KLR650: `MotorbikeData` (wheelieForce, kickstandRoll, leanForceMaxVel, counterLeanForce, jumpForwardLean…, VU-Docs
`MotorbikeData.yaml`), `BodyMass 250`, `InertiaModifier (1.88,2.5,1.6)`, `MotionDampingData Roll 1.9 Pitch 0.99`, 2 wheels
`DiffGearRatio 18.35`, health 600/zone, regeneration 50/s [verified]. Model: Jolt `MotorcycleController` (lean angle
control) with `MotorbikeData` mapped onto its lean/wheelie settings [inferred]. Quad/DPV: wheeled, 800 kg, quad
`MotionDampingData Linear 0.1`. Skid loader: tracked controller, 4 track wheels, no weapon [verified].

### 2.7 Stationary and gadget vehicles

| vehicle | data [verified] | model |
|---|---|---|
| TOW2 / Kornet (`UIHudIcon_StaticAT`) | chassis only, `BodyMass 5000`, health 150/zone, no regeneration (`RegenerationRate 0`), `UseAsTeamSpawner true`, yaw ring `ChildRotationBodyData` unconstrained, pitch −20…20°, weapon `stationary_atgm_firing` with `CustomWeaponType` → `LockingWeaponData {IsGuided true, IsHoming false}` (wire guidance) | static body, turret + guided missile (§3.2) |
| Centurion C-RAM, Pantsir-S1 (+ `_Carrier`, trailers) | health 1000/zone with **`ArmorMultiplier 99999`**, C-RAM `IsLockable false`, `Explosion null`, `PropellerEngineConfigData` present (turret drive) | effectively indestructible AA turret |
| M224 mortar (`weapons/gadgets/m224/m224_deployed`) | a `VehicleBlueprint`: `ArtilleryStrikeWeaponData`, two firing functions (HE, smoke) 200 m/s, reload 4 s, projectile `MissileEntityData Damage 400`, blast 60/r5 | gadget vehicle placed by the soldier |
| SOFLAM (`weapons/gadgets/soflam/soflam_projectile`) | a vehicle with `RemoteEntryComponentData`, `LaserPainterData` (`laserpainter_soflam`: `IsAlwaysActive true`, `LockAlways`, `LockTime 2`, `RayLength 500`, `AcceptanceAngle 15`, `TimeObjectIsPainted 0.5`), health 50 | remote-controlled designator |
| MAV, EOD bot, T-UGS, radio beacon | `VehicleBlueprint`s in every MP level's registry (`4-INTEGRATION-PART/vehicles-assets.md` §4.4) [report claim] | same framework |

### 2.8 Full MP roster: 49 blueprints, 2,048 spawn placements over 119 level×mode combinations [verified, `S\out\spawns_all.tsv`]

Modes: CQL/CQS ConquestLarge0/Small0, RL RushLarge0, SR SquadRush0, SQDM, TDM/TDMC, CAL/CAS/CAS1 ConquestAssault*, R0
Rush0, TS TankSuperiority0, AS AirSuperiority0, CTF. `xN` = N spawners in that mode. No vehicles on MP_Subway and all XP2
maps. (Supersedes the handoff's "XP4/XP5 spawners not found": the traversal of the current `level_bf3` finds them.)

| blueprint (`Vehicles/…`) | kind | spawns | levels (modes) |
|---|---|---|---|
| HumveeArmored | jeep | 30 | MP_001 (CQL×2,CQS) MP_003 (CQL×3,CQS×2) MP_007 (RL×6) MP_011 (CQL) MP_013 (CQL×3,CQS×2,RL×4) MP_017 (RL×4) MP_018 (RL×2) |
| GAZ-3937_Vodnik | jeep | 22 | MP_001 (CQL×2,CQS) MP_003 (CQL×3,CQS×2) MP_011 (CQL) MP_013 (CQL×3,CQS×2) MP_017 (CQS×3,RL×5) |
| VDV_Buggy | buggy | 90 | MP_007, MP_012, MP_017, MP_018, XP1_001…004, XP3 all four (CQ/CA modes) |
| GrowlerITV | buggy | 78 | MP_003 (RL), MP_007, MP_012, MP_017, MP_018, XP1_001, XP1_003, XP3 all four |
| XPack01/DPV | buggy | 47 | XP1_002, XP1_004 |
| XP3/QuadBike | ATV | 95 | XP3 all four (CQ, RL) |
| XP5/KLR650 | dirt bike | 226 | XP5 all four (CQ, CTF, RL) |
| XP5/Humvee_ASRAD, XP5/VodnikPhoenix | jeep (AA / armed) | 19, 12 | XP5 all four |
| XP4/HumveeModified, VodnikModified_V2, VanModified | jeep/van | 22, 14, 15 | XP4_FD, XP4_Parl, XP4_Quake, XP4_Rubble |
| XPack01/SkidLoader | utility (tracked) | 5 | XP1_002, XP1_003, XP1_004 |
| LAV25 / XP5 LAV25_Paradrop | IFV | 35 / 17 | MP_001, MP_003, MP_011 (RL), MP_017, MP_018 (RL), XP1_001, XP1_002, XP4_FD, XP4_Parl, XP5_001/004 (RL) / XP5 all four |
| BMP2 / XP5 BMP2_Paradrop | IFV | 110 / 17 | CQ on MP_001, MP_003, MP_017, XP4_FD, XP4_Parl; RL on XP5_001/004; **SQDM×4 on 24 levels** / XP5 all four |
| XPack01/BTR-90 | IFV | 9 | XP1_001 (CAL), XP1_002, XP1_003 (CAL) |
| AAV-7A1 | amphibious APC | 37 | MP_003 (RL), MP_017, MP_018 (RL), XP1_002 (Rush), XP1_004, XP3_Alborz/Desert (RL) |
| M1A2/M1Abrams, T90 | MBT | 147, 153 | 21 levels each: all base/XP1/XP3/XP5 vehicle maps plus XP4_FD and XP4_Parl (M1A2 not on MP_017, T-90 not on MP_001; TS×2 each on all XP3) |
| XP3/M1128-Stryker, XP3/2S25_SPRUT-SD | tank destroyer | 91, 70 | XP3 all four (CQ, RL, TS×6) |
| LAV25/LAV_AD, 9K22_Tunguska_M | mobile AA | 13, 24 | MP_007, MP_012, MP_018, XP1_004, XP3 all four (Tunguska also MP_017 RL) |
| XP3/HIMARS, XP3/STAR_1466 (BM-23) | artillery | 15, 13 | XP3 all four |
| AH1Z, Mi28 | attack heli | 37, 37 | MP_007, MP_012, MP_017, MP_018, XP1_002…004, XP3 all four, XP5 all four |
| AH6/AH6_Littlebird, Z11W | scout heli | 27, 24 | MP_013, MP_017, MP_018, XP1_003, XP3_Desert/Shield/Valley, XP4_FD, XP5 all four |
| Venom, KA-60_Kasatka | transport heli | 11, 3 | Venom: MP_007 (RL), MP_012, MP_013 (RL), MP_017, XP1_002, XP1_004, XP3_Alborz; Ka-60: MP_012 (CQL,CQS), XP3_Alborz (CQL) |
| F18-F/F18, SU-35BM-E | fighter | 25, 35 | MP_007, MP_012, MP_017, MP_018, XP3_Alborz/Desert/Valley (Su-35 also XP1_002, XP1_004) |
| XPack01/F35B | fighter (VTOL) | 10 | XP1_002, XP1_004 |
| F18_SpawnInAir, SU-35BM-E_SpawnInAir | fighter | 72, 72 | XP5 AS×12 on all four; XP5_002/004 CQ×6 |
| A-10_THUNDERBOLT, SU-25TM | attack jet | 6, 6 | RushLarge0 only: MP_007, MP_012, MP_018, XP3_Alborz/Desert/Valley |
| XP3/ac130, XP5/C130 | gunship | 20, 16 | XP3 all four (CQ, RL); XP5 all four (CQ) |
| RHIB, RHIB_XP3 | boat | 45, 4 | MP_017, MP_018, XP1_002…004, XP3_Desert (RL); XP3_Shield |
| TOW2, Kornet | static AT | 43, 92 | widespread (CQ, RL, CTF) |
| Centurion_C-RAM (+ `_Carrier`), Pantsir-S1 | static AA | 11+10, 16 | MP_007, MP_012 (all modes incl. TDM), MP_013, MP_017, MP_018, XP1_002, XP1_004, XP3_Desert |

---

## 3. Vehicle combat

### 3.1 Weapon pipeline and numbers

Chain [verified]: `WeaponComponentData {ProjectileSpawnOffset, WeaponFiring → WeaponFiringData, DamageGiverName,
CustomWeaponType (LockingWeaponData), SequentialFiring, ReloadTimeMultiplier, DamageMultiplier, ExplosionDamageMultiplier,
OverheatDropPerSecondMultiplier, LockTimeMultiplier, LockingAcceptanceAngleMultiplier}` → `WeaponFiringData {PrimaryFire →
FiringFunctionData, DeployTime, ReactivateCooldownTime, AltDeployTime}` → `FiringFunctionData {Dispersion[]
FiringDispersionData {MinAngle, MaxAngle, IncreasePerShot, DecreasePerSecond}, FireEffects1p/3p, Sound, Shot
ShotConfigData, FireLogic FireLogicData, Ammo AmmoConfigData, OverHeat OverHeatData}`. **The effective muzzle velocity is
`ShotConfigData.InitialSpeed.z`, not the projectile's `InitialSpeed`** (MBT cannon: shot 200 m/s, `BulletEntityData.
InitialSpeed 350`); fun-bots' hand-measured table uses 200 for the M1A2 cannon and 600 for its HMG, agreeing
(`C:\Users\olku\unfun-bots\ext\Shared\Constants\VehicleData.lua`, `M1Abrams.Speed`) [verified both].

| `vehicles/common/weapondata/…` | shot m/s | fire logic | ammo / reload | heat | projectile | blast |
|---|---|---|---|---|---|---|
| `mbt_cannon_firing` | 200, g 9.81 | single, 120 rpm | mag 1, reload 3 s | — | Bullet `StartDamage 300` | 100 dmg, r 5, inner 1.5 |
| `mbt_canister_firing` | 450 | 16 pellets/shell, dispersion 1° | 1, 3 s | — | 50 dmg, falloff 25→50 m | — |
| `tank_remote_hmg_firing` | 600 | auto 550 rpm, dispersion 0.3→1° +0.4/shot | unlimited | +0.075/shot, −0.5/s, penalty 1.2 s | 25→16.7 over 8→125 m | — |
| `heli_autocannon_firing`, `heli_gunnercannon_firing` | 600, g 0 | auto 650 rpm | 30, 2 s | — | 10 + blast | 25, r 3 |
| `jet_cannon_firing` | 900, g 0 | auto 2000 rpm (`ClientFireRateMultiplier 0.2`) | unlimited | +0.018/shot, −0.3/s | 15, TTL 0.67 s | — |
| `vulcan_cannon_firing` | 900 | auto 2000 rpm | unlimited | +0.014/shot | 10, TTL 1.2 s | — |
| `rocketpods_firing` | 300 | auto 300 rpm | 14, 4 s | — | Missile `EngineStrength 300`, TTL 3, g −15, `Damage 30` | 20, r 5 |
| `heatseeking_missile_firing` (+ `aim9_sidewinder_projectile`) | 50 | single, `DeployTime 2` | 1, 8 s | — | `EngineStrength 35`, `MaxSpeed 200`, guidance after 0.34 s, `MaxTurnAngle 155`, `Damage 225`, near-target detonation 12.5 m | 0 (r 4.5) |
| `hellfire_missile_firing` (+ `hellfire_projectile`) | 16 | 60 rpm | 1, 12 s | — | `MaxSpeed 250`, guidance after 1.75 s, arm 0.5 s | 350, r 4.5 |
| `mbt_laserguidedmissile_us/_ru` (guided shell) | — | — | — | — | `EngineStrength 15`, `MaxSpeed 100`, ignition 0.5 s | 350, r 4.5 |
| `tv_missile_firing` | 90 | single | 1, 15 s | — | `VehicleProjectileEntityData` TTL 7 → vehicle `agm-144_hellfire_tv` | 400, r 4.5 |
| `jet_guidedmissile_firing` (+ `maverick_projectile`) | 50 | single, `DeployTime 1.3` | 1, 12 s | — | `MaxSpeed 250`, guidance after 0.5 s | 350, r 4.5 |
| `stationary_atgm_firing` (+ `ifv_atgm_projectile`/`kornet_projectile`) | 20 | single | 1, 7.5 s | — | `EngineStrength 25`, `MaxSpeed 50`, `MaxTurnAngle 45`, `Damage 500` | 100, r 1.2 |
| `heli_flares_firing` / `jet_flare_firing` | 10 | 6 flares per shell (jet 3-shot burst), dispersion 20°/10° | 1, **11 s** | — | `flare_projectile` TTL 3.5, `LockableInfo RadarSignature 100` | — |
| `ecm_trigger_firing` | — | single | 1, **11 s** | — | trigger bullet | — |
| `tank_smoke_firing` | — | single | 1, **10 s** | — | trigger bullet | shockwave r 16 |
| `fire_extinguisher_trigger_firing` | — | single | 1, **24 s** | — | trigger bullet | — |

Overheat model [inferred]: heat += `HeatPerBullet` per shot, −`HeatDropPerSecond` × `OverheatDropPerSecondMultiplier`
per second; at ≥ 1 locked for `OverHeatPenaltyTime`; `OverHeatThreshold` = HUD warning level. Unlimited ammo is
`MagazineCapacity −1`. All of this is per-seat weapon state replicated to the seat's occupant only.

### 3.2 Locking, guidance, TV missile, Javelin vs SOFLAM

* `LockingWeaponData {IsHoming, IsGuided, IsGuidedWhenZoomed, FireOnlyWhenLockedOn, LockingController,
  SecondaryLockingController, WarnLock}`; `LockingControllerData {ZoomLevelLock[] {LockType LockOnHeat|LockOnRadar|
  LockOnLaserPainted|LockAlways}, LockTime, ReleaseTime, ReleaseOnNewTargetTime, SampleRate 0.06, RayLength,
  AcceptanceAngle, AngleConstant, DistanceConstant, Sensitivity, LockOnVisibleTargetsOnly, LockOnEmptyVehicles,
  PositionOnly, LockOnWorldSpacePos}` [verified]. Heli heat-seeker lock: 1.25 s, 550 m, 10°; jet: 1.25 s, 650 m, 7.5°;
  land AA: 1.25 s, 650 m, 10°; heli Hellfire: laser-painted lock 1.25 s/700 m/5° with a radar fallback 1.75 s/400 m;
  guided shell: laser 1.25 s/400 m/7°, radar fallback 2.25 s/300 m; Maverick: `LockAlways`, `PositionOnly`,
  `LockOnWorldSpacePos`, 2,000 m (fires at a ground point) [verified].
* Targets: `VehicleEntityData.VehicleLockableInfo {HeatSignature, RadarSignature, LowAltitudeHeight,
  LowAltitudeHeatSignature}` — air 10/1, below 25 m heat 1 (with the `*belowradar` unlock, §3.5); ground `HeatSignature −1`
  (not heat-lockable), `RadarSignature 10`; TOW/Kornet/C-RAM −1/−1; flares carry `RadarSignature 100` (decoys)
  [verified]. Model [inferred]: a locker samples candidates every `SampleRate` within `RayLength` and `AcceptanceAngle`
  (scaled by `LockingAcceptanceAngleMultiplier`), accumulates lock over `LockTime` × `LockTimeMultiplier`, loses it after
  `ReleaseTime`; missiles home with `MaxTurnAngle`/s after `TimeToActivateGuidingSystem` and re-target the strongest
  signature in their cone (flares win while burning).
* **TV missile is a vehicle**: `tv_missile_firing` spawns `VehicleProjectileEntityData {vehicle → agm-144_hellfire_tv,
  TimeToLive 7}`; that blueprint has `RemoteEntryComponentData`, a `StaticCameraData` (input `EIARoll`/`EIAPitch`),
  `JetEngineConfigData {MaxVelocity 550, EnginePowerMultiplier 5, ForceMagnitudeMultiplier 5}`, `BodyMass 250`,
  `GravityModifier 0.1`, health 1 [verified]. Implement it as a vehicle the gunner remote-enters (same code path as
  SOFLAM/MAV).
* Wire-guided TOW/Kornet and the guided shell: `IsGuided true` — the missile steers toward the shooter's current aim ray
  [inferred].
* Javelin vs SOFLAM: the SOFLAM is a `LaserPainterData` vehicle (`IsAlwaysActive`, paints after 2 s within 500 m/15°,
  paint lasts 0.5 s after losing it) [verified]; laser-painted targets are lockable by `LockOnLaserPainted` controllers
  (Hellfire, guided shell, Javelin). The Javelin's own `LockingControllerData` (direct lock vs painted lock) lives in the
  soldier weapon data, not read here.

### 3.3 Countermeasures [verified data; behaviour inferred]

Flares: 6 decoys, 11 s reload (heli pilot and gunner variants, jet 3-shot burst). ECM:
`JammingComponentData.AirMissileJamming {EnableAirMissileJamming true, MaxRadius/MinRadius 50, Theta/Phi 45,
QuietModeProbability 1, QuietModeTime 4, JammingTime 4}`, 11 s reload — missiles inside the 50 m cone lose lock for 4 s.
Smoke (tanks/IFVs): `SmokeJammingData {MaxRadius 15, MinRadius 4, JammingTime 5}` + 10 s reload — blocks laser/radar
locks and spotting through the cloud. Fire extinguisher: 24 s reload; restores health to `EmergencyRepairHealth` (§3.4).
All gated by unlocks (§3.5).

### 3.4 Damage model

**Per-vehicle health data** [verified, `S\out\health_all.txt`]:

| vehicle class | zone `MaxHealth` (Front/Rear/Left/Right) | angle calc (`MinDamageAngle`, `DamageAngleMultiplier`) | `MaxShieldHealth` | `DisabledDamageThreshold` | regen delay/rate | decay delay/rate | `EmergencyRepairHealth` |
|---|---|---|---|---|---|---|---|
| M1A2, T-90 | 1000 all | F 81°/×1, R 36°/×2.75, L/R 45°/×1.8 | R/L/R 175 | 500 | 12 s / 30 | 5 s / 18 | 510 |
| LAV-25, BMP-2, LAV-AD, Tunguska, M1128, 2S25 | 1000 all (AAV 1150, BM-23 800) | all 50°/×1.4 | 175 all (AAV rear 0) | 600 (M1128/2S25 400, AAV 575) | 12–20 s / 30 | 7 s / 18 | 30 (M1128/2S25 450) |
| Humvee, Vodnik (+ XP5) | 900 | off | 0 | 700 (XP5 540) | 12 s / 15 | 12 s / 30 | 30 |
| VDV, Growler, DPV | 500 | off | 0 | 300 | 12 s / 32 | 7 s / 15 | 30 |
| quad 300, skid loader 350, KLR650 600, RHIB 350 | — | off | 0 | 100 / 100 / 180 / 100 | 12/32, 12/22, 12/50, 7/20 | | 30 |
| AH-1Z, Mi-28, Z-11W | 500 | off | 0 | 170 | 12 s / 10 | 7 s / 8 | 185 |
| Venom, Ka-60 | 750 | off | 0 | 250 | 10 s / 10 | 7 s / 8 | 30 |
| F/A-18, Su-35, F-35B, A-10, Su-25 | 500 | off | 0 | 170 | 10 s / 18 | 7 s / 6.5 | 185 |
| AC-130 | 2000 | off | 0 | 250 | 0 / 0 | — | 0 |
| TOW2, Kornet | 150 | off | 0 | 0 | 5 s / 0 | — | 30 |
| C-RAM, Pantsir | 1000, `ArmorMultiplier 99999` | off | 0 | 0 | 0 | — | 30 |

All have `HealthZonesShareDamage true`; `AngleOfImpactData.Enabled false` everywhere; tanks `UpsideDownDamage 50`
after 1 s past `UpsideDownAngle 120`, helicopters 350 after 4 s past 160; `WaterDamage` 30 (ground, IFVs 0 =
amphibious), 100 helis, 250 jets, with `BelowWaterDamageDelay`/`WaterDamageOffset`; `VelocityDamageThreshold`/
`VelocityDamageMagnifier` 75/5 ground, 20/5 helis and F-35B, 50/100 other jets, 600/1 AC-130;
`KillSoldierCollisionSpeedThreshold 5` [verified].

**Material relation (armour vs projectile).** `MaterialContainerAsset.MaterialNames` (180 names, `materials/
materialcontainer`) **does name the materials** — IW4L thought grid rows were nameless [verified, `S\out\matnames.txt`].
Vehicle `MaterialPair`s: buggy `Light`(37), Humvee-class `Armored`(36), MBT `MBT`(39), IFV `IFV`(40), mobile AA `AA`(41),
attack heli `Attack`(43), scout `Scout`(44), jet `Fighter`(47)/`Bomber`(46), static AT `StationaryDamage`(48), static AA
`AA`(49), boat `Boat`(133). Projectile pairs carry a damage *property*: MBT shell bullet (HeavyShell 28, **TankShell 86**),
its blast (Tank 60, **BlastDamage 79**), rockets/TV/Hellfire (… , **Rocket 115**), heli cannon blast (**25mm 154**),
stingers (**AA 82**). The level grid cell `InteractionGrid[MaterialIndexMap[property]][MaterialIndexMap[vehicle material]]
.PhysicsPropertyProperties` holds `MaterialRelationDamageData {DamageProtectionMultiplier, DamageProtectionThreshold,
DamagePenetrationMultiplier, CollisionDamageMultiplier, CollisionDamageThreshold, ExplosionCoverDamageModifier,
InflictsDemolitionDamage}`; the `[material][material]` cells for the same pairs are empty [verified on MP_007, 118×118
grid, `S\scripts\grid.py`]. `DamageProtectionMultiplier` on MP_007:

| property \ target | MBT | IFV | mobile AA | Armored (jeep) | Light (buggy) | Attack heli | Fighter | Static AT |
|---|---|---|---|---|---|---|---|---|
| TankShell | 0.9 | 1.1 | 1.22 | 1 | 1 | 2 | 2 | 1.5 |
| BlastDamage (splash) | 0 | 0 | 0 | 1 | 1 | 1 | 1 | 0 |
| Rocket | 2 | 2 | 2 | 2 | 2 | 1.45 | 2 | 1 |
| Missile (87) | 0.9 | 1.157 | 1.157 | 1 | 1 | 2.25 | 2.25 | 1.5 |
| AA | 0 | 0 | 0 | 1.25 | 1.25 | 0.9 | 1 | 0 |
| 25mm | 0 | 0 | 0 | 1 | 1 | 1.75 | 1.75 | 0 |
| 40mm | 0.75 | 0.75 | 1 | 2.25 | 2 | 2.5 | 2.5 | 1.5 |
| Minigun (114) | 0.2 | 0.2 | 0.2 | 1.5 | 1.5 | 0.67 | 0.85 | 0 |
| BulletDamage (small arms) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |

Proposed damage function [inferred, to calibrate]: `dmg = base × DamageProtectionMultiplier(cell) × weapon
DamageMultiplier (or ExplosionDamageMultiplier for blasts) × zoneFactor / ArmorMultiplier`, where `base` is the bullet's
`StartDamage→EndDamage` falloff, the missile's `Damage` for a direct hit, or `BlastDamage` with linear falloff from
`InnerBlastRadius` to `BlastRadius`; `zoneFactor` = 1 unless the zone has `UseDamageAngleCalculation`, then a function of
the hit angle against `MinDamageAngle` reaching `DamageAngleMultiplier` (rear ×2.75 for MBTs). Zone = which of the four
hull quadrants the hit direction falls in. Damage hits `MaxShieldHealth` first: the shields sit exactly on the MBT's
rear/left/right zones and the unlock tree has `reactivearmorleft/rear/rightdummyunlock` + `mbtreactivearmor`, so shields
are reactive armour, active only with that unlock [inferred, strong]. Sanity check: tank shell vs MBT front = 300 × 0.9 =
270 → 27 % of 1000 [inferred; verify in VU]. Missing cells (no `MaterialRelationDamageData`) and
`DamageProtectionThreshold` semantics are unknown.

**States** [inferred from the data pattern]: health ≤ `DisabledDamageThreshold` → *disabled* (no drive, burning); after
`DecayDelay` health bleeds at `DecayRate`/s to death; fire extinguisher sets health to `EmergencyRepairHealth` (tanks 510
> 500, air 185 > 170 — just above the threshold, which is why the pairs match); above the threshold, after
`RegenerationDelay` without damage, health regenerates at `RegenerationRate` × `RegenerationRateMultiplier` (regen
upgrade) — the upper limit of passive regeneration is not in the data. Repair tool: material `RepairTool` 130/149. At 0:
`Explosion` (`ifvexplosion`, `mbtexplosion`, `heliexplosion`, `jetexplosion`) and the wreck for
`VehicleSpawnReferenceObjectData.WreckDuration`.

**Collisions and roadkill** [inferred]: impact speed above `VelocityDamageThreshold` deals (v − threshold) ×
`VelocityDamageMagnifier`; soldiers struck above `KillSoldierCollisionSpeedThreshold` (5 m/s = 18 km/h) die, credited to
the driver; vehicle–vehicle impulses come from Jolt's contacts (IW4L had to add the struck mass by hand). Units of the
thresholds unknown (m/s assumed).

### 3.5 Vehicle upgrades (unlock trees) [verified]

Per-vehicle `UnlockComponentData {UnlockAsset → ValueUnlockAsset persistence/unlocks/vehicles/*, InvertUnlockTest}` subtrees:
AH-1Z: airradar, belowradar, ecmjammer, envgoptics, extinguisher, flarelauncher(+gunner), heatseekermissile,
hellfiremissile, laserdesignator, proximityscan(+gunner), regenupgrade(+gunner), reloadupgrade(pilot/gunner),
rocketguidance, stealth(+gunner), tvguidedmissile, zoomoptics, kit meshes. M1A2: atgmshell, canistershell, coaxhmg,
coaxlmg, envgoptics, proximityscan, reactivearmor (+ left/rear/right dummies), regenupgrade, reloadupgrade1,
smokelaunchers, stealth, tankcommander, zoomoptics, kitunlock1–9. F/A-18: airradar, avionicsupgrade, belowradar,
ecmjammerpod, fireextinguisher, flarelauncher, heatseekerstance, kitheatseeking/missiles/pods, maverickmissiles,
proximityscan, regenupgrade, reload, rocketstance, stealth. `InvertUnlockTest true` subtrees are defaults removed when
the upgrade is equipped. With no persistence service, expose the unlock set as a per-player loadout (all unlocked, or
server-configured) and evaluate subtrees at spawn; the value unlocks feed the `*Multiplier` fields on
`VehicleEntityData`/`WeaponComponentData` (`RegenerationRateMultiplier`, `ReloadTimeMultiplier`, `LockTimeMultiplier`…)
[inferred].

---

## 4. Seats, cameras, turrets, HUD, rendering, audio

### 4.1 Seats, entry, exit [verified fields]

`PlayerEntryComponentData`/`SoldierEntryComponentData` (soldier keeps own weapon; buggy/ITV/Venom door gunners) /
`RemoteEntryComponentData` (TV missile, SOFLAM): `EntryOrderNumber` (seat order: M1A2 driver 0, gunner 1, passengers 2…;
AH-1Z pilot 1, gunner 2 — sort by it, do not assume 0 = driver; the driver is `SeatType EST_Driver`), `HudData.SeatType EST_Driver|EST_Gunner|EST_Passenger`, `EntryRadius` (Humvee 1.25–3 m, AH-1Z 5, TOW 2),
`SoldierOffset`, `IsAllowedToExitInAir`, `LockSoldierAimingToEntry`, `ShowSoldierInEntry`, `Show1pSoldierInEntry`,
`HideSoldierForPassengers`, `IsShielded`, `ForbiddenForHuman`, `NumberOfStances`, `PoseConstraints`, `AntEntryID`
(`"VadsGunner"`, …, soldier pose in the seat — Ant topic), `InputConceptDefinition` (`Input/VeniceHeliInputConcepts`,
`VeniceGunnerInputConcepts`), `InputCurves` (`padinputcurve_*`), `AIData` (`ai/entries/*`). Exits:
`VehicleExitPointComponentData {Impulse, TerrainHeight, Ordinal, Velocity, InheritCameraDirection,
CheckForVehicleOverrun}` with `VehicleEntityData.ChoseExitPointByDirection`, `ExitSpeedThreshold`,
`ExitDirectionSpeedThreshold`, `ExitAllowed`. IW4L's working rules (enter distance clamp(0.75 × max `EntryRadius`,
2.4, 3.7 m), seat cycling, exit search order, seat state cleared on respawn and map restart) carry over (handoff 04 §4.2)
[report claim]. Seat→weapon association rule (seat itself, else sibling, else one level up) matched every vehicle by eye
[report claim]; prefer the explicit `WeaponInputRouterComponentData` where present (helis, jets, AA) [verified present,
semantics inferred].

### 4.2 Cameras [verified fields]

Per seat `CameraComponentData {Camera, FieldOfView (45/55), AlternateView → AlternateCameraViewData, RegularView,
IsFirstPerson, StanceData, ...}`:
* `StaticCameraData` (first person): **free-look limits** `LeftYawAngle/RightYawAngle 120`, `UpPitchAngle 85`,
  `DownPitchAngle 40–45` (AH-1Z), input `EIACameraYaw/Pitch`, `YawSensitivityNonZoomed/Zoomed`, `AccumulateInput`,
  `LeftCurve/RightCurve` (camera slides while looking sideways), `LoosePartPhysics` → `CameraLoosePartPhysicsData`
  (`vehicles/common/loosepart_camera_{heli,jeep,jet,tank}_{pitch,roll,bounce,turn}` — camera shake springs), `Hud` flags.
* `ChaseCameraData` (third person): `TargetOffset (2.8,3,−7)`, `WantedDistance 12`, `MaxDistance 10`, `SnapDistance 20`,
  `WantedAngleDeg 6`, `ToWantedPositionScale (100,200,100)`, `VelocityDrag 25`, `MaxVelocity 100`, `ForceFieldRadius 2`,
  pill collision radii 0.5–1.5, `InheritTargetVelocity`, `KeepTargetPitch`, `UpdateRate 200` (AH-1Z) — a spring camera.
* `AlternateCameraViewData`: zoom FOV 15, `FLIREnabled` (thermal optics unlock), `InputSuppression` (pitch/roll scaled
  0.125 while zoomed), cockpit HUD meshes (`vehicles/common/cockpits/*`).
In Godot: one `Camera3D` driven from C++ each frame; FLIR as a `CompositorEffect` or environment swap (render topic).

### 4.3 Turrets [verified fields; slew semantics inferred]

`ChildComponentData.MovingBody` → `ChildRotationBodyData {Mass, Inertia, AngularMomentumMultiplier,
AngularMomentumThreshold, AngularMomentumDamping(Acc|Deacc), ForceModifier, AngularConstraintMin/Max, UseAngularConstraint,
RotationAxis 0 pitch|1 yaw, RotationInput EIARoll (yaw!)|EIAPitch, UseLinearInput, WorldSpaceLockEfficiency,
ExtendedConstraints}`. There is **no deg/s slew field**: rate emerges from input × `AngularMomentumMultiplier`, capped by
`AngularMomentumThreshold`, damped by `AngularMomentumDamping`. Values: M1A2 main ring 120/120/0.5, gun 120/120/0.32
(−20…0°), roof HMG 750/10/0.35–0.5 (−45…15°); Humvee HMG ring 750/10/0.35, cradle −45…25°; AH-1Z gunner ring −120…120°;
TOW ring 250/120/0.2, cradle −20…20°. **`WorldSpaceLockEfficiency`: M1A2 main ring 0.0001, its gun 0.85, HMGs 1.0** — the
main turret is *not* world-stabilised in yaw while the gun is mostly stabilised in pitch. IW4L made every gun ring hold the
world view yaw; the data says otherwise for MBT main guns [verified data, behaviour inferred]. Implementation: a turret is a
1-DOF angle in the parent frame integrated in the sim step from the seat's aim command (target world direction →
desired local angle → angular-momentum controller with the data's constants), clamped to the constraint; measure BF3's
traverse rates in VU to fit the controller.

### 4.4 HUD [verified fields]

`VehicleEntityData.HudData` `VehicleHudData {MinimapIcon (UIHudIcon_Jeep … StaticAA), Customization, CustomizationOffset}`;
per seat `EntryComponentHudData {SeatType, Frustum, Visible, MaximizeMiniMapOnEntry}`; camera `HudData {UseRangeMeter,
UseAimWarning, UsePredictedSight, UseWeaponOrientations, UseVelocityVectorMarker, UseLockingController, UseThrust,
UseGForce, UseSkidSlip, UseClimbRate, LowAmmoWarning 0.3, CrosshairScaleMin/Max, ...}`; `WarningSystemComponentData`
(missile tracking/locked/inbound sounds, `PlayerType WPTVictim`). IW4L's HUD layout (health, km/h, heat/reload, altitude,
rotor %, top-down outline with turret limits) remains a good default (handoff 04 §4.5).

### 4.5 Rendering composite meshes

Facts [verified, handoff 04 §4.1 + this dump]: composite part vertices are part-local, placed by part transforms equal to
the accumulated component chain; visible parts = `HealthStates[0].PartIndex`; rotor blades **are** separate parts
(AH-1Z main `LowRpmModel.PartIndex 10`, blurred disc `HighRpmModel.PartIndex 11`; tail 15/16); tracks are
`SkinnedMeshAsset` + `SkeletonAsset` (`m1a2_tracks_skeleton`) with `TrackWheelComponentData.BoneName` per wheel; wrecks are
separate meshes; unlock subtrees hide/show kit parts. CPU-skinning a vehicle per frame cost IW4L 2.4 ms with 16 bots;
rigid parts cost 0.13 ms [report claim].

Godot [verified API in `extension_api.json`]: one `RenderingServer` instance per *rigid part group* (body, each wheel,
each turret ring/gun, each rotor state, flaps), created with `instance_create2`, moved with `instance_set_transform` every
render frame from interpolated sim state (`instance_teleport` on spawn/reset); 4.8 has no RenderingServer-level physics
interpolation (`instance_set_interpolated` is absent; `Node3D.get_global_transform_interpolated` is node-side) — so we
interpolate ourselves. Wheels: spin about the axle by `Wheel::mAngle`, offset by suspension length, steer angle. Tracks:
skeleton via `skeleton_create`/`skeleton_bone_set_transform` + `instance_attach_skeleton`, bones following the road
wheels' suspension; belt UV scroll = track speed × `ScrollFactor` through an instance uniform
(`instance_geometry_set_shader_parameter`). Rotors: show `LowRpmModel` below `ChangeModelRpm`, else `HighRpmModel`; spin
= engine rpm × `RotationMultiplier` about `RotationAxis` [inferred]. Flaps: rotate by input × `VisualFlapAngleLimit` at
`VisualFlapTurnSpeed`. Effects (dust, exhaust, wakes, downwash `TriggerGroundEffectHeight 80` + `MaterialRelationVehicleData.
GroundEffect`) via the effects topic.

### 4.6 Audio

Handoff 04 §4.4 stands: `EngineComponentData.SoundEffect` → `SoundPatchAsset` waves (EA-XAS v1), roles by wave name, a
hand-tuned mixer (gear bands, rpm targets, pitch/gain curves) because the patch graph was never evaluated [report claim].
Godot [verified API]: `AudioStreamPlayer3D` per layer with `set_pitch_scale`, `set_doppler_tracking`,
`set_attenuation_model`, `set_attenuation_filter_cutoff_hz`; `Camera3D.set_doppler_tracking`; `AudioStreamSynchronized`
and `AudioStreamInteractive` exist for layered loops. Drive it from replicated rpm/throttle/speed/slip/turret rate, entirely
client-side; also warning sounds from `WarningSystemComponentData`.

---

## 5. Networking

### 5.1 Ticks and threads [inferred design]

* Fixed sim tick 60 Hz on its own thread (server and client), Jolt `Update(1/60, 1 collision step)`; render thread only
  reads double-buffered snapshots. IW4L's measured lesson: a 20 Hz tick inside the render frame made every 5th frame
  20–26 ms (handoff 04 §10) [report claim]. BF3's own tick is disputed in community sources (10 Hz vs 30 Hz)
  [report claim]; it does not constrain us.
* Snapshots 30 Hz (every 2nd tick) to each client, delta-compressed against the last acked snapshot, prioritised
  (aircraft and nearby vehicles first). Commands from clients every tick, each packet carrying the last 3 commands
  (redundancy).

### 5.2 Command structure

BF3's vehicle input is a vector of `EntryInputActionEnum` levels: `EIAThrottle, EIABrake, EIAYaw, EIAPitch, EIARoll,
EIAStrafe, EIACameraYaw, EIACameraPitch, EIAFire, EIAFireCountermeasure, EIAZoom, EIAReload, EIASelectWeapon1–9,
EIAChangeEntry(1–8), EIAInteract, EIAGearUp/Down, EIAHandBrake, EIAClutch, EIAToggleCamera…` (`VU-Docs/types/fb/
EntryInputActionEnum.yaml`) [verified]; fun-bots drives helicopters by setting exactly these levels from PIDs
(`unfun-bots/ext/Server/Bot/VehicleChopperControl.lua`: `input:SetLevel(EntryInputActionEnum.EIAYaw/EIAThrottle/EIAPitch/
EIARoll, …)`) [verified]. Command = `{tick u32, seat u8, axes i8×8 (throttle, brake, yaw, pitch, roll, strafe, camYaw,
camPitch), aim world yaw/pitch u16×2 (for turret seats), buttons u32 (fire, alt-fire, countermeasure, zoom, weapon select,
change seat, interact, gear, handbrake, boost), view-time for lag compensation}` ≈ 20 B. Input smoothing
(`VehicleInputData` inertia) happens inside the sim, so the command carries raw levels. Bots generate the same command on
the server.

### 5.3 Replicated vehicle state

Quantised: position (cm), orientation (smallest-three 3×10 bit), linear/angular velocity, controller state needed for exact
restore (per wheel suspension length + angular velocity, steer angle, engine rpm, gear, clutch; rotor rpm; flap angles;
input-inertia filter values), turret angles, health/shield/disabled flags, weapon heat/ammo/reload per seat (to the seat
owner only), lock state, occupants. IW4L measured ≈ 180 B per changed vehicle row + 24 B per occupant and zero cost for
parked vehicles (handoff 04 §10) [report claim]. Remote clients need less (pose, velocities, wheel visuals, turret angles,
rpm for audio) — two LOD tiers of the row.

### 5.4 Driver prediction and reconciliation with Jolt [inferred design]

* Client keeps a `PredictionWorld` (static shapes shared, own vehicle dynamic, other vehicles/soldiers as kinematic bodies
  at their extrapolated poses). Each tick: record `SaveState` of the own vehicle (body + `VehicleConstraint`) and the
  command, step.
* On a server snapshot for tick N: compare with the recorded state N; if position error > 2 cm, angle > 0.5° or velocity
  > 5 cm/s (tune), `RestoreState` the server's state into the prediction body, replay commands N+1…now (100 ms RTT = 6
  ticks), and render the difference away over ~150 ms (visual offset, not physics).
* Why it mostly agrees: with our own `CROSS_PLATFORM_DETERMINISTIC` Jolt, the same binary, the same static world built in
  the same order (same BodyIDs) and the vehicle in its own island, a free-driving vehicle's step depends only on its state,
  its command and the static world — expect zero corrections while not touching other dynamic bodies, and corrections
  during contacts (the server's other bodies are dynamic, the client's are kinematic). Verify with a recorded command log
  replayed in both worlds (§6.2 step 4). IW4L's bit-exact integer-ms sub-steps are not reproducible with a contact solver;
  smoothing replaces them.
* Boarding, exiting and seat switches are server-authored events applied at a tick both sides agree on (IW4L did them in
  the authority's "touch" phase so no predicted body deviated) [report claim].

### 5.5 Passengers and gunners

A client who is not the driver sees the vehicle interpolated (~100 ms buffer, Hermite on position/velocity, slerp
orientation); the camera and its own body ride that interpolated frame. The gunner's turret aim is predicted locally
(turret angles are local state driven only by that seat's command, so the client can run them immediately and send the
resulting aim with the fire command); the server validates and lag-compensates. When the driver changes seats the vehicle
switches from interpolated to predicted on that client at the next snapshot (snap + smooth).

### 5.6 Fast aircraft and latency

At 300–500 km/h a 100 ms interpolation delay is 8–14 m. Needed: server-side lag compensation (rewind vehicle hulls to the
shooter's view time; IW4L's journal notes that rewinding must include every vehicle, as must BF3's), extrapolation (dead
reckoning with angular velocity) only when snapshots are late, client-side lock-on sampling at `SampleRate 0.06` with the
server deciding the lock, missiles simulated on the server (client spawns a predicted visual at fire time and blends to
the server missile). Snapshot priority by speed × proximity keeps jets fresh.

### 5.7 Bots in vehicles

Server-side, same command path. Ground: IW4L's walk-graph routing with hull-clearance filtering, pure pursuit, back-up
logic (handoff 04 §9) [report claim]. Air: fun-bots' PID controllers on yaw/height/tilt/roll toward a target point above
the objective (`VehicleChopperControl.lua`, `VehicleJetControl.lua`) [verified], plus `VehicleSpawnReferenceObjectData.
BotBailWhenHealthBelow/BotBailOutDelay` and the seats' `AIData` (`ai/entries/*_ai_entry`) and weapons' `AIData`
(`ai/weapons/vehicle_*_ai_weapon`) [verified references, contents unread].

---

## 6. C++ design and ordered plan

### 6.1 Classes (library `bf3` Godot-independent; `bf3_godot` GDExtension)

```cpp
// bf3/vehicle/def.h — immutable, built once per blueprint from EBX, cached under user:// by content digest
struct WheelDef   { Vec3 pos; float radius, width, springLength, stiffness, damping, progStart, progExp, attachOffsetY;
                    float mass, diffRatio, lonMax, latMax, brakeForce, handBrakeForce; float pacejka[5];
                    Curve2D steerSensitivity, transmissionLoss; uint8_t physicsType /*wptNormal|Follow|Static*/,
                    drivingType, steeringType; int16_t part, track; };
struct EngineDef  { enum Kind { Combustion, Jet, Propeller } kind; Vec3 pos; Curve rpmTorque; float rpmMin, rpmMax,
                    powerMul, accel, decel; BoostDef boost; JetDef jet; PropellerDef prop; RotorParams rotor; };
struct WingDef    { float lift, flapLift, drag, flapDrag, baseAoA, dragRotMod; int inputForFlap;
                    Curve2D liftCoef, dragCoef, maxFlapScale; };
struct TurretDef  { int parent, seat; Vec3 pivot; uint8_t axis; float min, max; bool constrained;
                    float momentumMul, momentumThreshold, dampAcc, dampDeacc, forceMod, worldLock; int16_t part; };
struct SeatDef    { SeatType type; int order; Vec3 pos; float entryRadius; bool ownWeapon, remote, exposed, exitInAir;
                    CameraDef cam1p, cam3p, zoom; std::vector<int> weapons; };
struct WeaponDef  { Vec3 muzzleOffset; int turret; FiringDef firing /*Shot, FireLogic, Ammo, OverHeat, dispersion*/;
                    ProjectileRef proj; LockingDef locking; float dmgMul, expMul, reloadMul; };
struct HealthDef  { ZoneDef zones[4]; float disabledAt, regenDelay, regenRate, decayDelay, decayRate, emergency,
                    armorMul, upsideDownDmg, upsideDownDelay, upsideDownAngle, waterDmg, velDmgThreshold, velDmgMag,
                    killSoldierSpeed; uint8_t material; };
struct VehicleDef { std::string blueprint; VehicleKind kind; ChassisDef chassis /*VehicleConfigData*/;
                    std::vector<WheelDef> wheels; std::vector<TrackDef> tracks; std::vector<EngineDef> engines;
                    std::vector<WingDef> wings; std::optional<FloatDef> floatPhys; std::optional<MotorbikeDef> bike;
                    std::vector<TurretDef> turrets; std::vector<SeatDef> seats; std::vector<WeaponDef> weapons;
                    std::vector<Vec3> exits; HealthDef health; RotorVisual rotors[2]; PartMap parts;
                    JPH::RefConst<JPH::Shape> hull /*Collision body*/, raycastHull /*Raycast body*/;
                    std::vector<UnlockSubtree> unlocks; SoundSet sounds; };

// bf3/vehicle/sim.h — one per live vehicle, inside a SimWorld
class VehicleSim : public JPH::PhysicsStepListener {      // forces for rotor/wings/float/aero/turrets each step
  const VehicleDef* def; JPH::BodyID body; JPH::Ref<JPH::VehicleConstraint> ground; // wheeled/tracked/motorbike
  RotorcraftModel rotor; FixedWingModel wings; BuoyancyModel buoy; TurretSet turrets; InputFilter input;
public:  void apply(const VehicleCommand&);  void OnStep(const JPH::PhysicsStepListenerContext&) override;
         void save(StateWriter&) const;  void restore(StateReader&);   // body + constraint + model state
};
class Bf3WheeledController : public JPH::WheeledVehicleController { /* BF3 tyre/K-term model if Jolt's falls short */ };

class SimWorld { JPH::PhysicsSystem sys; StaticWorldRef statics; /* bodies, vehicles, characters */ };
class ServerWorld : public SimWorld { /* authority: damage, seats, spawns, wrecks, roadkill, lag-comp history */ };
class PredictionWorld : public SimWorld { /* own vehicle dynamic, others kinematic; rollback/replay */ };

class VehicleDamage  { /* zones, angle, shields, MaterialGrid lookup, disabled/decay/regen, water, upside-down, collisions */ };
class MaterialGrid   { float protection(uint8_t property, uint8_t material) const; /* from MaterialGridData */ };
class SeatSystem     { /* enter/exit/switch, occupant transforms, remote entries */ };
class VehicleWeapons { /* fire logic, heat, ammo, reload, locking, guidance, countermeasures, projectiles */ };
class VehicleNet     { /* command buffers, snapshot rows (2 LODs), deltas, reconciliation, interpolation */ };

// bf3_godot (GDExtension, main thread only)
class VehicleView    { /* RenderingServer instances per part group, wheel/track/rotor/flap animation, effects */ };
class VehicleCameraRig { /* StaticCameraData free-look + LoosePart shake, ChaseCameraData spring, zoom/FLIR */ };
class VehicleAudio   { /* AudioStreamPlayer3D layers driven by replicated rpm/speed/slip */ };
class VehicleHud     { /* draws HudData-driven panels */ };
```

### 6.2 Ordered plan (each step ends with an in-game check)

1. **Jolt world from the level, Humvee drops in.** Own Jolt in the extension; static world of MP_001 Grand Bazaar CQL from
   the collision converter; spawn the Humvee at its two CQL spawns as a plain rigid body with BF3 mass/CoM/inertia/gravity
   factor. *Check:* it settles within 1 cm of the spawn height IW4L measured (ground spawns sit within 1 in of the ground),
   for every ground spawn of MP_001 and Caspian (catch the VDV fall-through). Measure Jolt memory with and without a second
   `PhysicsSystem` sharing the shapes.
2. **Humvee drives.** `VehicleConstraint` + `WheeledVehicleController` mapped from `WheelConfigData`/engine/gearbox;
   keyboard input through the `VehicleInputData` filter; chase camera from `ChaseCameraData`. *Check:* 0→60 km/h time, top
   speed, braking distance, full-lock turning circle against reference clips the user records in VU/BF3 at the same spot;
   CPU of 40 Humvees on Caspian < 1 ms/step.
3. **Seats, turret, HMG, damage.** Entry/exit/switch with `EntryRadius` and exit points; gunner ring/cradle from
   `ChildRotationBodyData`; `tank_remote_hmg_firing` with heat; `VehicleDamage` with the material grid; disabled/decay/regen;
   wreck + respawn timers from the spawner. *Check:* RPG/tank-shell hit percentages vs VU; HMG overheats after the same
   time as BF3 (data: 550 rpm × 0.075 heat/round − 0.5/s ≈ +0.19/s → ≈ 5.3 s of continuous fire from cold, if overheat is at
   heat 1 [inferred]); exit placement never inside geometry.
4. **Listen server + client prediction.** Commands, snapshots, rollback/replay, smoothing; artificial 100–200 ms latency
   and 2 % loss. *Check:* zero corrections while driving freely on terrain (deterministic replay in both worlds from the
   same command log, bit-compare states), bounded corrections in collisions, passenger/gunner views smooth.
5. **Tracked: T-90 and M1A2.** `TrackedVehicleController`, track belt skinning + UV scroll, cannon (200 m/s, drop) +
   canister/guided shell unlocks, zone angles, reactive-armour shields, smoke. *Check:* pivot 65° in 2 s (IW4L) and BF3
   reference; M1A2 vs T-90 top speeds within a few km/h of each other as in BF3; turret traverse vs measured.
6. **AH-1Z (then Mi-28).** Rotorcraft model on `PropellerEngineConfigData`/`RotorParameters`, rotor part swap/spin,
   rockets, gunner cannon, TV missile as remote vehicle, flares/ECM, heat locks from Stinger/Igla and AA vehicles. *Check:*
   hover hold, climb rate, speed at 25°/36° nose-down vs reference; flare breaks a lock; TV missile 7 s life.
7. **F/A-18 (then Su-35).** Fixed-wing model, afterburner, gear logic, stall. *Check:* turn rate vs speed peaks near the
   `MaxFlapAngleScaleFactor` knee (≈ 305 km/h); top speed with/without boost; no flight above the `PowerFadeOutRange` band.
8. **RHIB, then amphibious LAV-25/AAV.** Buoyancy listener, water-jet steering, wreck sinking (`FilledDensity`). *Check:*
   planing speed and turn vs reference; LAV enters/leaves water on Noshahr.
9. **Stationary and gadget vehicles.** TOW/Kornet wire guidance, C-RAM/Pantsir, M224, SOFLAM painting + Javelin lock (with
   the weapons topic). *Check:* TOW follows the crosshair; painted target lockable for 0.5 s after paint loss.
10. **Bots in vehicles** (ground first, then helicopters with fun-bots-style PIDs), then the rest of the roster:
    KLR650/quad/DPV, Venom/Ka-60/AH-6/Z-11W, F-35B VTOL, A-10/Su-25, AC-130/C-130 gunships, HIMARS/BM-23,
    paradrop LAV/BMP, skid loader, XP4 modified vehicles. *Check per vehicle:* spawns on every level×mode of §2.8, enter,
    drive/fly, fire, die, respawn.

---

## 7. What I could not determine

* **Units/semantics** of `ForwardGearSpeeds`, `MaxVelocity`, `VelocityDamageThreshold`, the speed axis of
  `MaxFlapAngleScaleFactor`/`AngleOfAttack` (km/h inferred), `PowerFadeOutRange` (altitude inferred), `GravityMod`,
  `AdditionalGravityModifier`, `HorizontalForceModifier`, `CyclicFadeOutOffset`, `CenterOfMassHandlingOffset`,
  `Boost` on buggies (`ForwardStrength 4`), `ClientFireRateMultiplier`, `PreDestructionDamageThreshold`.
* BF3's exact **tyre/suspension formulas** (how `Stiffness`/`Damping` factors, the Pacejka-like K terms, friction max
  factors, `TransmissionLoss` and rolling resistance combine), the **helicopter and jet force laws** behind the fields, and
  the **turret angular-momentum controller** (`AngularMomentumMultiplier/Threshold`): only names and values are in the data.
  Fit against reference measurements; do not expect closed-form parity.
* The **damage formula**: how `MinDamageAngle`/`DamageAngleMultiplier` scale a hit, `DamageProtectionThreshold`,
  behaviour of grid cells without `MaterialRelationDamageData`, whether `HealthZonesShareDamage` means one pool, the cap of
  passive regeneration, how splash is split across zones. Grid lookups were verified on MP_007 only; other levels carry
  their own `MaterialGridData`.
* The soldier-side anti-vehicle weapons (RPG/SMAW/Javelin/C4/mines) — their projectile `MaterialPair`s and `Damage` were
  not read (weapons topic).
* `WeaponInputRouterComponentData` routing semantics, `AIEntryData`/`AIWeaponData` contents, `ArtilleryStrikeWeaponData`,
  `MotorbikeData` field meanings, the AC-130/C-130 orbit logic (likely event graph), the `_SpawnInAir` jets' spawn logic.
* Whether Jolt's vehicle step is bit-identical between a full server world and a small prediction world in practice
  (islands should make it so; untested), Linux symbol interposition between Godot's internal Jolt and ours (untested), and
  real CPU/memory numbers for the shared-shape design.
* BF3's actual server tick and whether its vehicles were driver-predicted exactly as proposed (`UsePrediction true` is the
  only primary evidence found; DICE talks on BF3 vehicle netcode were not found).
* Vehicle audio patch-graph semantics (unchanged from the handoff).

---

## Independent verification (2026-10-10)

Done by a separate verifier who tried to refute each claim without using this report's own probe or its sparse Godot clone.
Sources:
* `GS` = `C:\Users\olku\AppData\Local\Temp\claude\C--Users-olku-BF3-reimplementation\192b4390-9a90-436b-abe1-9117ba9cb3e5\scratchpad\godot-engine\godot-src`.
  This is a full clone at `c971f93e7e76b0ef919bf6009e7b868bea04db7f` (`git log` checked), made by another agent. Jolt sits under `thirdparty\jolt_physics`.
* `PE` = `…\scratchpad\probe-ebx`, the EBX data probe's independent `ebx-dump` and its dumps:
  * `out2`: the MP_007 grid and `materials/materialcontainer`;
  * `out4`: M1A2, AH-1Z and Humvee blueprints;
  * `ebx-partitions.tsv` and `all-entries.tsv`.
* `VV` = `…\scratchpad\verify-vehicles`. It holds:
  * `dumps\*.json`, written with PE's dumper: T-90, F/A-18, Su-35, A-10, C-RAM, Pantsir, the weapondata partitions, the gadgets and `full_squaddeathmatch`;
  * `types_all.tsv`: every instance of `VehicleBlueprint`, `VehicleSpawnReferenceObjectData` and `VehicleProjectileEntityData` over all MP/XP superbundles;
  * `g1-3.py`, which run the grid lookups;
  * `web2\Architecture.md` and `web2\VehicleStressTest.cpp`, downloaded from GitHub on 2026-10-10.
* `API` = `C:\Users\olku\BF3 reimplementation\research\godot-api\extension_api.json`. The 4.7 baseline is `…\scratchpad\godot-engine\godot-cpp\gdextension\extension_api-4-7.json` (header `v4.7.stable.official`).

| claim id | claim (short) | verdict | evidence (with source) | correction |
|---|---|---|---|---|
| C1 | 4.8 dev7 API: force integration, direct state, test_motion, queries incl. new `*_into`; no space step/save/restore, no Jolt class | confirmed | **API, `PhysicsServer3D`:** has `body_set_force_integration_callback(body: RID, callable: Callable, userdata: Variant = null)`, `body_set_omit_force_integration`, `body_set_state_sync_callback`, `body_get_direct_state` and `body_test_motion(RID, PhysicsTestMotionParameters3D, PhysicsTestMotionResult3D) -> bool`. Its `space_*` methods are only create, set_active, is_active, set_param, get_param and get_direct_state. It has no step, sync or flush method. `_step`, `_sync`, `_flush_queries` and `_end_sync` exist only as virtuals of `PhysicsServer3DExtension`. **Queries:** `PhysicsDirectSpaceState3D` has six `*_into` methods and there are 12 `Physics*Result2D/3D` classes. None of these is in the 4.7 JSON, so all are new in 4.8. **Other:** `RigidBody3D._integrate_forces` exists, and no class name contains "Jolt" [verified]. | none |
| C2 | Godot steps the whole space with `Update(step,1,…)`; zero Jolt gravity, gravity as a per-body force; callbacks in `flush_queries`; Vehicle* compiled in; no `JPH_CROSS_PLATFORM_DETERMINISTIC` | confirmed | All paths under `GS\modules\jolt_physics` unless noted [verified]:<br>• `spaces\jolt_space_3d.cpp:138`: `SetGravity(JPH::Vec3::sZero())`; `:196`: `Update(p_step, 1, temp_allocator, job_system)`; `:77-85`: an active-body loop calls `pre_step`.<br>• `objects\jolt_body_3d.cpp:147-167`: Godot damping, then `AddForce(total_gravity / inverse_mass + constant_force)`. This is skipped when `custom_integrator` is set.<br>• `objects\jolt_body_3d.cpp:1076-1110`: `call_queries` runs the force-integration and state-sync callables. `jolt_physics_server_3d.cpp:1665-1673` calls it from `flush_queries`.<br>• `GS\main\main.cpp`: `sync` + `flush_queries` at 5017-5018, `physics_process` at 5028, `end_sync` + `step` at 5061-5062.<br>• `SCsub:121-132` lists the 12 `Vehicle/*.cpp`. `SCsub:150-157` defines only `JPH_ENABLE_ASSERTS`, `JPH_DEBUG_RENDERER` and `JPH_DOUBLE_PRECISION`. No SCsub, SConstruct or `*.py` in `GS` mentions `CROSS_PLATFORM_DETERMINISTIC`. | The cited `jolt_project_settings.cpp` defaults 10/2 are at lines 37-38, not 36-37 (trivial). |
| C3 | Engine-module reach: `get_singleton()->get_space(rid)->get_physics_system()`, `get_jolt_body()`, `get_jolt_ref()` | confirmed | All declarations are public [verified]:<br>• `GS\modules\jolt_physics\jolt_physics_server_3d.h:126` (public from :122) and `:444` `get_space`;<br>• `spaces\jolt_space_3d.h:113` `JPH::PhysicsSystem &get_physics_system()`;<br>• `objects\jolt_object_3d.h:120-121`;<br>• `shapes\jolt_shape_3d.h:87` `const JPH::Shape *get_jolt_ref()`.<br>"Custom build per update" follows from module linkage [inferred]. | `get_jolt_ref()` returns a raw `const JPH::Shape*`. A second system needs its own `RefConst` to keep the shape alive. |
| C4 | Jolt determinism and rollback rules; `VehicleConstraint`/`WheeledVehicleController` `SaveState/RestoreState` | confirmed | **`VV\web2\Architecture.md:785-842`** (master; byte-identical to the copy fetched earlier):<br>• deterministic only with the same API call order and the same binary;<br>• `CROSS_PLATFORM_DETERMINISTIC` is "approximately 8% slower";<br>• rollback by `SaveState`/`RestoreState`; no bodies or constraints may be added or removed in between; BodyIDs must be kept, or created with `BodyInterface::CreateBodyWithID`.<br>**Jolt 5.6.0 in `GS\thirdparty\jolt_physics`** (`GS\thirdparty\README.md:512-516`, commit `e77f1755`):<br>• `Jolt\Physics\PhysicsSystem.h:165,168`;<br>• `Vehicle\VehicleConstraint.h:199-200` and `WheeledVehicleController.h:171-172`;<br>• also `TrackedVehicleController.h:145-146` and `MotorcycleController.h:93-94` [verified]. | none |
| C5 | Route: own pinned cross-platform-deterministic Jolt in the gameplay library; server, prediction and cosmetic systems share static shapes; Godot physics empty; `VehicleConstraint` + custom controllers | confirmed (premises) | **Premises:**<br>• Architecture.md "Working With Multiple Physics Systems": "The only object that can be shared between PhysicsSystems is a Shape".<br>• `VehicleController.h:59-77`: every hook (`ConstructWheel`, `AllowSleep`, `PreCollide`, `PostCollide`, `SolveLongitudinalAndLateralConstraints`, `SaveState/RestoreState`) is pure virtual, protected, with `friend class VehicleConstraint`, so it can be subclassed.<br>• Godot exposes no step or restore (C1).<br>• `GS\servers\register_server_types.cpp:373` registers `"Dummy"`.<br>• `VV\web2\VehicleStressTest.cpp:92-107` (Jolt commit e77f1755) builds 15×15 = 225 four-wheel vehicles with `VehicleCollisionTesterRay`. It has no timing figure, so "< 1 ms" is still inferred.<br>**Core principle:** not touched (caches under `user://`) [verified docs/source; merit inferred]. | **Caveats:**<br>• Bodies, not only shapes, must be created per system.<br>• Destruction or projectile bodies added or removed inside a rollback window break `RestoreState`, so the prediction world must be kept free of them.<br>• CPU and memory are untested. |
| C6 | 49 blueprints, 2,048 placements, 119 level×mode; none on MP_Subway/XP2; SQDM BMP-2 on 24 levels | confirmed | **Blueprints:** `VV\types_all.tsv` has 57 `VehicleBlueprint`s. They are the 49 in `spawns_all.tsv` (identical set, diffed), plus `vehicles/_old/humvee`, `agm-144_hellfire_tv` and 6 gadgets.<br>**No MP_Subway/XP2 vehicles:** no `levels/mp_subway*` or `levels/xp2_*` partition holds a `VehicleSpawnReferenceObjectData`.<br>**XP4/XP5:** their spawners come from the `gameplay/level_setups/dynamic_vehiclespawners/dynamicspawn_*` and `xp5/dynamic_vehiclespawners/*` prefabs.<br>**SQDM** (`PE\all-entries.tsv`):<br>• `full_squaddeathmatch` holds 4 spawners, all `vehicles/bmp2/bmp2`, `TeamNeutral` (`VV\dumps`). It is in the SQDM bundle of 20 levels; `full_squaddeathmatch_xp4` (4 spawners) is in the 4 XP4 levels; 24 in total.<br>• MP_Subway and all 4 XP2 maps use `full_squaddeathmatch_novehicles`.<br>All of the above [verified]. | The 2,048 and 119 totals come only from the author's traversal; they are internally consistent (2,048 rows, 119 unique level×mode). The blueprint of the XP4 SQDM spawners was not re-read. |
| C7 | Damage multiplier = grid cell `[map[projectile PhysicsPropertyIndex]][map[vehicle PhysicsMaterialIndex]]` → `DamageProtectionMultiplier`; MP_007 values | confirmed | `VV\g1-3.py` over `PE\out2` [verified]:<br>• **Grid:** 118 rows, a 180-entry `MaterialIndexMap`, 600 `MaterialRelationDamageData`. The container has 180 `MaterialNames` (28 HeavyShell, 36 Armored, 37 Light, 39 MBT, 40 IFV, 41/49/82 AA, 43 Attack, 47 Fighter, 48 StationaryDamage, 77 BulletDamage, 79 BlastDamage, 86 TankShell, 87 Missile, 114 Minigun, 115 Rocket, 154 25mm).<br>• **Material pairs:** T-90 (39, 39), F-18 (47, 47), C-RAM (49, 49). `mbt_cannon_firing` has (mat 28 HeavyShell, prop 86 TankShell) and (60 Tank, 79 BlastDamage).<br>• **Values:** all 8 checked rows of §3.4's table reproduce exactly, e.g. TankShell→MBT 0.9, Rocket→MBT 2, BlastDamage→MBT 0, 25mm→Attack 1.75, BulletDamage→all 0. The TankShell×MBT cell also has `DamageProtectionThreshold 2`, `CollisionDamageThreshold 30` and `InflictsDemolitionDamage true`, and its `PhysicsMaterialProperties` is empty.<br>• **Index used:** `[HeavyShell][MBT]` has no damage data, so the *property* index is the key. | **Notes:**<br>• Every tested cell is symmetric (`[a][b] == [b][a]`), so lookup direction does not matter on MP_007.<br>• Scout (44) and Boat (133) map to grid row 0 on MP_007, because those vehicles are not on that level.<br>• The 40mm row was not re-checked. |
| C8 | MBT 1000/zone, angles F 81/×1, R 36/×2.75, L/R 45/×1.8, shield 175 R/L/R, disabled 500, emergency 510; attack heli 500/170/185; jets 500, regen 18/s after 10 s; C-RAM/Pantsir `ArmorMultiplier 99999` | confirmed | All [verified]:<br>• **M1A2** (`PE\out4`, named fields `FrontHealthZone`/`LeftHealthZone`/`RearHealthZone`/`RightHealthZone`): 1000/81°/×1/shield 0; 1000/45/×1.8/175; 1000/36/×2.75/175; 1000/45/×1.8/175. `DisabledDamageThreshold 500`, `EmergencyRepairHealth 510`, regen 12 s/30, decay 5 s/18.<br>• **T-90** (`VV\dumps`): identical to the M1A2.<br>• **AH-1Z:** 500×4, 170, 185, regen 12 s/10.<br>• **F/A-18 and A-10:** 500, 170, 185, regen 10 s/18.<br>• **C-RAM and Pantsir:** `ArmorMultiplier 99999`.<br>• **Unlocks:** the M1A2 references `reactivearmorleft/rear/rightdummyunlock`, `persistence/unlocks/vehicles/mbtreactivearmor` and `vehicles/common/logicalprefabs/reactivearmor`. | Semantics are still inferred; see extra-1 on reactive armour. |
| C9 | Effective muzzle speed = `ShotConfigData.InitialSpeed.z` (MBT 200 vs bullet 350); fun-bots 200/600 | confirmed | `VV\dumps` [verified]:<br>• `mbt_cannon_firing`: `ShotConfigData.InitialSpeed (0,0,200)`, `BulletEntityData.InitialSpeed 350`, damage 300/300, gravity −9.81.<br>• `tank_remote_hmg_firing`: shot 600, bullet 350.<br>`C:\Users\olku\unfun-bots\ext\Shared\Constants\VehicleData.lua:34-43` gives `Speed = { { 200, 610 }, 600, 350 }`. | The Lua comment (line 33) only labels the column "Bullet-Speeds"; it does not say the values were hand-measured. |
| C10 | TV missile, SOFLAM, M224, MAV, EOD bot are `VehicleBlueprint`s; TV missile via `VehicleProjectileEntityData`, `RemoteEntryComponentData`, `JetEngineConfigData`, health 1 | confirmed | All [verified]:<br>• **Blueprints** (`VV\types_all.tsv`): `agm-144_hellfire_tv`, `soflam_projectile`, `m224_deployed`, `mav`, `eodbot`, and also `t-ugs_vehicle` and `radiobeacon_projectile`.<br>• **Spawners:** 7 `VehicleProjectileEntityData` (`tv_missile_firing`, `m224`, `mav_pda`, `eodbot_pda`, `soflam_pda`, `t-ugs`, `radio_beacon`).<br>• **`tv_missile_firing`:** Vehicle → `agm-144_hellfire_tv`, TTL 7, shot 90, reload 15.<br>• **agm-144:** `RemoteEntryComponentData`, `JetEngineConfigData` (MaxVelocity 550, power 5, force 5), 4 zones `MaxHealth 1`.<br>• **SOFLAM:** remote entry, zones 50.<br>• **M224:** `ArtilleryStrikeWeaponData`, shot 200, reload 4, `EquipmentFakeVehicle true`. | T-UGS and the radio beacon (a report claim in §2.7) are now verified as `VehicleBlueprint`s. |
| C11 | AH-1Z flight data: lift spring 40000/damping 20000, collective idle 0.9 vs `RotorParameters` 0.99, AoA curve, tail ×75, rotor parts 10/11 | confirmed | `PE\out4\vehicles__ah1z__ah1z.json` [verified]:<br>• **Main** `PropellerType_Rotor`: `LiftForceSpringConstant 40000`, `LiftForceDampingConstant 20000`, `CollectiveInputIdle 0.9`, Pitch/Roll/Banking 20/25/40.<br>• **`RotorParameters`:** `CollectiveInputIdle 0.99`, `AngleOfAttack` (0,0.1)…(200,3.5).<br>• **Tail** `PropellerType_Regular`: `ForceMagnitudeMultiplier 75`.<br>• **`RotorComponentData`:** main Low/High `PartIndex` 10/11, `raY`, 0.008, `ChangeModelRpm 10000`; tail 15/16, `raX`. | Both rotors carry `ForceMagnitudeInputType FMITYaw`, not only the tail. |
| C12 | Jet `MaxFlapAngleScaleFactor` peaks ~250-305, collapses by 320 (= 310-315 km/h sweet spot); `LiftCoefficient` peaks 18-24° (stall); afterburner `Boost.ForwardStrength 1.5` | corrected | **F/A-18 data** (`VV\dumps\vehicles__f18-f__f18.json`) [verified]:<br>• Pitch flap wing (`InputForFlap 0`): (250,1.75)(305,1.7)(320,1.05).<br>• Yaw (`1`): (250,1.85)(300,0.5).<br>• Roll pair (`2`, Lift 25 each): (200,1.25)(250,1.2)(285,0.85).<br>• `Boost {ForwardStrength 1.5, DissipationTime 9999}`.<br>• `LiftCoefficient`: roll pair peaks at (18,9), pitch wing at (24,6). The Lift-20 wing (FlapLift 0) rises monotonically to its last key (28,10).<br>**Community:** the AnandTech FAQ thread says "Keep your speed between 310-315" [report claim, read: https://forums.anandtech.com/threads/official-at-battlefield-3-faq-and-news-thread.2141077/post-34076970]; a Battlelog platoon page says 313 [search summary only]. | Only two wing types show a stall peak, at 18° and 24°. The Lift-20 wing has none within ±28°. Roll authority already drops between 250 and 285. The km/h axis remains inferred, so §2.4's "stall = beyond the 24° peak" should read "beyond 18° (main pair) / 24° (pitch flap wing)". |
| C13 | No deg/s turret rate; angular-momentum controller; M1A2 ring `WorldSpaceLockEfficiency` 0.0001, gun 0.85; IW4L's every-ring-holds-world-yaw contradicted | confirmed | **M1A2** (`PE\out4`): ring axis 1 `EIARoll` 120/120/0.5, `ForceModifier 14000`, unconstrained, lock 0.0001. Gun axis 0 `EIAPitch` 120/120/0.32, −20…0°, lock 0.85. HMG ring/cradle 750/10, lock 1.0.<br>**Rate field:** none among the 21 `ChildRotationBodyData` fields is a deg/s rate.<br>**T-90** (`VV\dumps`): ring lock 0.0, gun 1.0, −20…4°.<br>**IW4L:** `C:\Users\olku\iw4L\crates\vehicle_iw4\src\parts.rs:98-109` makes every yaw ring "follow the look", and handoff `sections\04-gameplay-sim.md:277` has a gun seat's "ring holds the view yaw".<br>All [verified data; behaviour inferred]. | The data agrees with IW4L for gunner-seat HMG rings (lock 1.0). The contradiction is only for the MBT main ring (0.0001 on M1A2, 0.0 on T-90). |
| C14 | AH-1Z free-look 120/120/85/40-45; chase (2.8,3,−7)/12; alternate view zoom FOV 15 and FLIR | confirmed | `PE\out4` AH-1Z [verified]:<br>• `StaticCameraData`: 120/120/85/40 and 120/120/85/45.<br>• `ChaseCameraData`: `TargetOffset (2.8,3,−7)`, `WantedDistance 12`, `MaxDistance 10`, `SnapDistance 20`.<br>• Three `AlternateCameraViewData`: FOV 55 without FLIR, FOV 15 without FLIR, and FOV 55 with `FLIREnabled true`. | FLIR is a separate FOV-55 view, not the FOV-15 zoom. §4.2's "zoom FOV 15, FLIREnabled" should be read as two views. |
| C15 | Prediction world with `SaveState`/`RestoreState` rollback; free driving replays without corrections given same-binary Jolt and identical static build order | corrected | **(1) Contact ordering depends on BodyIDs.** Jolt orders contact constraints by `mSortKey` = `SubShapeIDPair::GetHash()` = `HashBytes` over (Body1ID, SubShapeID1, Body2ID, SubShapeID2) (`GS\thirdparty\jolt_physics\Jolt\Physics\Constraints\ContactConstraintManager.cpp:869,1135,1484-1485`; `Collision\Shape\SubShapeIDPair.h:51-57`).<br>**(2) The quantised snapshot cannot restore the constraint exactly.** `VehicleConstraint::SaveState` (`Vehicle\VehicleConstraint.cpp:646-671`) writes:<br>• the controller state;<br>• per wheel: `mContactBodyID`, contact position, normal and lateral, suspension length, and the warm-start lambdas of the suspension, longitudinal and lateral parts;<br>• the pitch-roll part and `mCurrentStep`.<br>§5.3's quantised row has none of these, and it quantises position and orientation.<br>**(3) Determinism is guaranteed only for identical worlds and call sequences** (Architecture.md:785-842). The prediction world differs (others kinematic, no debris). [verified source; outcome untested] | Zero corrections while driving freely are plausible only if all of the following hold:<br>• the vehicle has the **same BodyID** in both worlds (`CreateBodyWithID`, `BodyInterface.h:52`), as do the static bodies (wheel state stores contact BodyIDs);<br>• restores use **full-precision** body + constraint + controller state, including warm-start impulses, not the quantised §5.3 snapshot;<br>• no other body is within wheel-probe or hull reach.<br>With quantised snapshots, every reconciliation leaves residual error, so ε must sit above the quantisation error. Test with the §6.2 step-4 bit-compare before relying on it. |
| C16 | RenderingServer has `instance_create2`, `instance_set_transform`, `instance_teleport`, `instance_geometry_set_shader_parameter`, `skeleton_*`, `instance_attach_skeleton`; no instance-level physics interpolation | confirmed | **API `RenderingServer` (542 methods):** all named methods are present, plus `skeleton_create` and `skeleton_bone_set_transform`. **Interpolation:** `instance_set_interpolated` and `instance_reset_physics_interpolation` are absent, in 4.7 too. `Node3D.get_global_transform_interpolated` exists [verified]. | RenderingServer does have `multimesh_set_physics_interpolated` and related calls for MultiMesh, so the absence is instance-level only. |
| extra-1 | §3.4: "Damage hits `MaxShieldHealth` first" (reactive armour as a 175-point pool) | corrected | **Community:** the Battlefield wiki "Armor Piercing Shell" page (MediaWiki API wikitext, https://battlefield.fandom.com/api.php?action=parse&page=Armor_Piercing_Shell&prop=wikitext) says all AP shell shots are negated when they hit Reactive Armor, that frontal hits never disable an MBT, and that 2 rear AP rounds kill [report claim].<br>**Arithmetic check:** 300 × 0.9 = 270 is more than the 175 shield, so a subtract-first pool would let 95 through. Rear 270 × 2.75 = 742 ≥ 500 is consistent with "2 rear rounds" [inferred]. | Model the shield as absorbing one whole hit per side (shield > 0 → hit negated, shield consumed), not as a damage pool, until VU measurement says otherwise. |
| extra-2 | §2.0: `VehicleEntityData.UsePrediction = true` taken as evidence that Frostbite predicted vehicles | corrected | `UsePrediction` is declared on `ControllableEntityData` (`C:\Users\olku\VU-Docs\types\fb\ControllableEntityData.yaml`); `VehicleEntityData` inherits it (`VehicleEntityData.yaml:3`). It is `true` even on the deployed M224 mortar (`VV\dumps\weapons__gadgets__m224__m224_deployed.json`) [verified]. | This is a generic controllable flag, weak evidence for a driver-prediction design. |
| extra-3 | §2.8: supersedes the handoff's "XP4/XP5 spawners not found" | confirmed | The handoff says so (`sections\04-gameplay-sim.md:192,483`). `VV\types_all.tsv` shows the XP4/XP5 spawners live in `dynamicspawn_*`, `outpostspawn_*` and `xp5/dynamic_vehiclespawners/*` prefabs, not in level partitions [verified]. | none |
| extra-4 | §1.1/§1.2 small facts: `VehicleBody3D` = Bullet `btRaycastVehicle` port; `max_physics_steps_per_frame` 8; Jolt 5.6.0; `"Dummy"` server | confirmed | All [verified]:<br>• `GS\scene\3d\physics\vehicle_body_3d.cpp`: `btVehicleJacobianEntry` at :38, `intersect_ray` at :485, `_update_suspension` at :545, `_update_friction` at :729;<br>• `GS\core\config\engine.h:74` and `main\main.cpp:2235` (default 8);<br>• `GS\thirdparty\README.md:515`;<br>• `servers\register_server_types.cpp:373`. | none |
| extra-5 | Read-only discipline and core principle | confirmed | **Read-only:**<br>• No file under `C:\Users\olku\iw4L\crates` is newer than 2026-10-09 (`find -newermt`), and there is no `target` dir there. The probe's Cargo.toml uses path dependencies; outputs went to scratch.<br>• No file in `C:\Program Files\EA Games\Battlefield 3` is newer than 2026-10-01.<br>**Core principle:**<br>• Every recommendation keeps converted data under `user://`, keyed by content digest.<br>• VU/BF3 are used only as measurement references.<br>• Nothing patches, repacks or hooks the install.<br>All [verified]. | None found. Path dependencies compile IW4L sources in place without writing there, which satisfies the rule in effect. |

**Not determined by this verification:**
* the 2,048/119 totals, by an independent traversal;
* the XP4 SQDM spawner blueprint;
* the 40mm grid row;
* whether `WorldSpaceLockEfficiency` really means yaw stabilisation in BF3;
* the units of the jet speed axis;
* any Jolt run of the shared-shape design or the prediction replay;
* an EA primary source for "315 km/h": `ea.com/en-gb/news/battlefield3-tips-flying-jets` returned 404.
