#pragma once

#include <cctype>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bf3::tools::json {

struct Value {
	enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
	bool b = false;
	double num = 0.0;
	std::string str;
	std::vector<Value> items;
	std::vector<std::pair<std::string, Value>> members;

	const Value *get(std::string_view p_key) const {
		for (const auto &m : members) {
			if (m.first == p_key) {
				return &m.second;
			}
		}
		return nullptr;
	}
	double number_or(std::string_view p_key, double p_default) const {
		const Value *v = get(p_key);
		return v && v->type == Type::Number ? v->num : p_default;
	}
	std::string string_or(std::string_view p_key, std::string p_default) const {
		const Value *v = get(p_key);
		return v && v->type == Type::String ? v->str : p_default;
	}
};

class Parser {
public:
	explicit Parser(std::string_view p_text) : t_(p_text) {}

	bool parse(Value &r_out) {
		return value(r_out, 0) && (ws(), i_ == t_.size());
	}

private:
	void ws() {
		while (i_ < t_.size() && (t_[i_] == ' ' || t_[i_] == '\n' || t_[i_] == '\r' || t_[i_] == '\t')) {
			++i_;
		}
	}
	bool value(Value &r_out, int p_depth) {
		if (p_depth > 64) {
			return false;
		}
		ws();
		if (i_ >= t_.size()) {
			return false;
		}
		const char c = t_[i_];
		if (c == '{') {
			r_out.type = Value::Type::Object;
			++i_;
			ws();
			if (i_ < t_.size() && t_[i_] == '}') {
				++i_;
				return true;
			}
			while (true) {
				ws();
				std::string key;
				if (!string(key)) {
					return false;
				}
				ws();
				if (i_ >= t_.size() || t_[i_] != ':') {
					return false;
				}
				++i_;
				Value v;
				if (!value(v, p_depth + 1)) {
					return false;
				}
				r_out.members.emplace_back(std::move(key), std::move(v));
				ws();
				if (i_ < t_.size() && t_[i_] == ',') {
					++i_;
					continue;
				}
				if (i_ < t_.size() && t_[i_] == '}') {
					++i_;
					return true;
				}
				return false;
			}
		}
		if (c == '[') {
			r_out.type = Value::Type::Array;
			++i_;
			ws();
			if (i_ < t_.size() && t_[i_] == ']') {
				++i_;
				return true;
			}
			while (true) {
				Value v;
				if (!value(v, p_depth + 1)) {
					return false;
				}
				r_out.items.push_back(std::move(v));
				ws();
				if (i_ < t_.size() && t_[i_] == ',') {
					++i_;
					continue;
				}
				if (i_ < t_.size() && t_[i_] == ']') {
					++i_;
					return true;
				}
				return false;
			}
		}
		if (c == '"') {
			r_out.type = Value::Type::String;
			return string(r_out.str);
		}
		if (t_.substr(i_, 4) == "true" || t_.substr(i_, 5) == "false") {
			r_out.type = Value::Type::Bool;
			r_out.b = t_[i_] == 't';
			i_ += r_out.b ? 4 : 5;
			return true;
		}
		if (t_.substr(i_, 4) == "null") {
			i_ += 4;
			return true;
		}

		const size_t start = i_;
		while (i_ < t_.size() && (std::isdigit(static_cast<unsigned char>(t_[i_])) || t_[i_] == '-' || t_[i_] == '+' || t_[i_] == '.' || t_[i_] == 'e' || t_[i_] == 'E')) {
			++i_;
		}
		if (i_ == start) {
			return false;
		}
		const std::string num(t_.substr(start, i_ - start));
		char *end = nullptr;
		r_out.type = Value::Type::Number;
		r_out.num = std::strtod(num.c_str(), &end);
		return end != nullptr && *end == '\0';
	}
	bool string(std::string &r_out) {
		if (i_ >= t_.size() || t_[i_] != '"') {
			return false;
		}
		++i_;
		while (i_ < t_.size() && t_[i_] != '"') {
			if (t_[i_] == '\\') {
				if (++i_ >= t_.size()) {
					return false;
				}
				const char e = t_[i_];
				r_out.push_back(e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e);
				if (e == 'u') {
					r_out.pop_back();
					r_out += "\\u";
				}
				++i_;
				continue;
			}
			r_out.push_back(t_[i_++]);
		}
		if (i_ >= t_.size()) {
			return false;
		}
		++i_;
		return true;
	}

	std::string_view t_;
	size_t i_ = 0;
};

inline bool parse(std::string_view p_text, Value &r_out) {
	Parser p(p_text);
	return p.parse(r_out);
}

}
