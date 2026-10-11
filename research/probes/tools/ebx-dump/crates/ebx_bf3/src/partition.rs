use std::ops::Range;

use crate::value::{Instance, Ref};
use crate::{Error, Guid, Section, name_hash};

pub(crate) mod code {
    pub const VOID: u8 = 0;
    pub const VALUE_TYPE: u8 = 2;
    pub const CLASS: u8 = 3;
    pub const ARRAY: u8 = 4;
    pub const CSTRING: u8 = 7;
    pub const ENUM: u8 = 8;
    pub const FILE_REF: u8 = 9;
    pub const BOOLEAN: u8 = 10;
    pub const INT8: u8 = 11;
    pub const UINT8: u8 = 12;
    pub const INT16: u8 = 13;
    pub const UINT16: u8 = 14;
    pub const INT32: u8 = 15;
    pub const UINT32: u8 = 16;
    pub const INT64: u8 = 17;
    pub const UINT64: u8 = 18;
    pub const FLOAT32: u8 = 19;
    pub const FLOAT64: u8 = 20;
    pub const GUID: u8 = 21;
    pub const SHA1: u8 = 22;
}

use code::*;

const MAGIC: u32 = 0x0FB2_D1CE;
const MAGIC_BIG_ENDIAN: u32 = 0xCED1_B20F;
const HEADER_LEN: usize = 80;
const NULL_STRING: u32 = u32::MAX;
const EXTERNAL: u32 = 0x8000_0000;
const NO_TYPE: u16 = u16::MAX;
pub(crate) const BASE_DEPTH: usize = 32;
const NEST_DEPTH: usize = 64;

pub(crate) fn scalar_size(code: u8) -> Option<u32> {
    match code {
        BOOLEAN | INT8 | UINT8 => Some(1),
        INT16 | UINT16 => Some(2),
        CLASS | ARRAY | CSTRING | ENUM | FILE_REF | INT32 | UINT32 | FLOAT32 => Some(4),
        INT64 | UINT64 | FLOAT64 => Some(8),
        GUID => Some(16),
        SHA1 => Some(20),
        _ => None,
    }
}

pub(crate) fn le16(bytes: &[u8], at: usize) -> Option<u16> {
    Some(u16::from_le_bytes(bytes.get(at..at + 2)?.try_into().ok()?))
}

pub(crate) fn le32(bytes: &[u8], at: usize) -> Option<u32> {
    Some(u32::from_le_bytes(bytes.get(at..at + 4)?.try_into().ok()?))
}

pub(crate) fn le64(bytes: &[u8], at: usize) -> Option<u64> {
    Some(u64::from_le_bytes(bytes.get(at..at + 8)?.try_into().ok()?))
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct Import {
    pub partition: Guid,
    pub instance: Guid,
}

#[derive(Clone, Copy)]
pub(crate) struct Name {
    start: u32,
    end: u32,
}

pub(crate) struct Field {
    pub name: Name,
    pub hash: u32,
    pub code: u8,
    pub reference: u16,
    pub offset: i32,
}

pub(crate) struct Complex {
    pub name: Name,
    pub hash: u32,
    pub fields: Range<u32>,
    pub code: u8,
    pub size: u32,
    pub base: Option<u32>,
}

pub(crate) struct InstanceEntry {
    pub guid: Guid,
    pub complex: u32,
    pub offset: u32,
}

pub(crate) struct ArrayEntry {
    pub start: u32,
    pub len: u32,
    pub complex: u32,
}

/// One EBX partition, validated in full when parsed. Field values are decoded from the kept bytes
/// on access, through the partition's own type descriptors.
pub struct Partition {
    pub(crate) bytes: Box<[u8]>,
    guid: Guid,
    primary: u32,
    imports: Vec<Import>,
    names: Box<str>,
    strings: Box<str>,
    pub(crate) fields: Vec<Field>,
    pub(crate) complexes: Vec<Complex>,
    pub(crate) instances: Vec<InstanceEntry>,
    pub(crate) arrays: Vec<ArrayEntry>,
    by_guid: Vec<u32>,
}

const _: () = {
    const fn shared<T: Send + Sync>() {}
    shared::<Partition>();
};

struct Tables {
    imports: usize,
    names: usize,
    fields: usize,
    complexes: usize,
    instance_repeaters: usize,
    array_repeaters: usize,
    end: usize,
}

fn table_layout(header: &[u32; 12]) -> Option<Tables> {
    let [
        _,
        _,
        _,
        imports,
        _,
        instance_repeaters,
        complexes,
        fields,
        names_len,
        _,
        arrays,
        _,
    ] = header.map(|word| word as usize);
    let span = |at: usize, count: usize, size: usize| at.checked_add(count.checked_mul(size)?);
    let align = |at: usize| at.checked_next_multiple_of(16);
    let imports_at = HEADER_LEN;
    let names_at = span(imports_at, imports, 32)?;
    let fields_at = span(names_at, names_len, 1)?;
    let complexes_at = align(span(fields_at, fields, 16)?)?;
    let instance_repeaters_at = span(complexes_at, complexes, 16)?;
    let array_repeaters_at = align(span(instance_repeaters_at, instance_repeaters, 12)?)?;
    let end = align(span(array_repeaters_at, arrays, 12)?)?;
    Some(Tables {
        imports: imports_at,
        names: names_at,
        fields: fields_at,
        complexes: complexes_at,
        instance_repeaters: instance_repeaters_at,
        array_repeaters: array_repeaters_at,
        end,
    })
}

impl Partition {
    pub fn parse(bytes: impl Into<Box<[u8]>>) -> Result<Partition, Error> {
        let bytes: Box<[u8]> = bytes.into();
        let mut header = [0u32; 12];
        for (i, word) in header.iter_mut().enumerate() {
            *word = le32(&bytes, i * 4).ok_or(Error::Truncated(Section::Header))?;
        }
        let guid = Guid::read(&bytes, 0x30).ok_or(Error::Truncated(Section::Header))?;
        let primary_guid = Guid::read(&bytes, 0x40).ok_or(Error::Truncated(Section::Header))?;
        match header[0] {
            MAGIC => {}
            MAGIC_BIG_ENDIAN => return Err(Error::BigEndian),
            magic => return Err(Error::BadMagic(magic)),
        }
        let [
            _,
            meta_size,
            payload_size,
            import_count,
            _,
            instance_repeater_count,
            complex_count,
            field_count,
            names_len,
            strings_len,
            array_count,
            instance_payload_len,
        ] = header;
        if u32::try_from(bytes.len()).ok() != meta_size.checked_add(payload_size) {
            return Err(Error::SizeMismatch {
                meta_size,
                payload_size,
                len: bytes.len(),
            });
        }
        let tables = table_layout(&header).ok_or(Error::Truncated(Section::Header))?;
        if tables.end != meta_size as usize {
            return Err(Error::MetaLayout {
                computed: tables.end,
                meta_size,
            });
        }

        let imports = (0..import_count as usize)
            .map(|i| {
                let at = tables.imports + i * 32;
                Some(Import {
                    partition: Guid::read(&bytes, at)?,
                    instance: Guid::read(&bytes, at + 16)?,
                })
            })
            .collect::<Option<Vec<_>>>()
            .ok_or(Error::Truncated(Section::Imports))?;

        let names_region = bytes
            .get(tables.names..tables.names + names_len as usize)
            .ok_or(Error::Truncated(Section::Names))?;
        let names: Box<str> = std::str::from_utf8(names_region)
            .map_err(|_| Error::NotUtf8(Section::Names))?
            .into();
        let mut lookup = Vec::new();
        let mut start = 0;
        for piece in names.split('\0') {
            let end = start + piece.len();
            if !piece.is_empty() {
                let name = Name {
                    start: start as u32,
                    end: end as u32,
                };
                lookup.push((name_hash(piece), name));
            }
            start = end + 1;
        }
        lookup.sort_unstable_by_key(|entry| entry.0);
        let resolve = |hash: u32| {
            lookup
                .binary_search_by_key(&hash, |entry| entry.0)
                .map(|i| lookup[i].1)
                .map_err(|_| Error::UnknownName(hash))
        };

        let mut fields = Vec::with_capacity(field_count as usize);
        for i in 0..field_count as usize {
            let at = tables.fields + i * 16;
            let read = || {
                Some((
                    le32(&bytes, at)?,
                    le16(&bytes, at + 4)?,
                    le16(&bytes, at + 6)?,
                    le32(&bytes, at + 8)? as i32,
                ))
            };
            let (hash, flags, reference, offset) =
                read().ok_or(Error::Truncated(Section::Fields))?;
            fields.push(Field {
                name: resolve(hash)?,
                hash,
                code: ((flags >> 4) & 0x1F) as u8,
                reference,
                offset,
            });
        }

        let mut complexes = Vec::with_capacity(complex_count as usize);
        for i in 0..complex_count as usize {
            let at = tables.complexes + i * 16;
            let read = || {
                Some((
                    le32(&bytes, at)?,
                    le32(&bytes, at + 4)?,
                    *bytes.get(at + 8)?,
                    le16(&bytes, at + 10)?,
                    le16(&bytes, at + 12)?,
                ))
            };
            let (hash, field_start, count, flags, size) =
                read().ok_or(Error::Truncated(Section::Complexes))?;
            let field_end = field_start
                .checked_add(u32::from(count))
                .filter(|&end| end as usize <= fields.len())
                .ok_or(Error::BadComplex(i as u32))?;
            complexes.push(Complex {
                name: resolve(hash)?,
                hash,
                fields: field_start..field_end,
                code: ((flags >> 4) & 0x1F) as u8,
                size: u32::from(size),
                base: None,
            });
        }

        let read_triples = |at: usize, count: u32, section: Section| {
            (0..count as usize)
                .map(|i| {
                    let at = at + i * 12;
                    Some([
                        le32(&bytes, at)?,
                        le32(&bytes, at + 4)?,
                        le32(&bytes, at + 8)?,
                    ])
                })
                .collect::<Option<Vec<_>>>()
                .ok_or(Error::Truncated(section))
        };
        let instance_repeaters = read_triples(
            tables.instance_repeaters,
            instance_repeater_count,
            Section::InstanceRepeaters,
        )?;
        let array_repeaters =
            read_triples(tables.array_repeaters, array_count, Section::ArrayRepeaters)?;

        let strings_at = meta_size as usize;
        let strings_region = bytes
            .get(strings_at..strings_at + strings_len as usize)
            .ok_or(Error::Truncated(Section::Strings))?;
        let strings: Box<str> = std::str::from_utf8(strings_region)
            .map_err(|_| Error::NotUtf8(Section::Strings))?
            .into();

        let payload_at = strings_at + strings_len as usize;
        let payload_end = payload_at + instance_payload_len as usize;
        if payload_end > bytes.len() {
            return Err(Error::Truncated(Section::Instances));
        }

        let mut partition = Partition {
            guid,
            primary: 0,
            imports,
            names,
            strings,
            fields,
            complexes,
            instances: Vec::new(),
            arrays: Vec::with_capacity(array_repeaters.len()),
            by_guid: Vec::new(),
            bytes: Box::default(),
        };
        partition.link_complexes()?;

        let mut at = payload_at;
        for [internal, count, complex] in instance_repeaters {
            let size = partition.expect_kind(complex, CLASS)?.size as usize;
            if internal != 0 {
                return Err(Error::InternalInstances {
                    complex: partition.complex_name(complex).to_owned(),
                });
            }
            for _ in 0..count {
                let guid = Guid::read(&bytes, at).ok_or(Error::Truncated(Section::Instances))?;
                let data = at + 16;
                at = data + size;
                if at > payload_end {
                    return Err(Error::Truncated(Section::Instances));
                }
                partition.instances.push(InstanceEntry {
                    guid,
                    complex,
                    offset: data as u32,
                });
            }
        }

        let arrays_at = payload_end;
        for (index, [offset, len, complex]) in array_repeaters.into_iter().enumerate() {
            let member = partition.array_member(complex)?;
            let stride = partition.stride(member) as usize;
            let start = arrays_at
                .checked_add(offset as usize)
                .ok_or(Error::Truncated(Section::Arrays))?;
            let end = (len as usize)
                .checked_mul(stride)
                .and_then(|size| start.checked_add(size))
                .ok_or(Error::Truncated(Section::Arrays))?;
            if end > bytes.len() {
                return Err(Error::Truncated(Section::Arrays));
            }
            if len != 0 {
                let stored = start
                    .checked_sub(4)
                    .and_then(|at| le32(&bytes, at))
                    .ok_or(Error::Truncated(Section::Arrays))?;
                if stored != len {
                    return Err(Error::ArrayCount {
                        array: index as u32,
                        count: len,
                        stored,
                    });
                }
            }
            partition.arrays.push(ArrayEntry {
                start: start as u32,
                len,
                complex,
            });
        }

        partition.primary = partition
            .instances
            .iter()
            .position(|instance| instance.guid == primary_guid)
            .ok_or(Error::MissingPrimary(primary_guid))? as u32;
        let mut by_guid: Vec<u32> = (0..partition.instances.len() as u32).collect();
        by_guid.sort_unstable_by_key(|&i| partition.instances[i as usize].guid);
        partition.by_guid = by_guid;
        partition.bytes = bytes;
        partition.validate()?;
        Ok(partition)
    }

    pub fn guid(&self) -> Guid {
        self.guid
    }

    pub fn primary_guid(&self) -> Guid {
        self.instances[self.primary as usize].guid
    }

    pub fn primary(&self) -> Instance<'_> {
        Instance::new(self, self.primary)
    }

    pub fn imports(&self) -> &[Import] {
        &self.imports
    }

    pub fn instance_count(&self) -> usize {
        self.instances.len()
    }

    pub fn instance(&self, index: usize) -> Option<Instance<'_>> {
        (index < self.instances.len()).then(|| Instance::new(self, index as u32))
    }

    pub fn instances(&self) -> impl ExactSizeIterator<Item = Instance<'_>> + '_ {
        (0..self.instances.len() as u32).map(|index| Instance::new(self, index))
    }

    /// Instances whose type is `type_name` or derives from it.
    pub fn instances_of<'a>(&'a self, type_name: &'a str) -> impl Iterator<Item = Instance<'a>> {
        let hash = name_hash(type_name);
        self.instances()
            .filter(move |instance| self.derives_from(instance.complex(), hash, type_name))
    }

    pub fn instance_by_guid(&self, guid: &Guid) -> Option<Instance<'_>> {
        let slot = self
            .by_guid
            .binary_search_by_key(guid, |&i| self.instances[i as usize].guid)
            .ok()?;
        Some(Instance::new(self, self.by_guid[slot]))
    }

    /// Follows a reference that stays inside this partition; external references need an
    /// [`crate::EbxDb`].
    pub fn resolve(&self, reference: Ref) -> Option<Instance<'_>> {
        match reference {
            Ref::Internal(index) => self.instance(index as usize),
            Ref::External {
                partition,
                instance,
            } if partition == self.guid => self.instance_by_guid(&instance),
            _ => None,
        }
    }

    pub(crate) fn name(&self, name: Name) -> &str {
        self.names
            .get(name.start as usize..name.end as usize)
            .unwrap_or("")
    }

    pub(crate) fn complex_name(&self, complex: u32) -> &str {
        match self.complexes.get(complex as usize) {
            Some(desc) => self.name(desc.name),
            None => "",
        }
    }

    pub(crate) fn field_name(&self, field: u32) -> &str {
        match self.fields.get(field as usize) {
            Some(desc) => self.name(desc.name),
            None => "",
        }
    }

    pub(crate) fn derives_from(&self, complex: u32, hash: u32, type_name: &str) -> bool {
        let mut current = Some(complex);
        for _ in 0..BASE_DEPTH {
            let Some(desc) = current.and_then(|c| self.complexes.get(c as usize)) else {
                return false;
            };
            if desc.hash == hash && self.name(desc.name) == type_name {
                return true;
            }
            current = desc.base;
        }
        false
    }

    pub(crate) fn string(&self, offset: u32) -> Option<Option<&str>> {
        if offset == NULL_STRING {
            return Some(None);
        }
        let rest = self.strings.get(offset as usize..)?;
        let end = rest.find('\0')?;
        Some(Some(&rest[..end]))
    }

    pub(crate) fn reference(&self, raw: u32) -> Option<Ref> {
        if raw == 0 {
            return Some(Ref::Null);
        }
        if raw & EXTERNAL != 0 {
            let import = self.imports.get((raw & !EXTERNAL) as usize)?;
            return Some(Ref::External {
                partition: import.partition,
                instance: import.instance,
            });
        }
        ((raw as usize) <= self.instances.len()).then_some(Ref::Internal(raw - 1))
    }

    pub(crate) fn enumerator(&self, complex: u32, value: i32) -> Option<&str> {
        let desc = self
            .complexes
            .get(complex as usize)
            .filter(|desc| desc.code == ENUM)?;
        desc.fields.clone().find_map(|field| {
            let field = &self.fields[field as usize];
            (field.offset == value).then(|| self.name(field.name))
        })
    }

    pub(crate) fn stride(&self, field: u32) -> u32 {
        let desc = &self.fields[field as usize];
        match desc.code {
            VALUE_TYPE => self.complexes[desc.reference as usize].size,
            code => scalar_size(code).unwrap_or(0),
        }
    }

    pub(crate) fn array_member(&self, complex: u32) -> Result<u32, Error> {
        let desc = self.expect_kind(complex, ARRAY)?;
        if desc.fields.is_empty() {
            return Err(Error::UnexpectedKind {
                complex: self.name(desc.name).to_owned(),
                code: ARRAY,
            });
        }
        Ok(desc.fields.start)
    }

    fn expect_kind(&self, complex: u32, code: u8) -> Result<&Complex, Error> {
        let desc = self
            .complexes
            .get(complex as usize)
            .ok_or(Error::BadComplex(complex))?;
        if desc.code != code {
            return Err(Error::UnexpectedKind {
                complex: self.name(desc.name).to_owned(),
                code: desc.code,
            });
        }
        Ok(desc)
    }

    fn unsupported(&self, complex: u32, field: u32) -> Error {
        Error::UnknownTypeCode {
            complex: self.complex_name(complex).to_owned(),
            field: self.field_name(field).to_owned(),
            code: self.fields[field as usize].code,
        }
    }

    fn out_of_bounds(&self, complex: u32, field: u32) -> Error {
        Error::FieldOutOfBounds {
            complex: self.complex_name(complex).to_owned(),
            field: self.field_name(field).to_owned(),
        }
    }

    fn link_complexes(&mut self) -> Result<(), Error> {
        for complex in 0..self.complexes.len() as u32 {
            match self.complexes[complex as usize].code {
                CLASS | VALUE_TYPE => {
                    let base = self.check_members(complex)?;
                    self.complexes[complex as usize].base = base;
                }
                ARRAY => {
                    let member = self.array_member(complex)?;
                    self.check_element(complex, member)?;
                }
                _ => {}
            }
        }
        for complex in 0..self.complexes.len() {
            let mut current = self.complexes[complex].base;
            let mut depth = 0;
            while let Some(base) = current {
                depth += 1;
                if depth >= BASE_DEPTH {
                    return Err(Error::TooDeep);
                }
                current = self.complexes[base as usize].base;
            }
        }
        Ok(())
    }

    fn check_members(&self, complex: u32) -> Result<Option<u32>, Error> {
        let desc = &self.complexes[complex as usize];
        let mut base = None;
        for field in desc.fields.clone() {
            let member = &self.fields[field as usize];
            let size = match member.code {
                VOID => {
                    if base.is_some() {
                        return Err(Error::DuplicateBase {
                            complex: self.name(desc.name).to_owned(),
                        });
                    }
                    base = Some(u32::from(member.reference));
                    self.expect_kind(u32::from(member.reference), desc.code)?
                        .size
                }
                VALUE_TYPE => {
                    self.expect_kind(u32::from(member.reference), VALUE_TYPE)?
                        .size
                }
                ENUM => {
                    self.expect_kind(u32::from(member.reference), ENUM)?;
                    4
                }
                code => scalar_size(code).ok_or_else(|| self.unsupported(complex, field))?,
            };
            let end = u32::try_from(member.offset)
                .ok()
                .and_then(|offset| offset.checked_add(size));
            if end.is_none_or(|end| end > desc.size) {
                return Err(self.out_of_bounds(complex, field));
            }
        }
        Ok(base)
    }

    fn check_element(&mut self, complex: u32, member: u32) -> Result<(), Error> {
        let desc = &self.fields[member as usize];
        match desc.code {
            VALUE_TYPE => self
                .expect_kind(u32::from(desc.reference), VALUE_TYPE)
                .map(|_| ()),
            ENUM => {
                // An enum element names no enum type (its reference is 0), so it has no names.
                self.fields[member as usize].reference = NO_TYPE;
                Ok(())
            }
            code if scalar_size(code).is_some() => Ok(()),
            _ => Err(self.unsupported(complex, member)),
        }
    }

    fn validate(&self) -> Result<(), Error> {
        let mut validator = Validator {
            partition: self,
            arrays: vec![Visit::New; self.arrays.len()],
            budget: self.bytes.len().saturating_mul(16).saturating_add(4096),
        };
        for entry in &self.instances {
            validator.object(entry.complex, entry.offset as usize, 0)?;
        }
        for index in 0..self.arrays.len() as u32 {
            validator.array(index, 0)?;
        }
        Ok(())
    }
}

#[derive(Clone, Copy)]
enum Visit {
    New,
    Open,
    Done,
}

/// Checks every reference, array index and string offset once, so decoding never meets a bad
/// one. Arrays are visited once each; the budget bounds descriptors that overlap on purpose.
struct Validator<'p> {
    partition: &'p Partition,
    arrays: Vec<Visit>,
    budget: usize,
}

impl Validator<'_> {
    fn spend(&mut self, depth: usize) -> Result<(), Error> {
        if depth > NEST_DEPTH {
            return Err(Error::TooDeep);
        }
        self.budget = self.budget.checked_sub(1).ok_or(Error::Overlapping)?;
        Ok(())
    }

    fn object(&mut self, complex: u32, at: usize, depth: usize) -> Result<(), Error> {
        self.spend(depth)?;
        let partition = self.partition;
        for field in partition.complexes[complex as usize].fields.clone() {
            let member = &partition.fields[field as usize];
            let field_at = at + member.offset as usize;
            if member.code == VOID {
                self.object(u32::from(member.reference), field_at, depth + 1)?;
            } else {
                self.value(field, field_at, depth + 1)?;
            }
        }
        Ok(())
    }

    fn value(&mut self, field: u32, at: usize, depth: usize) -> Result<(), Error> {
        let partition = self.partition;
        let member = &partition.fields[field as usize];
        let word = || le32(&partition.bytes, at).ok_or(Error::Truncated(Section::Instances));
        match member.code {
            VALUE_TYPE => self.object(u32::from(member.reference), at, depth),
            CLASS => {
                self.spend(depth)?;
                let raw = word()?;
                partition
                    .reference(raw)
                    .map(|_| ())
                    .ok_or(Error::BadReference(raw))
            }
            ARRAY => self.array(word()?, depth),
            CSTRING | FILE_REF => {
                self.spend(depth)?;
                let offset = word()?;
                partition
                    .string(offset)
                    .map(|_| ())
                    .ok_or(Error::BadString(offset))
            }
            _ => Ok(()),
        }
    }

    fn array(&mut self, index: u32, depth: usize) -> Result<(), Error> {
        match self.arrays.get(index as usize) {
            None => return Err(Error::BadArray(index)),
            Some(Visit::Done) => return Ok(()),
            Some(Visit::Open) => return Err(Error::ArrayCycle(index)),
            Some(Visit::New) => {}
        }
        self.spend(depth)?;
        let partition = self.partition;
        let entry = &partition.arrays[index as usize];
        let member = partition.complexes[entry.complex as usize].fields.start;
        if matches!(
            partition.fields[member as usize].code,
            VALUE_TYPE | CLASS | ARRAY | CSTRING | FILE_REF
        ) {
            self.arrays[index as usize] = Visit::Open;
            let stride = partition.stride(member) as usize;
            for i in 0..entry.len as usize {
                self.value(member, entry.start as usize + i * stride, depth + 1)?;
            }
        }
        self.arrays[index as usize] = Visit::Done;
        Ok(())
    }
}
