use crate::dbobject::parse_db_object;
use crate::error::Error;
use crate::ids::{Guid, Sha1};
use crate::record::Record;

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Toc {
    pub name: Option<String>,
    pub cas: bool,
    pub bundles: Vec<TocBundle>,
    pub chunks: Vec<TocChunk>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct TocBundle {
    pub id: String,
    pub offset: u64,
    pub size: u64,
    pub checksum: Option<Sha1>,
    pub base: bool,
    pub delta: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct TocChunk {
    pub id: Guid,
    pub location: TocChunkLocation,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TocChunkLocation {
    Cas(Sha1),
    Superbundle { offset: u64, size: u64 },
}

pub fn parse_toc(plain: &[u8]) -> Result<Toc, Error> {
    let root = parse_db_object(plain)?;
    let toc = Record::new("toc", &root)?;
    let mut bundles = Vec::new();
    for entry in toc.list("bundles")? {
        let entry = Record::new("toc bundle", entry)?;
        bundles.push(TocBundle {
            id: entry.str("id")?.to_owned(),
            offset: entry.u64("offset")?,
            size: entry.u64("size")?,
            checksum: entry.opt_sha1("checksum")?,
            base: entry.flag("base")?,
            delta: entry.flag("delta")?,
        });
    }
    let mut chunks = Vec::new();
    for entry in toc.list("chunks")? {
        let entry = Record::new("toc chunk", entry)?;
        let location = match entry.opt_sha1("sha1")? {
            Some(sha1) => TocChunkLocation::Cas(sha1),
            None => TocChunkLocation::Superbundle {
                offset: entry.u64("offset")?,
                size: entry.u64("size")?,
            },
        };
        chunks.push(TocChunk {
            id: entry.guid("id")?,
            location,
        });
    }
    Ok(Toc {
        name: toc.opt_str("name")?.map(str::to_owned),
        cas: toc.flag("cas")?,
        bundles,
        chunks,
    })
}
