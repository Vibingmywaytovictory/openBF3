use std::fmt;
use std::str::FromStr;

/// The 16 bytes as stored. Text form is the Microsoft layout: the first three groups are
/// little-endian, so a partition GUID prints like `88d61a88-ecc8-11df-b39c-fd673456481d`.
#[derive(Clone, Copy, Default, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct Guid(pub [u8; 16]);

impl Guid {
    pub const ZERO: Guid = Guid([0; 16]);

    pub fn is_zero(&self) -> bool {
        self.0 == [0; 16]
    }

    pub(crate) fn read(bytes: &[u8], at: usize) -> Option<Guid> {
        Some(Guid(bytes.get(at..at + 16)?.try_into().ok()?))
    }
}

impl fmt::Display for Guid {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let b = &self.0;
        write!(
            f,
            "{:02x}{:02x}{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
            b[3],
            b[2],
            b[1],
            b[0],
            b[5],
            b[4],
            b[7],
            b[6],
            b[8],
            b[9],
            b[10],
            b[11],
            b[12],
            b[13],
            b[14],
            b[15]
        )
    }
}

impl fmt::Debug for Guid {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        fmt::Display::fmt(self, f)
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ParseGuidError;

impl fmt::Display for ParseGuidError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str("expected a GUID of the form xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx")
    }
}

impl std::error::Error for ParseGuidError {}

impl FromStr for Guid {
    type Err = ParseGuidError;

    fn from_str(text: &str) -> Result<Guid, ParseGuidError> {
        let text = text.trim_start_matches('{').trim_end_matches('}');
        let groups: Vec<&str> = text.split('-').collect();
        let lengths = [8, 4, 4, 4, 12];
        if groups.len() != lengths.len() || groups.iter().zip(lengths).any(|(g, n)| g.len() != n) {
            return Err(ParseGuidError);
        }
        let hex: String = groups.concat();
        if !hex.bytes().all(|b| b.is_ascii_hexdigit()) {
            return Err(ParseGuidError);
        }
        let mut text_order = [0u8; 16];
        for (i, byte) in text_order.iter_mut().enumerate() {
            let pair = hex.get(i * 2..i * 2 + 2).ok_or(ParseGuidError)?;
            *byte = u8::from_str_radix(pair, 16).map_err(|_| ParseGuidError)?;
        }
        let t = text_order;
        Ok(Guid([
            t[3], t[2], t[1], t[0], t[5], t[4], t[7], t[6], t[8], t[9], t[10], t[11], t[12], t[13],
            t[14], t[15],
        ]))
    }
}
