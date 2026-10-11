use crate::dbobject::{DbValue, read_db_element};
use crate::error::{Error, need};
use crate::ids::{Guid, Sha1};
use crate::payload::chunk_is_framed;
use crate::record::Record;

pub const BINARY_MANIFEST_MAGIC: u32 = 0x970D_1C13;

pub const BINARY_MANIFEST_ALPHA_MAGIC: u32 = 0x0D1C_EDB8;

const BINARY_HEADER_LEN: usize = 0x24;

const BINARY_PAYLOAD_ALIGN: u64 = 16;

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct BundleManifest {
    pub path: Option<String>,
    pub ebx: Vec<EbxEntry>,
    pub res: Vec<ResEntry>,
    pub chunks: Vec<ChunkEntry>,
    /// A binary manifest stores these in an order unrelated to its chunk
    /// table; only `h32`, the owning asset's name hash, ties one to a chunk.
    pub chunk_meta: Vec<ChunkMeta>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Payload {
    Cas,
    Inline(Box<[u8]>),
    Bundle { offset: u64 },
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct EbxEntry {
    pub name: String,
    pub sha1: Sha1,
    pub stored_size: u64,
    pub original_size: u64,
    pub payload: Payload,
}

impl EbxEntry {
    pub fn is_framed(&self) -> bool {
        self.stored_size != self.original_size
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ResEntry {
    pub name: String,
    pub sha1: Sha1,
    pub stored_size: u64,
    pub original_size: u64,
    pub res_type: u32,
    pub res_meta: [u8; 16],
    pub payload: Payload,
}

impl ResEntry {
    pub fn is_framed(&self) -> bool {
        self.stored_size != self.original_size
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ChunkRange {
    pub start: u32,
    pub end: u32,
    pub logical_offset: u32,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ChunkMeta {
    pub h32: u32,
    pub first_mip: Option<u32>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ChunkEntry {
    pub id: Guid,
    /// Hashes the whole stored payload, also when the bundle holds only its tail.
    pub sha1: Sha1,
    pub stored_size: u64,
    pub range: Option<ChunkRange>,
    pub tail_only: bool,
    pub meta: Option<ChunkMeta>,
    pub payload: Payload,
}

impl ChunkEntry {
    pub fn is_framed(&self) -> bool {
        chunk_is_framed(&self.id)
    }
}

pub fn parse_cas_manifest(element: &[u8]) -> Result<BundleManifest, Error> {
    let (root, end) = read_db_element(element, 0)?;
    if end > element.len() {
        return Err(Error::Truncated {
            what: "CAS bundle manifest",
            need: end as u64,
            have: element.len() as u64,
        });
    }
    let bundle = Record::new("CAS bundle manifest", &root.value)?;

    let mut ebx = Vec::new();
    for entry in bundle.list("ebx")? {
        let entry = Record::new("CAS ebx entry", entry)?;
        let (stored_size, payload) = cas_payload(&entry)?;
        ebx.push(EbxEntry {
            name: entry.str("name")?.to_owned(),
            sha1: entry.sha1("sha1")?,
            stored_size,
            original_size: entry.opt_u64("originalSize")?.unwrap_or(stored_size),
            payload,
        });
    }

    let mut res = Vec::new();
    for entry in bundle.list("res")? {
        let entry = Record::new("CAS res entry", entry)?;
        let (stored_size, payload) = cas_payload(&entry)?;
        res.push(ResEntry {
            name: entry.str("name")?.to_owned(),
            sha1: entry.sha1("sha1")?,
            stored_size,
            original_size: entry.opt_u64("originalSize")?.unwrap_or(stored_size),
            res_type: entry.hash32("resType")?.unwrap_or(0),
            res_meta: res_meta(entry.opt_blob("resMeta")?)?,
            payload,
        });
    }

    let chunk_list = bundle.list("chunks")?;
    let chunk_meta = chunk_metas(bundle.list("chunkMeta")?, chunk_list.len())?;
    let mut chunks = Vec::with_capacity(chunk_list.len());
    for (at, entry) in chunk_list.iter().enumerate() {
        let entry = Record::new("CAS chunk entry", entry)?;
        let (stored_size, payload) = cas_payload(&entry)?;
        let range = match entry.opt_u32("rangeStart")? {
            None => None,
            Some(start) => Some(ChunkRange {
                start,
                end: entry
                    .opt_u32("rangeEnd")?
                    .unwrap_or(u32::try_from(stored_size).unwrap_or(u32::MAX)),
                logical_offset: entry.opt_u32("logicalOffset")?.unwrap_or(0),
            }),
        };
        chunks.push(ChunkEntry {
            id: entry.guid("id")?,
            sha1: entry.sha1("sha1")?,
            stored_size,
            range,
            tail_only: false,
            meta: chunk_meta.get(at).copied(),
            payload,
        });
    }

    Ok(BundleManifest {
        path: bundle.opt_str("path")?.map(str::to_owned),
        ebx,
        res,
        chunks,
        chunk_meta,
    })
}

fn cas_payload(entry: &Record<'_, '_>) -> Result<(u64, Payload), Error> {
    let size = entry.opt_u64("size")?;
    match entry.opt_blob("idata")? {
        Some(inline) => {
            let len = inline.len() as u64;
            if size.is_some_and(|size| size != len) {
                return Err(Error::Layout {
                    what: "CAS bundle entry",
                    problem: "inline data length differs from its size",
                });
            }
            Ok((len, Payload::Inline(inline.into())))
        }
        None => Ok((entry.u64("size")?, Payload::Cas)),
    }
}

fn res_meta(blob: Option<&[u8]>) -> Result<[u8; 16], Error> {
    match blob {
        None => Ok([0; 16]),
        Some(bytes) => bytes.try_into().map_err(|_| Error::Layout {
            what: "res entry",
            problem: "resMeta is not 16 bytes",
        }),
    }
}

fn chunk_metas(list: &[DbValue<'_>], chunk_count: usize) -> Result<Vec<ChunkMeta>, Error> {
    if !list.is_empty() && list.len() != chunk_count {
        return Err(Error::Layout {
            what: "chunkMeta",
            problem: "has a different length than the chunk list",
        });
    }
    list.iter()
        .map(|value| {
            let entry = Record::new("chunkMeta entry", value)?;
            let first_mip = match entry.field("meta") {
                None => None,
                Some(meta) => Record::new("chunkMeta meta", meta)?.opt_u32("firstMip")?,
            };
            Ok(ChunkMeta {
                h32: entry.hash32("h32")?.unwrap_or(0),
                first_mip,
            })
        })
        .collect()
}

pub fn binary_manifest_len(prefix: &[u8]) -> Result<u64, Error> {
    need("binary bundle manifest", prefix, 4)?;
    Ok(4 + u64::from(be32(prefix, 0)))
}

pub fn parse_binary_manifest(head: &[u8], bundle_len: u64) -> Result<BundleManifest, Error> {
    need("binary bundle manifest", head, BINARY_HEADER_LEN)?;
    let meta_end = binary_manifest_len(head)?;
    need("binary bundle manifest", head, meta_end as usize)?;
    let head = &head[..meta_end as usize];
    let magic = be32(head, 4);
    if magic != BINARY_MANIFEST_MAGIC && magic != BINARY_MANIFEST_ALPHA_MAGIC {
        return Err(Error::BadMagic {
            what: "binary bundle manifest",
            got: magic,
        });
    }
    let total = be32(head, 0x08) as usize;
    let ebx_count = be32(head, 0x0C) as usize;
    let res_count = be32(head, 0x10) as usize;
    let chunk_count = be32(head, 0x14) as usize;
    let strings_at = 4 + be32(head, 0x18) as usize;
    let chunk_meta_at = 4 + be32(head, 0x1C) as usize;
    let chunk_meta_len = be32(head, 0x20) as usize;
    if ebx_count
        .checked_add(res_count)
        .and_then(|sum| sum.checked_add(chunk_count))
        != Some(total)
    {
        return Err(Error::Layout {
            what: "binary bundle manifest",
            problem: "entry counts do not add up",
        });
    }
    let named = ebx_count + res_count;
    let tables_len = total
        .checked_mul(20)
        .and_then(|sha1s| sha1s.checked_add(named.checked_mul(12)?))
        .and_then(|len| len.checked_add(res_count.checked_mul(20)?))
        .and_then(|len| len.checked_add(chunk_count.checked_mul(28)?))
        .and_then(|len| len.checked_add(BINARY_HEADER_LEN))
        .ok_or(Error::Layout {
            what: "binary bundle manifest",
            problem: "entry counts overflow",
        })?;
    need("binary bundle manifest tables", head, tables_len)?;
    if strings_at > head.len() {
        return Err(Error::Layout {
            what: "binary bundle manifest",
            problem: "string block starts past the manifest",
        });
    }

    let mut at = BINARY_HEADER_LEN;
    let sha1s = (0..total)
        .map(|index| Sha1(array(head, at + index * 20)))
        .collect::<Vec<_>>();
    at += total * 20;
    let records = (0..named)
        .map(|index| {
            let record = at + index * 12;
            (
                be32(head, record) as usize,
                u64::from(be32(head, record + 4)),
                u64::from(be32(head, record + 8)),
            )
        })
        .collect::<Vec<_>>();
    at += named * 12;
    let res_types = (0..res_count)
        .map(|index| be32(head, at + index * 4))
        .collect::<Vec<_>>();
    at += res_count * 4;
    let res_metas = (0..res_count)
        .map(|index| array::<16>(head, at + index * 16))
        .collect::<Vec<_>>();
    at += res_count * 16;
    let chunk_records = (0..chunk_count)
        .map(|index| {
            let record = at + index * 28;
            (
                Guid::from_big_endian_fields(array(head, record)),
                ChunkRange {
                    start: be32(head, record + 16),
                    end: be32(head, record + 20),
                    logical_offset: be32(head, record + 24),
                },
            )
        })
        .collect::<Vec<_>>();

    let chunk_meta = if chunk_meta_len == 0 {
        Vec::new()
    } else {
        let section_end = chunk_meta_at
            .checked_add(chunk_meta_len)
            .filter(|&end| end <= head.len())
            .ok_or(Error::Layout {
                what: "binary bundle manifest",
                problem: "chunkMeta runs past the manifest",
            })?;
        let (element, end) = read_db_element(&head[..section_end], chunk_meta_at)?;
        if end != section_end {
            return Err(Error::Layout {
                what: "binary bundle manifest",
                problem: "chunkMeta does not fill its section",
            });
        }
        let list = element.value.as_list().ok_or(Error::FieldType {
            what: "binary bundle manifest",
            field: "chunkMeta",
        })?;
        chunk_metas(list, chunk_count)?
    };

    let name_at = |offset: usize| -> Result<String, Error> {
        let start = strings_at
            .checked_add(offset)
            .filter(|&start| start < head.len())
            .ok_or(Error::Layout {
                what: "binary bundle manifest",
                problem: "name offset is past the string block",
            })?;
        let len = head[start..]
            .iter()
            .position(|&b| b == 0)
            .ok_or(Error::Layout {
                what: "binary bundle manifest",
                problem: "name has no terminator",
            })?;
        std::str::from_utf8(&head[start..start + len])
            .map(str::to_owned)
            .map_err(|_| Error::BadUtf8 {
                what: "binary bundle entry name",
            })
    };

    let mut cursor = meta_end;
    let mut place = |len: u64| -> u64 {
        let offset = cursor.next_multiple_of(BINARY_PAYLOAD_ALIGN);
        cursor = offset + len;
        offset
    };

    let mut ebx = Vec::with_capacity(ebx_count);
    let mut res = Vec::with_capacity(res_count);
    for (index, &(name_offset, stored_size, original_size)) in records.iter().enumerate() {
        let name = name_at(name_offset)?;
        let payload = Payload::Bundle {
            offset: place(stored_size),
        };
        if index < ebx_count {
            ebx.push(EbxEntry {
                name,
                sha1: sha1s[index],
                stored_size,
                original_size,
                payload,
            });
        } else {
            let res_index = index - ebx_count;
            res.push(ResEntry {
                name,
                sha1: sha1s[index],
                stored_size,
                original_size,
                res_type: res_types[res_index],
                res_meta: res_metas[res_index],
                payload,
            });
        }
    }

    let mut chunks = Vec::with_capacity(chunk_count);
    for (index, (id, range)) in chunk_records.into_iter().enumerate() {
        if range.end < range.start {
            return Err(Error::Layout {
                what: "binary bundle chunk",
                problem: "range ends before it starts",
            });
        }
        let stored_size = u64::from(range.end - range.start);
        let ranged = range.start != 0 || range.logical_offset != 0;
        chunks.push(ChunkEntry {
            id,
            sha1: sha1s[named + index],
            stored_size,
            range: ranged.then_some(range),
            tail_only: ranged,
            meta: None,
            payload: Payload::Bundle {
                offset: place(stored_size),
            },
        });
    }

    if cursor > bundle_len {
        return Err(Error::Truncated {
            what: "binary bundle payloads",
            need: cursor,
            have: bundle_len,
        });
    }

    Ok(BundleManifest {
        path: None,
        ebx,
        res,
        chunks,
        chunk_meta,
    })
}

fn be32(bytes: &[u8], at: usize) -> u32 {
    u32::from_be_bytes(array(bytes, at))
}

fn array<const N: usize>(bytes: &[u8], at: usize) -> [u8; N] {
    let mut out = [0u8; N];
    out.copy_from_slice(&bytes[at..at + N]);
    out
}
