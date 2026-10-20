#pragma once

#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace bf3::core {

struct Error {
	std::string message;
};

template <class T>
class [[nodiscard]] Result {
public:
	Result(T p_value) :
			v_(std::in_place_index<0>, std::move(p_value)) {}
	Result(Error p_error) :
			v_(std::in_place_index<1>, std::move(p_error)) {}

	bool ok() const { return v_.index() == 0; }
	explicit operator bool() const { return ok(); }

	T &value() { return *std::get_if<0>(&v_); }
	const T &value() const { return *std::get_if<0>(&v_); }
	T &operator*() { return value(); }
	const T &operator*() const { return value(); }
	T *operator->() { return &value(); }
	const T *operator->() const { return &value(); }
	const Error &error() const { return *std::get_if<1>(&v_); }

private:
	std::variant<T, Error> v_;
};

class [[nodiscard]] Status {
public:
	Status() = default;
	Status(Error p_error) :
			error_(std::move(p_error)) {}
	static Status success() { return {}; }

	bool ok() const { return !error_.has_value(); }
	explicit operator bool() const { return ok(); }
	const Error &error() const { return *error_; }

private:
	std::optional<Error> error_;
};

inline Error make_error(std::string p_message) {
	return Error{ std::move(p_message) };
}

inline Error error_at(const std::string &p_where, const Error &p_error) {
	return Error{ p_where + ": " + p_error.message };
}

}

#define BF3_TRY(expr) \
	do { \
		auto &&bf3_try_result_ = (expr); \
		if (!bf3_try_result_.ok()) { \
			return bf3_try_result_.error(); \
		} \
	} while (0)
