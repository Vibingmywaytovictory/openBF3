use crate::error::{Error, need};

pub const OBFUSCATION_HEADER_LEN: usize = 0x22C;

const KEY_AT: usize = 0x128;

const KEY_LEN: usize = 0x101;

const KEY_SEED: u8 = 0x7B;

const PLAIN_KEY_MARK: &[u8; 16] = b"2e33ae4783feead2";

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Obfuscation {
    Plain,
    Xor,
    SignedXor,
    Signed,
}

pub fn obfuscation_of(raw: &[u8]) -> Result<Obfuscation, Error> {
    if raw.len() < 4 || raw[0..3] != [0x00, 0xD1, 0xCE] {
        return Ok(Obfuscation::Plain);
    }
    match raw[3] {
        0x00 => Ok(Obfuscation::Xor),
        0x01 => Ok(Obfuscation::SignedXor),
        0x03 => Ok(Obfuscation::Signed),
        _ => Err(Error::BadMagic {
            what: "obfuscation header",
            got: u32::from_le_bytes([raw[0], raw[1], raw[2], raw[3]]),
        }),
    }
}

pub fn deobfuscate(mut raw: Vec<u8>) -> Result<Vec<u8>, Error> {
    let kind = obfuscation_of(&raw)?;
    if kind == Obfuscation::Plain {
        return Ok(raw);
    }
    need("obfuscation header", &raw, OBFUSCATION_HEADER_LEN)?;
    let mut key = [0u8; KEY_LEN];
    for (slot, raw_byte) in key.iter_mut().zip(&raw[KEY_AT..KEY_AT + KEY_LEN]) {
        *slot = raw_byte ^ KEY_SEED;
    }
    let xored = kind != Obfuscation::Signed
        && &raw[KEY_AT + 0x10..KEY_AT + 0x20] != PLAIN_KEY_MARK
        && key.iter().any(|&byte| byte != 0);
    raw.drain(..OBFUSCATION_HEADER_LEN);
    if xored {
        for block in raw.chunks_mut(KEY_LEN) {
            for (byte, key_byte) in block.iter_mut().zip(key) {
                *byte ^= key_byte;
            }
        }
    }
    Ok(raw)
}
