//! res-oracle: parity oracle for the openBF3 C++ resource decoders (bf3_core/res, tools/resdump).
//! Reads the install read-only through the ebx-dump readers; writes only under <data_dir>.
//!
//!   export <sb> <tag> <data_dir>   read every MeshSet / DxTexture / HavokPhysicsData /
//!                                  HavokDestructionPhysicsData res of a superbundle (read-only)
//!                                  and write res payload + meta files and the referenced chunks
//!   summary <data_dir> <tag>...    canonical text summary of the exported files (IW4L decoders)
//!   trunc <data_dir> <tag>...      the same files cut short: each decoder's error text
//!
//! File formats (little-endian):
//!   <data_dir>/<tag>/index.tsv           "<file>\t<name>\t<res type hex>" per res, sorted by name
//!   <data_dir>/<tag>/res/<NNNNN>.res     "BF3R" u32 1, u32 resType, u8[16] resMeta,
//!                                        u32 viewLogicalOffset (0xFFFFFFFF none), u32 nameLen, name,
//!                                        u64 payloadLen, payload
//!   <data_dir>/chunks/<32 hex>.chunk     decompressed full chunk, file name = raw GUID bytes

use std::collections::BTreeMap;
use std::fmt::Write as _;
use std::io::Write as _;
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

use bf3_install::{Bf3Install, Guid};
use resource_bf3::{
    ChunkId, DxTexture, GeometrySource, HavokPhysicsData, MeshKind, MeshSet, PixelFormat,
    PrimitiveType, ResourceError, Shape, SubsetCategory, Transform, VertexElement, VertexFormat,
    VertexUsage, convex_pieces, triangles,
};

const ROOT: &str = r"C:\Program Files\EA Games\Battlefield 3";
const MESH: u32 = 0x49B1_56D4;
const TEX: u32 = 0x5C49_54A6;
const HAVOK: u32 = 0x9104_3F65;
const HAVOK_DESTR: u32 = 0x4864_737B;

// ---------------------------------------------------------------------------------------------
// FNV-1a 64

struct Fnv(u64);

impl Fnv {
    fn new() -> Self {
        Self(0xcbf2_9ce4_8422_2325)
    }
    fn bytes(&mut self, b: &[u8]) {
        for &x in b {
            self.0 ^= u64::from(x);
            self.0 = self.0.wrapping_mul(0x0000_0100_0000_01b3);
        }
    }
    fn u8(&mut self, v: u8) {
        self.bytes(&[v]);
    }
    fn u16(&mut self, v: u16) {
        self.bytes(&v.to_le_bytes());
    }
    fn u32(&mut self, v: u32) {
        self.bytes(&v.to_le_bytes());
    }
    fn f32(&mut self, v: f32) {
        self.u32(v.to_bits());
    }
    fn f3(&mut self, v: [f32; 3]) {
        v.iter().for_each(|&x| self.f32(x));
    }
    fn f4(&mut self, v: [f32; 4]) {
        v.iter().for_each(|&x| self.f32(x));
    }
    fn xf(&mut self, t: &Transform) {
        t.axis.iter().for_each(|&r| self.f3(r));
        self.f3(t.origin);
    }
    fn h(&self) -> String {
        format!("{:016x}", self.0)
    }
}

fn hex(b: &[u8]) -> String {
    b.iter().map(|x| format!("{x:02x}")).collect()
}

fn fb(v: f32) -> String {
    format!("{:08x}", v.to_bits())
}

fn f3s(v: [f32; 3]) -> String {
    format!("{},{},{}", fb(v[0]), fb(v[1]), fb(v[2]))
}

fn err(e: &ResourceError) -> String {
    e.to_string()
}

// ---------------------------------------------------------------------------------------------
// export

fn write_res(path: &Path, res_type: u32, meta: &[u8; 16], view: u32, name: &str, payload: &[u8]) {
    let mut out = Vec::with_capacity(payload.len() + 64);
    out.extend_from_slice(b"BF3R");
    out.extend_from_slice(&1u32.to_le_bytes());
    out.extend_from_slice(&res_type.to_le_bytes());
    out.extend_from_slice(meta);
    out.extend_from_slice(&view.to_le_bytes());
    out.extend_from_slice(&(name.len() as u32).to_le_bytes());
    out.extend_from_slice(name.as_bytes());
    out.extend_from_slice(&(payload.len() as u64).to_le_bytes());
    out.extend_from_slice(payload);
    std::fs::write(path, out).expect("write res");
}

fn cmd_export(sb: &str, tag: &str, data: &str) {
    let t0 = Instant::now();
    let install = Bf3Install::open(Path::new(ROOT)).expect("open install");
    let m = install.mount_superbundle(sb).expect("mount");
    m.load_all_bundles().expect("load bundles");
    let data = PathBuf::from(data);
    let res_dir = data.join(tag).join("res");
    let chunk_dir = data.join("chunks");
    std::fs::create_dir_all(&res_dir).unwrap();
    std::fs::create_dir_all(&chunk_dir).unwrap();

    let mut names: BTreeMap<String, String> = BTreeMap::new();
    for name in m.res_names() {
        if let Some(info) = m.res_info(&name)
            && matches!(info.res_type, MESH | TEX | HAVOK | HAVOK_DESTR)
        {
            names.entry(name.to_ascii_lowercase()).or_insert(name);
        }
    }
    let mut index = String::new();
    let (mut res_bytes, mut chunk_bytes, mut chunks_new, mut missing) = (0u64, 0u64, 0usize, 0usize);
    let mut save_chunk = |id: ChunkId| -> bool {
        let path = chunk_dir.join(format!("{}.chunk", hex(&id.0)));
        if path.exists() {
            return true;
        }
        match m.read_chunk(&Guid(id.0)) {
            Ok(bytes) => {
                chunk_bytes += bytes.len() as u64;
                chunks_new += 1;
                std::fs::write(&path, &bytes).expect("write chunk");
                true
            }
            Err(e) => {
                eprintln!("chunk {}: {e}", Guid(id.0));
                missing += 1;
                false
            }
        }
    };
    for (n, name) in names.values().enumerate() {
        let (info, bytes) = match m.read_res(name) {
            Ok(x) => x,
            Err(e) => {
                eprintln!("res {name}: {e}");
                continue;
            }
        };
        let mut view = u32::MAX;
        match info.res_type {
            MESH => {
                if let Ok(mesh) = MeshSet::parse(&bytes, &info.res_meta) {
                    for lod in &mesh.lods {
                        if let GeometrySource::Chunk(id) = lod.geometry {
                            save_chunk(id);
                        }
                    }
                }
            }
            TEX => {
                if let Ok(tex) = DxTexture::parse(&bytes) {
                    if !tex.chunk_id.is_zero() && save_chunk(tex.chunk_id) {
                        view = m
                            .chunk_info(&Guid(tex.chunk_id.0))
                            .and_then(|i| i.range)
                            .map_or(0, |r| r.logical_offset);
                    }
                }
            }
            _ => {}
        }
        let file = format!("{n:05}.res");
        write_res(&res_dir.join(&file), info.res_type, &info.res_meta, view, name, &bytes);
        res_bytes += bytes.len() as u64;
        writeln!(index, "{file}\t{name}\t{:08x}", info.res_type).unwrap();
    }
    std::fs::write(data.join(tag).join("index.tsv"), index).unwrap();
    eprintln!(
        "{sb}: {} res ({res_bytes} B), {chunks_new} new chunks ({chunk_bytes} B), {missing} missing, {:.1?}",
        names.len(),
        t0.elapsed()
    );
}

// ---------------------------------------------------------------------------------------------
// summary

struct Res {
    name: String,
    res_type: u32,
    meta: [u8; 16],
    view: u32,
    payload: Vec<u8>,
}

fn read_res(path: &Path) -> Res {
    let b = std::fs::read(path).expect("read res");
    let u32_at = |at: usize| u32::from_le_bytes(b[at..at + 4].try_into().unwrap());
    assert_eq!(&b[0..4], b"BF3R");
    let res_type = u32_at(8);
    let meta: [u8; 16] = b[12..28].try_into().unwrap();
    let view = u32_at(28);
    let name_len = u32_at(32) as usize;
    let name = String::from_utf8(b[36..36 + name_len].to_vec()).unwrap();
    let at = 36 + name_len;
    let len = u64::from_le_bytes(b[at..at + 8].try_into().unwrap()) as usize;
    Res { name, res_type, meta, view, payload: b[at + 8..at + 8 + len].to_vec() }
}

fn load_chunk(data: &Path, id: &ChunkId) -> Option<Vec<u8>> {
    std::fs::read(data.join("chunks").join(format!("{}.chunk", hex(&id.0)))).ok()
}

fn usage_raw(u: VertexUsage) -> u8 {
    match u {
        VertexUsage::Position => 1,
        VertexUsage::BoneIndices => 2,
        VertexUsage::BoneIndices2 => 3,
        VertexUsage::BoneWeights => 4,
        VertexUsage::BoneWeights2 => 5,
        VertexUsage::Normal => 6,
        VertexUsage::Tangent => 7,
        VertexUsage::Binormal => 8,
        VertexUsage::BinormalSign => 9,
        VertexUsage::Color0 => 30,
        VertexUsage::Color1 => 31,
        VertexUsage::TexCoord(n) => 33 + n,
        VertexUsage::RadiosityTexCoord => 41,
        VertexUsage::SubMaterialIndex => 50,
        VertexUsage::Other(v) => v,
    }
}

fn format_raw(f: VertexFormat) -> u8 {
    use VertexFormat::*;
    match f {
        None => 0,
        Float => 1,
        Float2 => 2,
        Float3 => 3,
        Float4 => 4,
        Half => 5,
        Half2 => 6,
        Half3 => 7,
        Half4 => 8,
        Byte4 => 10,
        Byte4N => 11,
        UByte4 => 12,
        UByte4N => 13,
        Short => 14,
        Short2 => 15,
        Short3 => 16,
        Short4 => 17,
        ShortN => 18,
        Short2N => 19,
        Short3N => 20,
        Short4N => 21,
        UShort2 => 22,
        UShort4 => 23,
        UShort2N => 24,
        UShort4N => 25,
        UByteN => 50,
        Other(v) => v,
    }
}

fn prim_raw(p: PrimitiveType) -> u8 {
    match p {
        PrimitiveType::PointList => 0,
        PrimitiveType::LineList => 1,
        PrimitiveType::LineStrip => 2,
        PrimitiveType::TriangleList => 3,
        PrimitiveType::TriangleStrip => 5,
        PrimitiveType::Other(v) => v,
    }
}

fn cat_raw(c: Option<SubsetCategory>) -> String {
    match c {
        Some(SubsetCategory::Opaque) => "0".into(),
        Some(SubsetCategory::Transparent) => "1".into(),
        Some(SubsetCategory::TransparentDecal) => "2".into(),
        Some(SubsetCategory::ZOnly) => "3".into(),
        None => "-".into(),
    }
}

fn opt_h<T>(v: &Option<Vec<T>>, f: impl Fn(&mut Fnv, &T)) -> String {
    match v {
        None => "-".into(),
        Some(v) => {
            let mut h = Fnv::new();
            v.iter().for_each(|x| f(&mut h, x));
            h.h()
        }
    }
}

/// Bone lanes of one vertex element over a subset's vertices, as the C++ decoder exposes them:
/// indices as u16 lanes, weights as f32 lanes.
fn bone_lanes(
    geometry: &[u8],
    vertex_offset: usize,
    stride: usize,
    count: usize,
    e: VertexElement,
    as_index: bool,
) -> String {
    let mut h = Fnv::new();
    for v in 0..count {
        let at = vertex_offset + v * stride + usize::from(e.offset);
        let lanes = e.format.decode(&geometry[at..]).unwrap();
        for l in lanes {
            if as_index {
                h.u16(l as u16);
            } else {
                h.f32(l);
            }
        }
    }
    h.h()
}

fn mesh_summary(o: &mut String, data: &Path, r: &Res) {
    let mesh = match MeshSet::parse(&r.payload, &r.meta) {
        Ok(m) => m,
        Err(e) => {
            writeln!(o, "mesh error={}", err(&e)).unwrap();
            return;
        }
    };
    let kind = match mesh.kind {
        MeshKind::Rigid => 0,
        MeshKind::Skinned => 1,
        MeshKind::Composite => 2,
    };
    writeln!(
        o,
        "mesh kind={kind} flags={:#x} lods={} hash={:08x} bmin={} bmax={} name={} short={}",
        mesh.flags,
        mesh.lods.len(),
        mesh.name_hash,
        f3s(mesh.bounds_min),
        f3s(mesh.bounds_max),
        mesh.name,
        mesh.short_name
    )
    .unwrap();
    for (li, lod) in mesh.lods.iter().enumerate() {
        let geom = match lod.geometry {
            GeometrySource::Chunk(id) => format!("chunk:{}", hex(&id.0)),
            GeometrySource::Inline { offset, len } => format!("inline:{offset}+{len}"),
        };
        let mut ch = Fnv::new();
        for list in &lod.category_subsets {
            ch.u32(list.len() as u32);
            ch.bytes(list);
        }
        writeln!(
            o,
            "lod {li} flags={:#x} ib={} v={} i={} geom={geom} subsets={} cats={},{},{},{}:{} hash={:08x} name={} short={}",
            lod.flags,
            lod.index_format.bytes(),
            lod.vertex_data_size,
            lod.index_data_size,
            lod.subsets.len(),
            lod.category_subsets[0].len(),
            lod.category_subsets[1].len(),
            lod.category_subsets[2].len(),
            lod.category_subsets[3].len(),
            ch.h(),
            lod.name_hash,
            lod.name,
            lod.short_name
        )
        .unwrap();
        if !lod.parts.is_empty() {
            let mut h = Fnv::new();
            for p in &lod.parts {
                h.f3(p.bounds_min);
                h.f3(p.bounds_max);
                h.xf(&p.transform);
            }
            writeln!(o, "parts {} {}", lod.parts.len(), h.h()).unwrap();
        }
        if !lod.bones.is_empty() {
            let mut h = Fnv::new();
            for b in &lod.bones {
                h.u32(b.index);
                h.u32(b.name_hash);
            }
            let first: Vec<String> =
                lod.bones.iter().take(4).map(|b| format!("{}:{:08x}", b.index, b.name_hash)).collect();
            writeln!(o, "bones {} {} first={}", lod.bones.len(), h.h(), first.join(",")).unwrap();
        }
        let chunk_bytes;
        let geometry: Option<&[u8]> = match lod.geometry {
            GeometrySource::Chunk(id) => {
                chunk_bytes = load_chunk(data, &id);
                chunk_bytes.as_deref()
            }
            GeometrySource::Inline { .. } => lod.inline_geometry(&r.payload).ok().flatten(),
        };
        if geometry.is_none() {
            writeln!(o, "geom missing").unwrap();
        } else {
            writeln!(o, "geom len={}", geometry.unwrap().len()).unwrap();
        }
        for (si, s) in lod.subsets.iter().enumerate() {
            let mut sh = Fnv::new();
            s.bone_slots.iter().for_each(|&x| sh.u16(x));
            let mut rh = Fnv::new();
            s.tex_coord_ratios.iter().for_each(|&x| rh.f32(x));
            let d = &s.declaration;
            let decl: Vec<String> = d
                .elements
                .iter()
                .map(|e| format!("{}:{}:{}:{}", usage_raw(e.usage), format_raw(e.format), e.offset, e.stream))
                .collect();
            let pm: Vec<String> = s.part_mask.iter().map(|w| format!("{w:x}")).collect();
            writeln!(
                o,
                "sub {si} mat={} prim={} pc={} si={} vo={} vc={} stride={} bpv={} slots={}:{} cat={} pmask={} decl={} ec={} sc={} strides={},{},{},{} ratios={} mname={}",
                s.material_index,
                prim_raw(s.primitive_type),
                s.primitive_count,
                s.start_index,
                s.vertex_offset,
                s.vertex_count,
                s.vertex_stride,
                s.bones_per_vertex,
                s.bone_slots.len(),
                sh.h(),
                cat_raw(s.category),
                pm.join(","),
                decl.join(";"),
                d.element_count,
                d.stream_count,
                d.stream_strides[0],
                d.stream_strides[1],
                d.stream_strides[2],
                d.stream_strides[3],
                rh.h(),
                s.material_name
            )
            .unwrap();
            let Some(geometry) = geometry else { continue };
            match lod.decode_subset(geometry, si) {
                Err(e) => writeln!(o, "dec error={}", err(&e)).unwrap(),
                Ok(dec) => {
                    let mut ph = Fnv::new();
                    dec.positions.iter().for_each(|&p| ph.f3(p));
                    let mut ih = Fnv::new();
                    dec.indices.iter().for_each(|&i| ih.u32(i));
                    let mut bones = String::new();
                    for (label, usage, as_index) in [
                        ("bi", VertexUsage::BoneIndices, true),
                        ("bi2", VertexUsage::BoneIndices2, true),
                        ("bw", VertexUsage::BoneWeights, false),
                        ("bw2", VertexUsage::BoneWeights2, false),
                    ] {
                        if let Some(e) = d.find(usage) {
                            let h = bone_lanes(
                                &geometry[..lod.vertex_data_size as usize],
                                s.vertex_offset as usize,
                                usize::from(s.vertex_stride),
                                s.vertex_count as usize,
                                e,
                                as_index,
                            );
                            write!(bones, " {label}={h}").unwrap();
                        }
                    }
                    writeln!(
                        o,
                        "dec n={} pos={} nrm={} tan={} bs={} uv0={} uv1={} uv2={} rad={} col={} part={}{bones} idx={}:{}",
                        dec.positions.len(),
                        ph.h(),
                        opt_h(&dec.normals, |h, &v| h.f3(v)),
                        opt_h(&dec.tangents, |h, &v| h.f3(v)),
                        opt_h(&dec.binormal_signs, |h, &v| h.f32(v)),
                        opt_h(&dec.uv0, |h, v| v.iter().for_each(|&x| h.f32(x))),
                        opt_h(&dec.uv1, |h, v| v.iter().for_each(|&x| h.f32(x))),
                        opt_h(&dec.uv2, |h, v| v.iter().for_each(|&x| h.f32(x))),
                        opt_h(&dec.radiosity_uv, |h, v| v.iter().for_each(|&x| h.f32(x))),
                        opt_h(&dec.colors, |h, v| h.bytes(v)),
                        opt_h(&dec.parts, |h, &v| h.u16(v)),
                        dec.indices.len(),
                        ih.h()
                    )
                    .unwrap();
                }
            }
        }
    }
}

fn pixel_name(p: Option<PixelFormat>) -> &'static str {
    match p {
        Some(PixelFormat::Bc1) => "bc1",
        Some(PixelFormat::Bc2) => "bc2",
        Some(PixelFormat::Bc3) => "bc3",
        Some(PixelFormat::Bc4) => "bc4",
        Some(PixelFormat::Bc5) => "bc5",
        Some(PixelFormat::Rgba8) => "rgba8",
        Some(PixelFormat::L8) => "l8",
        Some(PixelFormat::L16) => "l16",
        None => "-",
    }
}

fn texture_summary(o: &mut String, data: &Path, r: &Res) {
    let tex = match DxTexture::parse(&r.payload) {
        Ok(t) => t,
        Err(e) => {
            writeln!(o, "tex error={}", err(&e)).unwrap();
            return;
        }
    };
    let raw = |at: usize| u32::from_le_bytes(r.payload[at..at + 4].try_into().unwrap());
    let sizes: Vec<String> = tex.mip_sizes.iter().map(|s| s.to_string()).collect();
    writeln!(
        o,
        "tex kind={} fmt={} pixel={} normal={} flags={:#x} srgb={} {}x{}x{} slices={} faces={} mips={} base={} chunk={} chain={} hash={:08x} group={} sizes={}",
        raw(4),
        raw(8),
        pixel_name(tex.format.pixel_format()),
        u8::from(tex.format.is_normal_map()),
        tex.flags,
        u8::from(tex.is_srgb()),
        tex.width,
        tex.height,
        tex.depth,
        tex.slice_count,
        tex.faces(),
        tex.mip_count,
        tex.mip_base,
        hex(&tex.chunk_id.0),
        tex.mip_chain_size,
        tex.name_hash,
        tex.texture_group,
        sizes.join(",")
    )
    .unwrap();
    if tex.chunk_id.is_zero() {
        return;
    }
    let Some(chunk) = load_chunk(data, &tex.chunk_id) else {
        writeln!(o, "chunk missing").unwrap();
        return;
    };
    writeln!(o, "chunk len={}", chunk.len()).unwrap();
    match tex.split_mips(&chunk, 0) {
        Err(e) => writeln!(o, "split error={}", err(&e)).unwrap(),
        Ok(mips) => {
            for m in &mips {
                let mut h = Fnv::new();
                h.bytes(m.data);
                writeln!(
                    o,
                    "mip f={} m={} {}x{}x{} pitch={} len={} {}",
                    m.face,
                    m.mip,
                    m.width,
                    m.height,
                    m.depth,
                    m.row_pitch,
                    m.data.len(),
                    h.h()
                )
                .unwrap();
                if m.face == 0 && m.width * m.height <= 65536 {
                    match m.texels(tex.format) {
                        None => {}
                        Some(t) => {
                            let mut h = Fnv::new();
                            t.iter().for_each(|&x| h.f4(x));
                            writeln!(o, "texels m={} {}", m.mip, h.h()).unwrap();
                        }
                    }
                }
            }
        }
    }
    if r.view != u32::MAX && r.view != 0 {
        let lo = r.view as usize;
        let first = tex.first_mip_at(u64::from(r.view));
        let first = first.map_or("-".to_string(), |f| f.to_string());
        if lo > chunk.len() {
            writeln!(o, "view lo={lo} beyond chunk").unwrap();
        } else {
            match tex.split_mips(&chunk[lo..], u64::from(r.view)) {
                Err(e) => writeln!(o, "view lo={lo} first={first} error={}", err(&e)).unwrap(),
                Ok(mips) => {
                    let mut h = Fnv::new();
                    for m in &mips {
                        h.u32(m.face);
                        h.u8(m.mip);
                        h.u32(m.data.len() as u32);
                        h.bytes(m.data);
                    }
                    writeln!(o, "view lo={lo} first={first} n={} {}", mips.len(), h.h()).unwrap();
                }
            }
        }
    }
}

fn shape_letter(s: &Shape) -> char {
    match s {
        Shape::MoppBvTree { .. } => 'M',
        Shape::List { .. } => 'L',
        Shape::ConvexTranslate { .. } => 'T',
        Shape::ConvexTransform { .. } => 'X',
        Shape::Box { .. } => 'B',
        Shape::Sphere { .. } => 'S',
        Shape::Capsule { .. } => 'C',
        Shape::Cylinder { .. } => 'Y',
        Shape::ConvexVertices { .. } => 'V',
        Shape::CompressedMesh(_) => 'Q',
        Shape::ExtendedMesh(_) => 'E',
        Shape::Unsupported { .. } => 'U',
    }
}

fn opt16(v: Option<u16>) -> u32 {
    v.map_or(u32::MAX, u32::from)
}

/// Pre-order walk: letters, node count, full-content hash.
fn shape_walk(s: &Shape, letters: &mut String, nodes: &mut usize, h: &mut Fnv) {
    *nodes += 1;
    let c = shape_letter(s);
    letters.push(c);
    h.u8(c as u8);
    match s {
        Shape::MoppBvTree { child } => shape_walk(child, letters, nodes, h),
        Shape::List { children, collision_filter_infos } => {
            h.u32(children.len() as u32);
            children.iter().for_each(|c| shape_walk(c, letters, nodes, h));
            collision_filter_infos.iter().for_each(|&x| h.u32(x));
        }
        Shape::ConvexTranslate { translation, child } => {
            h.f3(*translation);
            shape_walk(child, letters, nodes, h);
        }
        Shape::ConvexTransform { transform, child } => {
            h.xf(transform);
            shape_walk(child, letters, nodes, h);
        }
        Shape::Box { half_extents, radius, user_data } => {
            h.f3(*half_extents);
            h.f32(*radius);
            h.u32(*user_data);
        }
        Shape::Sphere { radius, user_data } => {
            h.f32(*radius);
            h.u32(*user_data);
        }
        Shape::Capsule { a, b, radius, user_data } => {
            h.f3(*a);
            h.f3(*b);
            h.f32(*radius);
            h.u32(*user_data);
        }
        Shape::Cylinder { a, b, cylinder_radius, radius, user_data } => {
            h.f3(*a);
            h.f3(*b);
            h.f32(*cylinder_radius);
            h.f32(*radius);
            h.u32(*user_data);
        }
        Shape::ConvexVertices { points, planes, radius, user_data } => {
            h.u32(points.len() as u32);
            points.iter().for_each(|&p| h.f3(p));
            h.u32(planes.len() as u32);
            planes.iter().for_each(|&p| h.f4(p));
            h.f32(*radius);
            h.u32(*user_data);
        }
        Shape::CompressedMesh(m) => {
            h.f32(m.error);
            h.f3(m.bounds_min);
            h.f3(m.bounds_max);
            h.u32(m.big_vertices.len() as u32);
            m.big_vertices.iter().for_each(|&p| h.f3(p));
            h.u32(m.big_triangles.len() as u32);
            for t in &m.big_triangles {
                t.indices.iter().for_each(|&i| h.u16(i));
                h.u32(t.part);
                h.u16(t.material);
                h.u32(opt16(t.transform_index));
            }
            h.u32(m.chunks.len() as u32);
            for c in &m.chunks {
                h.f3(c.offset);
                h.u32(c.vertices.len() as u32);
                c.vertices.iter().for_each(|v| v.iter().for_each(|&x| h.u16(x)));
                h.u32(c.indices.len() as u32);
                c.indices.iter().for_each(|&x| h.u16(x));
                h.u32(c.strip_lengths.len() as u32);
                c.strip_lengths.iter().for_each(|&x| h.u16(x));
                h.u32(c.part);
                h.u32(c.material);
                h.u32(opt16(c.reference));
                h.u32(opt16(c.transform_index));
            }
            h.u32(m.transforms.len() as u32);
            m.transforms.iter().for_each(|t| h.xf(t));
            h.u32(m.convex_piece_count);
        }
        Shape::ExtendedMesh(m) => {
            h.u32(m.triangle_subparts.len() as u32);
            for t in &m.triangle_subparts {
                h.xf(&t.transform);
                h.u32(t.vertices.len() as u32);
                t.vertices.iter().for_each(|&p| h.f3(p));
                h.u32(t.triangles.len() as u32);
                t.triangles.iter().for_each(|tr| tr.iter().for_each(|&x| h.u32(x)));
                h.u32(t.materials.len() as u32);
                t.materials.iter().for_each(|&x| h.u32(x));
                h.u32(t.user_data);
            }
            h.u32(m.shape_subparts.len() as u32);
            for s in &m.shape_subparts {
                h.f4(s.rotation);
                h.f3(s.translation);
                h.u32(s.user_data);
                h.u32(s.children.len() as u32);
                s.children.iter().for_each(|c| shape_walk(c, letters, nodes, h));
            }
        }
        Shape::Unsupported { class } => {
            h.u32(class.len() as u32);
            h.bytes(class.as_bytes());
        }
    }
}

fn havok_summary(o: &mut String, r: &Res) {
    let hk = match HavokPhysicsData::parse(&r.payload, &r.meta) {
        Ok(h) => h,
        Err(e) => {
            writeln!(o, "havok error={}", err(&e)).unwrap();
            return;
        }
    };
    let mut th = Fnv::new();
    hk.part_translations.iter().for_each(|&p| th.f3(p));
    let mut ah = Fnv::new();
    hk.local_aabbs.iter().for_each(|&(a, b)| {
        ah.f3(a);
        ah.f3(b)
    });
    let mut mh = Fnv::new();
    mh.bytes(&hk.material_indices);
    let mut fh = Fnv::new();
    hk.material_flags_and_indices.iter().for_each(|&x| fh.u32(x));
    let mut ch = Fnv::new();
    for obj in &hk.packfile.objects {
        ch.u32(obj.offset);
        ch.bytes(obj.class.as_bytes());
        ch.u8(0);
    }
    let mut dh = Fnv::new();
    dh.bytes(&hk.packfile.data);
    writeln!(
        o,
        "havok parts={} trans={}:{} aabbs={}:{} mats={}:{} mfi={}:{} scale={} mcu={} hmi={} ver={} objs={}:{} data={}:{} contents={}",
        hk.part_count,
        hk.part_translations.len(),
        th.h(),
        hk.local_aabbs.len(),
        ah.h(),
        hk.material_indices.len(),
        mh.h(),
        hk.material_flags_and_indices.len(),
        fh.h(),
        fb(hk.scale),
        hk.material_count_used,
        hk.highest_material_index,
        hk.packfile.version,
        hk.packfile.objects.len(),
        ch.h(),
        hk.packfile.data.len(),
        dh.h(),
        hk.packfile.contents_offset
    )
    .unwrap();
    match hk.shapes() {
        Err(e) => writeln!(o, "roots error={}", err(&e)).unwrap(),
        Ok(shapes) => {
            writeln!(o, "roots {}", shapes.len()).unwrap();
            for (i, s) in shapes.iter().enumerate() {
                let (mut letters, mut nodes, mut h) = (String::new(), 0usize, Fnv::new());
                shape_walk(s, &mut letters, &mut nodes, &mut h);
                if letters.len() > 48 {
                    letters.truncate(48);
                    letters.push_str("...");
                }
                let tris = triangles(s, &Transform::IDENTITY);
                let mut tri_h = Fnv::new();
                for t in &tris {
                    t.v.iter().for_each(|&v| tri_h.f3(v));
                    tri_h.u32(t.material);
                    tri_h.u32(t.part);
                    tri_h.u32(t.user_data);
                }
                let pieces = convex_pieces(s, &Transform::IDENTITY);
                let mut cp_h = Fnv::new();
                for p in &pieces {
                    cp_h.u32(p.points.len() as u32);
                    p.points.iter().for_each(|&v| cp_h.f3(v));
                    cp_h.u32(p.planes.len() as u32);
                    p.planes.iter().for_each(|&v| cp_h.f4(v));
                    cp_h.f32(p.radius);
                    cp_h.u32(p.part);
                    cp_h.u32(p.user_data);
                }
                writeln!(
                    o,
                    "root {i} nodes={nodes} tree={letters} th={} tris={}:{} convex={}:{}",
                    h.h(),
                    tris.len(),
                    tri_h.h(),
                    pieces.len(),
                    cp_h.h()
                )
                .unwrap();
            }
        }
    }
    match hk.part_connections() {
        Err(e) => writeln!(o, "conn error={}", err(&e)).unwrap(),
        Ok(c) if c.is_empty() => {}
        Ok(c) => {
            let mut h = Fnv::new();
            c.iter().for_each(|p| p.iter().for_each(|&x| h.u32(x)));
            writeln!(o, "conn {}:{}", c.len(), h.h()).unwrap();
        }
    }
    match hk.group_subparts() {
        Err(e) => writeln!(o, "group error={}", err(&e)).unwrap(),
        Ok(g) if g.is_empty() => {}
        Ok(g) => {
            let mut h = Fnv::new();
            for s in &g {
                h.f4(s.rotation);
                h.f3(s.translation);
                h.u32(s.user_data);
            }
            writeln!(o, "group {}:{}", g.len(), h.h()).unwrap();
        }
    }
}

fn cmd_summary(data: &str, tags: &[String]) {
    let data = PathBuf::from(data);
    let mut time: BTreeMap<&str, (usize, Duration)> = BTreeMap::new();
    let stdout = std::io::stdout();
    let mut w = std::io::BufWriter::new(stdout.lock());
    for tag in tags {
        writeln!(w, "## {tag}").unwrap();
        let index = std::fs::read_to_string(data.join(tag).join("index.tsv")).expect("index");
        for line in index.lines() {
            let file = line.split('\t').next().unwrap();
            let r = read_res(&data.join(tag).join("res").join(file));
            let mut o = String::new();
            let (label, t0) = match r.res_type {
                MESH => ("MeshSet", Instant::now()),
                TEX => ("DxTexture", Instant::now()),
                HAVOK => ("HavokPhysicsData", Instant::now()),
                _ => ("HavokDestructionPhysicsData", Instant::now()),
            };
            writeln!(o, "== {} {} len={}", r.name, label, r.payload.len()).unwrap();
            match r.res_type {
                MESH => mesh_summary(&mut o, &data, &r),
                TEX => texture_summary(&mut o, &data, &r),
                _ => havok_summary(&mut o, &r),
            }
            let e = time.entry(label).or_default();
            e.0 += 1;
            e.1 += t0.elapsed();
            w.write_all(o.as_bytes()).unwrap();
        }
    }
    for (label, (n, d)) in time {
        eprintln!("{label}: {n} res, {:.3} s (decode + hash)", d.as_secs_f64());
    }
}

/// Damaged-input parity: payloads cut at 1/4, 1/2, 3/4 and len-1, geometry and texture chunks
/// cut in half. Prints the decoder's error text (or ok) for each attempt.
fn cmd_trunc(data: &str, tags: &[String]) {
    let data = PathBuf::from(data);
    let stdout = std::io::stdout();
    let mut w = std::io::BufWriter::new(stdout.lock());
    for tag in tags {
        writeln!(w, "## {tag}").unwrap();
        let index = std::fs::read_to_string(data.join(tag).join("index.tsv")).expect("index");
        for line in index.lines() {
            let file = line.split('\t').next().unwrap();
            let r = read_res(&data.join(tag).join("res").join(file));
            let mut o = String::new();
            writeln!(o, "== {} {:08x}", r.name, r.res_type).unwrap();
            let len = r.payload.len();
            let mut cuts = vec![len / 4, len / 2, 3 * len / 4, len.saturating_sub(1)];
            cuts.dedup();
            for cut in cuts {
                let p = &r.payload[..cut];
                let res = match r.res_type {
                    MESH => MeshSet::parse(p, &r.meta).map(|m| format!("lods={}", m.lods.len())),
                    TEX => DxTexture::parse(p).map(|t| format!("mips={}", t.mip_count)),
                    _ => HavokPhysicsData::parse(p, &r.meta).and_then(|h| h.shapes().map(|s| format!("roots={}", s.len()))),
                };
                match res {
                    Ok(x) => writeln!(o, "cut {cut} ok {x}").unwrap(),
                    Err(e) => writeln!(o, "cut {cut} error={e}").unwrap(),
                }
            }
            if r.res_type == MESH
                && let Ok(mesh) = MeshSet::parse(&r.payload, &r.meta)
            {
                for (li, lod) in mesh.lods.iter().enumerate() {
                    let chunk;
                    let geometry: Option<&[u8]> = match lod.geometry {
                        GeometrySource::Chunk(id) => {
                            chunk = load_chunk(&data, &id);
                            chunk.as_deref()
                        }
                        GeometrySource::Inline { .. } => lod.inline_geometry(&r.payload).ok().flatten(),
                    };
                    let Some(g) = geometry else { continue };
                    for cut in [g.len() / 2, (lod.vertex_data_size as usize).min(g.len())] {
                        let mut parts = Vec::new();
                        for si in 0..lod.subsets.len() {
                            parts.push(match lod.decode_subset(&g[..cut], si) {
                                Ok(d) => format!("ok{}", d.indices.len()),
                                Err(e) => e.to_string(),
                            });
                        }
                        writeln!(o, "lod {li} cut {cut}: {}", parts.join("|")).unwrap();
                    }
                }
            }
            if r.res_type == TEX
                && let Ok(tex) = DxTexture::parse(&r.payload)
                && !tex.chunk_id.is_zero()
                && let Some(chunk) = load_chunk(&data, &tex.chunk_id)
            {
                let cut = chunk.len() / 2;
                match tex.split_mips(&chunk[..cut], 0) {
                    Ok(m) => writeln!(o, "split cut {cut} ok {}", m.len()).unwrap(),
                    Err(e) => writeln!(o, "split cut {cut} error={e}").unwrap(),
                }
                let lo = tex.logical_offset(1);
                if (lo as usize) < chunk.len() {
                    match tex.split_mips(&chunk[lo as usize..], lo + 4) {
                        Ok(m) => writeln!(o, "split lo+4 ok {}", m.len()).unwrap(),
                        Err(e) => writeln!(o, "split lo+4 error={e}").unwrap(),
                    }
                }
            }
            w.write_all(o.as_bytes()).unwrap();
        }
    }
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    match args.first().map(String::as_str) {
        Some("export") => cmd_export(&args[1], &args[2], &args[3]),
        Some("summary") => cmd_summary(&args[1], &args[2..]),
        Some("trunc") => cmd_trunc(&args[1], &args[2..]),
        _ => eprintln!("usage: export <sb> <tag> <data_dir> | summary <data_dir> <tag>..."),
    }
}
