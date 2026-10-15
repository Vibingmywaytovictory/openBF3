//! level-oracle: parity oracle for the openBF3 C++ level traversal (bf3_core/level, `bf3dump level`).
//! Reads the install read-only through the ebx-dump readers and traverses one level x mode with
//! IW4L's level_bf3 (World::traverse), then prints the canonical listing `bf3dump level` prints.
//!
//!   level <LEVEL> <MODE> [--legacy-mvdb]
//!                           canonical listing on stdout (see below)
//!   modes <LEVEL>           the modes LevelDescriptionAsset lists, one per line
//!   levels                  every MP level id of the install, one per line
//!
//! Listing (every section sorted bytewise; floats as f32 bit patterns, 8 lowercase hex digits):
//!   level / bundle / settings / subworld-bundle / absent / partitions / world / skipped lines,
//!   `S` sub-worlds, `G` static model groups, `P` placements (every placed object), `V` mesh
//!   variants of the mesh census, `C` collision placements, `census` / `collision` totals.
//!
//! The mesh census is the geometry builder's definition when the handoff's Metro and Bazaar counts
//! were taken (IW4L bab7c1a, geometry/mod.rs `variants` + `models`): a scene mesh per (MeshAsset,
//! MeshVariationDatabase entry, hidden composite parts, bake of a non-uniform transform); a mesh
//! that draws no triangle (undrawn shaders, ZOnly subsets, hidden parts) has no instance; world
//! kind = placed at most 4 times with a placed bounding radius of at least 16 m; terrain and water
//! each add one world mesh and instance. `--legacy-mvdb` looks variations up as IW4L did then (a
//! variation entry with an empty `Materials` list still counts as the mesh's entry).

use std::collections::{BTreeMap, HashMap};
use std::fmt::Write as _;
use std::path::Path;
use std::sync::Arc;

use bf3_install::{Bf3Install, Guid as InstallGuid, LevelMount};
use ebx_bf3::{EbxDb, Instance, Struct};
use level_bf3::{
    ChunkId, GameMode, LevelSource, LoadOptions, MeshEntityType, ObjectKind, PlacedObject,
    ResInfo, SourceError, Transform, World, field_id,
};
use resource_bf3::{GeometrySource, MeshSet, SubsetFilter};

const ROOT: &str = r"C:\Program Files\EA Games\Battlefield 3";

const MAX_LODS: usize = 4;
const WORLD_RADIUS: f32 = 16.0;
const WORLD_MAX_PLACEMENTS: usize = 4;
const UNDRAWN_SHADERS: [&str; 5] = [
    "invisible",
    "occluder",
    "shadowmesh",
    "ui_characterbackdrop",
    "ui_menu",
];

struct Mount(LevelMount);

impl LevelSource for Mount {
    fn bundle_names(&self) -> Vec<String> {
        self.0.bundle_names()
    }
    fn load_bundles(&self, names: &[String]) -> Result<(), SourceError> {
        Ok(self.0.load_bundles(names)?)
    }
    fn ebx_names(&self) -> Vec<String> {
        self.0.ebx_names()
    }
    fn read_ebx(&self, name: &str) -> Result<Vec<u8>, SourceError> {
        Ok(self.0.read_ebx(name)?)
    }
    fn res_names(&self) -> Vec<String> {
        self.0.res_names()
    }
    fn res_info(&self, name: &str) -> Option<ResInfo> {
        self.0.res_info(name).map(|info| ResInfo {
            res_type: info.res_type,
            res_meta: info.res_meta,
        })
    }
    fn read_res(&self, name: &str) -> Result<(ResInfo, Vec<u8>), SourceError> {
        let (info, bytes) = self.0.read_res(name)?;
        Ok((
            ResInfo {
                res_type: info.res_type,
                res_meta: info.res_meta,
            },
            bytes,
        ))
    }
    fn read_chunk(&self, id: &ChunkId) -> Result<Vec<u8>, SourceError> {
        Ok(self.0.read_chunk(&InstallGuid(id.0))?)
    }
    fn chunk_first_mip(&self, owner: &str) -> Option<u32> {
        self.0.asset_chunk_meta(owner)?.first_mip
    }
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let install = Bf3Install::open(Path::new(ROOT)).unwrap_or_else(|error| fail(&error.to_string()));
    match (args.get(1).map(String::as_str), args.len()) {
        (Some("level"), 4) => print!("{}", listing(&install, &args[2], &args[3], false)),
        (Some("level"), 5) if args[4] == "--legacy-mvdb" => {
            print!("{}", listing(&install, &args[2], &args[3], true));
        }
        (Some("modes"), 3) => {
            let mount = Mount(mount(&install, &args[2]));
            let info = level_bf3::level_info(&mount, &args[2])
                .unwrap_or_else(|error| fail(&error.to_string()));
            for mode in info.modes {
                println!("{mode}");
            }
        }
        (Some("levels"), 2) => {
            for level in install.levels() {
                println!("{}", level.id);
            }
        }
        _ => fail("usage: level-oracle level <LEVEL> <MODE> | modes <LEVEL> | levels"),
    }
}

fn fail(message: &str) -> ! {
    eprintln!("level-oracle: {message}");
    std::process::exit(1);
}

fn mount(install: &Arc<Bf3Install>, level: &str) -> LevelMount {
    install
        .mount_level(level)
        .unwrap_or_else(|error| fail(&error.to_string()))
}

fn hex(value: f32) -> String {
    format!("{:08x}", value.to_bits())
}

fn transform_hex(t: &Transform) -> String {
    let mut out = String::new();
    for row in t.rows() {
        for v in row {
            out.push_str(&hex(v));
            out.push(' ');
        }
    }
    out.push_str(&hex(t.scale));
    out
}

fn bundle_key(name: &str) -> String {
    let key = name.replace('\\', "/").to_ascii_lowercase();
    let key = key.trim_start_matches('/');
    key.strip_prefix("win32/").unwrap_or(key).to_owned()
}

/// `<partition name>#<instance index>`.
fn id(db: &EbxDb, instance: Instance<'_>) -> String {
    let name = db.name_of(&instance.partition().guid()).unwrap_or("?");
    format!("{name}#{}", instance.index())
}

fn listing(install: &Arc<Bf3Install>, level: &str, mode: &str, legacy_mvdb: bool) -> String {
    let source = Mount(mount(install, level));
    let loaded = level_bf3::load_level(&source, level, GameMode::from_id(mode), &LoadOptions::default())
        .unwrap_or_else(|error| fail(&error.to_string()));
    let world = World::traverse(&loaded, &source).unwrap_or_else(|error| fail(&error.to_string()));
    let db = &loaded.db;

    let mut out = String::new();
    let _ = writeln!(out, "level {} mode {}", loaded.info.id, mode);
    let _ = writeln!(out, "bundle {}", bundle_key(&loaded.bundles.level));
    for name in &loaded.bundles.settings {
        let _ = writeln!(out, "settings {}", bundle_key(name));
    }
    for name in &loaded.bundles.subworlds {
        let _ = writeln!(out, "subworld-bundle {}", bundle_key(name));
    }
    for name in &loaded.bundles.absent {
        let _ = writeln!(out, "absent {name}");
    }
    let stats = &loaded.stats;
    let _ = writeln!(
        out,
        "partitions {} duplicates {} failed {} imports {} unresolved {}",
        stats.partitions,
        stats.duplicates,
        stats.failed.len(),
        stats.imports,
        stats.unresolved_imports
    );
    let ws = &world.stats;
    let _ = writeln!(
        out,
        "world objects {} subworlds {} scopes {} groups {} excluded {} excluded_overrides {} transform_overrides {} unresolved_objects {} missing_blueprints {} depth_limited {}",
        world.objects.len(),
        world.subworlds.len(),
        world.scopes.len(),
        world.groups.len(),
        ws.excluded,
        ws.excluded_overrides,
        ws.transform_overrides,
        ws.unresolved_objects,
        ws.missing_blueprints,
        ws.depth_limited
    );
    let mut lines = Vec::new();
    for name in &ws.skipped_subworlds {
        lines.push(format!("skipped {name}"));
    }
    for name in &ws.absent_subworlds {
        lines.push(format!("absent-subworld {name}"));
    }
    flush(&mut out, lines);

    let mut lines = Vec::new();
    for (at, subworld) in world.subworlds.iter().enumerate() {
        lines.push(format!(
            "S {at} {} parent {} mvdb {}",
            subworld.name,
            subworld.parent.map_or("-".to_owned(), |p| p.to_string()),
            subworld.variations.len()
        ));
    }
    flush(&mut out, lines);

    let mut lines = Vec::new();
    for (at, group) in world.groups.iter().enumerate() {
        lines.push(format!(
            "G {at} sw {} res {} members {} instances {} explicit {} havok {} subparts {} mismatches {} t {}",
            group.subworld,
            group.physics_asset.as_deref().unwrap_or("-"),
            group.members,
            group.instances,
            group.explicit,
            group.havok_placed,
            group.subparts,
            group.part_mismatches,
            transform_hex(&group.transform)
        ));
    }
    flush(&mut out, lines);

    let mut lines = Vec::new();
    for object in &world.objects {
        lines.push(placement_line(db, &world, object));
    }
    flush(&mut out, lines);

    let census = census(&source, db, &world, legacy_mvdb);
    flush(&mut out, census.lines);
    let _ = writeln!(
        out,
        "census placements {} variants {} meshes {} model {} world {} instances {} failures {}",
        census.placements,
        census.variants,
        census.meshes,
        census.model,
        census.world,
        census.instances,
        census.failures
    );

    let (lines, groups, objects) = collision(&source, db, &world);
    flush(&mut out, lines);
    let _ = writeln!(out, "collision groups {groups} objects {objects}");
    out
}

fn flush(out: &mut String, mut lines: Vec<String>) {
    lines.sort();
    for line in lines {
        out.push_str(&line);
        out.push('\n');
    }
}

fn entity_type_label(entity_type: MeshEntityType) -> &'static str {
    match entity_type {
        MeshEntityType::StaticModel => "StaticModel",
        MeshEntityType::MeshProxy => "MeshProxy",
        MeshEntityType::Breakable => "Breakable",
        MeshEntityType::Dynamic => "Dynamic",
        MeshEntityType::VegetationTree => "VegetationTree",
        MeshEntityType::GroupMember => "GroupMember",
        MeshEntityType::Banger => "Banger",
        MeshEntityType::Vehicle => "Vehicle",
        MeshEntityType::Other => "Other",
    }
}

fn placement_line(db: &EbxDb, world: &World<'_>, object: &PlacedObject<'_>) -> String {
    let mut line = format!(
        "P {} {} {} ref {} sw {} var {:08x}",
        object.kind.label(),
        id(db, object.entity),
        object.entity.type_name(),
        object.reference.map_or("-".to_owned(), |r| id(db, r)),
        object.subworld,
        object.variation
    );
    match object.group {
        Some(slot) => {
            let _ = write!(
                line,
                " grp {}/{}/{}/{}/{}",
                slot.group,
                slot.member,
                slot.instance,
                slot.havok_subpart.map_or("-".to_owned(), |s| s.to_string()),
                slot.physics_part.map_or("-".to_owned(), |s| s.to_string())
            );
        }
        None => line.push_str(" grp -"),
    }
    match object.kind {
        ObjectKind::Mesh(mesh) => {
            let _ = write!(
                line,
                " mesh {} {} phys {} hid {}",
                id(db, mesh.asset),
                entity_type_label(mesh.entity_type),
                mesh.physics.map_or("-".to_owned(), |p| id(db, p)),
                hidden_label(db, mesh.asset, object.entity)
            );
        }
        _ => line.push_str(" mesh -"),
    }
    let _ = write!(
        line,
        " start_hidden {} t {}",
        u8::from(starts_hidden(world, object)),
        transform_hex(&object.transform)
    );
    line
}

fn hidden_label(db: &EbxDb, asset: Instance<'_>, entity: Instance<'_>) -> String {
    if !asset.is_a("CompositeMeshAsset") {
        return "-".to_owned();
    }
    match hidden_parts(db, entity) {
        None => "noparts".to_owned(),
        Some(parts) => format!(
            "[{}]",
            parts.iter().map(u16::to_string).collect::<Vec<_>>().join(",")
        ),
    }
}

/// IW4L bab7c1a geometry/mod.rs `hidden_parts`: the parts of later health states that the first
/// state does not show; `None` without a `PartComponentData`.
fn hidden_parts(db: &EbxDb, entity: Instance<'_>) -> Option<Vec<u16>> {
    const MAX_COMPONENTS: usize = 4096;
    let mut intact = Vec::new();
    let mut damaged = Vec::new();
    let mut found = false;
    let mut pending: Vec<Instance<'_>> = db.follow_all(entity, "Components").collect();
    let mut seen = 0;
    while let Some(component) = pending.pop() {
        seen += 1;
        if seen > MAX_COMPONENTS {
            break;
        }
        pending.extend(db.follow_all(component, "Components"));
        if !component.is_a("PartComponentData") {
            continue;
        }
        found = true;
        for (state, health) in db.follow_all(component, "HealthStates").enumerate() {
            let Some(part) = health
                .get("PartIndex")
                .and_then(|value| value.as_i64())
                .and_then(|part| u16::try_from(part).ok())
            else {
                continue;
            };
            if state == 0 {
                intact.push(part);
            } else {
                damaged.push(part);
            }
        }
    }
    damaged.sort_unstable();
    damaged.dedup();
    damaged.retain(|part| !intact.contains(part));
    found.then_some(damaged)
}

/// IW4L geometry/mod.rs `hidden_outside_spawns`, first half: the placement's `Visible` is fed by a
/// `BoolEntityData` whose `DefaultValue` is false (destruction ruins, backdrops).
fn starts_hidden(world: &World<'_>, object: &PlacedObject<'_>) -> bool {
    let Some(reference) = object.reference else {
        return false;
    };
    world
        .scopes
        .connections(object.scope)
        .property
        .iter()
        .any(|connection| {
            connection.target == reference
                && connection.target_field == field_id("Visible")
                && connection.source.is_a("BoolEntityData")
                && connection.source_field == field_id("Value")
                && connection
                    .source
                    .get("DefaultValue")
                    .and_then(|value| value.as_bool())
                    == Some(false)
        })
}

// ---------------------------------------------------------------------------------------------
// Mesh census

struct Census {
    lines: Vec<String>,
    placements: usize,
    variants: usize,
    meshes: usize,
    model: usize,
    world: usize,
    instances: usize,
    failures: usize,
}

struct Variant<'a> {
    asset: Instance<'a>,
    name: String,
    entry: Option<Instance<'a>>,
    hidden: Vec<u16>,
    bake: Option<[[f32; 3]; 3]>,
    key: Option<[i32; 9]>,
    placements: Vec<Transform>,
}

fn census(source: &Mount, db: &EbxDb, world: &World<'_>, legacy_mvdb: bool) -> Census {
    let mut variants: Vec<Variant<'_>> = Vec::new();
    let mut by_key: HashMap<(String, String, Vec<u16>, Option<[i32; 9]>), usize> = HashMap::new();
    let mut placements = 0;
    for object in &world.objects {
        let ObjectKind::Mesh(mesh) = object.kind else {
            continue;
        };
        let Some(name) = db.name_of(&mesh.asset.partition().guid()) else {
            continue;
        };
        let entry = if legacy_mvdb {
            legacy_mesh_variation(db, world, object.subworld, mesh.asset, object.variation)
        } else {
            world.mesh_variation(object.subworld, mesh.asset, object.variation)
        };
        let hidden = if mesh.asset.is_a("CompositeMeshAsset") {
            hidden_parts(db, object.entity).unwrap_or_default()
        } else {
            Vec::new()
        };
        let (placed, bake) = bake_split(&object.transform);
        placements += 1;
        let key_bake = bake.map(|linear| bake_key(&linear));
        let key = (
            id(db, mesh.asset),
            entry.map_or("-".to_owned(), |e| id(db, e)),
            hidden.clone(),
            key_bake,
        );
        let at = *by_key.entry(key).or_insert_with(|| {
            variants.push(Variant {
                asset: mesh.asset,
                name: name.to_owned(),
                entry,
                hidden,
                bake,
                key: key_bake,
                placements: Vec::new(),
            });
            variants.len() - 1
        });
        variants[at].placements.push(placed);
    }

    // Decode every distinct MeshSet once.
    let mut by_name: BTreeMap<&str, Vec<usize>> = BTreeMap::new();
    for (at, variant) in variants.iter().enumerate() {
        by_name.entry(variant.name.as_str()).or_default().push(at);
    }
    let names: Vec<&str> = by_name.keys().copied().collect();
    let decoded = parallel(&names, |name| SourceMesh::read(source, name));
    let sources: HashMap<&str, Result<SourceMesh, String>> = names.iter().copied().zip(decoded).collect();

    let mut census = Census {
        lines: Vec::new(),
        placements,
        variants: variants.len(),
        meshes: 0,
        model: 0,
        world: 0,
        instances: 0,
        failures: 0,
    };
    for name in &names {
        if sources[name].is_err() {
            census.failures += 1;
            census.lines.push(format!("F {name}"));
        }
    }
    for variant in &variants {
        let Ok(mesh) = &sources[variant.name.as_str()] else {
            continue;
        };
        let drawn = drawn_slots(db, variant.asset, variant.entry);
        let lods: Vec<bool> = mesh
            .lods
            .iter()
            .map(|subsets| {
                subsets.iter().any(|subset| {
                    drawn.get(subset.material_index as usize).copied().unwrap_or(true)
                        && draws_triangle(subset, &variant.hidden)
                })
            })
            .collect();
        let bake_scale = variant.bake.map_or(1.0, |linear| {
            linear
                .iter()
                .map(|row| (row[0] * row[0] + row[1] * row[1] + row[2] * row[2]).sqrt())
                .fold(0.0, f32::max)
        });
        let placed_scale = variant
            .placements
            .iter()
            .map(|placed| placed.normalized().scale)
            .fold(0.0, f32::max)
            * bake_scale;
        let radius = mesh.radius() * placed_scale;
        let world_kind = variant.placements.len() <= WORLD_MAX_PLACEMENTS && radius >= WORLD_RADIUS;
        let kept = if world_kind {
            lods.first().copied().unwrap_or(false)
        } else {
            lods.first().copied().unwrap_or(false) || lods.get(1).copied().unwrap_or(false)
        };
        if kept {
            census.meshes += 1;
            census.instances += variant.placements.len();
            if world_kind {
                census.world += 1;
            } else {
                census.model += 1;
            }
        }
        census.lines.push(format!(
            "V {} entry {} hid [{}] bake {} n {} radius {} kind {} lods {} kept {}",
            id(db, variant.asset),
            variant.entry.map_or("-".to_owned(), |e| id(db, e)),
            variant.hidden.iter().map(u16::to_string).collect::<Vec<_>>().join(","),
            variant.key.map_or("-".to_owned(), |key| {
                key.iter().map(i32::to_string).collect::<Vec<_>>().join(",")
            }),
            variant.placements.len(),
            hex(radius),
            if world_kind { "world" } else { "model" },
            lods.iter().map(|&l| if l { '1' } else { '0' }).collect::<String>(),
            u8::from(kept)
        ));
    }
    for object in &world.objects {
        if matches!(object.kind, ObjectKind::Terrain | ObjectKind::Water) {
            census.meshes += 1;
            census.world += 1;
            census.instances += 1;
        }
    }
    census
}

/// `--legacy-mvdb`: the MeshVariationDatabase lookup before IW4L skipped entries with an empty
/// `Materials` list (IW4L bab7c1a variations.rs), which the handoff's Bazaar counts were taken with.
fn legacy_mesh_variation<'a>(
    db: &'a EbxDb,
    world: &World<'a>,
    subworld: u32,
    mesh: Instance<'a>,
    variation: u32,
) -> Option<Instance<'a>> {
    let lookup = |variation: u32| {
        let mut at = Some(subworld);
        while let Some(index) = at {
            let sw = &world.subworlds[index as usize];
            if let Some(database) = sw.variations.database() {
                for list in ["Entries", "RedirectEntries"] {
                    for entry in db.follow_all(database, list) {
                        if db.follow(entry, "Mesh") == Some(mesh)
                            && entry.get("VariationAssetNameHash").and_then(|v| v.as_u32()).unwrap_or(0) == variation
                        {
                            return Some(entry);
                        }
                    }
                }
            }
            at = sw.parent;
        }
        None
    };
    lookup(variation).or_else(|| (variation != 0).then(|| lookup(0)).flatten())
}

/// Per `MeshAsset.Materials` slot (IW4L geometry/materials.rs `slots` + `resolve` at bab7c1a):
/// whether the slot's shader draws, i.e. its partition name contains none of the undrawn words.
fn drawn_slots(db: &EbxDb, asset: Instance<'_>, entry: Option<Instance<'_>>) -> Vec<bool> {
    let materials: Vec<Instance<'_>> = db.follow_all(asset, "Materials").collect();
    let variations: Vec<Struct<'_>> = entry
        .and_then(|entry| entry.get("Materials")?.as_array())
        .into_iter()
        .flatten()
        .filter_map(|value| value.as_struct())
        .collect();
    let count = materials.len().max(variations.len());
    (0..count)
        .map(|slot| {
            let material = materials.get(slot).copied();
            let variation = material
                .and_then(|material| {
                    variations
                        .iter()
                        .copied()
                        .find(|variation| db.follow(*variation, "Material") == Some(material))
                })
                .or_else(|| variations.get(slot).copied());
            let shader = material
                .or_else(|| variation.and_then(|variation| db.follow(variation, "Material")))
                .and_then(|material| material.get("Shader")?.as_struct())
                .and_then(|shader| db.follow(shader, "Shader"))
                .and_then(|shader| db.name_of(&shader.partition().guid()));
            !shader.is_some_and(|name| UNDRAWN_SHADERS.iter().any(|word| name.contains(word)))
        })
        .collect()
}

struct SourceMesh {
    bounds_min: [f32; 3],
    bounds_max: [f32; 3],
    lods: Vec<Vec<resource_bf3::DecodedSubset>>,
}

impl SourceMesh {
    fn read(source: &Mount, name: &str) -> Result<Self, String> {
        let (info, res) = source.read_res(name).map_err(|error| error.to_string())?;
        if info.res_type != MeshSet::RES_TYPE {
            return Err(format!("res type {:08X}", info.res_type));
        }
        let set = MeshSet::parse(&res, &info.res_meta).map_err(|error| error.to_string())?;
        let mut lods = Vec::new();
        for lod in set.lods.iter().take(MAX_LODS) {
            let geometry = match lod.geometry {
                GeometrySource::Chunk(id) => source
                    .read_chunk(&id)
                    .map_err(|error| format!("chunk {id}: {error}"))?,
                GeometrySource::Inline { .. } => lod
                    .inline_geometry(&res)
                    .map_err(|error| error.to_string())?
                    .map_or_else(Vec::new, <[u8]>::to_vec),
            };
            lods.push(
                lod.decode(&geometry, SubsetFilter::Drawn)
                    .map_err(|error| error.to_string())?,
            );
        }
        Ok(Self {
            bounds_min: set.bounds_min,
            bounds_max: set.bounds_max,
            lods,
        })
    }

    fn radius(&self) -> f32 {
        (0..3)
            .map(|axis| (self.bounds_max[axis] - self.bounds_min[axis]).powi(2))
            .sum::<f32>()
            .sqrt()
            * 0.5
    }
}

/// IW4L geometry/meshes.rs `surfaces`: whether one triangle survives the hidden parts and the
/// degenerate test.
fn draws_triangle(subset: &resource_bf3::DecodedSubset, hidden: &[u16]) -> bool {
    let is_hidden = |index: u32| {
        subset
            .parts
            .as_ref()
            .and_then(|parts| parts.get(index as usize))
            .is_some_and(|part| hidden.binary_search(part).is_ok())
    };
    subset.indices.chunks_exact(3).any(|triangle| {
        let [a, b, c] = [triangle[0], triangle[1], triangle[2]];
        if !hidden.is_empty() && (is_hidden(a) || is_hidden(b) || is_hidden(c)) {
            return false;
        }
        a != b && b != c && a != c
    })
}

// IW4L geometry/meshes.rs `Bake::split` (private there), operation for operation.
fn bake_split(transform: &Transform) -> (Transform, Option<[[f32; 3]; 3]>) {
    const TOLERANCE: f32 = 1e-3;
    let rows = [transform.right, transform.up, transform.forward]
        .map(|row| row.map(|component| component * transform.scale));
    let lengths = rows.map(length);
    let largest = lengths[0].max(lengths[1]).max(lengths[2]);
    let orthogonal = (0..3).all(|a| {
        let b = (a + 1) % 3;
        dot(rows[a], rows[b]).abs() <= TOLERANCE * lengths[a] * lengths[b]
    });
    let positive = determinant(rows) > 0.0;
    if transform.is_uniform(TOLERANCE) && orthogonal && positive && largest > 0.0 {
        return (*transform, None);
    }
    let x = normalize(rows[0]);
    let y = normalize(sub(rows[1], scale(x, dot(rows[1], x))));
    let z = cross(x, y);
    let basis = [x, y, z];
    let linear = basis.map(|axis| rows.map(|row| dot(row, axis)));
    if determinant(linear).abs() < 1e-12 {
        return (*transform, None);
    }
    let placed = Transform {
        right: x,
        up: y,
        forward: z,
        trans: transform.trans,
        scale: 1.0,
    };
    (placed, Some(linear))
}

fn bake_key(linear: &[[f32; 3]; 3]) -> [i32; 9] {
    let mut key = [0; 9];
    for (slot, value) in key.iter_mut().zip(linear.iter().flatten()) {
        *slot = (value * 4096.0).round() as i32;
    }
    key
}

fn dot(a: [f32; 3], b: [f32; 3]) -> f32 {
    a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
}

fn cross(a: [f32; 3], b: [f32; 3]) -> [f32; 3] {
    [
        a[1] * b[2] - a[2] * b[1],
        a[2] * b[0] - a[0] * b[2],
        a[0] * b[1] - a[1] * b[0],
    ]
}

fn sub(a: [f32; 3], b: [f32; 3]) -> [f32; 3] {
    [a[0] - b[0], a[1] - b[1], a[2] - b[2]]
}

fn scale(a: [f32; 3], s: f32) -> [f32; 3] {
    a.map(|component| component * s)
}

fn length(a: [f32; 3]) -> f32 {
    dot(a, a).sqrt()
}

fn normalize(a: [f32; 3]) -> [f32; 3] {
    let len = length(a);
    if len > 1e-12 { scale(a, 1.0 / len) } else { a }
}

fn determinant(m: [[f32; 3]; 3]) -> f32 {
    dot(m[0], cross(m[1], m[2]))
}

// ---------------------------------------------------------------------------------------------
// Collision placements

/// Static model groups (their Havok res at the group transform) and every non-group mesh object
/// with a `PhysicsEntityData`: the asset IW4L collision/mod.rs `pick_asset` chooses and the frame
/// `frame_of` builds (axes times placement scale over the asset's prebaked scale, origin).
fn collision(source: &Mount, db: &EbxDb, world: &World<'_>) -> (Vec<String>, usize, usize) {
    let mut lines = Vec::new();
    let mut groups = 0;
    for (at, group) in world.groups.iter().enumerate() {
        let Some(name) = &group.physics_asset else {
            continue;
        };
        groups += 1;
        lines.push(format!(
            "C group {at} res {name} subparts {} t {}",
            group.subparts,
            transform_hex(&group.transform)
        ));
    }
    let mut objects = 0;
    for object in &world.objects {
        if object.group.is_some() {
            continue;
        }
        let Some(mesh) = object.kind.mesh() else {
            continue;
        };
        let Some(physics) = mesh.physics else {
            continue;
        };
        let bodies: Vec<&str> = db
            .follow_all(physics, "RigidBodies")
            .map(|body| {
                body.get("RigidBodyType")
                    .and_then(|value| value.enum_name())
                    .unwrap_or("?")
            })
            .collect();
        let Some((name, asset_scale)) = pick_asset(source, db, physics, &object.transform) else {
            lines.push(format!(
                "C object {} res - bodies {}",
                id(db, object.entity),
                bodies.join(",")
            ));
            continue;
        };
        objects += 1;
        let factor = object.transform.scale / asset_scale;
        let mut frame = String::new();
        for row in [object.transform.right, object.transform.up, object.transform.forward] {
            for v in row {
                frame.push_str(&hex(v * factor));
                frame.push(' ');
            }
        }
        for v in object.transform.trans {
            frame.push_str(&hex(v));
            frame.push(' ');
        }
        lines.push(format!(
            "C object {} res {name} present {} scale {} bodies {} frame {}",
            id(db, object.entity),
            u8::from(source.res_info(name).is_some()),
            hex(asset_scale),
            bodies.join(","),
            frame.trim_end()
        ));
    }
    (lines, groups, objects)
}

fn pick_asset<'a>(
    source: &Mount,
    db: &'a EbxDb,
    physics: Instance<'a>,
    transform: &Transform,
) -> Option<(&'a str, f32)> {
    let [x, y, z] = transform.axis_scales();
    let placed = (x * y * z).abs().cbrt().max(1e-6);
    let mut assets: Vec<(&str, f32)> = db
        .follow_all(physics, "ScaledAssets")
        .chain(db.follow(physics, "Asset"))
        .filter_map(|asset| {
            let name = asset.get("Name")?.as_str()?;
            let scale = asset
                .get("Scale")
                .and_then(|value| value.as_f32())
                .filter(|&scale| scale > 0.0)
                .unwrap_or(1.0);
            Some((name, scale))
        })
        .collect();
    let off = |scale: f32| (scale / placed).ln().abs();
    assets.sort_by(|a, b| off(a.1).total_cmp(&off(b.1)));
    assets
        .iter()
        .copied()
        .find(|(name, _)| source.res_info(name).is_some())
        .or_else(|| assets.first().copied())
}

fn parallel<T: Sync, R: Send>(items: &[T], work: impl Fn(&T) -> R + Sync) -> Vec<R> {
    let lanes = std::thread::available_parallelism().map_or(1, |n| n.get()).min(items.len().max(1));
    let next = std::sync::atomic::AtomicUsize::new(0);
    let mut done: Vec<(usize, R)> = std::thread::scope(|scope| {
        let handles: Vec<_> = (0..lanes)
            .map(|_| {
                scope.spawn(|| {
                    let mut out = Vec::new();
                    loop {
                        let at = next.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
                        let Some(item) = items.get(at) else {
                            return out;
                        };
                        out.push((at, work(item)));
                    }
                })
            })
            .collect();
        handles.into_iter().flat_map(|h| h.join().unwrap()).collect()
    });
    done.sort_unstable_by_key(|(at, _)| *at);
    done.into_iter().map(|(_, r)| r).collect()
}
