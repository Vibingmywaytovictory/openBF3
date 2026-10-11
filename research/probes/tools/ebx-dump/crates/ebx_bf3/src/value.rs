use std::fmt;

use crate::partition::code::*;
use crate::partition::{BASE_DEPTH, Partition, le16, le32, le64};
use crate::{Guid, name_hash};

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Ref {
    Null,
    /// Index of an instance in the partition the reference was read from.
    Internal(u32),
    External {
        partition: Guid,
        instance: Guid,
    },
}

impl Ref {
    pub fn is_null(&self) -> bool {
        matches!(self, Ref::Null)
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Value<'a> {
    Bool(bool),
    I8(i8),
    U8(u8),
    I16(i16),
    U16(u16),
    I32(i32),
    U32(u32),
    I64(i64),
    U64(u64),
    F32(f32),
    F64(f64),
    Guid(Guid),
    Sha1([u8; 20]),
    /// `None` is a null string; [`Value::as_str`] reads it as empty.
    String(Option<&'a str>),
    Enum {
        value: i32,
        name: Option<&'a str>,
    },
    Ref(Ref),
    Struct(Struct<'a>),
    Array(Array<'a>),
}

impl<'a> Value<'a> {
    pub fn as_bool(&self) -> Option<bool> {
        match *self {
            Value::Bool(value) => Some(value),
            _ => None,
        }
    }

    /// Any integer or enum value that fits.
    pub fn as_i64(&self) -> Option<i64> {
        match *self {
            Value::I8(v) => Some(v.into()),
            Value::U8(v) => Some(v.into()),
            Value::I16(v) => Some(v.into()),
            Value::U16(v) => Some(v.into()),
            Value::I32(v) => Some(v.into()),
            Value::U32(v) => Some(v.into()),
            Value::I64(v) => Some(v),
            Value::U64(v) => i64::try_from(v).ok(),
            Value::Enum { value, .. } => Some(value.into()),
            _ => None,
        }
    }

    pub fn as_u64(&self) -> Option<u64> {
        match *self {
            Value::U64(v) => Some(v),
            _ => u64::try_from(self.as_i64()?).ok(),
        }
    }

    pub fn as_i32(&self) -> Option<i32> {
        i32::try_from(self.as_i64()?).ok()
    }

    pub fn as_u32(&self) -> Option<u32> {
        u32::try_from(self.as_i64()?).ok()
    }

    pub fn as_f32(&self) -> Option<f32> {
        match *self {
            Value::F32(v) => Some(v),
            Value::F64(v) => Some(v as f32),
            _ => None,
        }
    }

    pub fn as_f64(&self) -> Option<f64> {
        match *self {
            Value::F32(v) => Some(v.into()),
            Value::F64(v) => Some(v),
            _ => None,
        }
    }

    pub fn as_str(&self) -> Option<&'a str> {
        match *self {
            Value::String(text) => Some(text.unwrap_or("")),
            _ => None,
        }
    }

    pub fn enum_name(&self) -> Option<&'a str> {
        match *self {
            Value::Enum { name, .. } => name,
            _ => None,
        }
    }

    pub fn as_guid(&self) -> Option<Guid> {
        match *self {
            Value::Guid(guid) => Some(guid),
            _ => None,
        }
    }

    pub fn as_ref(&self) -> Option<Ref> {
        match *self {
            Value::Ref(reference) => Some(reference),
            _ => None,
        }
    }

    pub fn as_struct(&self) -> Option<Struct<'a>> {
        match *self {
            Value::Struct(object) => Some(object),
            _ => None,
        }
    }

    pub fn as_array(&self) -> Option<Array<'a>> {
        match *self {
            Value::Array(array) => Some(array),
            _ => None,
        }
    }

    pub fn is_null(&self) -> bool {
        matches!(self, Value::Ref(Ref::Null) | Value::String(None))
    }

    pub fn get(&self, field: &str) -> Option<Value<'a>> {
        self.as_struct()?.get(field)
    }

    /// Dotted field path with optional array indices, e.g. `"Transform.trans.x"` or
    /// `"MemberDatas[3].InstanceTransforms[0]"`.
    pub fn get_path(&self, path: &str) -> Option<Value<'a>> {
        let mut value = *self;
        for segment in path.split('.') {
            let split = segment.find('[').unwrap_or(segment.len());
            let (name, mut rest) = segment.split_at(split);
            if !name.is_empty() {
                value = value.get(name)?;
            }
            while let Some(inner) = rest.strip_prefix('[') {
                let close = inner.find(']')?;
                let index: usize = inner[..close].parse().ok()?;
                value = value.as_array()?.get(index)?;
                rest = &inner[close + 1..];
            }
            if !rest.is_empty() {
                return None;
            }
        }
        Some(value)
    }

    pub fn as_vec3(&self) -> Option<[f32; 3]> {
        self.as_struct()?.vec3()
    }

    pub fn as_transform(&self) -> Option<[[f32; 3]; 4]> {
        self.as_struct()?.transform()
    }
}

#[derive(Clone, Copy)]
pub struct Instance<'a> {
    partition: &'a Partition,
    index: u32,
}

impl<'a> Instance<'a> {
    pub(crate) fn new(partition: &'a Partition, index: u32) -> Instance<'a> {
        Instance { partition, index }
    }

    pub(crate) fn complex(&self) -> u32 {
        self.partition.instances[self.index as usize].complex
    }

    pub fn partition(&self) -> &'a Partition {
        self.partition
    }

    pub fn index(&self) -> u32 {
        self.index
    }

    pub fn guid(&self) -> Guid {
        self.partition.instances[self.index as usize].guid
    }

    pub fn is_primary(&self) -> bool {
        self.guid() == self.partition.primary_guid()
    }

    pub fn data(&self) -> Struct<'a> {
        let entry = &self.partition.instances[self.index as usize];
        Struct {
            partition: self.partition,
            complex: entry.complex,
            offset: entry.offset,
        }
    }

    pub fn type_name(&self) -> &'a str {
        self.partition.complex_name(self.complex())
    }

    /// The instance's type first, then each base type up to the root.
    pub fn type_chain(&self) -> TypeChain<'a> {
        self.data().type_chain()
    }

    pub fn is_a(&self, type_name: &str) -> bool {
        self.data().is_a(type_name)
    }

    pub fn get(&self, field: &str) -> Option<Value<'a>> {
        self.data().get(field)
    }

    pub fn get_path(&self, path: &str) -> Option<Value<'a>> {
        Value::Struct(self.data()).get_path(path)
    }

    /// Every field, base types first, in declaration order.
    pub fn fields(&self) -> Fields<'a> {
        self.data().fields()
    }
}

impl<'a> From<Instance<'a>> for Struct<'a> {
    fn from(instance: Instance<'a>) -> Struct<'a> {
        instance.data()
    }
}

impl PartialEq for Instance<'_> {
    fn eq(&self, other: &Self) -> bool {
        std::ptr::eq(self.partition, other.partition) && self.index == other.index
    }
}

impl Eq for Instance<'_> {}

impl fmt::Debug for Instance<'_> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{} ", self.guid())?;
        fmt::Debug::fmt(&self.data(), f)
    }
}

#[derive(Clone, Copy)]
pub struct Struct<'a> {
    partition: &'a Partition,
    complex: u32,
    offset: u32,
}

impl PartialEq for Struct<'_> {
    fn eq(&self, other: &Self) -> bool {
        std::ptr::eq(self.partition, other.partition)
            && self.complex == other.complex
            && self.offset == other.offset
    }
}

impl<'a> Struct<'a> {
    pub fn partition(&self) -> &'a Partition {
        self.partition
    }

    pub fn type_name(&self) -> &'a str {
        self.partition.complex_name(self.complex)
    }

    pub fn type_chain(&self) -> TypeChain<'a> {
        TypeChain {
            partition: self.partition,
            next: Some(self.complex),
            remaining: BASE_DEPTH,
        }
    }

    pub fn is_a(&self, type_name: &str) -> bool {
        self.partition
            .derives_from(self.complex, name_hash(type_name), type_name)
    }

    /// The most derived field of that name.
    pub fn get(&self, field: &str) -> Option<Value<'a>> {
        let hash = name_hash(field);
        let partition = self.partition;
        let mut complex = self.complex;
        let mut at = self.offset as usize;
        for _ in 0..BASE_DEPTH {
            let desc = partition.complexes.get(complex as usize)?;
            let mut base = None;
            for index in desc.fields.clone() {
                let member = &partition.fields[index as usize];
                let member_at = at + member.offset as usize;
                if member.code == VOID {
                    base = Some((u32::from(member.reference), member_at));
                } else if member.hash == hash && partition.name(member.name) == field {
                    return decode(partition, index, member_at);
                }
            }
            (complex, at) = base?;
        }
        None
    }

    pub fn get_path(&self, path: &str) -> Option<Value<'a>> {
        Value::Struct(*self).get_path(path)
    }

    pub fn fields(&self) -> Fields<'a> {
        let mut chain = [(0u32, 0u32); BASE_DEPTH];
        let mut depth = 0;
        let mut next = Some((self.complex, self.offset));
        while let Some((complex, at)) = next {
            if depth == BASE_DEPTH {
                break;
            }
            chain[depth] = (complex, at);
            depth += 1;
            next = self.partition.complexes[complex as usize]
                .fields
                .clone()
                .map(|index| &self.partition.fields[index as usize])
                .find(|member| member.code == VOID)
                .map(|member| (u32::from(member.reference), at + member.offset as u32));
        }
        Fields {
            partition: self.partition,
            chain,
            depth,
            current: 0..0,
            at: 0,
        }
    }

    pub fn vec3(&self) -> Option<[f32; 3]> {
        Some([
            self.get("x")?.as_f32()?,
            self.get("y")?.as_f32()?,
            self.get("z")?.as_f32()?,
        ])
    }

    /// A `LinearTransform` as its rows `right`, `up`, `forward`, `trans`.
    pub fn transform(&self) -> Option<[[f32; 3]; 4]> {
        Some([
            self.get("right")?.as_vec3()?,
            self.get("up")?.as_vec3()?,
            self.get("forward")?.as_vec3()?,
            self.get("trans")?.as_vec3()?,
        ])
    }
}

impl fmt::Debug for Struct<'_> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let mut out = f.debug_struct(self.type_name());
        for (name, value) in self.fields() {
            out.field(name, &value);
        }
        out.finish()
    }
}

pub struct TypeChain<'a> {
    partition: &'a Partition,
    next: Option<u32>,
    remaining: usize,
}

impl<'a> Iterator for TypeChain<'a> {
    type Item = &'a str;

    fn next(&mut self) -> Option<&'a str> {
        let complex = self.next?;
        self.remaining = self.remaining.checked_sub(1)?;
        let desc = self.partition.complexes.get(complex as usize)?;
        self.next = desc.base;
        Some(self.partition.name(desc.name))
    }
}

pub struct Fields<'a> {
    partition: &'a Partition,
    chain: [(u32, u32); BASE_DEPTH],
    depth: usize,
    current: std::ops::Range<u32>,
    at: u32,
}

impl<'a> Iterator for Fields<'a> {
    type Item = (&'a str, Value<'a>);

    fn next(&mut self) -> Option<(&'a str, Value<'a>)> {
        loop {
            if let Some(index) = self.current.next() {
                let member = &self.partition.fields[index as usize];
                if member.code == VOID {
                    continue;
                }
                let at = self.at as usize + member.offset as usize;
                if let Some(value) = decode(self.partition, index, at) {
                    return Some((self.partition.name(member.name), value));
                }
                continue;
            }
            self.depth = self.depth.checked_sub(1)?;
            let (complex, at) = self.chain[self.depth];
            self.current = self.partition.complexes[complex as usize].fields.clone();
            self.at = at;
        }
    }
}

#[derive(Clone, Copy)]
pub struct Array<'a> {
    partition: &'a Partition,
    member: u32,
    start: u32,
    len: u32,
    stride: u32,
}

impl PartialEq for Array<'_> {
    fn eq(&self, other: &Self) -> bool {
        std::ptr::eq(self.partition, other.partition)
            && self.member == other.member
            && self.start == other.start
            && self.len == other.len
    }
}

impl<'a> Array<'a> {
    pub fn len(&self) -> usize {
        self.len as usize
    }

    pub fn is_empty(&self) -> bool {
        self.len == 0
    }

    pub fn get(&self, index: usize) -> Option<Value<'a>> {
        if index >= self.len as usize {
            return None;
        }
        let at = self.start as usize + index * self.stride as usize;
        decode(self.partition, self.member, at)
    }

    pub fn iter(&self) -> ArrayIter<'a> {
        ArrayIter {
            array: *self,
            next: 0,
        }
    }
}

impl<'a> IntoIterator for Array<'a> {
    type Item = Value<'a>;
    type IntoIter = ArrayIter<'a>;

    fn into_iter(self) -> ArrayIter<'a> {
        self.iter()
    }
}

impl fmt::Debug for Array<'_> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_list().entries(self.iter()).finish()
    }
}

pub struct ArrayIter<'a> {
    array: Array<'a>,
    next: usize,
}

impl<'a> Iterator for ArrayIter<'a> {
    type Item = Value<'a>;

    fn next(&mut self) -> Option<Value<'a>> {
        let value = self.array.get(self.next)?;
        self.next += 1;
        Some(value)
    }

    fn size_hint(&self) -> (usize, Option<usize>) {
        let left = self.array.len().saturating_sub(self.next);
        (left, Some(left))
    }
}

impl ExactSizeIterator for ArrayIter<'_> {}

fn decode(partition: &Partition, field: u32, at: usize) -> Option<Value<'_>> {
    let member = partition.fields.get(field as usize)?;
    let bytes = &partition.bytes[..];
    let byte = || bytes.get(at).copied();
    Some(match member.code {
        VALUE_TYPE => Value::Struct(Struct {
            partition,
            complex: u32::from(member.reference),
            offset: at as u32,
        }),
        CLASS => Value::Ref(partition.reference(le32(bytes, at)?)?),
        ARRAY => {
            let entry = partition.arrays.get(le32(bytes, at)? as usize)?;
            let element = partition
                .complexes
                .get(entry.complex as usize)?
                .fields
                .start;
            Value::Array(Array {
                partition,
                member: element,
                start: entry.start,
                len: entry.len,
                stride: partition.stride(element),
            })
        }
        CSTRING | FILE_REF => Value::String(partition.string(le32(bytes, at)?)?),
        ENUM => {
            let value = le32(bytes, at)? as i32;
            Value::Enum {
                value,
                name: partition.enumerator(u32::from(member.reference), value),
            }
        }
        BOOLEAN => Value::Bool(byte()? != 0),
        INT8 => Value::I8(byte()? as i8),
        UINT8 => Value::U8(byte()?),
        INT16 => Value::I16(le16(bytes, at)? as i16),
        UINT16 => Value::U16(le16(bytes, at)?),
        INT32 => Value::I32(le32(bytes, at)? as i32),
        UINT32 => Value::U32(le32(bytes, at)?),
        INT64 => Value::I64(le64(bytes, at)? as i64),
        UINT64 => Value::U64(le64(bytes, at)?),
        FLOAT32 => Value::F32(f32::from_bits(le32(bytes, at)?)),
        FLOAT64 => Value::F64(f64::from_bits(le64(bytes, at)?)),
        GUID => Value::Guid(Guid::read(bytes, at)?),
        SHA1 => Value::Sha1(bytes.get(at..at + 20)?.try_into().ok()?),
        _ => return None,
    })
}
