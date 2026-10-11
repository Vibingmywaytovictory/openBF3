use std::collections::HashMap;
use std::collections::hash_map::Entry;
use std::num::NonZeroUsize;
use std::path::Path;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, PoisonError, RwLock};

use superbundle_bf3::{
    BundleManifest, ChunkMeta, ChunkRange, DeltaBundle, DeltaView, EBX_HEAD_LEN, Guid, Payload,
    Sha1, Toc, TocBundle, TocChunkLocation, asset_name_hash, binary_manifest_len, decode_blocks,
    decode_chunk, delta_table_len, ebx_partition, parse_binary_manifest, parse_cas_manifest,
};

use super::Bf3Error;
use super::file::DiskFile;
use super::install::{Bf3Install, Bf3Level, SuperbundleSite, read_toc, superbundle_file};

const CHUNK_SUPERBUNDLES: [&str; 5] = [
    "Win32/Chunks0",
    "Win32/Chunks1",
    "Win32/Chunks2",
    "Win32/MpChunks",
    "Win32/SpChunks",
];

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ResInfo {
    pub res_type: u32,
    pub res_meta: [u8; 16],
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ChunkInfo {
    pub sha1: Sha1,
    pub stored_size: u64,
    pub range: Option<ChunkRange>,
    pub tail_only: bool,
    pub meta: Option<ChunkMeta>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct BundleSummary {
    pub name: String,
    pub ebx: usize,
    pub res: usize,
    pub chunks: usize,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum EntryRef<'a> {
    Ebx(&'a str),
    Res(&'a str),
    Chunk(Guid),
}

pub struct LevelMount {
    install: Arc<Bf3Install>,
    level: Bf3Level,
    bundles: Vec<BundleSite>,
    bundle_keys: HashMap<String, usize>,
    chunk_superbundles: Vec<String>,
    toc_chunks: HashMap<Guid, TocChunkSite>,
    index: RwLock<Index>,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum BundleFormat {
    Cas,
    Binary,
}

struct BundleSite {
    id: String,
    format: BundleFormat,
    bytes: BundleBytes,
}

enum BundleBytes {
    File {
        file: Arc<DiskFile>,
        offset: u64,
        len: u64,
    },
    Delta {
        view: DeltaView<Arc<DiskFile>, Arc<DiskFile>>,
        patch: Arc<DiskFile>,
    },
}

impl BundleBytes {
    fn len(&self) -> u64 {
        match self {
            Self::File { len, .. } => *len,
            Self::Delta { view, .. } => view.len(),
        }
    }

    fn read_vec(&self, offset: u64, len: u64) -> Result<Vec<u8>, Bf3Error> {
        let end = offset.checked_add(len).filter(|&end| end <= self.len());
        let Some(_) = end else {
            return Err(Bf3Error::OutOfFile {
                path: self.path().to_path_buf(),
                offset,
                len,
                file_len: self.len(),
            });
        };
        match self {
            Self::File {
                file, offset: at, ..
            } => file.read_vec(at + offset, len),
            Self::Delta { view, patch } => {
                let mut buf = vec![0u8; len as usize];
                view.read_at(offset, &mut buf)
                    .map_err(|error| Bf3Error::format(patch.path().display(), error))?;
                Ok(buf)
            }
        }
    }

    fn path(&self) -> &Path {
        match self {
            Self::File { file, .. } => file.path(),
            Self::Delta { patch, .. } => patch.path(),
        }
    }
}

#[derive(Clone)]
enum TocChunkSite {
    Cas(Sha1),
    File {
        file: Arc<DiskFile>,
        offset: u64,
        size: u64,
    },
}

#[derive(Clone)]
struct Stored {
    sha1: Sha1,
    size: u64,
    place: Place,
}

#[derive(Clone)]
enum Place {
    Cas,
    Inline(Arc<[u8]>),
    Bundle { bundle: usize, offset: u64 },
}

struct EbxRecord {
    name: String,
    stored: Stored,
    original_size: u64,
}

struct ResRecord {
    name: String,
    stored: Stored,
    original_size: u64,
    info: ResInfo,
}

struct ChunkRecord {
    stored: Stored,
    range: Option<ChunkRange>,
    tail_only: bool,
    meta: Option<ChunkMeta>,
}

#[derive(Default)]
struct Index {
    loaded: Vec<Option<BundleSummary>>,
    load_order: Vec<usize>,
    ebx: Vec<EbxRecord>,
    ebx_keys: HashMap<String, usize>,
    res: Vec<ResRecord>,
    res_keys: HashMap<String, usize>,
    chunks: HashMap<Guid, ChunkRecord>,
    chunk_order: Vec<Guid>,
    asset_chunk_meta: HashMap<u32, ChunkMeta>,
}

impl Index {
    fn register(&mut self, bundle: usize, name: &str, manifest: BundleManifest) {
        if self.loaded[bundle].is_some() {
            return;
        }
        let summary = BundleSummary {
            name: name.to_owned(),
            ebx: manifest.ebx.len(),
            res: manifest.res.len(),
            chunks: manifest.chunks.len(),
        };
        let place = |payload: Payload| match payload {
            Payload::Cas => Place::Cas,
            Payload::Inline(bytes) => Place::Inline(Arc::from(bytes)),
            Payload::Bundle { offset } => Place::Bundle { bundle, offset },
        };
        for entry in manifest.ebx {
            if let Entry::Vacant(slot) = self.ebx_keys.entry(asset_key(&entry.name)) {
                slot.insert(self.ebx.len());
                self.ebx.push(EbxRecord {
                    stored: Stored {
                        sha1: entry.sha1,
                        size: entry.stored_size,
                        place: place(entry.payload),
                    },
                    original_size: entry.original_size,
                    name: entry.name,
                });
            }
        }
        for entry in manifest.res {
            if let Entry::Vacant(slot) = self.res_keys.entry(asset_key(&entry.name)) {
                slot.insert(self.res.len());
                self.res.push(ResRecord {
                    stored: Stored {
                        sha1: entry.sha1,
                        size: entry.stored_size,
                        place: place(entry.payload),
                    },
                    original_size: entry.original_size,
                    info: ResInfo {
                        res_type: entry.res_type,
                        res_meta: entry.res_meta,
                    },
                    name: entry.name,
                });
            }
        }
        for entry in manifest.chunks {
            let record = ChunkRecord {
                stored: Stored {
                    sha1: entry.sha1,
                    size: entry.stored_size,
                    place: place(entry.payload),
                },
                range: entry.range,
                tail_only: entry.tail_only,
                meta: entry.meta,
            };
            match self.chunks.entry(entry.id) {
                Entry::Vacant(slot) => {
                    slot.insert(record);
                    self.chunk_order.push(entry.id);
                }
                Entry::Occupied(mut slot) => {
                    if slot.get().tail_only && !record.tail_only {
                        slot.insert(record);
                    }
                }
            }
        }
        for meta in manifest.chunk_meta {
            self.asset_chunk_meta.entry(meta.h32).or_insert(meta);
        }
        self.loaded[bundle] = Some(summary);
        self.load_order.push(bundle);
    }
}

impl LevelMount {
    pub fn mount(install: Arc<Bf3Install>, level: Bf3Level) -> Result<Self, Bf3Error> {
        let site = install
            .site(&level.superbundle)
            .ok_or_else(|| Bf3Error::UnknownLevel(level.id.clone()))?;
        let bundles = superbundle_bundles(&install, site)?;
        let mut bundle_keys = HashMap::with_capacity(bundles.len());
        for (at, bundle) in bundles.iter().enumerate() {
            bundle_keys.entry(bundle_key(&bundle.id)).or_insert(at);
        }

        let expansion_chunks = level
            .expansion
            .map(|expansion| format!("Win32/Xp{expansion}Chunks"));
        let mut chunk_superbundles = Vec::new();
        let mut toc_chunks = HashMap::new();
        for name in CHUNK_SUPERBUNDLES
            .iter()
            .copied()
            .chain(expansion_chunks.as_deref())
        {
            let Some(site) = install.site(name) else {
                continue;
            };
            let mut chunks = HashMap::new();
            for toc_path in [&site.base_toc, &site.patch_toc].into_iter().flatten() {
                let toc = read_toc(toc_path)?;
                let file = if toc
                    .chunks
                    .iter()
                    .any(|chunk| matches!(chunk.location, TocChunkLocation::Superbundle { .. }))
                {
                    Some(install.files().get(&superbundle_file(toc_path))?)
                } else {
                    None
                };
                for chunk in toc.chunks {
                    let located = match (chunk.location, &file) {
                        (TocChunkLocation::Cas(sha1), _) => TocChunkSite::Cas(sha1),
                        (TocChunkLocation::Superbundle { offset, size }, Some(file)) => {
                            TocChunkSite::File {
                                file: Arc::clone(file),
                                offset,
                                size,
                            }
                        }
                        (TocChunkLocation::Superbundle { .. }, None) => continue,
                    };
                    chunks.insert(chunk.id, located);
                }
            }
            for (id, located) in chunks {
                toc_chunks.entry(id).or_insert(located);
            }
            chunk_superbundles.push(site.name.clone());
        }

        let index = Index {
            loaded: vec![None; bundles.len()],
            ..Index::default()
        };
        Ok(Self {
            install,
            level,
            bundles,
            bundle_keys,
            chunk_superbundles,
            toc_chunks,
            index: RwLock::new(index),
        })
    }

    pub fn level(&self) -> &Bf3Level {
        &self.level
    }

    pub fn install(&self) -> &Arc<Bf3Install> {
        &self.install
    }

    pub fn bundle_names(&self) -> Vec<String> {
        self.bundles
            .iter()
            .map(|bundle| bundle.id.clone())
            .collect()
    }

    pub fn chunk_superbundles(&self) -> &[String] {
        &self.chunk_superbundles
    }

    pub fn toc_chunk_count(&self) -> usize {
        self.toc_chunks.len()
    }

    pub fn has_chunk(&self, id: &Guid) -> bool {
        self.toc_chunks.contains_key(id)
            || self
                .index
                .read()
                .unwrap_or_else(PoisonError::into_inner)
                .chunks
                .contains_key(id)
    }

    pub fn load_all_bundles(&self) -> Result<(), Bf3Error> {
        let all = (0..self.bundles.len()).collect::<Vec<_>>();
        self.load_bundle_indices(all)
    }

    pub fn load_bundles<S: AsRef<str>>(&self, names: &[S]) -> Result<(), Bf3Error> {
        let mut wanted = Vec::with_capacity(names.len());
        for name in names {
            let name = name.as_ref();
            let at = *self
                .bundle_keys
                .get(&bundle_key(name))
                .ok_or_else(|| Bf3Error::UnknownBundle(name.to_owned()))?;
            if !wanted.contains(&at) {
                wanted.push(at);
            }
        }
        self.load_bundle_indices(wanted)
    }

    fn load_bundle_indices(&self, mut wanted: Vec<usize>) -> Result<(), Bf3Error> {
        {
            let index = self.index.read().unwrap_or_else(PoisonError::into_inner);
            wanted.retain(|&at| index.loaded[at].is_none());
        }
        if wanted.is_empty() {
            return Ok(());
        }
        let manifests = parallel_map(&wanted, |&at| self.read_manifest(at));
        let mut index = self.index.write().unwrap_or_else(PoisonError::into_inner);
        let mut failure = None;
        for (&at, manifest) in wanted.iter().zip(manifests) {
            match manifest {
                Ok(manifest) => index.register(at, &self.bundles[at].id, manifest),
                Err(error) => {
                    failure.get_or_insert(error);
                }
            }
        }
        failure.map_or(Ok(()), Err)
    }

    fn read_manifest(&self, at: usize) -> Result<BundleManifest, Bf3Error> {
        let bundle = &self.bundles[at];
        let where_ = || format!("{} bundle {}", bundle.bytes.path().display(), bundle.id);
        match bundle.format {
            BundleFormat::Cas => {
                let bytes = bundle.bytes.read_vec(0, bundle.bytes.len())?;
                parse_cas_manifest(&bytes).map_err(|error| Bf3Error::format(where_(), error))
            }
            BundleFormat::Binary => {
                let prefix = bundle.bytes.read_vec(0, 4)?;
                let len = binary_manifest_len(&prefix)
                    .map_err(|error| Bf3Error::format(where_(), error))?;
                let head = bundle.bytes.read_vec(0, len)?;
                parse_binary_manifest(&head, bundle.bytes.len())
                    .map_err(|error| Bf3Error::format(where_(), error))
            }
        }
    }

    /// Probe addition: every bundle's own manifest, so entries can be attributed to bundles.
    pub fn bundle_manifests(&self) -> Vec<(String, Result<BundleManifest, Bf3Error>)> {
        let all = (0..self.bundles.len()).collect::<Vec<_>>();
        let manifests = parallel_map(&all, |&at| self.read_manifest(at));
        all.iter()
            .zip(manifests)
            .map(|(&at, manifest)| (self.bundles[at].id.clone(), manifest))
            .collect()
    }

    pub fn loaded_bundles(&self) -> Vec<BundleSummary> {
        let index = self.index.read().unwrap_or_else(PoisonError::into_inner);
        index
            .load_order
            .iter()
            .filter_map(|&at| index.loaded[at].clone())
            .collect()
    }

    pub fn ebx_names(&self) -> Vec<String> {
        let index = self.index.read().unwrap_or_else(PoisonError::into_inner);
        index.ebx.iter().map(|record| record.name.clone()).collect()
    }

    pub fn res_names(&self) -> Vec<String> {
        let index = self.index.read().unwrap_or_else(PoisonError::into_inner);
        index.res.iter().map(|record| record.name.clone()).collect()
    }

    pub fn chunk_ids(&self) -> Vec<Guid> {
        let index = self.index.read().unwrap_or_else(PoisonError::into_inner);
        index.chunk_order.clone()
    }

    pub fn has_ebx(&self, name: &str) -> bool {
        let index = self.index.read().unwrap_or_else(PoisonError::into_inner);
        index.ebx_keys.contains_key(&asset_key(name))
    }

    pub fn res_info(&self, name: &str) -> Option<ResInfo> {
        let index = self.index.read().unwrap_or_else(PoisonError::into_inner);
        let &at = index.res_keys.get(&asset_key(name))?;
        Some(index.res[at].info)
    }

    /// Only CAS bundles pair a chunk with its meta entry; for a chunk from a
    /// binary bundle ask [`Self::asset_chunk_meta`] with the owning asset.
    pub fn chunk_meta(&self, id: &Guid) -> Option<ChunkMeta> {
        self.chunk_info(id)?.meta
    }

    pub fn asset_chunk_meta(&self, owner: &str) -> Option<ChunkMeta> {
        let index = self.index.read().unwrap_or_else(PoisonError::into_inner);
        index.asset_chunk_meta.get(&asset_name_hash(owner)).copied()
    }

    pub fn chunk_info(&self, id: &Guid) -> Option<ChunkInfo> {
        let index = self.index.read().unwrap_or_else(PoisonError::into_inner);
        index.chunks.get(id).map(|record| ChunkInfo {
            sha1: record.stored.sha1,
            stored_size: record.stored.size,
            range: record.range,
            tail_only: record.tail_only,
            meta: record.meta,
        })
    }

    pub fn read_ebx(&self, name: &str) -> Result<Vec<u8>, Bf3Error> {
        let (stored, original_size, name) = {
            let index = self.index.read().unwrap_or_else(PoisonError::into_inner);
            let &at = index
                .ebx_keys
                .get(&asset_key(name))
                .ok_or_else(|| Bf3Error::EbxNotLoaded(name.to_owned()))?;
            let record = &index.ebx[at];
            (
                record.stored.clone(),
                record.original_size,
                record.name.clone(),
            )
        };
        self.decode_named(&stored, original_size, &name)
    }

    pub fn read_res(&self, name: &str) -> Result<(ResInfo, Vec<u8>), Bf3Error> {
        let (stored, original_size, info, name) = {
            let index = self.index.read().unwrap_or_else(PoisonError::into_inner);
            let &at = index
                .res_keys
                .get(&asset_key(name))
                .ok_or_else(|| Bf3Error::ResNotLoaded(name.to_owned()))?;
            let record = &index.res[at];
            (
                record.stored.clone(),
                record.original_size,
                record.info,
                record.name.clone(),
            )
        };
        Ok((info, self.decode_named(&stored, original_size, &name)?))
    }

    pub fn read_chunk(&self, id: &Guid) -> Result<Vec<u8>, Bf3Error> {
        let record = {
            let index = self.index.read().unwrap_or_else(PoisonError::into_inner);
            index
                .chunks
                .get(id)
                .map(|record| (record.stored.clone(), record.tail_only, record.range))
        };
        let stored = match record {
            Some((stored, false, _)) => self.read_stored_bytes(&stored)?,
            Some((stored, true, _)) if self.install.cas().contains(&stored.sha1) => {
                self.install.cas().read(&stored.sha1)?
            }
            Some((_, true, range)) => {
                let whole = match self.toc_chunks.get(id) {
                    Some(site) => self.read_toc_chunk(site)?,
                    None => return Err(Bf3Error::ChunkIncomplete(*id)),
                };
                let expected = range.map_or(0, |range| u64::from(range.end));
                if whole.len() as u64 != expected {
                    return Err(Bf3Error::SizeMismatch {
                        what: format!("whole copy of chunk {id}"),
                        expected,
                        got: whole.len() as u64,
                    });
                }
                whole
            }
            None => match self.toc_chunks.get(id) {
                Some(site) => self.read_toc_chunk(site)?,
                None => return Err(Bf3Error::ChunkNotFound(*id)),
            },
        };
        decode_chunk(id, &stored).map_err(|error| Bf3Error::format(format!("chunk {id}"), error))
    }

    pub fn ebx_partition_index(&self) -> Result<HashMap<Guid, String>, Bf3Error> {
        let records = {
            let index = self.index.read().unwrap_or_else(PoisonError::into_inner);
            index
                .ebx
                .iter()
                .map(|record| {
                    (
                        record.name.clone(),
                        record.stored.clone(),
                        record.original_size,
                    )
                })
                .collect::<Vec<_>>()
        };
        let partitions = parallel_map(&records, |(name, stored, original_size)| {
            let head = if stored.size == *original_size {
                self.read_stored_prefix(stored, EBX_HEAD_LEN as u64)?
            } else {
                self.decode_named(stored, *original_size, name)?
            };
            ebx_partition(&head).map_err(|error| Bf3Error::format(format!("ebx {name}"), error))
        });
        let mut out = HashMap::with_capacity(records.len());
        for ((name, ..), partition) in records.iter().zip(partitions) {
            out.entry(partition?).or_insert_with(|| name.clone());
        }
        Ok(out)
    }

    pub fn entry_sha1(&self, entry: EntryRef<'_>) -> Option<Sha1> {
        self.stored_of(entry).ok().map(|stored| stored.sha1)
    }

    pub fn read_stored(&self, entry: EntryRef<'_>) -> Result<Vec<u8>, Bf3Error> {
        let stored = self.stored_of(entry)?;
        self.read_stored_bytes(&stored)
    }

    fn stored_of(&self, entry: EntryRef<'_>) -> Result<Stored, Bf3Error> {
        let index = self.index.read().unwrap_or_else(PoisonError::into_inner);
        match entry {
            EntryRef::Ebx(name) => index
                .ebx_keys
                .get(&asset_key(name))
                .map(|&at| index.ebx[at].stored.clone())
                .ok_or_else(|| Bf3Error::EbxNotLoaded(name.to_owned())),
            EntryRef::Res(name) => index
                .res_keys
                .get(&asset_key(name))
                .map(|&at| index.res[at].stored.clone())
                .ok_or_else(|| Bf3Error::ResNotLoaded(name.to_owned())),
            EntryRef::Chunk(id) => index
                .chunks
                .get(&id)
                .map(|record| record.stored.clone())
                .ok_or(Bf3Error::ChunkNotFound(id)),
        }
    }

    fn decode_named(
        &self,
        stored: &Stored,
        original_size: u64,
        name: &str,
    ) -> Result<Vec<u8>, Bf3Error> {
        let bytes = self.read_stored_bytes(stored)?;
        if stored.size == original_size {
            return Ok(bytes);
        }
        let decoded = decode_blocks(&bytes).map_err(|error| Bf3Error::format(name, error))?;
        if decoded.len() as u64 != original_size {
            return Err(Bf3Error::SizeMismatch {
                what: name.to_owned(),
                expected: original_size,
                got: decoded.len() as u64,
            });
        }
        Ok(decoded)
    }

    fn read_stored_bytes(&self, stored: &Stored) -> Result<Vec<u8>, Bf3Error> {
        let bytes = match &stored.place {
            Place::Cas => self.install.cas().read(&stored.sha1)?,
            Place::Inline(bytes) => bytes.to_vec(),
            Place::Bundle { bundle, offset } => {
                self.bundles[*bundle].bytes.read_vec(*offset, stored.size)?
            }
        };
        if bytes.len() as u64 != stored.size {
            return Err(Bf3Error::SizeMismatch {
                what: format!("payload {}", stored.sha1),
                expected: stored.size,
                got: bytes.len() as u64,
            });
        }
        Ok(bytes)
    }

    fn read_stored_prefix(&self, stored: &Stored, len: u64) -> Result<Vec<u8>, Bf3Error> {
        let len = len.min(stored.size);
        match &stored.place {
            Place::Cas => self.install.cas().read_prefix(&stored.sha1, len),
            Place::Inline(bytes) => Ok(bytes[..len as usize].to_vec()),
            Place::Bundle { bundle, offset } => self.bundles[*bundle].bytes.read_vec(*offset, len),
        }
    }

    fn read_toc_chunk(&self, site: &TocChunkSite) -> Result<Vec<u8>, Bf3Error> {
        match site {
            TocChunkSite::Cas(sha1) => self.install.cas().read(sha1),
            TocChunkSite::File { file, offset, size } => file.read_vec(*offset, *size),
        }
    }
}

fn superbundle_bundles(
    install: &Bf3Install,
    site: &SuperbundleSite,
) -> Result<Vec<BundleSite>, Bf3Error> {
    let open = |toc_path: &Path| -> Result<(Toc, Arc<DiskFile>), Bf3Error> {
        let toc = read_toc(toc_path)?;
        let file = install.files().get(&superbundle_file(toc_path))?;
        Ok((toc, file))
    };
    let base = site.base_toc.as_deref().map(open).transpose()?;
    let patch = site.patch_toc.as_deref().map(open).transpose()?;

    let plain = |toc: &Toc, file: &Arc<DiskFile>, entry: &TocBundle| BundleSite {
        id: entry.id.clone(),
        format: if toc.cas {
            BundleFormat::Cas
        } else {
            BundleFormat::Binary
        },
        bytes: BundleBytes::File {
            file: Arc::clone(file),
            offset: entry.offset,
            len: entry.size,
        },
    };

    let Some((patch_toc, patch_file)) = &patch else {
        return Ok(base
            .iter()
            .flat_map(|(toc, file)| toc.bundles.iter().map(|entry| plain(toc, file, entry)))
            .collect());
    };

    let base_entries = base
        .as_ref()
        .map(|(toc, _)| {
            toc.bundles
                .iter()
                .map(|entry| (bundle_key(&entry.id), entry))
                .collect::<HashMap<_, _>>()
        })
        .unwrap_or_else(HashMap::new);
    let missing_base = |entry: &TocBundle| Bf3Error::Format {
        at: format!("{} bundle {}", patch_file.path().display(), entry.id),
        source: superbundle_bf3::Error::Layout {
            what: "patch bundle",
            problem: "refers to a base bundle that does not exist",
        },
    };

    let mut out = Vec::with_capacity(patch_toc.bundles.len());
    for entry in &patch_toc.bundles {
        if entry.base {
            let (base_toc, base_file) = base.as_ref().ok_or_else(|| missing_base(entry))?;
            let base_entry = base_entries
                .get(&bundle_key(&entry.id))
                .ok_or_else(|| missing_base(entry))?;
            out.push(plain(base_toc, base_file, base_entry));
        } else if entry.delta {
            let (base_toc, base_file) = base.as_ref().ok_or_else(|| missing_base(entry))?;
            if base_toc.cas {
                return Err(Bf3Error::Format {
                    at: format!("{} bundle {}", patch_file.path().display(), entry.id),
                    source: superbundle_bf3::Error::Layout {
                        what: "delta bundle",
                        problem: "its base superbundle is CAS",
                    },
                });
            }
            let at = || format!("{} bundle {}", patch_file.path().display(), entry.id);
            let header = patch_file.read_vec(entry.offset, entry.size.min(16))?;
            let table_len =
                delta_table_len(&header).map_err(|error| Bf3Error::format(at(), error))?;
            let table = patch_file.read_vec(entry.offset, table_len.min(entry.size))?;
            let delta = DeltaBundle::parse(&table, entry.offset, entry.size)
                .map_err(|error| Bf3Error::format(at(), error))?;
            out.push(BundleSite {
                id: entry.id.clone(),
                format: BundleFormat::Binary,
                bytes: BundleBytes::Delta {
                    view: DeltaView::new(delta, Arc::clone(patch_file), Arc::clone(base_file)),
                    patch: Arc::clone(patch_file),
                },
            });
        } else {
            out.push(plain(patch_toc, patch_file, entry));
        }
    }

    if site.delta
        && let Some((base_toc, base_file)) = &base
    {
        let patched = patch_toc
            .bundles
            .iter()
            .map(|entry| bundle_key(&entry.id))
            .collect::<std::collections::HashSet<_>>();
        out.extend(
            base_toc
                .bundles
                .iter()
                .filter(|entry| !patched.contains(&bundle_key(&entry.id)))
                .map(|entry| plain(base_toc, base_file, entry)),
        );
    }
    Ok(out)
}

fn asset_key(name: &str) -> String {
    name.replace('\\', "/").to_ascii_lowercase()
}

fn bundle_key(name: &str) -> String {
    let key = asset_key(name);
    let key = key.trim_start_matches('/');
    key.strip_prefix("win32/").unwrap_or(key).to_owned()
}

fn parallel_map<T: Sync, R: Send>(items: &[T], work: impl Fn(&T) -> R + Sync) -> Vec<R> {
    let lanes = std::thread::available_parallelism()
        .map_or(1, NonZeroUsize::get)
        .min(items.len());
    if lanes <= 1 {
        return items.iter().map(&work).collect();
    }
    let next = AtomicUsize::new(0);
    let mut done = std::thread::scope(|scope| {
        let lanes = (0..lanes)
            .map(|_| {
                scope.spawn(|| {
                    let mut out = Vec::new();
                    loop {
                        let slot = next.fetch_add(1, Ordering::Relaxed);
                        let Some(item) = items.get(slot) else {
                            return out;
                        };
                        out.push((slot, work(item)));
                    }
                })
            })
            .collect::<Vec<_>>();
        lanes
            .into_iter()
            .flat_map(|lane| {
                lane.join()
                    .unwrap_or_else(|panic| std::panic::resume_unwind(panic))
            })
            .collect::<Vec<_>>()
    });
    done.sort_unstable_by_key(|(slot, _)| *slot);
    done.into_iter().map(|(_, result)| result).collect()
}
