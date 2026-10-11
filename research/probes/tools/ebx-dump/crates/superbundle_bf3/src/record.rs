use crate::dbobject::DbValue;
use crate::error::Error;
use crate::ids::{Guid, Sha1};

#[derive(Clone, Copy)]
pub(crate) struct Record<'v, 'a> {
    what: &'static str,
    value: &'v DbValue<'a>,
}

impl<'v, 'a> Record<'v, 'a> {
    pub(crate) fn new(what: &'static str, value: &'v DbValue<'a>) -> Result<Self, Error> {
        if value.as_object().is_none() {
            return Err(Error::Layout {
                what,
                problem: "is not a DbObject object",
            });
        }
        Ok(Self { what, value })
    }

    pub(crate) fn field(&self, field: &'static str) -> Option<&'v DbValue<'a>> {
        self.value.get(field)
    }

    fn required(&self, field: &'static str) -> Result<&'v DbValue<'a>, Error> {
        self.field(field).ok_or(Error::MissingField {
            what: self.what,
            field,
        })
    }

    fn wrong(&self, field: &'static str) -> Error {
        Error::FieldType {
            what: self.what,
            field,
        }
    }

    pub(crate) fn str(&self, field: &'static str) -> Result<&'a str, Error> {
        self.required(field)?
            .as_str()
            .ok_or_else(|| self.wrong(field))
    }

    pub(crate) fn opt_str(&self, field: &'static str) -> Result<Option<&'a str>, Error> {
        self.field(field)
            .map(|value| value.as_str().ok_or_else(|| self.wrong(field)))
            .transpose()
    }

    pub(crate) fn opt_i64(&self, field: &'static str) -> Result<Option<i64>, Error> {
        self.field(field)
            .map(|value| value.as_i64().ok_or_else(|| self.wrong(field)))
            .transpose()
    }

    pub(crate) fn u64(&self, field: &'static str) -> Result<u64, Error> {
        let value = self.opt_i64(field)?.ok_or(Error::MissingField {
            what: self.what,
            field,
        })?;
        self.non_negative(field, value)
    }

    pub(crate) fn opt_u64(&self, field: &'static str) -> Result<Option<u64>, Error> {
        self.opt_i64(field)?
            .map(|value| self.non_negative(field, value))
            .transpose()
    }

    pub(crate) fn opt_u32(&self, field: &'static str) -> Result<Option<u32>, Error> {
        self.opt_i64(field)?
            .map(|value| {
                u32::try_from(value).map_err(|_| Error::FieldRange {
                    what: self.what,
                    field,
                    value,
                })
            })
            .transpose()
    }

    pub(crate) fn hash32(&self, field: &'static str) -> Result<Option<u32>, Error> {
        self.opt_i64(field)?
            .map(|value| {
                i32::try_from(value)
                    .map(|value| value as u32)
                    .or_else(|_| u32::try_from(value))
                    .map_err(|_| Error::FieldRange {
                        what: self.what,
                        field,
                        value,
                    })
            })
            .transpose()
    }

    fn non_negative(&self, field: &'static str, value: i64) -> Result<u64, Error> {
        u64::try_from(value).map_err(|_| Error::FieldRange {
            what: self.what,
            field,
            value,
        })
    }

    pub(crate) fn flag(&self, field: &'static str) -> Result<bool, Error> {
        match self.field(field) {
            None => Ok(false),
            Some(value) => value.as_bool().ok_or_else(|| self.wrong(field)),
        }
    }

    pub(crate) fn guid(&self, field: &'static str) -> Result<Guid, Error> {
        self.required(field)?
            .as_guid()
            .ok_or_else(|| self.wrong(field))
    }

    pub(crate) fn sha1(&self, field: &'static str) -> Result<Sha1, Error> {
        self.required(field)?
            .as_sha1()
            .ok_or_else(|| self.wrong(field))
    }

    pub(crate) fn opt_sha1(&self, field: &'static str) -> Result<Option<Sha1>, Error> {
        self.field(field)
            .map(|value| value.as_sha1().ok_or_else(|| self.wrong(field)))
            .transpose()
    }

    pub(crate) fn opt_blob(&self, field: &'static str) -> Result<Option<&'a [u8]>, Error> {
        self.field(field)
            .map(|value| value.as_blob().ok_or_else(|| self.wrong(field)))
            .transpose()
    }

    pub(crate) fn list(&self, field: &'static str) -> Result<&'v [DbValue<'a>], Error> {
        match self.field(field) {
            None => Ok(&[]),
            Some(value) => value.as_list().ok_or_else(|| self.wrong(field)),
        }
    }
}
