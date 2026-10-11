use std::fmt;

#[derive(Debug)]
pub enum Error {
    Truncated {
        what: &'static str,
        need: u64,
        have: u64,
    },
    BadMagic {
        what: &'static str,
        got: u32,
    },
    UnknownDbTag {
        tag: u8,
        at: usize,
    },
    BadDbObject {
        at: usize,
        problem: &'static str,
    },
    BadUtf8 {
        what: &'static str,
    },
    MissingField {
        what: &'static str,
        field: &'static str,
    },
    FieldType {
        what: &'static str,
        field: &'static str,
    },
    FieldRange {
        what: &'static str,
        field: &'static str,
        value: i64,
    },
    Layout {
        what: &'static str,
        problem: &'static str,
    },
    Block {
        at: u64,
        problem: BlockProblem,
    },
    OutOfMemory {
        bytes: u64,
    },
    Io(std::io::Error),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BlockProblem {
    HeaderCut,
    DataCut,
    TooLarge,
    Inflate,
    InflatedLength,
    RangeNotOnBlock,
    RangeLogicalOffset,
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Truncated { what, need, have } => {
                write!(f, "{what} is truncated: need {need} bytes, have {have}")
            }
            Self::BadMagic { what, got } => write!(f, "{what} has bad magic {got:#010x}"),
            Self::UnknownDbTag { tag, at } => {
                write!(f, "unsupported DbObject tag {tag:#04x} at byte {at}")
            }
            Self::BadDbObject { at, problem } => {
                write!(f, "malformed DbObject at byte {at}: {problem}")
            }
            Self::BadUtf8 { what } => write!(f, "{what} is not UTF-8"),
            Self::MissingField { what, field } => write!(f, "{what} has no `{field}`"),
            Self::FieldType { what, field } => write!(f, "{what} `{field}` has the wrong type"),
            Self::FieldRange { what, field, value } => {
                write!(f, "{what} `{field}` is out of range ({value})")
            }
            Self::Layout { what, problem } => write!(f, "{what}: {problem}"),
            Self::Block { at, problem } => {
                let problem = match problem {
                    BlockProblem::HeaderCut => "block header runs past the payload",
                    BlockProblem::DataCut => "block data runs past the payload",
                    BlockProblem::TooLarge => "block claims more than 64 KiB",
                    BlockProblem::Inflate => "zlib stream is invalid",
                    BlockProblem::InflatedLength => "zlib stream inflates to the wrong length",
                    BlockProblem::RangeNotOnBlock => "range start is not a block boundary",
                    BlockProblem::RangeLogicalOffset => {
                        "blocks before the range do not add up to its logical offset"
                    }
                };
                write!(f, "payload block at byte {at}: {problem}")
            }
            Self::OutOfMemory { bytes } => write!(f, "cannot allocate {bytes} bytes"),
            Self::Io(error) => write!(f, "read failed: {error}"),
        }
    }
}

impl std::error::Error for Error {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::Io(error) => Some(error),
            _ => None,
        }
    }
}

impl From<std::io::Error> for Error {
    fn from(error: std::io::Error) -> Self {
        Self::Io(error)
    }
}

pub(crate) fn need(what: &'static str, bytes: &[u8], need: usize) -> Result<(), Error> {
    if bytes.len() < need {
        return Err(Error::Truncated {
            what,
            need: need as u64,
            have: bytes.len() as u64,
        });
    }
    Ok(())
}
