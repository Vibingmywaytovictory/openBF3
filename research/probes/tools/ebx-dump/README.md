# ebx-dump: read-only EBX probe of the BF3 install

Research tool (Rust), not part of the shipped project. It opens `C:\Program Files\EA Games\Battlefield 3`
(hard-coded `ROOT` in `src/main.rs`) **read-only** and writes only to the paths given on the command line.

## Origin of the code

* `crates/superbundle_bf3`, `crates/ebx_bf3`: byte-identical copies of `C:\Users\olku\iw4L\crates\superbundle_bf3\src`
  and `...\ebx_bf3\src` (IW4L, Apache-2.0).
* `crates/bf3_install`: copy of `C:\Users\olku\iw4L\crates\asset_transport\src\bf3\*.rs` (`mod.rs` became `lib.rs`),
  with three probe additions: `LevelMount::mount` made `pub`, `Bf3Install::mount_superbundle(name)` (mount any
  superbundle, e.g. `Win32/Globals`, not only MP levels), `LevelMount::bundle_manifests()` (per-bundle manifests so
  entries can be attributed to bundles).
* `src/main.rs`: the probe. `scripts/*.py`: post-processing of its JSON (run with `python -I`).

## Build (outside every read-only tree)

```
set CARGO_TARGET_DIR=<scratch>\target
cargo build --release
```

Rust 1.96, edition 2024; deps: flate2 (zlib-rs), serde_json.

## Commands

`<sb>` is a superbundle name as in `layout.toc` (`Win32/Levels/MP_Subway/MP_Subway`), comma-separated for several
(first one wins on duplicate partitions), or `@all-mp` = every MP/XP level superbundle + Globals/Weapons/UI/default_settings.

| command | output |
|---|---|
| `list <out.tsv>` | every ebx/res entry (and chunk count) of every bundle of every non-chunk, non-Loc superbundle: `superbundle bundle kind name res_type original_size` (1.09 M rows, 140 MB) |
| `find <sb> <needle>...` | partitions whose name contains a needle: name, primary type, superbundle, bundle |
| `types <sb> <Type>...` | every instance of the exact types: type, partition, instance GUID |
| `dump <sb> <out_dir> <depth> <follow> <root>...` | one JSON per root: the root partition and the breadth-first closure of referenced partitions whose name contains one of the comma-separated `<follow>` substrings (`*` = all, `-` = none), up to `<depth>`. `fx/` and `sound/` are never followed unless `*`; mesh/texture/shader/sound asset partitions are listed but not expanded. Every instance with all fields (base types first); references are `{"$ref": partition-name or "internal", "instance": guid, "type": T}`; floats are shortest round-trip f32 text |
| `res <sb> <needle>...` | res entries: name, res type, size, superbundle, bundle |
| `chunk <sb> <guid>...` | chunk presence (bundle index or chunk superbundles), decoded size, first 48 bytes |
| `reshead <sb> <res>...` | res type, 16-byte res meta, size, first 64 bytes |

Examples used for `research/probes/ebx`:

```
ebx-dump list all-entries.tsv
python -I scripts/partlist.py all-entries.tsv ebx-partitions.tsv
ebx-dump dump Win32/Levels/MP_Subway/MP_Subway out1 3 "weapons/common/bullets,weapons/common/zoomlevels,gunsway,aimingcontrollers,ai/weapons,_firing,projectile,40mm,grenade,rocket,explos,firstpersoncamera,launchersway,aimassist" weapons/m16a4/m16a4 weapons/ak74m/ak74 weapons/m249/m249 weapons/model98b/model_98b weapons/m9/m9 weapons/remington870/remington870mcs weapons/rpg7/rpg7 weapons/smaw/smaw weapons/gadgets/m320/m320 weapons/m67/m67
ebx-dump dump Win32/Levels/MP_007/MP_007 out2 2 "characters/soldiers,gameplay/teams,gameplay/kits" gameplay/teams/us characters/soldiers/mpsoldier gameplay/kits/usassault
ebx-dump dump Win32/Levels/MP_007/MP_007 out2 0 - levels/mp_007/mp_007/materialgrid_win32/grid materials/materialcontainer
python -I scripts/grid.py out2/levels__mp_007__mp_007__materialgrid_win32__grid.json out2/materials__materialcontainer.json "556=5b6b84ac" "head=8b81bca1,body=6609a12f"
ebx-dump dump Win32/Levels/MP_007/MP_007 out4 2 "ai/" vehicles/humveearmored/humveearmored
python -I scripts/summarize.py weapons-summary.json out1/*.json
ebx-dump chunk Win32/Levels/MP_Subway/MP_Subway 54620b35-bdb6-a4f1-c73a-44712121a7a1
```

Timing on the user's machine: `list` 3.3 s; loading one level superbundle's ~12.5 k partitions 2.2 s; `@all-mp`
(50,486 unique partitions) 32 s on a cold file cache, about 2.5 s warm (`types @all-mp LevelData`,
2026-10-14; the C++ `bf3dump ebxload @all-mp --walk` takes about 1.2 s warm).
