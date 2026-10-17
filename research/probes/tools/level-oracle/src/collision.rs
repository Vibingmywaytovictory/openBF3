//! `collision <LEVEL> <MODE>`: the collision soup of one level x mode through IW4L's own pieces,
//! the parity oracle of the C++ `bf3_core` collision soup (`bf3dump collision`, ctest
//! `core.level.collision`, the Godot `--collision-check`).
//!
//! Placements are the `C` lines of the level listing: each static model group's Havok res at the
//! group transform (IW4L collision/mod.rs `frame_of(t, 1)`), each non-group mesh object's
//! `PhysicsEntityData` asset that `pick_asset` chooses, at `frame_of(t, asset scale)`. Objects the
//! level starts hidden (`starts_hidden`) are listed but not built. The leaves of every root shape
//! are found like IW4L collision/havok.rs `leaves` (Mopp / List / ConvexTranslate /
//! ConvexTransform / ExtendedMesh shape subparts, then its triangle subparts) and each one goes
//! through `resource_bf3::triangles` (a TrianglesSubpart through the walk of havok.rs
//! `subpart_mesh`). Per triangle, as IW4L collision/mod.rs and havok.rs `decode` do:
//!   material = MaterialFlagsAndIndices[index]: mesh triangles by their stored index, convex leaves
//!              by user_data bits 8..15 when in range, else MaterialIndices[first part + part];
//!   part     = mesh: the chunk's part (0 for a triangle subpart); convex: user_data & 0xFF;
//!   dropped  when the owner's PhysicsPartInfos[part].HealthStateIndex is not 0 (a destroyed
//!            state; the owner is the object, or the group member placed at the leaf's subpart);
//!   surface  = level_bf3::collision::Materials (the grid row voted from its relations' asset
//!            names), else the owner mesh name's word (foliage -> bark unless see-through), else
//!            concrete;
//!   body     = 0 collision, 1 raycast, 2 character (RigidBodies[root].RigidBodyType; groups 0);
//!   flags    = 1 see-through (material bit 27) | 2 invisible blocker (mesh name has
//!            "invisible" or "collisionmesh").
//! Terrain (`terrain`): each `TerrainEntityData`'s streaming tree heightfield through
//! `TerrainStreamingTree::height_grid` at the first depth whose spacing reaches 2 m (IW4L
//! collision/terrain.rs's depth rule, without its near-area cap, block merging and paved decals);
//! every cell (whose centre has a sane 64 m tile) with no hole corner, as two triangles faced +Y:
//! material 0, surface dirt, body 0, flags 4 (openBF3 TC, docs/design/terrain-collision.md §6:
//! terrain collision everywhere; before TC only the cells of the tiles that held a Havok triangle
//! or touched one, IW4L's near-object rule).
//! Unlike IW4L's IW soup nothing is welded, re-wound, cropped or turned into brushes, and foliage
//! is kept (tagged by its surface): this is the Frostbite-space soup a Jolt static world is built
//! from. Quantisation: every coordinate to whole millimetres (`(f64(c) * 1000).round()`); a
//! triangle with a non-finite or |c| > 1e7 m coordinate is dropped (`nonfinite`), one with two
//! equal quantised corners too (`degenerate`). Triangle hash: FNV-1a 64 over the nine i64
//! millimetre values, the u32 material (little endian), then surface, body and flags bytes.
//! Placement hash: FNV-1a over its triangles in walk order (the same byte stream). Tile (64 m x
//! 64 m over X/Z) of a triangle: floor((qx0 + qx1 + qx2) / 192000), the same over Z. Tile and
//! total sums are wrapping u64 sums of the triangle hashes, so they do not depend on order.
//!
//! Lines (sorted bytewise), then the totals:
//!   T group <index> res <name> tris <n> destroyed <n> degenerate <n> nonfinite <n> hash <16 hex>
//!   T object <partition#index> res <name|-> <built|hidden|missing|nores> tris ... hash ...
//!   T terrain <partition#index> res <name|-> <built|missing|nores> depth <d> side <n> holes <n> cells <n> open <n> tris ... hash ...
//!   K <tile x> <tile z> tris <n> sum <16 hex>
//!   soup placements <n> built <n> hidden <n> missing <n> triangles <n> terrain <n> destroyed <n> degenerate <n> nonfinite <n> tiles <n> sum <16 hex>
//!   surface <id> <triangles>   (non-zero only)
//!   body <id> <triangles>

use std::collections::{BTreeMap, HashMap};
use std::fmt::Write as _;
use std::sync::Arc;

use bf3_install::Bf3Install;
use ebx_bf3::{EbxDb, Instance};
use level_bf3::collision::{Lookup, Materials, classify, surface};
use level_bf3::{GameMode, LevelSource, LoadOptions, World};
use resource_bf3::{HavokPhysicsData, HeightGrid, Shape, TerrainStreamingTree, Transform as Frame, TrianglesSubpart, terrain_streaming_tree_name};

use super::{Mount, fail, id, mount, parallel, pick_asset, starts_hidden};

const MAX_DEPTH: u32 = 64;
const SEE_THROUGH: u32 = 0x0800_0000;
const TILE_MM_X3: i64 = 64 * 1000 * 3;
const MAX_COORD: f64 = 1e7;
const TERRAIN_SPACING: f32 = 2.0;
const MAX_SIDE: usize = 4097;
const TERRAIN_FLAG: u8 = 4;
const TILE_M: f64 = 64.0;

struct LeafRef<'a> {
    shape: Option<&'a Shape>,
    triangles: Option<&'a TrianglesSubpart>,
    frame: Frame,
    subpart: Option<u32>,
    first_part: u32,
}

fn leaves<'a>(shape: &'a Shape, frame: &Frame, subpart: Option<(u32, u32)>, depth: u32, out: &mut Vec<LeafRef<'a>>) {
    if depth > MAX_DEPTH {
        return;
    }
    match shape {
        Shape::MoppBvTree { child } => leaves(child, frame, subpart, depth + 1, out),
        Shape::List { children, .. } => {
            for child in children {
                leaves(child, frame, subpart, depth + 1, out);
            }
        }
        Shape::ConvexTranslate { translation, child } => leaves(
            child,
            &frame.then_inner(&Frame::from_translation(*translation)),
            subpart,
            depth + 1,
            out,
        ),
        Shape::ConvexTransform { transform, child } => leaves(child, &frame.then_inner(transform), subpart, depth + 1, out),
        Shape::ExtendedMesh(mesh) => {
            for (index, part) in mesh.shape_subparts.iter().enumerate() {
                let placed = frame.then_inner(&part.transform());
                for child in &part.children {
                    leaves(child, &placed, Some((index as u32, part.user_data)), depth + 1, out);
                }
            }
            for part in &mesh.triangle_subparts {
                out.push(LeafRef {
                    shape: None,
                    triangles: Some(part),
                    frame: *frame,
                    subpart: subpart.map(|(index, _)| index),
                    first_part: subpart.map_or(0, |(_, first)| first),
                });
            }
        }
        Shape::Unsupported { .. } => {}
        _ => out.push(LeafRef {
            shape: Some(shape),
            triangles: None,
            frame: *frame,
            subpart: subpart.map(|(index, _)| index),
            first_part: subpart.map_or(0, |(_, first)| first),
        }),
    }
}

/// One raw triangle of a leaf: corners, material value, physics part.
struct Raw {
    v: [[f32; 3]; 3],
    material: u32,
    part: u32,
}

fn leaf_triangles(leaf: &LeafRef<'_>, havok: &HavokPhysicsData) -> Vec<Raw> {
    let material = |index: u32| havok.material_flags_and_indices.get(index as usize).copied().unwrap_or(0);
    if let Some(subpart) = leaf.triangles {
        let placed = leaf.frame.then_inner(&subpart.transform);
        let mut out = Vec::new();
        for (index, triangle) in subpart.triangles.iter().enumerate() {
            let corners = triangle.map(|v| subpart.vertices.get(v as usize).copied());
            if let [Some(a), Some(b), Some(c)] = corners {
                let value = subpart.materials.get(index).copied().unwrap_or(0);
                out.push(Raw {
                    v: [a, b, c].map(|p| placed.transform_point(p)),
                    material: material(value),
                    part: 0,
                });
            }
        }
        return out;
    }
    let shape = leaf.shape.expect("leaf");
    let triangles = resource_bf3::triangles(shape, &leaf.frame);
    if let Shape::CompressedMesh(_) = shape {
        return triangles
            .iter()
            .map(|t| Raw {
                v: t.v,
                material: material(t.material),
                part: t.part,
            })
            .collect();
    }
    let user_data = match shape {
        Shape::Box { user_data, .. }
        | Shape::Sphere { user_data, .. }
        | Shape::Capsule { user_data, .. }
        | Shape::Cylinder { user_data, .. }
        | Shape::ConvexVertices { user_data, .. } => *user_data,
        _ => return Vec::new(),
    };
    let part = user_data & 0xFF;
    let local = (user_data >> 8) & 0xFF;
    let index = if (local as usize) < havok.material_flags_and_indices.len() {
        local
    } else {
        havok
            .material_indices
            .get((leaf.first_part + part) as usize)
            .map_or(0, |&index| u32::from(index))
    };
    let value = material(index);
    triangles
        .iter()
        .map(|t| Raw {
            v: t.v,
            material: value,
            part,
        })
        .collect()
}

fn fnv(hash: &mut u64, bytes: &[u8]) {
    for &b in bytes {
        *hash ^= u64::from(b);
        *hash = hash.wrapping_mul(0x0000_0100_0000_01B3);
    }
}

const FNV_OFFSET: u64 = 0xcbf2_9ce4_8422_2325;

#[derive(Default)]
struct Built {
    tris: usize,
    destroyed: usize,
    degenerate: usize,
    nonfinite: usize,
    hash: u64,
    tiles: Vec<((i32, i32), (usize, u64))>,
    surfaces: [usize; 32],
    bodies: [usize; 3],
}

struct Owner<'a> {
    entity: Option<Instance<'a>>,
    name: &'a str,
}

fn intact(entity: Option<Instance<'_>>, part: u32, cache: &mut HashMap<(ebx_bf3::Guid, u32), Vec<bool>>) -> bool {
    let Some(entity) = entity else {
        return true;
    };
    let states = cache.entry((entity.partition().guid(), entity.index())).or_insert_with(|| {
        entity
            .get("PhysicsPartInfos")
            .and_then(|value| value.as_array())
            .into_iter()
            .flatten()
            .map(|info| info.get("HealthStateIndex").and_then(|value| value.as_i64()).is_none_or(|state| state == 0))
            .collect()
    });
    states.get(part as usize).copied().unwrap_or(true)
}

fn invisible(name: &str) -> bool {
    let name = name.to_ascii_lowercase();
    name.contains("invisible") || name.contains("collisionmesh")
}

fn surface_of(materials: &Materials, value: u32, name: &str) -> u8 {
    match materials.lookup(value) {
        Lookup::Surface(s) => s,
        _ => match classify(name) {
            Some(surface::FOLIAGE) if value & SEE_THROUGH == 0 => surface::BARK,
            Some(s) => s,
            None => surface::CONCRETE,
        },
    }
}

/// Builds one placement: `roots` with their body kinds; `owner_of(subpart)` names the owner.
fn build<'a>(
    havok: &HavokPhysicsData,
    shapes: &[Shape],
    bodies: &[u8],
    frame: &Frame,
    owner_of: &dyn Fn(Option<u32>) -> Owner<'a>,
    materials: &Materials,
) -> Built {
    let mut built = Built {
        hash: FNV_OFFSET,
        ..Built::default()
    };
    let mut cache = HashMap::new();
    let mut tiles: BTreeMap<(i32, i32), (usize, u64)> = BTreeMap::new();
    for (root, shape) in shapes.iter().enumerate() {
        let body = bodies.get(root).copied().unwrap_or(0);
        let mut refs = Vec::new();
        leaves(shape, frame, None, 0, &mut refs);
        for leaf in &refs {
            let owner = owner_of(leaf.subpart);
            let blocker = invisible(owner.name);
            for raw in leaf_triangles(leaf, havok) {
                if !intact(owner.entity, raw.part, &mut cache) {
                    built.destroyed += 1;
                    continue;
                }
                let surface = surface_of(materials, raw.material, owner.name);
                let flags = u8::from(raw.material & SEE_THROUGH != 0) | (u8::from(blocker) << 1);
                add(&mut built, &mut tiles, &raw.v, raw.material, surface, body, flags);
            }
        }
    }
    built.tiles = tiles.into_iter().collect();
    built
}

/// Quantises, checks and hashes one kept triangle into `built` and its tile.
fn add(built: &mut Built, tiles: &mut BTreeMap<(i32, i32), (usize, u64)>, v: &[[f32; 3]; 3], material: u32, surface: u8, body: u8, flags: u8) {
    let mut q = [0i64; 9];
    for (k, c) in v.iter().flatten().enumerate() {
        let c = f64::from(*c);
        if !c.is_finite() || c.abs() > MAX_COORD {
            built.nonfinite += 1;
            return;
        }
        q[k] = (c * 1000.0).round() as i64;
    }
    let corner = |k: usize| [q[k * 3], q[k * 3 + 1], q[k * 3 + 2]];
    if corner(0) == corner(1) || corner(1) == corner(2) || corner(0) == corner(2) {
        built.degenerate += 1;
        return;
    }
    let mut bytes = Vec::with_capacity(9 * 8 + 7);
    for v in q {
        bytes.extend_from_slice(&v.to_le_bytes());
    }
    bytes.extend_from_slice(&material.to_le_bytes());
    bytes.extend_from_slice(&[surface, body, flags]);
    let mut tri_hash = FNV_OFFSET;
    fnv(&mut tri_hash, &bytes);
    fnv(&mut built.hash, &bytes);
    let tx = (q[0] + q[3] + q[6]).div_euclid(TILE_MM_X3) as i32;
    let tz = (q[2] + q[5] + q[8]).div_euclid(TILE_MM_X3) as i32;
    let tile = tiles.entry((tx, tz)).or_insert((0, 0));
    tile.0 += 1;
    tile.1 = tile.1.wrapping_add(tri_hash);
    built.tris += 1;
    built.surfaces[usize::from(surface & 31)] += 1;
    built.bodies[usize::from(body.min(2))] += 1;
}

fn body_kinds(db: &EbxDb, physics: Option<Instance<'_>>) -> Vec<u8> {
    let Some(physics) = physics else {
        return Vec::new();
    };
    db.follow_all(physics, "RigidBodies")
        .map(|body| match body.get("RigidBodyType").and_then(|value| value.enum_name()) {
            Some("RBTypeRaycast") => 1,
            Some("RBTypeCharacter") => 2,
            _ => 0,
        })
        .collect()
}

fn frame_of(transform: &level_bf3::Transform, asset_scale: f32) -> Frame {
    let factor = transform.scale / asset_scale;
    Frame {
        axis: [transform.right, transform.up, transform.forward].map(|row| row.map(|v| v * factor)),
        origin: transform.trans,
    }
}

/// The terrain grid of a `TerrainEntityData`: its streaming tree's heightfield at the depth whose
/// spacing first reaches TERRAIN_SPACING (IW4L collision/terrain.rs, without its near-area cap and
/// block merging).
fn terrain_grid(source: &Mount, asset: &str) -> Result<(u8, HeightGrid), String> {
    let (_, bytes) = source.read_res(asset).map_err(|error| format!("res {asset}: {error}"))?;
    let tree_name = terrain_streaming_tree_name(&bytes).map_err(|error| format!("res {asset}: {error}"))?;
    let (_, tree_bytes) = source.read_res(tree_name).map_err(|error| format!("res {tree_name}: {error}"))?;
    let tree = TerrainStreamingTree::parse(&tree_bytes).map_err(|error| format!("res {tree_name}: {error}"))?;
    let Some(heightfield) = &tree.heightfield else {
        return Err(format!("res {tree_name}: streaming tree without a heightfield"));
    };
    let Some(root) = heightfield.nodes.first() else {
        return Err(format!("res {tree_name}: empty heightfield tree"));
    };
    let cells = heightfield.interior_cells() as usize;
    let span = (root.bounds_max[0] - root.bounds_min[0]).max(root.bounds_max[2] - root.bounds_min[2]);
    let mut depth = 0u8;
    while depth < heightfield.max_level() && span / (cells << depth) as f32 > TERRAIN_SPACING * 1.001 && (cells << (depth + 1)) < MAX_SIDE {
        depth += 1;
    }
    let grid = tree
        .height_grid(depth, |chunk| source.read_chunk(&chunk).ok())
        .map_err(|error| format!("res {tree_name}: {error}"))?;
    Ok((depth, grid))
}

/// Tile index of a cell centre; None when it is not a sane coordinate.
fn centre_tile(origin: f32, spacing: f32, index: usize) -> Option<i32> {
    let centre = f64::from(origin) + (index as f64 + 0.5) * f64::from(spacing);
    let tile = (centre / TILE_M).floor();
    (tile.abs() < 1e7).then_some(tile as i32)
}

/// Winds a terrain triangle so that it faces +Y (Havok's counter-clockwise front).
fn face_up(t: [[f32; 3]; 3]) -> [[f32; 3]; 3] {
    let [a, b, c] = t;
    let u = [b[0] - a[0], b[1] - a[1], b[2] - a[2]];
    let v = [c[0] - a[0], c[1] - a[1], c[2] - a[2]];
    if u[2] * v[0] - u[0] * v[2] < 0.0 { [a, c, b] } else { t }
}

/// One terrain object: every grid cell none of whose four samples is a hole (TC: the whole grid), as two triangles (a, d, b) and (a, c, d) of its corners a = (x, z),
/// b = (x + 1, z), c = (x, z + 1), d = (x + 1, z + 1), faced up, at the object's frame; rows of
/// cells along +Z, cells along +X. Material 0, surface dirt, body collision, flags 4.
fn terrain(source: &Mount, db: &EbxDb, object: &level_bf3::PlacedObject<'_>, label: &str) -> (String, Option<Built>) {
    let Some(asset) = db.follow(object.entity, "TerrainAsset").and_then(|asset| asset.get("Name")?.as_str()) else {
        return (format!("T terrain {label} res - nores"), None);
    };
    let (depth, grid) = match terrain_grid(source, asset) {
        Ok(found) => found,
        Err(error) => {
            eprintln!("terrain missing: {error}");
            return (format!("T terrain {label} res {asset} missing"), None);
        }
    };
    let frame = frame_of(&object.transform, 1.0);
    let mut built = Built {
        hash: FNV_OFFSET,
        ..Built::default()
    };
    let mut tiles: BTreeMap<(i32, i32), (usize, u64)> = BTreeMap::new();
    let (mut cells, mut open) = (0usize, 0usize);
    let [ox, oz] = grid.origin_xz;
    let [sx, sz] = grid.sample_spacing;
    for z in 0..grid.height.saturating_sub(1) {
        let Some(_) = centre_tile(oz, sz, z) else {
            continue;
        };
        for x in 0..grid.width.saturating_sub(1) {
            let Some(_) = centre_tile(ox, sx, x) else {
                continue;
            };
            cells += 1;
            let hole = |i: usize, j: usize| grid.holes[(x + i) + (z + j) * grid.width];
            if hole(0, 0) || hole(1, 0) || hole(0, 1) || hole(1, 1) {
                open += 1;
                continue;
            }
            let point = |i: usize, j: usize| frame.transform_point([ox + (x + i) as f32 * sx, grid.at(x + i, z + j), oz + (z + j) as f32 * sz]);
            let (a, b, c, d) = (point(0, 0), point(1, 0), point(0, 1), point(1, 1));
            for t in [[a, d, b], [a, c, d]] {
                add(&mut built, &mut tiles, &face_up(t), 0, surface::DIRT, 0, TERRAIN_FLAG);
            }
        }
    }
    built.tiles = tiles.into_iter().collect();
    let holes = grid.holes.iter().filter(|&&hole| hole).count();
    let line = format!(
        "T terrain {label} res {asset} built depth {depth} side {} holes {holes} cells {cells} open {open} tris {} destroyed 0 degenerate {} nonfinite {} hash {:016x}",
        grid.width, built.tris, built.degenerate, built.nonfinite, built.hash
    );
    (line, Some(built))
}

pub fn run(install: &Arc<Bf3Install>, level: &str, mode: &str) -> String {
    let source = Mount(mount(install, level));
    let loaded = level_bf3::load_level(&source, level, GameMode::from_id(mode), &LoadOptions::default())
        .unwrap_or_else(|error| fail(&error.to_string()));
    let world = World::traverse(&loaded, &source).unwrap_or_else(|error| fail(&error.to_string()));
    let db = &loaded.db;
    let materials = Materials::read(db, loaded.root());

    let mut lines = Vec::new();
    let mut tiles: BTreeMap<(i32, i32), (usize, u64)> = BTreeMap::new();
    let mut totals = Built::default();
    let (mut placements, mut built_count, mut hidden, mut missing) = (0usize, 0usize, 0usize, 0usize);
    let absorb = |built: &Built, totals: &mut Built, tiles: &mut BTreeMap<(i32, i32), (usize, u64)>| {
        totals.tris += built.tris;
        totals.destroyed += built.destroyed;
        totals.degenerate += built.degenerate;
        totals.nonfinite += built.nonfinite;
        for k in 0..32 {
            totals.surfaces[k] += built.surfaces[k];
        }
        for k in 0..3 {
            totals.bodies[k] += built.bodies[k];
        }
        for &(key, (count, sum)) in &built.tiles {
            let tile = tiles.entry(key).or_insert((0, 0));
            tile.0 += count;
            tile.1 = tile.1.wrapping_add(sum);
        }
    };
    let counts = |built: &Built| {
        format!(
            "tris {} destroyed {} degenerate {} nonfinite {} hash {:016x}",
            built.tris, built.destroyed, built.degenerate, built.nonfinite, built.hash
        )
    };

    // Groups: members by Havok subpart (a later member wins, as in IW4L's HashMap insert).
    for (at, group) in world.groups.iter().enumerate() {
        let (Some(name), Some(havok)) = (&group.physics_asset, &group.havok) else {
            continue;
        };
        placements += 1;
        built_count += 1;
        let shapes = havok.shapes().unwrap_or_else(|error| fail(&format!("group {at}: {error}")));
        let mut members: HashMap<u32, usize> = HashMap::new();
        for (placed, object) in world.objects.iter().enumerate() {
            if let Some(slot) = object.group
                && slot.group as usize == at
                && let Some(subpart) = slot.havok_subpart
            {
                members.insert(subpart, placed);
            }
        }
        let owner_of = |subpart: Option<u32>| {
            let member = subpart.and_then(|s| members.get(&s)).map(|&placed| &world.objects[placed]);
            Owner {
                entity: member.map(|object| object.entity),
                name: member.and_then(|object| object.kind.mesh()).map_or("", |mesh| mesh.name()),
            }
        };
        let built = build(havok, &shapes, &[], &frame_of(&group.transform, 1.0), &owner_of, &materials);
        lines.push(format!("T group {at} res {name} {}", counts(&built)));
        absorb(&built, &mut totals, &mut tiles);
    }

    // Objects: the chosen asset, read and decoded once per name on all cores.
    struct Job<'a> {
        at: usize,
        name: Option<(&'a str, f32)>,
        hidden: bool,
    }
    let mut jobs = Vec::new();
    for (at, object) in world.objects.iter().enumerate() {
        if object.group.is_some() {
            continue;
        }
        let Some(mesh) = object.kind.mesh() else {
            continue;
        };
        let Some(physics) = mesh.physics else {
            continue;
        };
        jobs.push(Job {
            at,
            name: pick_asset(&source, db, physics, &object.transform),
            hidden: starts_hidden(&world, object),
        });
    }
    let mut names: Vec<&str> = jobs.iter().filter(|job| !job.hidden).filter_map(|job| job.name.map(|(name, _)| name)).collect();
    names.sort_unstable();
    names.dedup();
    let decoded = parallel(&names, |name| {
        let (info, bytes) = source.read_res(name).map_err(|error| error.to_string())?;
        let havok = HavokPhysicsData::parse(&bytes, &info.res_meta).map_err(|error| error.to_string())?;
        let shapes = havok.shapes().map_err(|error| error.to_string())?;
        Ok::<_, String>((havok, shapes))
    });
    let assets: HashMap<&str, &Result<(HavokPhysicsData, Vec<Shape>), String>> = names.iter().copied().zip(decoded.iter()).collect();
    let results = parallel(&jobs, |job| {
        let object = &world.objects[job.at];
        let mesh = object.kind.mesh().expect("mesh");
        let label = id(db, object.entity);
        let Some((name, scale)) = job.name else {
            return (format!("T object {label} res - nores"), None);
        };
        if job.hidden {
            return (format!("T object {label} res {name} hidden"), None);
        }
        let Some(Ok((havok, shapes))) = assets.get(name) else {
            return (format!("T object {label} res {name} missing"), None);
        };
        let bodies = body_kinds(db, mesh.physics);
        let owner_of = |_: Option<u32>| Owner {
            entity: Some(object.entity),
            name: mesh.name(),
        };
        let built = build(havok, shapes, &bodies, &frame_of(&object.transform, scale), &owner_of, &materials);
        (format!("T object {label} res {name} built {}", counts(&built)), Some(built))
    });
    for (line, built) in results {
        placements += 1;
        match &built {
            Some(built) => {
                built_count += 1;
                absorb(built, &mut totals, &mut tiles);
            }
            None if line.ends_with(" hidden") => hidden += 1,
            None if line.ends_with(" missing") => missing += 1,
            None => {}
        }
        lines.push(line);
    }
    // Terrain: every heightfield cell (TC).
    let mut terrain_tris = 0usize;
    for object in &world.objects {
        if !matches!(object.kind, level_bf3::ObjectKind::Terrain) {
            continue;
        }
        let (line, built) = terrain(&source, db, object, &id(db, object.entity));
        if let Some(built) = &built {
            terrain_tris += built.tris;
            absorb(built, &mut totals, &mut tiles);
        }
        lines.push(line);
    }
    let mut out = String::new();
    lines.sort();
    for line in &lines {
        out.push_str(line);
        out.push('\n');
    }
    let mut sum = 0u64;
    for (&(tx, tz), &(count, tile_sum)) in &tiles {
        let _ = writeln!(out, "K {tx} {tz} tris {count} sum {tile_sum:016x}");
        sum = sum.wrapping_add(tile_sum);
    }
    let _ = writeln!(
        out,
        "soup placements {placements} built {built_count} hidden {hidden} missing {missing} triangles {} terrain {terrain_tris} destroyed {} degenerate {} nonfinite {} tiles {} sum {sum:016x}",
        totals.tris,
        totals.destroyed,
        totals.degenerate,
        totals.nonfinite,
        tiles.len()
    );
    for (k, &n) in totals.surfaces.iter().enumerate() {
        if n > 0 {
            let _ = writeln!(out, "surface {k} {n}");
        }
    }
    for (k, &n) in totals.bodies.iter().enumerate() {
        let _ = writeln!(out, "body {k} {n}");
    }
    out
}
