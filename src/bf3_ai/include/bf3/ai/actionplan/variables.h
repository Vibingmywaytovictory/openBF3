#pragma once

#include <cstddef>
#include <list>
#include <memory>

namespace bf3::ai {

class BotScriptVariable {
public:
	virtual ~BotScriptVariable() = default;
};

template <class T> class BAPVariable final : public BotScriptVariable {
public:
	explicit BAPVariable(const T &p_value) : value(p_value) {}

	T value;
};

class BAPContext {
public:
	void add_variable(std::unique_ptr<BotScriptVariable> p_variable);
	template <class T> BAPVariable<T> &add(const T &p_value) {
		auto variable = std::make_unique<BAPVariable<T>>(p_value);
		BAPVariable<T> &held = *variable;
		add_variable(std::move(variable));
		return held;
	}
	size_t size() const { return variables_.size(); }
	void clear();

private:
	std::list<std::unique_ptr<BotScriptVariable>> variables_;
};

}
