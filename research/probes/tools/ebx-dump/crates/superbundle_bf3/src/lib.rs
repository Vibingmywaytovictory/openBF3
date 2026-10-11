#![forbid(unsafe_code)]

mod catalog;
mod dbobject;
mod delta;
mod ebx;
mod error;
mod ids;
mod layout;
mod manifest;
mod obfuscation;
mod package;
mod payload;
mod record;
mod toc;

pub use catalog::{
    CAS_ENTRY_HEADER_LEN, CAS_ENTRY_MAGIC, CATALOG_MAGIC, CatalogEntry, cas_archive_name,
    check_cas_entry_header, parse_catalog,
};
pub use dbobject::{DbElement, DbValue, parse_db_object, read_db_element};
pub use delta::{
    DELTA_HEADER_LEN, DELTA_MAGIC, DeltaBundle, DeltaRun, DeltaSource, DeltaView, ReadAt,
    delta_table_len,
};
pub use ebx::{EBX_HEAD_LEN, EBX_MAGIC, EbxHead, ebx_partition, parse_ebx_head};
pub use error::{BlockProblem, Error};
pub use ids::{Guid, Sha1, asset_name_hash};
pub use layout::{Layout, SuperbundleDecl, parse_layout};
pub use manifest::{
    BINARY_MANIFEST_ALPHA_MAGIC, BINARY_MANIFEST_MAGIC, BundleManifest, ChunkEntry, ChunkMeta,
    ChunkRange, EbxEntry, Payload, ResEntry, binary_manifest_len, parse_binary_manifest,
    parse_cas_manifest,
};
pub use obfuscation::{OBFUSCATION_HEADER_LEN, Obfuscation, deobfuscate, obfuscation_of};
pub use package::{PackageManifest, parse_package_manifest};
pub use payload::{
    BLOCK_HEADER_LEN, Block, MAX_BLOCK_LEN, blocks, chunk_is_framed, decode_blocks,
    decode_blocks_into, decode_chunk, decode_chunk_range, decoded_len,
};
pub use toc::{Toc, TocBundle, TocChunk, TocChunkLocation, parse_toc};
