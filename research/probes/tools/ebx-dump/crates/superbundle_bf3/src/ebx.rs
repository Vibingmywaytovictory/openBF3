use crate::error::{Error, need};
use crate::ids::Guid;

pub const EBX_MAGIC: [u8; 4] = [0xCE, 0xD1, 0xB2, 0x0F];

pub const EBX_HEAD_LEN: usize = 0x50;

const PARTITION_AT: usize = 0x30;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct EbxHead {
    pub meta_size: u32,
    pub payload_size: u32,
    pub partition: Guid,
    pub primary_instance: Guid,
}

impl EbxHead {
    pub fn file_len(&self) -> u64 {
        u64::from(self.meta_size) + u64::from(self.payload_size)
    }
}

pub fn ebx_partition(head: &[u8]) -> Result<Guid, Error> {
    need("EBX header", head, PARTITION_AT + 16)?;
    check_magic(head)?;
    Ok(guid_at(head, PARTITION_AT))
}

pub fn parse_ebx_head(head: &[u8]) -> Result<EbxHead, Error> {
    need("EBX header", head, EBX_HEAD_LEN)?;
    check_magic(head)?;
    Ok(EbxHead {
        meta_size: u32::from_le_bytes([head[4], head[5], head[6], head[7]]),
        payload_size: u32::from_le_bytes([head[8], head[9], head[10], head[11]]),
        partition: guid_at(head, PARTITION_AT),
        primary_instance: guid_at(head, PARTITION_AT + 16),
    })
}

fn check_magic(head: &[u8]) -> Result<(), Error> {
    if head[..4] != EBX_MAGIC {
        return Err(Error::BadMagic {
            what: "EBX partition",
            got: u32::from_le_bytes([head[0], head[1], head[2], head[3]]),
        });
    }
    Ok(())
}

fn guid_at(head: &[u8], at: usize) -> Guid {
    let mut guid = [0u8; 16];
    guid.copy_from_slice(&head[at..at + 16]);
    Guid(guid)
}
