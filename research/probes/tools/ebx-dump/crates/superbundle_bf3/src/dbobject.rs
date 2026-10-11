use crate::error::Error;
use crate::ids::{Guid, Sha1};

const MAX_DEPTH: usize = 64;

const TAG_TYPE_MASK: u8 = 0x1F;

const TAG_ANONYMOUS: u8 = 0x80;

#[derive(Clone, Debug, PartialEq)]
pub enum DbValue<'a> {
    Object(Vec<(&'a str, DbValue<'a>)>),
    List(Vec<DbValue<'a>>),
    Null,
    Bool(bool),
    String(&'a str),
    Int(i32),
    Long(i64),
    VarInt(i64),
    Float(f32),
    Double(f64),
    Timestamp(i64),
    RecordId([u8; 6]),
    Guid(Guid),
    Sha1(Sha1),
    Matrix44([f32; 16]),
    Blob(&'a [u8]),
    Attachment(Sha1),
    Timespan(i64),
}

impl<'a> DbValue<'a> {
    pub fn get(&self, key: &str) -> Option<&DbValue<'a>> {
        self.as_object()?
            .iter()
            .find(|(name, _)| name.eq_ignore_ascii_case(key))
            .map(|(_, value)| value)
    }

    pub fn as_object(&self) -> Option<&[(&'a str, DbValue<'a>)]> {
        match self {
            Self::Object(members) => Some(members),
            _ => None,
        }
    }

    pub fn as_list(&self) -> Option<&[DbValue<'a>]> {
        match self {
            Self::List(items) => Some(items),
            _ => None,
        }
    }

    pub fn as_bool(&self) -> Option<bool> {
        match *self {
            Self::Bool(value) => Some(value),
            _ => None,
        }
    }

    pub fn as_str(&self) -> Option<&'a str> {
        match *self {
            Self::String(value) => Some(value),
            _ => None,
        }
    }

    pub fn as_i64(&self) -> Option<i64> {
        match *self {
            Self::Int(value) => Some(i64::from(value)),
            Self::Long(value) | Self::VarInt(value) => Some(value),
            _ => None,
        }
    }

    pub fn as_guid(&self) -> Option<Guid> {
        match *self {
            Self::Guid(value) => Some(value),
            _ => None,
        }
    }

    pub fn as_sha1(&self) -> Option<Sha1> {
        match *self {
            Self::Sha1(value) => Some(value),
            _ => None,
        }
    }

    pub fn as_blob(&self) -> Option<&'a [u8]> {
        match *self {
            Self::Blob(value) => Some(value),
            _ => None,
        }
    }
}

#[derive(Clone, Debug, PartialEq)]
pub struct DbElement<'a> {
    pub name: Option<&'a str>,
    pub value: DbValue<'a>,
}

pub fn parse_db_object(bytes: &[u8]) -> Result<DbValue<'_>, Error> {
    let (element, end) = read_db_element(bytes, 0)?;
    if end != bytes.len() {
        return Err(Error::BadDbObject {
            at: end,
            problem: "bytes follow the top-level element",
        });
    }
    Ok(element.value)
}

pub fn read_db_element(bytes: &[u8], at: usize) -> Result<(DbElement<'_>, usize), Error> {
    let mut reader = Reader { bytes, at };
    match reader.element(0)? {
        Some(element) => Ok((element, reader.at)),
        None => Err(Error::BadDbObject {
            at,
            problem: "element is an end marker",
        }),
    }
}

struct Reader<'a> {
    bytes: &'a [u8],
    at: usize,
}

impl<'a> Reader<'a> {
    fn element(&mut self, depth: usize) -> Result<Option<DbElement<'a>>, Error> {
        let tag_at = self.at;
        let tag = self.byte()?;
        let kind = tag & TAG_TYPE_MASK;
        if kind == 0 {
            return Ok(None);
        }
        let name = if tag & TAG_ANONYMOUS == 0 {
            Some(self.cstr()?)
        } else {
            None
        };
        let value = match kind {
            0x01..=0x03 => self.container(kind, depth)?,
            0x04 => DbValue::Null,
            0x06 => DbValue::Bool(self.byte()? != 0),
            0x07 => {
                let len = self.length()?;
                let raw = self.take(len)?;
                let end = raw.iter().rposition(|&b| b != 0).map_or(0, |last| last + 1);
                DbValue::String(
                    std::str::from_utf8(&raw[..end]).map_err(|_| Error::BadUtf8 {
                        what: "DbObject string",
                    })?,
                )
            }
            0x08 => DbValue::Int(i32::from_le_bytes(self.array()?)),
            0x09 => DbValue::Long(i64::from_le_bytes(self.array()?)),
            0x0A => DbValue::VarInt(zigzag(self.uleb()?)),
            0x0B => DbValue::Float(f32::from_le_bytes(self.array()?)),
            0x0C => DbValue::Double(f64::from_le_bytes(self.array()?)),
            0x0D => DbValue::Timestamp(i64::from_le_bytes(self.array()?)),
            0x0E => DbValue::RecordId(self.array()?),
            0x0F => DbValue::Guid(Guid(self.array()?)),
            0x10 => DbValue::Sha1(Sha1(self.array()?)),
            0x11 => {
                let raw: [u8; 64] = self.array()?;
                let mut matrix = [0f32; 16];
                for (slot, word) in matrix.iter_mut().zip(raw.chunks_exact(4)) {
                    *slot = f32::from_le_bytes([word[0], word[1], word[2], word[3]]);
                }
                DbValue::Matrix44(matrix)
            }
            0x13 => {
                let len = self.length()?;
                DbValue::Blob(self.take(len)?)
            }
            0x14 => DbValue::Attachment(Sha1(self.array()?)),
            0x15 => DbValue::Timespan(zigzag(self.uleb()?)),
            _ => return Err(Error::UnknownDbTag { tag, at: tag_at }),
        };
        Ok(Some(DbElement { name, value }))
    }

    fn container(&mut self, kind: u8, depth: usize) -> Result<DbValue<'a>, Error> {
        if depth >= MAX_DEPTH {
            return Err(Error::BadDbObject {
                at: self.at,
                problem: "nesting is too deep",
            });
        }
        let len = self.length()?;
        let end = self.at + len;
        if end > self.bytes.len() {
            return Err(Error::Truncated {
                what: "DbObject container",
                need: end as u64,
                have: self.bytes.len() as u64,
            });
        }
        let mut members = Vec::new();
        let mut items = Vec::new();
        let mut terminated = false;
        while self.at < end {
            match self.element(depth + 1)? {
                None => {
                    terminated = true;
                    break;
                }
                Some(element) if kind == 0x02 => {
                    members.push((element.name.unwrap_or(""), element.value));
                }
                Some(element) => items.push(element.value),
            }
        }
        if !terminated || self.at != end {
            return Err(Error::BadDbObject {
                at: self.at,
                problem: "container does not end with its terminator",
            });
        }
        Ok(if kind == 0x02 {
            DbValue::Object(members)
        } else {
            DbValue::List(items)
        })
    }

    fn byte(&mut self) -> Result<u8, Error> {
        let byte = *self.bytes.get(self.at).ok_or(Error::Truncated {
            what: "DbObject",
            need: self.at as u64 + 1,
            have: self.bytes.len() as u64,
        })?;
        self.at += 1;
        Ok(byte)
    }

    fn take(&mut self, len: usize) -> Result<&'a [u8], Error> {
        let end = self
            .at
            .checked_add(len)
            .filter(|&end| end <= self.bytes.len());
        let Some(end) = end else {
            return Err(Error::Truncated {
                what: "DbObject value",
                need: self.at as u64 + len as u64,
                have: self.bytes.len() as u64,
            });
        };
        let slice = &self.bytes[self.at..end];
        self.at = end;
        Ok(slice)
    }

    fn array<const N: usize>(&mut self) -> Result<[u8; N], Error> {
        let slice = self.take(N)?;
        let mut out = [0u8; N];
        out.copy_from_slice(slice);
        Ok(out)
    }

    fn cstr(&mut self) -> Result<&'a str, Error> {
        let rest = &self.bytes[self.at..];
        let Some(len) = rest.iter().position(|&b| b == 0) else {
            return Err(Error::BadDbObject {
                at: self.at,
                problem: "name has no terminator",
            });
        };
        let name = std::str::from_utf8(&rest[..len]).map_err(|_| Error::BadUtf8 {
            what: "DbObject name",
        })?;
        self.at += len + 1;
        Ok(name)
    }

    fn uleb(&mut self) -> Result<u64, Error> {
        let start = self.at;
        let mut value = 0u64;
        for shift in (0..64).step_by(7) {
            let byte = self.byte()?;
            value |= u64::from(byte & 0x7F) << shift;
            if byte & 0x80 == 0 {
                return Ok(value);
            }
        }
        Err(Error::BadDbObject {
            at: start,
            problem: "varint is longer than 64 bits",
        })
    }

    fn length(&mut self) -> Result<usize, Error> {
        let at = self.at;
        let len = self.uleb()?;
        usize::try_from(len)
            .ok()
            .filter(|&len| len <= self.bytes.len())
            .ok_or(Error::BadDbObject {
                at,
                problem: "length exceeds the buffer",
            })
    }
}

fn zigzag(value: u64) -> i64 {
    (value >> 1) as i64 ^ -((value & 1) as i64)
}
