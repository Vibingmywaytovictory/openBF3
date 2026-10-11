use flate2::{Decompress, FlushDecompress, Status};

use crate::error::{BlockProblem, Error};
use crate::ids::Guid;
use crate::manifest::ChunkRange;

pub const MAX_BLOCK_LEN: u32 = 0x10000;

pub const BLOCK_HEADER_LEN: usize = 8;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Block {
    pub stored_at: u64,
    pub stored_len: u32,
    pub decoded_at: u64,
    pub decoded_len: u32,
}

impl Block {
    pub fn is_verbatim(&self) -> bool {
        self.stored_len == MAX_BLOCK_LEN && self.decoded_len == MAX_BLOCK_LEN
    }

    fn data<'a>(&self, stored: &'a [u8]) -> &'a [u8] {
        let start = self.stored_at as usize + BLOCK_HEADER_LEN;
        &stored[start..start + self.stored_len as usize]
    }
}

pub fn chunk_is_framed(id: &Guid) -> bool {
    id.0[15] & 1 != 0
}

pub fn blocks(stored: &[u8]) -> Result<Vec<Block>, Error> {
    let mut out = Vec::new();
    let mut at = 0usize;
    let mut decoded_at = 0u64;
    while at < stored.len() {
        let Some(header) = stored.get(at..at + BLOCK_HEADER_LEN) else {
            return Err(block_error(at, BlockProblem::HeaderCut));
        };
        let decoded_len = u32::from_be_bytes([header[0], header[1], header[2], header[3]]);
        let stored_len = u32::from_be_bytes([header[4], header[5], header[6], header[7]]);
        if decoded_len > MAX_BLOCK_LEN {
            return Err(block_error(at, BlockProblem::TooLarge));
        }
        let data_end = (at + BLOCK_HEADER_LEN)
            .checked_add(stored_len as usize)
            .filter(|&end| end <= stored.len())
            .ok_or_else(|| block_error(at, BlockProblem::DataCut))?;
        out.push(Block {
            stored_at: at as u64,
            stored_len,
            decoded_at,
            decoded_len,
        });
        decoded_at += u64::from(decoded_len);
        at = data_end;
    }
    Ok(out)
}

pub fn decoded_len(stored: &[u8]) -> Result<u64, Error> {
    Ok(blocks(stored)?
        .last()
        .map_or(0, |block| block.decoded_at + u64::from(block.decoded_len)))
}

pub fn decode_blocks(stored: &[u8]) -> Result<Vec<u8>, Error> {
    let mut out = Vec::new();
    decode_blocks_into(stored, &mut out)?;
    Ok(out)
}

pub fn decode_blocks_into(stored: &[u8], out: &mut Vec<u8>) -> Result<(), Error> {
    let layout = blocks(stored)?;
    let total = layout
        .last()
        .map_or(0, |block| block.decoded_at + u64::from(block.decoded_len));
    out.try_reserve(total as usize)
        .map_err(|_| Error::OutOfMemory { bytes: total })?;
    let mut inflater = Decompress::new(true);
    for block in &layout {
        let data = block.data(stored);
        if block.is_verbatim() {
            out.extend_from_slice(data);
            continue;
        }
        let before = out.len();
        inflater.reset(true);
        let inflated = inflater.decompress_vec(data, out, FlushDecompress::Finish);
        match inflated {
            Ok(Status::StreamEnd) if out.len() - before == block.decoded_len as usize => {}
            Ok(Status::StreamEnd) => {
                return Err(block_error(
                    block.stored_at as usize,
                    BlockProblem::InflatedLength,
                ));
            }
            _ if block.stored_len == block.decoded_len => {
                out.truncate(before);
                out.extend_from_slice(data);
            }
            _ => return Err(block_error(block.stored_at as usize, BlockProblem::Inflate)),
        }
    }
    Ok(())
}

pub fn decode_chunk(id: &Guid, stored: &[u8]) -> Result<Vec<u8>, Error> {
    if chunk_is_framed(id) {
        decode_blocks(stored)
    } else {
        Ok(stored.to_vec())
    }
}

pub fn decode_chunk_range(id: &Guid, stored: &[u8], range: &ChunkRange) -> Result<Vec<u8>, Error> {
    let (start, end) = (range.start as usize, range.end as usize);
    if start > end || end > stored.len() {
        return Err(Error::Truncated {
            what: "ranged chunk",
            need: end as u64,
            have: stored.len() as u64,
        });
    }
    if !chunk_is_framed(id) {
        return Ok(stored[start..end].to_vec());
    }
    let mut at = 0usize;
    let mut decoded = 0u64;
    while at < start {
        let header = stored
            .get(at..at + BLOCK_HEADER_LEN)
            .ok_or_else(|| block_error(at, BlockProblem::HeaderCut))?;
        decoded += u64::from(u32::from_be_bytes([
            header[0], header[1], header[2], header[3],
        ]));
        at += BLOCK_HEADER_LEN
            + u32::from_be_bytes([header[4], header[5], header[6], header[7]]) as usize;
    }
    if at != start {
        return Err(block_error(start, BlockProblem::RangeNotOnBlock));
    }
    if decoded != u64::from(range.logical_offset) {
        return Err(block_error(start, BlockProblem::RangeLogicalOffset));
    }
    decode_blocks(&stored[start..end]).map_err(|error| match error {
        Error::Block { at, problem } => Error::Block {
            at: at + start as u64,
            problem,
        },
        other => other,
    })
}

fn block_error(at: usize, problem: BlockProblem) -> Error {
    Error::Block {
        at: at as u64,
        problem,
    }
}
