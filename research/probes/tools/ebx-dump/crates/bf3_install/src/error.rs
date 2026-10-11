use std::fmt;
use std::path::PathBuf;

use superbundle_bf3::{Guid, Sha1};

#[derive(Debug)]
pub enum Bf3Error {
    NotAnInstall {
        root: PathBuf,
    },
    Io {
        path: PathBuf,
        source: std::io::Error,
    },
    Format {
        at: String,
        source: superbundle_bf3::Error,
    },
    OutOfFile {
        path: PathBuf,
        offset: u64,
        len: u64,
        file_len: u64,
    },
    UnknownLevel(String),
    UnknownBundle(String),
    EbxNotLoaded(String),
    ResNotLoaded(String),
    ChunkNotFound(Guid),
    ChunkIncomplete(Guid),
    NotInCas(Sha1),
    SizeMismatch {
        what: String,
        expected: u64,
        got: u64,
    },
}

impl fmt::Display for Bf3Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::NotAnInstall { root } => write!(
                f,
                "{} is not a Battlefield 3 install (no Data/layout.toc and Data/cas.cat)",
                root.display()
            ),
            Self::Io { path, source } => write!(f, "{}: {source}", path.display()),
            Self::Format { at, source } => write!(f, "{at}: {source}"),
            Self::OutOfFile {
                path,
                offset,
                len,
                file_len,
            } => write!(
                f,
                "{}: read of {len} bytes at {offset} runs past the end ({file_len} bytes)",
                path.display()
            ),
            Self::UnknownLevel(level) => write!(f, "no Battlefield 3 level `{level}`"),
            Self::UnknownBundle(bundle) => write!(f, "no bundle `{bundle}` in this level"),
            Self::EbxNotLoaded(name) => write!(f, "no loaded bundle holds ebx `{name}`"),
            Self::ResNotLoaded(name) => write!(f, "no loaded bundle holds res `{name}`"),
            Self::ChunkNotFound(id) => write!(f, "chunk {id} is in no mounted bundle or toc"),
            Self::ChunkIncomplete(id) => write!(
                f,
                "chunk {id} is only a tail in its bundle and has no whole copy"
            ),
            Self::NotInCas(sha1) => write!(f, "payload {sha1} is in no CAS catalog"),
            Self::SizeMismatch {
                what,
                expected,
                got,
            } => write!(f, "{what}: expected {expected} bytes, got {got}"),
        }
    }
}

impl std::error::Error for Bf3Error {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::Io { source, .. } => Some(source),
            Self::Format { source, .. } => Some(source),
            _ => None,
        }
    }
}

impl Bf3Error {
    pub(crate) fn format(at: impl fmt::Display, source: superbundle_bf3::Error) -> Self {
        match source {
            superbundle_bf3::Error::Io(source) => Self::Io {
                path: PathBuf::from(at.to_string()),
                source,
            },
            source => Self::Format {
                at: at.to_string(),
                source,
            },
        }
    }
}
