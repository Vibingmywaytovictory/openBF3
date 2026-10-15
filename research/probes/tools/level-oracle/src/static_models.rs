//! `static-models <LEVEL> <MODE>`: the handoff §9 renderer count of one level x mode ("bf3 static
//! models: N placements on M meshes (... K placed hidden for destruction)" in IW4L's world report),
//! decomposed. It runs IW4L level_bf3's own builders (destruction::build, geometry::build_with,
//! lighting::build + place, destruction::place_instances) and applies the assets lane's rules
//! (crates/assets/src/lane/bf3: baked.rs `world_instances` / `in_world` / `charted`, models.rs
//! `scene_models` and `held_back_instances`), restated here because the assets crate pulls in the
//! whole renderer:
//!   world soup  = not a destructible view, and the mesh is world kind, or the level has lightmaps,
//!                 no LOD-0 surface glows (emissive or overlay material) and one is charted;
//!   placements  = every other scene instance with scale > 0 (models.rs then drops meshes without
//!                 drawable surfaces; none on Caspian);
//!   held back   = every destruction state's instance plus every object's static-parts instance.
//! Scene instances, in builder order: one per mesh placement not hidden at start (a destructible
//! placement gives its Initial view, its Static view and one view per state that shows a part:
//! destruction.rs + geometry/mod.rs `destruction_views`), the terrain and water, the projected
//! decal models, then the ground scatter merged into one model per square and reach.
//! Output: the totals, then one `row <side> | <view> | <count>` line per pair.

use std::collections::BTreeMap;
use std::fmt::Write as _;
use std::sync::Arc;

use bf3_install::Bf3Install;
use bf3_scene::{MeshKind, Surface};
use level_bf3::geometry::DestructionView;
use level_bf3::{BuildInput, GameMode, LoadOptions, ObjectKind, World};

use crate::{Mount, fail, mount};

pub fn run(install: &Arc<Bf3Install>, level: &str, mode: &str) -> String {
    let source = Mount(mount(install, level));
    let loaded = level_bf3::load_level(&source, level, GameMode::from_id(mode), &LoadOptions::default())
        .unwrap_or_else(|error| fail(&error.to_string()));
    let world = World::traverse(&loaded, &source).unwrap_or_else(|error| fail(&error.to_string()));
    let input = BuildInput {
        source: &source,
        level: &loaded,
        world: &world,
        scatter: LoadOptions::default().scatter,
    };
    let built = level_bf3::destruction::build(&input);
    let geometry = level_bf3::geometry::build_with(&input, &built).unwrap_or_else(|error| fail(&error.to_string()));
    let lighting = level_bf3::lighting::build(&input, &built)
        .unwrap_or_else(|error| fail(&error.to_string()))
        .map(|lighting| lighting.place(&geometry.placed, &geometry.decal_receivers).0);
    let mut table = built.table.clone();
    level_bf3::destruction::place_instances(&mut table, &geometry.destruction_views);

    let n = geometry.instances.len();
    let mut view: Vec<&str> = vec!["plain"; n];
    for &(instance, tag) in &geometry.destruction_views {
        view[instance as usize] = match tag {
            DestructionView::Initial(_) => "initial",
            DestructionView::Static(_) => "static",
            DestructionView::State(_) => "state",
            DestructionView::Decals(_) => "state-decals",
        };
    }
    for (at, tag) in view.iter_mut().enumerate() {
        if *tag != "plain" {
            continue;
        }
        let name = &geometry.meshes[geometry.instances[at].mesh as usize].name;
        if name.starts_with("bf3/decals/") {
            *tag = "decals";
        } else if name.starts_with("bf3/scatter/") {
            *tag = "scatter";
        } else if geometry.placed[at].is_none() {
            *tag = "terrain/water";
        }
    }
    let mut destructible = vec![false; n];
    for object in &table.objects {
        for instance in [object.instance, object.static_instance].into_iter().flatten() {
            destructible[instance as usize] = true;
        }
    }
    for state in &table.states {
        for instance in [state.instance, state.decals].into_iter().flatten() {
            destructible[instance as usize] = true;
        }
    }
    let charted = |instance: usize, surface: &Surface| {
        lighting.as_ref().is_some_and(|lighting| {
            lighting.instance_lightmaps.get(instance).copied().flatten().is_some_and(|lightmap| {
                !surface.lightmap_uv.is_empty()
                    && surface.lightmap_uv.len() == surface.vertices.len()
                    && (lightmap.atlas as usize) < lighting.atlases.len()
            })
        })
    };
    let in_world = |at: usize| {
        if destructible[at] {
            return false;
        }
        let mesh = &geometry.meshes[geometry.instances[at].mesh as usize];
        if mesh.kind == MeshKind::World {
            return true;
        }
        lighting.is_some()
            && mesh.lods.first().is_some_and(|lod| {
                let glows = |surface: &Surface| {
                    geometry
                        .materials
                        .get(surface.material as usize)
                        .is_some_and(|material| material.emissive.is_some() || material.overlay.is_some())
                };
                !lod.surfaces.iter().any(glows) && lod.surfaces.iter().any(|surface| charted(at, surface))
            })
    };
    let mut held = vec![false; n];
    for state in &table.states {
        if let Some(instance) = state.instance {
            held[instance as usize] = true;
        }
    }
    for object in &table.objects {
        if let Some(instance) = object.static_instance {
            held[instance as usize] = true;
        }
    }
    let mut rows: BTreeMap<(&str, &str), usize> = BTreeMap::new();
    let (mut placements, mut hidden) = (0usize, 0usize);
    for at in 0..n {
        let side = if in_world(at) {
            if geometry.meshes[geometry.instances[at].mesh as usize].kind == MeshKind::World {
                "world soup, world kind"
            } else {
                "world soup, lightmapped"
            }
        } else if geometry.instances[at].scale <= 0.0 {
            "skipped, scale <= 0"
        } else {
            placements += 1;
            hidden += usize::from(held[at]);
            "static model"
        };
        *rows.entry((side, view[at])).or_default() += 1;
    }
    let s = &geometry.stats;
    let mut out = String::new();
    let _ = writeln!(out, "level {level} mode {mode}");
    let _ = writeln!(
        out,
        "mesh objects {} hidden at start {} placed {} destructible {}",
        world.objects.iter().filter(|object| matches!(object.kind, ObjectKind::Mesh(_))).count(),
        s.area_hidden_placements,
        s.placements,
        s.destructible_placements
    );
    let _ = writeln!(out, "destruction objects {} slots {} states {}", table.objects.len(), table.slots.len(), table.states.len());
    let _ = writeln!(
        out,
        "scene meshes {} instances {} scatter models {} decal models {}",
        geometry.meshes.len(),
        n,
        s.scatter_models,
        s.decal_models
    );
    let _ = writeln!(
        out,
        "static models {placements} placed hidden for destruction {hidden} (held back {})",
        held.iter().filter(|held| **held).count()
    );
    for ((side, tag), count) in rows {
        let _ = writeln!(out, "row {side} | {tag} | {count}");
    }
    out
}
