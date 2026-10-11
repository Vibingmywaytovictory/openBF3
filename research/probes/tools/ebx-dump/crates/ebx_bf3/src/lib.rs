#![forbid(unsafe_code)]

mod db;
mod error;
mod guid;
mod partition;
mod value;

pub use db::EbxDb;
pub use error::{Error, Section};
pub use guid::{Guid, ParseGuidError};
pub use partition::{Import, Partition};
pub use value::{Array, ArrayIter, Fields, Instance, Ref, Struct, TypeChain, Value};

/// The hash EBX uses for type, field and enumerator names and for connection field ids.
pub fn name_hash(name: &str) -> u32 {
    name.bytes().fold(5381u32, |hash, byte| {
        hash.wrapping_mul(33) ^ u32::from(byte)
    })
}
