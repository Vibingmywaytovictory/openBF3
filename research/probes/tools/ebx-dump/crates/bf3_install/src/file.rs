use std::collections::HashMap;
use std::fs::File;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex, PoisonError};

use superbundle_bf3::ReadAt;

use super::Bf3Error;

pub(crate) struct DiskFile {
    path: PathBuf,
    file: File,
    len: u64,
}

impl DiskFile {
    pub(crate) fn open(path: &Path) -> Result<Self, Bf3Error> {
        let io = |source| Bf3Error::Io {
            path: path.to_path_buf(),
            source,
        };
        let file = File::open(path).map_err(io)?;
        let len = file.metadata().map_err(io)?.len();
        Ok(Self {
            path: path.to_path_buf(),
            file,
            len,
        })
    }

    pub(crate) fn path(&self) -> &Path {
        &self.path
    }

    pub(crate) fn len(&self) -> u64 {
        self.len
    }

    fn check_range(&self, offset: u64, len: u64) -> Result<(), Bf3Error> {
        if offset.checked_add(len).is_none_or(|end| end > self.len) {
            return Err(Bf3Error::OutOfFile {
                path: self.path.clone(),
                offset,
                len,
                file_len: self.len,
            });
        }
        Ok(())
    }

    pub(crate) fn read_into(&self, offset: u64, buf: &mut [u8]) -> Result<(), Bf3Error> {
        self.check_range(offset, buf.len() as u64)?;
        read_exact_at(&self.file, buf, offset).map_err(|source| Bf3Error::Io {
            path: self.path.clone(),
            source,
        })
    }

    pub(crate) fn read_vec(&self, offset: u64, len: u64) -> Result<Vec<u8>, Bf3Error> {
        self.check_range(offset, len)?;
        let mut buf = vec![0u8; len as usize];
        self.read_into(offset, &mut buf)?;
        Ok(buf)
    }
}

impl ReadAt for DiskFile {
    fn read_exact_at(&self, buf: &mut [u8], offset: u64) -> std::io::Result<()> {
        if offset
            .checked_add(buf.len() as u64)
            .is_none_or(|end| end > self.len)
        {
            return Err(std::io::ErrorKind::UnexpectedEof.into());
        }
        read_exact_at(&self.file, buf, offset)
    }
}

#[cfg(windows)]
fn read_exact_at(file: &File, mut buf: &mut [u8], mut offset: u64) -> std::io::Result<()> {
    use std::os::windows::fs::FileExt;
    while !buf.is_empty() {
        match file.seek_read(buf, offset) {
            Ok(0) => return Err(std::io::ErrorKind::UnexpectedEof.into()),
            Ok(read) => {
                buf = &mut buf[read..];
                offset += read as u64;
            }
            Err(error) if error.kind() == std::io::ErrorKind::Interrupted => {}
            Err(error) => return Err(error),
        }
    }
    Ok(())
}

#[cfg(unix)]
fn read_exact_at(file: &File, buf: &mut [u8], offset: u64) -> std::io::Result<()> {
    std::os::unix::fs::FileExt::read_exact_at(file, buf, offset)
}

#[derive(Default)]
pub(crate) struct FileCache {
    open: Mutex<HashMap<PathBuf, Arc<DiskFile>>>,
}

impl FileCache {
    pub(crate) fn get(&self, path: &Path) -> Result<Arc<DiskFile>, Bf3Error> {
        if let Some(hit) = self
            .open
            .lock()
            .unwrap_or_else(PoisonError::into_inner)
            .get(path)
        {
            return Ok(Arc::clone(hit));
        }
        let opened = Arc::new(DiskFile::open(path)?);
        let mut open = self.open.lock().unwrap_or_else(PoisonError::into_inner);
        Ok(Arc::clone(open.entry(path.to_path_buf()).or_insert(opened)))
    }
}

pub(crate) fn read_whole(path: &Path) -> Result<Vec<u8>, Bf3Error> {
    std::fs::read(path).map_err(|source| Bf3Error::Io {
        path: path.to_path_buf(),
        source,
    })
}

pub(crate) fn resolve(dir: &Path, relative: &str) -> Option<PathBuf> {
    let exact = relative
        .split('/')
        .fold(dir.to_path_buf(), |path, part| path.join(part));
    if exact.exists() {
        return Some(exact);
    }
    let mut path = dir.to_path_buf();
    for part in relative.split('/').filter(|part| !part.is_empty()) {
        let direct = path.join(part);
        if direct.exists() {
            path = direct;
            continue;
        }
        path = std::fs::read_dir(&path)
            .ok()?
            .filter_map(Result::ok)
            .find(|entry| {
                entry
                    .file_name()
                    .to_string_lossy()
                    .eq_ignore_ascii_case(part)
            })?
            .path();
    }
    Some(path)
}
