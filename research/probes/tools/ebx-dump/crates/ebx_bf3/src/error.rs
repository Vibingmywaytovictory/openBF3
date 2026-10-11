use std::fmt;

use crate::Guid;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Section {
    Header,
    Imports,
    Names,
    Fields,
    Complexes,
    InstanceRepeaters,
    ArrayRepeaters,
    Strings,
    Instances,
    Arrays,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Error {
    BadMagic(u32),
    BigEndian,
    Truncated(Section),
    SizeMismatch {
        meta_size: u32,
        payload_size: u32,
        len: usize,
    },
    MetaLayout {
        computed: usize,
        meta_size: u32,
    },
    NotUtf8(Section),
    UnknownName(u32),
    BadComplex(u32),
    UnexpectedKind {
        complex: String,
        code: u8,
    },
    UnknownTypeCode {
        complex: String,
        field: String,
        code: u8,
    },
    FieldOutOfBounds {
        complex: String,
        field: String,
    },
    DuplicateBase {
        complex: String,
    },
    InternalInstances {
        complex: String,
    },
    MissingPrimary(Guid),
    BadArray(u32),
    ArrayCount {
        array: u32,
        count: u32,
        stored: u32,
    },
    BadReference(u32),
    BadString(u32),
    ArrayCycle(u32),
    TooDeep,
    Overlapping,
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::BadMagic(magic) => write!(f, "not an EBX partition (magic {magic:#010x})"),
            Error::BigEndian => {
                f.write_str("big-endian (console) EBX partitions are not supported")
            }
            Error::Truncated(section) => {
                write!(f, "{section:?} runs past the end of the partition")
            }
            Error::SizeMismatch {
                meta_size,
                payload_size,
                len,
            } => write!(
                f,
                "header sizes {meta_size} + {payload_size} do not add up to the partition length {len}"
            ),
            Error::MetaLayout {
                computed,
                meta_size,
            } => write!(
                f,
                "descriptor tables end at {computed}, header says the metadata ends at {meta_size}"
            ),
            Error::NotUtf8(section) => write!(f, "{section:?} is not UTF-8"),
            Error::UnknownName(hash) => {
                write!(f, "name hash {hash:#010x} is not in the name table")
            }
            Error::BadComplex(index) => write!(f, "type descriptor index {index} is out of range"),
            Error::UnexpectedKind { complex, code } => {
                write!(f, "type `{complex}` has type code {code} where it is used")
            }
            Error::UnknownTypeCode {
                complex,
                field,
                code,
            } => write!(
                f,
                "field `{complex}.{field}` has unsupported type code {code}"
            ),
            Error::FieldOutOfBounds { complex, field } => {
                write!(f, "field `{complex}.{field}` lies outside its type")
            }
            Error::DuplicateBase { complex } => {
                write!(f, "type `{complex}` has more than one base")
            }
            Error::InternalInstances { complex } => {
                write!(f, "instances of `{complex}` carry internal data")
            }
            Error::MissingPrimary(guid) => {
                write!(f, "primary instance {guid} is not in the partition")
            }
            Error::BadArray(index) => write!(f, "array {index} is out of range"),
            Error::ArrayCount {
                array,
                count,
                stored,
            } => write!(
                f,
                "array {array} has {count} elements but {stored} is stored before its data"
            ),
            Error::BadReference(value) => {
                write!(f, "reference {value:#010x} names no instance or import")
            }
            Error::BadString(offset) => {
                write!(f, "string offset {offset:#x} is outside the string table")
            }
            Error::ArrayCycle(index) => write!(f, "array {index} contains itself"),
            Error::TooDeep => f.write_str("types or arrays nest deeper than the reader allows"),
            Error::Overlapping => {
                f.write_str("fields or arrays overlap so much that the partition cannot be checked")
            }
        }
    }
}

impl std::error::Error for Error {}
