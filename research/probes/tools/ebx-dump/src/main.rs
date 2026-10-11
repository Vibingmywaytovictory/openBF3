//! ebx-dump: read-only probe of a Battlefield 3 install.
//!
//! Opens the install through the IW4L readers (copied, unmodified apart from two accessors) and
//! never writes anything under the install root. All output goes to the paths given on the
//! command line.
//!
//! Commands
//!   list  <out.tsv>                                  every bundle entry of every superbundle
//!   find  <sb,sb,..> <needle> [needle..]             partitions whose name contains a needle
//!   types <sb,sb,..> <Type> [Type..]                 every instance of the given types (exact type)
//!   dump  <sb,sb,..> <out_dir> <depth> <follow> <root..>
//!                                                    JSON of root partitions + the closure of
//!                                                    referenced partitions whose name contains
//!                                                    one of the comma-separated <follow>
//!                                                    substrings ("*" = all), up to <depth>
//!   res   <sb,sb,..> <needle> [needle..]             res entries (name, type, size, bundle) matching
//!   chunk <sb> <guid> [guid..]                       chunk presence, decoded size, first bytes
//!   reshead <sb> <res name> [name..]                 res type, meta, size, first bytes
//!
//! `sb` is a superbundle name as in layout.toc (e.g. Win32/Levels/MP_Subway/MP_Subway), or
//! `@all-mp` for every MP level superbundle plus Globals/Weapons/UI/default_settings.

use std::collections::{BTreeMap, BTreeSet, HashMap, HashSet, VecDeque};
use std::io::Write;
use std::path::Path;
use std::sync::Arc;

use bf3_install::{Bf3Install, LevelMount};
use ebx_bf3::{EbxDb, Guid, Instance, Partition, Ref, Struct, Value};
use serde_json::{Map, Number, Value as J, json};

const ROOT: &str = r"C:\Program Files\EA Games\Battlefield 3";

fn res_type_name(t: u32) -> String {
    let n = match t {
        0xC6DBEE07 => "AnimatedPointCloud",
        0xD070EED1 => "AnimTrackData",
        0x51A3C853 => "AssetBank",
        0xE565EB15 => "DxShaderDatabase",
        0x10F0E5A1 => "DxShaderProgramDatabase",
        0x5C4954A6 => "DxTexture",
        0x70C5CB3E => "EnlightenDatabase",
        0xE156AF73 => "EnlightenProbeSet",
        0x59CEEB57 => "EnlightenShaderDatabase",
        0x5BDFDEFE => "EnlightenSystem",
        0x4864737B => "HavokDestructionPhysicsData",
        0x91043F65 => "HavokPhysicsData",
        0xC78B9D9D => "ImpulseResponse",
        0x36F3F2C0 => "IShaderDatabase",
        0xC417BBD3 => "ITexture",
        0x49B156D4 => "MeshSet",
        0x30B4A553 => "OccluderMesh",
        0x319D8CD0 => "RagdollResource",
        0x3568E2B7 => "RawFileData",
        0x41D57E10 => "RenderTexture",
        0x7AEFC446 => "StaticEnlightenDatabase",
        0x2D47A5FF => "SwfMovie",
        0x6BB6D7D2 => "Terrain",
        0x15E1F32E => "TerrainDecals",
        0x22FE8AC8 => "TerrainStreamingTree",
        0x1CA38E06 => "VisualTerrain",
        _ => return format!("0x{t:08X}"),
    };
    n.to_owned()
}

fn open() -> Arc<Bf3Install> {
    Bf3Install::open(Path::new(ROOT)).expect("open install")
}

fn expand_sbs(install: &Bf3Install, spec: &str) -> Vec<String> {
    let mut out = Vec::new();
    for part in spec.split(',') {
        if part == "@all-mp" {
            for name in install.superbundle_names() {
                let l = name.to_ascii_lowercase();
                let level = l.starts_with("win32/levels/mp_") || l.starts_with("win32/levels/xp");
                let global = matches!(
                    l.as_str(),
                    "win32/globals" | "win32/weapons" | "win32/ui" | "win32/default_settings_win32"
                );
                if level || global {
                    out.push(name.to_owned());
                }
            }
        } else {
            out.push(part.to_owned());
        }
    }
    out
}

fn mount(install: &Arc<Bf3Install>, sb: &str) -> LevelMount {
    let m = install.mount_superbundle(sb).unwrap_or_else(|e| panic!("mount {sb}: {e}"));
    m.load_all_bundles().unwrap_or_else(|e| panic!("load bundles {sb}: {e}"));
    m
}

/// Loads every EBX partition of the given superbundles into one database. First seen wins
/// (superbundles in the order given). Returns the db and partition name -> (superbundle, bundle).
struct Loaded {
    db: EbxDb,
    origin: HashMap<String, (String, String)>,
}

fn load_db(install: &Arc<Bf3Install>, sbs: &[String]) -> Loaded {
    let mut db = EbxDb::new();
    let mut origin = HashMap::new();
    for sb in sbs {
        let m = mount(install, sb);
        let mut bundle_of: HashMap<String, String> = HashMap::new();
        for (bundle, manifest) in m.bundle_manifests() {
            if let Ok(manifest) = manifest {
                for e in manifest.ebx {
                    bundle_of.entry(e.name.to_ascii_lowercase()).or_insert_with(|| bundle.clone());
                }
            }
        }
        let names = m.ebx_names();
        let mut fresh = 0usize;
        let mut failed = 0usize;
        for name in names {
            let key = name.to_ascii_lowercase();
            if origin.contains_key(&key) {
                continue;
            }
            match m.read_ebx(&name).map(|b| db.parse_insert(&name, b)) {
                Ok(Ok(true)) => {
                    fresh += 1;
                    let bundle = bundle_of.get(&key).cloned().unwrap_or_default();
                    origin.insert(key, (sb.clone(), bundle));
                }
                Ok(Ok(false)) => {}
                _ => failed += 1,
            }
        }
        eprintln!("{sb}: +{fresh} partitions ({failed} failed), db {}", db.len());
    }
    Loaded { db, origin }
}

fn num(f: f64) -> J {
    Number::from_f64(f).map_or_else(|| J::String(format!("{f}")), J::Number)
}

fn f32j(f: f32) -> J {
    // Shortest round-trip text of the f32, so 0.1f prints as 0.1 rather than 0.10000000149.
    match format!("{f}").parse::<f64>() {
        Ok(v) if v.is_finite() => num(v),
        _ => J::String(format!("{f}")),
    }
}

fn ref_json(db: &EbxDb, from: &Partition, r: Ref) -> J {
    match r {
        Ref::Null => J::Null,
        Ref::Internal(i) => match from.instance(i as usize) {
            Some(inst) => json!({"$ref": "internal", "instance": inst.guid().to_string(), "type": inst.type_name()}),
            None => json!({"$ref": "internal", "index": i, "unresolved": true}),
        },
        Ref::External { partition, instance } => {
            let pname = db.name_of(&partition).map(str::to_owned);
            let ty = db.instance(&partition, &instance).map(|i| i.type_name().to_owned());
            json!({
                "$ref": pname.unwrap_or_else(|| format!("<unloaded {partition}>")),
                "instance": instance.to_string(),
                "type": ty,
            })
        }
    }
}

fn value_json(db: &EbxDb, part: &Partition, v: Value<'_>) -> J {
    match v {
        Value::Bool(b) => J::Bool(b),
        Value::I8(x) => json!(x),
        Value::U8(x) => json!(x),
        Value::I16(x) => json!(x),
        Value::U16(x) => json!(x),
        Value::I32(x) => json!(x),
        Value::U32(x) => json!(x),
        Value::I64(x) => json!(x),
        Value::U64(x) => json!(x),
        Value::F32(x) => f32j(x),
        Value::F64(x) => num(x),
        Value::Guid(g) => J::String(g.to_string()),
        Value::Sha1(s) => J::String(s.iter().map(|b| format!("{b:02x}")).collect()),
        Value::String(s) => s.map_or(J::Null, |s| J::String(s.to_owned())),
        Value::Enum { value, name } => match name {
            Some(n) => J::String(n.to_owned()),
            None => json!(value),
        },
        Value::Ref(r) => ref_json(db, part, r),
        Value::Struct(s) => struct_json(db, part, s),
        Value::Array(a) => J::Array(a.iter().map(|x| value_json(db, part, x)).collect()),
    }
}

fn struct_json(db: &EbxDb, part: &Partition, s: Struct<'_>) -> J {
    let mut m = Map::new();
    m.insert("$type".into(), J::String(s.type_name().to_owned()));
    for (name, v) in s.fields() {
        m.insert(name.to_owned(), value_json(db, part, v));
    }
    J::Object(m)
}

fn instance_json(db: &EbxDb, inst: Instance<'_>) -> J {
    let mut m = Map::new();
    m.insert("$guid".into(), J::String(inst.guid().to_string()));
    m.insert("$type".into(), J::String(inst.type_name().to_owned()));
    if inst.is_primary() {
        m.insert("$primary".into(), J::Bool(true));
    }
    let chain: Vec<&str> = inst.type_chain().collect();
    m.insert("$chain".into(), json!(chain));
    for (name, v) in inst.fields() {
        m.insert(name.to_owned(), value_json(db, inst.partition(), v));
    }
    J::Object(m)
}

fn partition_json(loaded: &Loaded, name: &str) -> Option<J> {
    let part = loaded.db.get_by_name(name)?;
    let (sb, bundle) = loaded.origin.get(&name.to_ascii_lowercase()).cloned().unwrap_or_default();
    let instances: Vec<J> = part.instances().map(|i| instance_json(&loaded.db, i)).collect();
    Some(json!({
        "partition": loaded.db.name_of(&part.guid()).unwrap_or(name),
        "guid": part.guid().to_string(),
        "superbundle": sb,
        "bundle": bundle,
        "primary_type": part.primary().type_name(),
        "instances": instances,
    }))
}

fn collect_refs(v: Value<'_>, out: &mut Vec<Guid>) {
    match v {
        Value::Ref(Ref::External { partition, .. }) => out.push(partition),
        Value::Struct(s) => {
            for (_, x) in s.fields() {
                collect_refs(x, out);
            }
        }
        Value::Array(a) => {
            for x in a.iter() {
                collect_refs(x, out);
            }
        }
        _ => {}
    }
}

/// Partition types never worth following for gameplay data (renderable or audio payloads).
fn skip_type(t: &str) -> bool {
    matches!(
        t,
        "RigidMeshAsset"
            | "SkinnedMeshAsset"
            | "CompositeMeshAsset"
            | "TextureAsset"
            | "SoundWaveAsset"
            | "SoundPatchAsset"
            | "ShaderGraph"
            | "SurfaceShaderBaseAsset"
            | "MeshVariationDatabase"
    )
}

fn sanitize(name: &str) -> String {
    name.replace(['/', '\\'], "__")
}

fn cmd_list(out: &str) {
    let install = open();
    let mut w = std::io::BufWriter::new(std::fs::File::create(out).expect("create out"));
    writeln!(w, "superbundle\tbundle\tkind\tname\tres_type\toriginal_size").ok();
    let names: Vec<String> = install.superbundle_names().map(str::to_owned).collect();
    for sb in names {
        let l = sb.to_ascii_lowercase();
        if l.contains("chunks") || l.starts_with("win32/loc/") {
            continue;
        }
        let m = match install.mount_superbundle(&sb) {
            Ok(m) => m,
            Err(e) => {
                eprintln!("{sb}: {e}");
                continue;
            }
        };
        let mut n = 0usize;
        for (bundle, manifest) in m.bundle_manifests() {
            match manifest {
                Ok(manifest) => {
                    for e in &manifest.ebx {
                        writeln!(w, "{sb}\t{bundle}\tebx\t{}\t\t{}", e.name, e.original_size).ok();
                        n += 1;
                    }
                    for e in &manifest.res {
                        writeln!(
                            w,
                            "{sb}\t{bundle}\tres\t{}\t{}\t{}",
                            e.name,
                            res_type_name(e.res_type),
                            e.original_size
                        )
                        .ok();
                        n += 1;
                    }
                    writeln!(w, "{sb}\t{bundle}\tchunks\t{}\t\t", manifest.chunks.len()).ok();
                }
                Err(e) => eprintln!("{sb} {bundle}: {e}"),
            }
        }
        eprintln!("{sb}: {n} entries");
    }
}

fn cmd_find(sbs: &str, needles: &[String]) {
    let install = open();
    let loaded = load_db(&install, &expand_sbs(&install, sbs));
    let needles: Vec<String> = needles.iter().map(|n| n.to_ascii_lowercase()).collect();
    let mut rows: BTreeMap<String, String> = BTreeMap::new();
    for (name, part) in loaded.db.iter() {
        if needles.iter().any(|n| name.contains(n.as_str())) {
            let (sb, bundle) = loaded.origin.get(name).cloned().unwrap_or_default();
            rows.insert(name.to_owned(), format!("{}\t{sb}\t{bundle}", part.primary().type_name()));
        }
    }
    for (k, v) in rows {
        println!("{k}\t{v}");
    }
}

fn cmd_types(sbs: &str, types: &[String]) {
    let install = open();
    let loaded = load_db(&install, &expand_sbs(&install, sbs));
    let wanted: HashSet<&str> = types.iter().map(String::as_str).collect();
    let mut rows = BTreeSet::new();
    for (name, part) in loaded.db.iter() {
        for inst in part.instances() {
            if wanted.contains(inst.type_name()) {
                rows.insert(format!("{}\t{name}\t{}", inst.type_name(), inst.guid()));
            }
        }
    }
    for r in rows {
        println!("{r}");
    }
}

fn cmd_res(sbs: &str, needles: &[String]) {
    let install = open();
    let needles: Vec<String> = needles.iter().map(|n| n.to_ascii_lowercase()).collect();
    for sb in expand_sbs(&install, sbs) {
        let m = install.mount_superbundle(&sb).expect("mount");
        for (bundle, manifest) in m.bundle_manifests() {
            let Ok(manifest) = manifest else { continue };
            for e in &manifest.res {
                let l = e.name.to_ascii_lowercase();
                if needles.iter().any(|n| l.contains(n.as_str())) {
                    println!(
                        "{}\t{}\t{}\t{sb}\t{bundle}",
                        e.name,
                        res_type_name(e.res_type),
                        e.original_size
                    );
                }
            }
        }
    }
}

fn follows(name: &str, follow: &[String]) -> bool {
    if (name.starts_with("fx/") || name.starts_with("sound/")) && !follow.iter().any(|f| f == "*") {
        return false;
    }
    follow.iter().any(|f| f == "*" || name.contains(f.as_str()))
}

fn cmd_dump(sbs: &str, out_dir: &str, depth: usize, follow: &str, roots: &[String]) {
    let follow: Vec<String> = follow.split(',').map(|f| f.to_ascii_lowercase()).collect();
    let install = open();
    let loaded = load_db(&install, &expand_sbs(&install, sbs));
    std::fs::create_dir_all(out_dir).expect("out dir");
    for root in roots {
        let Some(part) = loaded.db.get_by_name(root) else {
            eprintln!("missing root {root}");
            continue;
        };
        // Breadth-first closure over external partition references.
        let mut seen: HashSet<Guid> = HashSet::new();
        let mut order: Vec<(usize, Guid)> = Vec::new();
        let mut queue = VecDeque::from([(0usize, part.guid())]);
        let mut unresolved: BTreeSet<String> = BTreeSet::new();
        let mut not_followed: BTreeSet<String> = BTreeSet::new();
        while let Some((d, g)) = queue.pop_front() {
            if !seen.insert(g) {
                continue;
            }
            let Some(p) = loaded.db.get(&g) else {
                unresolved.insert(g.to_string());
                continue;
            };
            let pname = loaded.db.name_of(&g).unwrap_or("");
            if d > 0 && (!follows(pname, &follow) || skip_type(p.primary().type_name())) {
                not_followed.insert(format!("{pname} ({})", p.primary().type_name()));
                continue;
            }
            order.push((d, g));
            if d >= depth || (d > 0 && skip_type(p.primary().type_name())) {
                continue;
            }
            let mut refs = Vec::new();
            for inst in p.instances() {
                for (_, v) in inst.fields() {
                    collect_refs(v, &mut refs);
                }
            }
            for r in refs {
                if !seen.contains(&r) {
                    queue.push_back((d + 1, r));
                }
            }
        }
        let mut parts = Vec::new();
        let mut index = Vec::new();
        for (d, g) in &order {
            let name = loaded.db.name_of(g).unwrap_or("?").to_owned();
            let p = loaded.db.get(g).unwrap();
            index.push(json!({"depth": d, "partition": name, "primary_type": p.primary().type_name(), "instances": p.instance_count()}));
            if *d == 0 || !skip_type(p.primary().type_name()) {
                if let Some(j) = partition_json(&loaded, &name) {
                    parts.push(j);
                }
            }
        }
        let doc = json!({
            "root": root,
            "depth_limit": depth,
            "closure": index,
            "follow_filter": follow,
            "unresolved_partitions": unresolved,
            "referenced_not_followed": not_followed,
            "partitions": parts,
        });
        let path = Path::new(out_dir).join(format!("{}.json", sanitize(root)));
        std::fs::write(&path, serde_json::to_string_pretty(&doc).unwrap()).expect("write");
        eprintln!("wrote {} ({} partitions)", path.display(), order.len());
    }
}

fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect::<Vec<_>>().join(" ")
}

/// Reports whether chunks exist for this superbundle's mount (bundle chunks + chunk superbundles)
/// and prints the decoded size and first bytes. Reads only.
fn cmd_chunk(sb: &str, ids: &[String]) {
    let install = open();
    let m = mount(&install, sb);
    for id in ids {
        let Some(g) = superbundle_bf3::Guid::parse(id) else {
            println!("{id}	bad guid");
            continue;
        };
        let info = m.chunk_info(&g);
        match m.read_chunk(&g) {
            Ok(bytes) => println!(
                "{id}	in_bundle_index={}	decoded={}	head={}",
                info.is_some(),
                bytes.len(),
                hex(&bytes[..bytes.len().min(48)])
            ),
            Err(e) => println!("{id}	in_bundle_index={}	error={e}", info.is_some()),
        }
    }
}

/// Prints res type, meta and the first bytes of named res entries. Reads only.
fn cmd_reshead(sb: &str, names: &[String]) {
    let install = open();
    let m = mount(&install, sb);
    for name in names {
        match m.read_res(name) {
            Ok((info, bytes)) => println!(
                "{name}	{}	meta={}	len={}	head={}",
                res_type_name(info.res_type),
                hex(&info.res_meta),
                bytes.len(),
                hex(&bytes[..bytes.len().min(64)])
            ),
            Err(e) => println!("{name}	error={e}"),
        }
    }
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    match args.first().map(String::as_str) {
        Some("list") => cmd_list(&args[1]),
        Some("find") => cmd_find(&args[1], &args[2..]),
        Some("types") => cmd_types(&args[1], &args[2..]),
        Some("res") => cmd_res(&args[1], &args[2..]),
        Some("chunk") => cmd_chunk(&args[1], &args[2..]),
        Some("reshead") => cmd_reshead(&args[1], &args[2..]),
        Some("dump") => cmd_dump(&args[1], &args[2], args[3].parse().expect("depth"), &args[4], &args[5..]),
        _ => eprintln!("usage: see the doc comment at the top of src/main.rs"),
    }
}
