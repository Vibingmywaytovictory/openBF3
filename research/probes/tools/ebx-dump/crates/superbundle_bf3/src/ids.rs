use std::fmt;

#[derive(Clone, Copy, Default, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct Guid(pub [u8; 16]);

impl Guid {
    pub fn from_big_endian_fields(bytes: [u8; 16]) -> Self {
        let mut out = bytes;
        out[0..4].reverse();
        out[4..6].reverse();
        out[6..8].reverse();
        Self(out)
    }

    pub fn from_slice(bytes: &[u8]) -> Option<Self> {
        bytes.get(..16)?.try_into().ok().map(Self)
    }

    pub fn is_zero(&self) -> bool {
        self.0 == [0; 16]
    }

    pub fn parse(text: &str) -> Option<Self> {
        let text = text.trim().trim_start_matches('{').trim_end_matches('}');
        let bytes = text.as_bytes();
        if bytes.len() != 36 || [8, 13, 18, 23].iter().any(|&at| bytes[at] != b'-') {
            return None;
        }
        let digits = bytes
            .iter()
            .copied()
            .filter(|&b| b != b'-')
            .collect::<Vec<_>>();
        if digits.len() != 32 {
            return None;
        }
        let mut big = [0u8; 16];
        for (slot, pair) in big.iter_mut().zip(digits.chunks_exact(2)) {
            *slot = (hex_value(pair[0])? << 4) | hex_value(pair[1])?;
        }
        Some(Self::from_big_endian_fields(big))
    }
}

impl fmt::Display for Guid {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let b = &self.0;
        write!(
            f,
            "{:08x}-{:04x}-{:04x}-{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
            u32::from_le_bytes([b[0], b[1], b[2], b[3]]),
            u16::from_le_bytes([b[4], b[5]]),
            u16::from_le_bytes([b[6], b[7]]),
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
        write!(f, "Guid({self})")
    }
}

#[derive(Clone, Copy, Default, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct Sha1(pub [u8; 20]);

impl Sha1 {
    pub fn from_slice(bytes: &[u8]) -> Option<Self> {
        bytes.get(..20)?.try_into().ok().map(Self)
    }

    pub fn parse(text: &str) -> Option<Self> {
        let text = text.trim().as_bytes();
        if text.len() != 40 {
            return None;
        }
        let mut out = [0u8; 20];
        for (slot, pair) in out.iter_mut().zip(text.chunks_exact(2)) {
            *slot = (hex_value(pair[0])? << 4) | hex_value(pair[1])?;
        }
        Some(Self(out))
    }
}

impl fmt::Display for Sha1 {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        for byte in self.0 {
            write!(f, "{byte:02x}")?;
        }
        Ok(())
    }
}

impl fmt::Debug for Sha1 {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "Sha1({self})")
    }
}

fn hex_value(digit: u8) -> Option<u8> {
    match digit {
        b'0'..=b'9' => Some(digit - b'0'),
        b'a'..=b'f' => Some(digit - b'a' + 10),
        b'A'..=b'F' => Some(digit - b'A' + 10),
        _ => None,
    }
}

pub fn asset_name_hash(name: &str) -> u32 {
    name.bytes().fold(5381u32, |hash, byte| {
        hash.wrapping_mul(33) ^ u32::from(byte.to_ascii_lowercase())
    })
}
