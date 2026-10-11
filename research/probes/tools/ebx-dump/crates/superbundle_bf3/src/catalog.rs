use crate::error::{Error, need};
use crate::ids::Sha1;

pub const CATALOG_MAGIC: &[u8; 16] = b"NyanNyanNyanNyan";

const CATALOG_ENTRY_LEN: usize = 32;

pub const CAS_ENTRY_HEADER_LEN: u64 = 32;

pub const CAS_ENTRY_MAGIC: u32 = 0xF00F_CEFA;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct CatalogEntry {
    pub sha1: Sha1,
    pub offset: u32,
    pub size: u32,
    pub archive: u32,
}

pub fn parse_catalog(plain: &[u8]) -> Result<Vec<CatalogEntry>, Error> {
    need("CAS catalog", plain, CATALOG_MAGIC.len())?;
    if &plain[..CATALOG_MAGIC.len()] != CATALOG_MAGIC {
        return Err(Error::BadMagic {
            what: "CAS catalog",
            got: u32::from_le_bytes([plain[0], plain[1], plain[2], plain[3]]),
        });
    }
    let body = &plain[CATALOG_MAGIC.len()..];
    if !body.len().is_multiple_of(CATALOG_ENTRY_LEN) {
        return Err(Error::Layout {
            what: "CAS catalog",
            problem: "entries are not 32 bytes each",
        });
    }
    Ok(body
        .chunks_exact(CATALOG_ENTRY_LEN)
        .map(|entry| {
            let word = |at: usize| {
                u32::from_le_bytes([entry[at], entry[at + 1], entry[at + 2], entry[at + 3]])
            };
            let mut sha1 = [0u8; 20];
            sha1.copy_from_slice(&entry[..20]);
            CatalogEntry {
                sha1: Sha1(sha1),
                offset: word(20),
                size: word(24),
                archive: word(28),
            }
        })
        .collect())
}

pub fn cas_archive_name(archive: u32) -> String {
    format!("cas_{archive:02}.cas")
}

pub fn check_cas_entry_header(header: &[u8], entry: &CatalogEntry) -> Result<(), Error> {
    need("CAS entry header", header, CAS_ENTRY_HEADER_LEN as usize)?;
    let magic = u32::from_le_bytes([header[0], header[1], header[2], header[3]]);
    if magic != CAS_ENTRY_MAGIC {
        return Err(Error::BadMagic {
            what: "CAS entry header",
            got: magic,
        });
    }
    let size = u32::from_le_bytes([header[24], header[25], header[26], header[27]]);
    if header[4..24] != entry.sha1.0 || size != entry.size {
        return Err(Error::Layout {
            what: "CAS entry header",
            problem: "does not match its catalog entry",
        });
    }
    Ok(())
}
