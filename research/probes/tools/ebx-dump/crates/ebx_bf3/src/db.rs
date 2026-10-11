use std::collections::HashMap;

use crate::{Error, Guid, Instance, Partition, Ref, Struct};

/// Parsed partitions of one mounted set, found by GUID or by name, with references followed
/// across partitions. Names are the bundle manifest names and compare case-insensitively.
#[derive(Default)]
pub struct EbxDb {
    partitions: Vec<(Box<str>, Partition)>,
    by_guid: HashMap<Guid, u32>,
    by_name: HashMap<Box<str>, u32>,
}

impl EbxDb {
    pub fn new() -> EbxDb {
        EbxDb::default()
    }

    /// Keeps the first partition seen for a GUID or name; a later one is dropped and `false`
    /// returned.
    pub fn insert(&mut self, name: &str, partition: Partition) -> bool {
        let key: Box<str> = name.to_ascii_lowercase().into();
        if self.by_name.contains_key(&key) || self.by_guid.contains_key(&partition.guid()) {
            return false;
        }
        let slot = self.partitions.len() as u32;
        self.by_guid.insert(partition.guid(), slot);
        self.by_name.insert(key.clone(), slot);
        self.partitions.push((key, partition));
        true
    }

    pub fn parse_insert(&mut self, name: &str, bytes: impl Into<Box<[u8]>>) -> Result<bool, Error> {
        Ok(self.insert(name, Partition::parse(bytes)?))
    }

    pub fn len(&self) -> usize {
        self.partitions.len()
    }

    pub fn is_empty(&self) -> bool {
        self.partitions.is_empty()
    }

    pub fn get(&self, guid: &Guid) -> Option<&Partition> {
        let slot = *self.by_guid.get(guid)?;
        Some(&self.partitions[slot as usize].1)
    }

    pub fn get_by_name(&self, name: &str) -> Option<&Partition> {
        let slot = if name.bytes().any(|b| b.is_ascii_uppercase()) {
            *self.by_name.get(name.to_ascii_lowercase().as_str())?
        } else {
            *self.by_name.get(name)?
        };
        Some(&self.partitions[slot as usize].1)
    }

    /// The lowercase name a partition was inserted under.
    pub fn name_of(&self, guid: &Guid) -> Option<&str> {
        let slot = *self.by_guid.get(guid)?;
        Some(&self.partitions[slot as usize].0)
    }

    pub fn iter(&self) -> impl ExactSizeIterator<Item = (&str, &Partition)> {
        self.partitions
            .iter()
            .map(|(name, partition)| (&**name, partition))
    }

    pub fn instance(&self, partition: &Guid, instance: &Guid) -> Option<Instance<'_>> {
        self.get(partition)?.instance_by_guid(instance)
    }

    /// Follows `reference` as read from a field of `from`.
    pub fn resolve<'a>(&'a self, from: &'a Partition, reference: Ref) -> Option<Instance<'a>> {
        match reference {
            Ref::Null => None,
            Ref::Internal(index) => from.instance(index as usize),
            Ref::External {
                partition,
                instance,
            } => self.instance(&partition, &instance),
        }
    }

    pub fn follow<'a>(
        &'a self,
        object: impl Into<Struct<'a>>,
        field: &str,
    ) -> Option<Instance<'a>> {
        let object = object.into();
        self.resolve(object.partition(), object.get(field)?.as_ref()?)
    }

    /// The instances an array-of-references field of `object` points at, skipping null and
    /// unresolved entries.
    pub fn follow_all<'a, S: Into<Struct<'a>>>(
        &'a self,
        object: S,
        field: &str,
    ) -> impl Iterator<Item = Instance<'a>> + use<'a, S> {
        let object = object.into();
        let array = object.get(field).and_then(|value| value.as_array());
        array
            .into_iter()
            .flatten()
            .filter_map(move |value| self.resolve(object.partition(), value.as_ref()?))
    }
}
