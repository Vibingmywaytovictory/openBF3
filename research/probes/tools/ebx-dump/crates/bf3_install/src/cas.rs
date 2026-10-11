use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex, PoisonError};

use superbundle_bf3::{
    CAS_ENTRY_HEADER_LEN, CatalogEntry, Sha1, cas_archive_name, check_cas_entry_header,
    deobfuscate, parse_catalog,
};

use super::Bf3Error;
use super::file::{DiskFile, read_whole, resolve};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct CasLocation {
    pub catalog: u8,
    pub archive: u32,
    pub offset: u32,
    pub size: u32,
}

pub struct CasStore {
    entries: HashMap<Sha1, CasLocation>,
    catalogs: Vec<PathBuf>,
    archives: Mutex<HashMap<(u8, u32), Arc<DiskFile>>>,
}

impl CasStore {
    pub(crate) fn open(data_dirs: &[PathBuf]) -> Result<Self, Bf3Error> {
        let mut entries = HashMap::new();
        let mut catalogs = Vec::new();
        for dir in data_dirs {
            let Some(path) = resolve(dir, "cas.cat") else {
                continue;
            };
            let plain = deobfuscate(read_whole(&path)?)
                .map_err(|error| Bf3Error::format(path.display(), error))?;
            let parsed =
                parse_catalog(&plain).map_err(|error| Bf3Error::format(path.display(), error))?;
            let catalog = u8::try_from(catalogs.len()).unwrap_or(u8::MAX);
            entries.reserve(parsed.len());
            for entry in parsed {
                entries.entry(entry.sha1).or_insert(CasLocation {
                    catalog,
                    archive: entry.archive,
                    offset: entry.offset,
                    size: entry.size,
                });
            }
            catalogs.push(dir.clone());
        }
        Ok(Self {
            entries,
            catalogs,
            archives: Mutex::new(HashMap::new()),
        })
    }

    pub fn len(&self) -> usize {
        self.entries.len()
    }

    pub fn is_empty(&self) -> bool {
        self.entries.is_empty()
    }

    pub fn contains(&self, sha1: &Sha1) -> bool {
        self.entries.contains_key(sha1)
    }

    pub fn location(&self, sha1: &Sha1) -> Option<CasLocation> {
        self.entries.get(sha1).copied()
    }

    pub fn catalog_dir(&self, catalog: u8) -> Option<&Path> {
        self.catalogs
            .get(usize::from(catalog))
            .map(PathBuf::as_path)
    }

    pub fn read(&self, sha1: &Sha1) -> Result<Vec<u8>, Bf3Error> {
        let location = self.location(sha1).ok_or(Bf3Error::NotInCas(*sha1))?;
        self.archive(location)?
            .read_vec(u64::from(location.offset), u64::from(location.size))
    }

    pub fn read_prefix(&self, sha1: &Sha1, len: u64) -> Result<Vec<u8>, Bf3Error> {
        let location = self.location(sha1).ok_or(Bf3Error::NotInCas(*sha1))?;
        self.archive(location)?.read_vec(
            u64::from(location.offset),
            len.min(u64::from(location.size)),
        )
    }

    pub fn check_entry_header(&self, sha1: &Sha1) -> Result<(), Bf3Error> {
        let location = self.location(sha1).ok_or(Bf3Error::NotInCas(*sha1))?;
        let archive = self.archive(location)?;
        let header_at = u64::from(location.offset)
            .checked_sub(CAS_ENTRY_HEADER_LEN)
            .ok_or_else(|| Bf3Error::OutOfFile {
                path: archive.path().to_path_buf(),
                offset: u64::from(location.offset),
                len: CAS_ENTRY_HEADER_LEN,
                file_len: archive.len(),
            })?;
        let header = archive.read_vec(header_at, CAS_ENTRY_HEADER_LEN)?;
        let entry = CatalogEntry {
            sha1: *sha1,
            offset: location.offset,
            size: location.size,
            archive: location.archive,
        };
        check_cas_entry_header(&header, &entry)
            .map_err(|error| Bf3Error::format(archive.path().display(), error))
    }

    fn archive(&self, location: CasLocation) -> Result<Arc<DiskFile>, Bf3Error> {
        let key = (location.catalog, location.archive);
        if let Some(hit) = self
            .archives
            .lock()
            .unwrap_or_else(PoisonError::into_inner)
            .get(&key)
        {
            return Ok(Arc::clone(hit));
        }
        let dir = self
            .catalog_dir(location.catalog)
            .unwrap_or_else(|| Path::new(""));
        let name = cas_archive_name(location.archive);
        let path = resolve(dir, &name).unwrap_or_else(|| dir.join(&name));
        let opened = Arc::new(DiskFile::open(&path)?);
        let mut archives = self.archives.lock().unwrap_or_else(PoisonError::into_inner);
        Ok(Arc::clone(archives.entry(key).or_insert(opened)))
    }
}
