#pragma once

#include <string>

namespace bf3dump {

std::string rust_f32_digits(float p_value);

std::string json_f64_text(double p_value);

std::string json_f32_text(float p_value);

int json_number_selfcheck();

}
