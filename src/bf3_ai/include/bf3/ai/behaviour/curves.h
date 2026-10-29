#pragma once

namespace bf3::ai {

class UrgeCurve {
public:
	virtual ~UrgeCurve() = default;
	virtual float calculate(float p_x) const = 0;
};

class UCConstant final : public UrgeCurve {
public:
	explicit UCConstant(float p_value) : value_(p_value) {}
	float calculate(float p_x) const override;

private:
	float value_ = 0.0f;
};

class UCLinear final : public UrgeCurve {
public:
	UCLinear(float p_slope, float p_offset) : slope_(p_slope), offset_(p_offset) {}
	float calculate(float p_x) const override;

private:
	float slope_ = 0.0f;
	float offset_ = 0.0f;
};

class UCXInverse final : public UrgeCurve {
public:
	UCXInverse(float p_numerator, float p_offset, float p_slope, float p_bias) :
			numerator_(p_numerator), offset_(p_offset), slope_(p_slope), bias_(p_bias) {}
	float calculate(float p_x) const override;

private:
	float numerator_ = 0.0f;
	float offset_ = 0.0f;
	float slope_ = 0.0f;
	float bias_ = 0.0f;
};

namespace SCurve {
float calculate(float p_x);
}

namespace JCurve {
float calculate(float p_x);
}

namespace DecliningSlopeCurve {
float calculate(float p_x);
}

}
