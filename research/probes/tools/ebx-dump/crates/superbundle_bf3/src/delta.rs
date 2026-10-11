use std::sync::Arc;

use crate::error::{Error, need};

pub const DELTA_MAGIC: u32 = 0xDE17_AAAA;

pub const DELTA_HEADER_LEN: usize = 16;

const DELTA_RUN_LEN: usize = 16;

pub trait ReadAt {
    fn read_exact_at(&self, buf: &mut [u8], offset: u64) -> std::io::Result<()>;
}

impl ReadAt for [u8] {
    fn read_exact_at(&self, buf: &mut [u8], offset: u64) -> std::io::Result<()> {
        let source = usize::try_from(offset)
            .ok()
            .and_then(|start| self.get(start..start.checked_add(buf.len())?))
            .ok_or_else(|| std::io::Error::from(std::io::ErrorKind::UnexpectedEof))?;
        buf.copy_from_slice(source);
        Ok(())
    }
}

impl ReadAt for Vec<u8> {
    fn read_exact_at(&self, buf: &mut [u8], offset: u64) -> std::io::Result<()> {
        self.as_slice().read_exact_at(buf, offset)
    }
}

impl<T: ReadAt + ?Sized> ReadAt for &T {
    fn read_exact_at(&self, buf: &mut [u8], offset: u64) -> std::io::Result<()> {
        (**self).read_exact_at(buf, offset)
    }
}

impl<T: ReadAt + ?Sized> ReadAt for Arc<T> {
    fn read_exact_at(&self, buf: &mut [u8], offset: u64) -> std::io::Result<()> {
        (**self).read_exact_at(buf, offset)
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DeltaSource {
    Patch { offset: u64 },
    Base { file: u32, offset: u64 },
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DeltaRun {
    pub start: u64,
    pub len: u64,
    pub source: DeltaSource,
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct DeltaBundle {
    runs: Vec<DeltaRun>,
    len: u64,
}

pub fn delta_table_len(header: &[u8]) -> Result<u64, Error> {
    need("delta bundle header", header, DELTA_HEADER_LEN)?;
    let magic = u32::from_be_bytes([header[4], header[5], header[6], header[7]]);
    if magic != DELTA_MAGIC {
        return Err(Error::BadMagic {
            what: "delta bundle",
            got: magic,
        });
    }
    let runs_len = u32::from_be_bytes([header[0], header[1], header[2], header[3]]);
    if !(runs_len as usize).is_multiple_of(DELTA_RUN_LEN) {
        return Err(Error::Layout {
            what: "delta bundle",
            problem: "run table is not a whole number of runs",
        });
    }
    Ok(DELTA_HEADER_LEN as u64 + u64::from(runs_len))
}

impl DeltaBundle {
    /// Run offsets are absolute in the patch and base superbundle files, so
    /// `read_at` takes those whole files, never a bundle-sized window.
    pub fn parse(table: &[u8], patch_offset: u64, patch_len: u64) -> Result<Self, Error> {
        let table_len = delta_table_len(table)?;
        need("delta run table", table, table_len as usize)?;
        let mut runs = Vec::with_capacity((table_len as usize - DELTA_HEADER_LEN) / DELTA_RUN_LEN);
        let mut patch_at = table_len;
        let mut start = 0u64;
        for run in table[DELTA_HEADER_LEN..table_len as usize].chunks_exact(DELTA_RUN_LEN) {
            let len = u64::from(u32::from_be_bytes([run[0], run[1], run[2], run[3]]));
            let file = u32::from_be_bytes([run[4], run[5], run[6], run[7]]);
            let offset = u64::from_be_bytes([
                run[8], run[9], run[10], run[11], run[12], run[13], run[14], run[15],
            ]);
            let source = if file == 0 {
                let source = DeltaSource::Patch {
                    offset: patch_offset + patch_at,
                };
                patch_at += len;
                source
            } else {
                offset.checked_add(len).ok_or(Error::Layout {
                    what: "delta bundle",
                    problem: "base run overflows",
                })?;
                DeltaSource::Base { file, offset }
            };
            if len != 0 {
                runs.push(DeltaRun { start, len, source });
            }
            start += len;
        }
        if patch_at > patch_len {
            return Err(Error::Truncated {
                what: "delta bundle patch data",
                need: patch_at,
                have: patch_len,
            });
        }
        Ok(Self { runs, len: start })
    }

    pub fn len(&self) -> u64 {
        self.len
    }

    pub fn is_empty(&self) -> bool {
        self.len == 0
    }

    pub fn runs(&self) -> &[DeltaRun] {
        &self.runs
    }

    pub fn read_at(
        &self,
        patch: &dyn ReadAt,
        base: &dyn ReadAt,
        offset: u64,
        buf: &mut [u8],
    ) -> Result<(), Error> {
        let end = offset
            .checked_add(buf.len() as u64)
            .filter(|&end| end <= self.len)
            .ok_or(Error::Truncated {
                what: "delta bundle read",
                need: offset.saturating_add(buf.len() as u64),
                have: self.len,
            })?;
        let mut first = self
            .runs
            .partition_point(|run| run.start + run.len <= offset);
        let mut at = offset;
        let mut filled = 0usize;
        while at < end {
            let run = self.runs.get(first).ok_or(Error::Layout {
                what: "delta bundle",
                problem: "runs do not cover the read",
            })?;
            let within = at - run.start;
            let take = (run.len - within).min(end - at) as usize;
            let out = &mut buf[filled..filled + take];
            match run.source {
                DeltaSource::Patch { offset } => patch.read_exact_at(out, offset + within)?,
                DeltaSource::Base { offset, .. } => base.read_exact_at(out, offset + within)?,
            }
            filled += take;
            at += take as u64;
            first += 1;
        }
        Ok(())
    }
}

pub struct DeltaView<P, B> {
    bundle: DeltaBundle,
    patch: P,
    base: B,
}

impl<P: ReadAt, B: ReadAt> DeltaView<P, B> {
    pub fn new(bundle: DeltaBundle, patch: P, base: B) -> Self {
        Self {
            bundle,
            patch,
            base,
        }
    }

    pub fn bundle(&self) -> &DeltaBundle {
        &self.bundle
    }

    pub fn len(&self) -> u64 {
        self.bundle.len()
    }

    pub fn is_empty(&self) -> bool {
        self.bundle.is_empty()
    }

    pub fn read_at(&self, offset: u64, buf: &mut [u8]) -> Result<(), Error> {
        self.bundle.read_at(&self.patch, &self.base, offset, buf)
    }
}

impl<P: ReadAt, B: ReadAt> ReadAt for DeltaView<P, B> {
    fn read_exact_at(&self, buf: &mut [u8], offset: u64) -> std::io::Result<()> {
        self.read_at(offset, buf).map_err(|error| match error {
            Error::Io(error) => error,
            other => std::io::Error::new(std::io::ErrorKind::InvalidData, other.to_string()),
        })
    }
}
