#pragma once
// JSON number text exactly as the ebx-dump probe (Rust) writes it, so bf3dump output can be
// compared byte for byte:
// - an f32 is printed by Rust's Display (shortest round-trip digits; on an exact tie between two
//   equally short candidates the larger magnitude wins), read back as f64 and written by
//   serde_json 1.0.151, which formats f64 with zmij 1.0.23;
// - zmij writes the shortest round-trip digits, fixed notation for decimal exponents -5..=15
//   ("123.0", "0.00001"), otherwise "d.ddde+N" / "d.ddde-N" with an unpadded exponent.
// Non-finite values are the caller's business (the probe writes them as strings).

#include <string>

namespace bf3dump {

// Rust `format!("{}", f32)` digits in scientific form ("1.5820313e0"), finite values only.
std::string rust_f32_digits(float p_value);

// serde_json (zmij) text of a finite f64.
std::string json_f64_text(double p_value);

// The probe's text of a finite f32: json_f64_text(rust_f32_digits(p_value) parsed as f64).
std::string json_f32_text(float p_value);

// Synthetic cases for the two rules above (no install needed). Prints failures to stderr and
// returns the number of failed cases.
int json_number_selfcheck();

} // namespace bf3dump
