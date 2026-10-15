#pragma once
// bf3_sim internal: the process-wide Jolt runtime (default allocator hooks, trace hook, the type factory and the
// type registry), reference counted (docs/design/m1.md §5.6). PhysicsWorld, every StaticWorldShapes and every
// CollisionWorld hold a reference, so the factory and the allocator outlive every Jolt object of the process.
// Thread-safe: acquire and release may run on any thread.

#include <memory>

namespace bf3::sim {

// Acquires a reference (the first one sets Jolt up); the returned handle releases it when the last copy goes
// (the last release tears Jolt's factory and registry down). Never null.
std::shared_ptr<void> jolt_runtime_acquire();

} // namespace bf3::sim
