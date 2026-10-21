#pragma once

#include "bf3/core/result.h"
#include "bf3/core/ui/swf_movie.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core::ui {

class SwfLibrary;

namespace avm1_op {
constexpr uint8_t kEnd = 0x00;
constexpr uint8_t kNextFrame = 0x04;
constexpr uint8_t kPreviousFrame = 0x05;
constexpr uint8_t kPlay = 0x06;
constexpr uint8_t kStop = 0x07;
constexpr uint8_t kToggleQuality = 0x08;
constexpr uint8_t kStopSounds = 0x09;
constexpr uint8_t kAdd = 0x0A;
constexpr uint8_t kSubtract = 0x0B;
constexpr uint8_t kMultiply = 0x0C;
constexpr uint8_t kDivide = 0x0D;
constexpr uint8_t kEquals = 0x0E;
constexpr uint8_t kLess = 0x0F;
constexpr uint8_t kAnd = 0x10;
constexpr uint8_t kOr = 0x11;
constexpr uint8_t kNot = 0x12;
constexpr uint8_t kStringEquals = 0x13;
constexpr uint8_t kStringLength = 0x14;
constexpr uint8_t kStringExtract = 0x15;
constexpr uint8_t kPop = 0x17;
constexpr uint8_t kToInteger = 0x18;
constexpr uint8_t kGetVariable = 0x1C;
constexpr uint8_t kSetVariable = 0x1D;
constexpr uint8_t kSetTarget2 = 0x20;
constexpr uint8_t kStringAdd = 0x21;
constexpr uint8_t kGetProperty = 0x22;
constexpr uint8_t kSetProperty = 0x23;
constexpr uint8_t kCloneSprite = 0x24;
constexpr uint8_t kRemoveSprite = 0x25;
constexpr uint8_t kTrace = 0x26;
constexpr uint8_t kStartDrag = 0x27;
constexpr uint8_t kEndDrag = 0x28;
constexpr uint8_t kStringLess = 0x29;
constexpr uint8_t kThrow = 0x2A;
constexpr uint8_t kCastOp = 0x2B;
constexpr uint8_t kImplementsOp = 0x2C;
constexpr uint8_t kRandomNumber = 0x30;
constexpr uint8_t kMBStringLength = 0x31;
constexpr uint8_t kCharToAscii = 0x32;
constexpr uint8_t kAsciiToChar = 0x33;
constexpr uint8_t kGetTime = 0x34;
constexpr uint8_t kMBStringExtract = 0x35;
constexpr uint8_t kMBCharToAscii = 0x36;
constexpr uint8_t kMBAsciiToChar = 0x37;
constexpr uint8_t kDelete = 0x3A;
constexpr uint8_t kDelete2 = 0x3B;
constexpr uint8_t kDefineLocal = 0x3C;
constexpr uint8_t kCallFunction = 0x3D;
constexpr uint8_t kReturn = 0x3E;
constexpr uint8_t kModulo = 0x3F;
constexpr uint8_t kNewObject = 0x40;
constexpr uint8_t kDefineLocal2 = 0x41;
constexpr uint8_t kInitArray = 0x42;
constexpr uint8_t kInitObject = 0x43;
constexpr uint8_t kTypeOf = 0x44;
constexpr uint8_t kTargetPath = 0x45;
constexpr uint8_t kEnumerate = 0x46;
constexpr uint8_t kAdd2 = 0x47;
constexpr uint8_t kLess2 = 0x48;
constexpr uint8_t kEquals2 = 0x49;
constexpr uint8_t kToNumber = 0x4A;
constexpr uint8_t kToString = 0x4B;
constexpr uint8_t kPushDuplicate = 0x4C;
constexpr uint8_t kStackSwap = 0x4D;
constexpr uint8_t kGetMember = 0x4E;
constexpr uint8_t kSetMember = 0x4F;
constexpr uint8_t kIncrement = 0x50;
constexpr uint8_t kDecrement = 0x51;
constexpr uint8_t kCallMethod = 0x52;
constexpr uint8_t kNewMethod = 0x53;
constexpr uint8_t kInstanceOf = 0x54;
constexpr uint8_t kEnumerate2 = 0x55;
constexpr uint8_t kBitAnd = 0x60;
constexpr uint8_t kBitOr = 0x61;
constexpr uint8_t kBitXor = 0x62;
constexpr uint8_t kBitLShift = 0x63;
constexpr uint8_t kBitRShift = 0x64;
constexpr uint8_t kBitURShift = 0x65;
constexpr uint8_t kStrictEquals = 0x66;
constexpr uint8_t kGreater = 0x67;
constexpr uint8_t kStringGreater = 0x68;
constexpr uint8_t kExtends = 0x69;
constexpr uint8_t kGotoFrame = 0x81;
constexpr uint8_t kGetUrl = 0x83;
constexpr uint8_t kStoreRegister = 0x87;
constexpr uint8_t kConstantPool = 0x88;
constexpr uint8_t kWaitForFrame = 0x8A;
constexpr uint8_t kSetTarget = 0x8B;
constexpr uint8_t kGoToLabel = 0x8C;
constexpr uint8_t kWaitForFrame2 = 0x8D;
constexpr uint8_t kDefineFunction2 = 0x8E;
constexpr uint8_t kTry = 0x8F;
constexpr uint8_t kWith = 0x94;
constexpr uint8_t kPush = 0x96;
constexpr uint8_t kJump = 0x99;
constexpr uint8_t kGetUrl2 = 0x9A;
constexpr uint8_t kDefineFunction = 0x9B;
constexpr uint8_t kIf = 0x9D;
constexpr uint8_t kCall = 0x9E;
constexpr uint8_t kGotoFrame2 = 0x9F;
}

const char *avm1_op_name(uint8_t p_op);

namespace avm1_function_flag {
constexpr uint16_t kPreloadThis = 0x0001;
constexpr uint16_t kSuppressThis = 0x0002;
constexpr uint16_t kPreloadArguments = 0x0004;
constexpr uint16_t kSuppressArguments = 0x0008;
constexpr uint16_t kPreloadSuper = 0x0010;
constexpr uint16_t kSuppressSuper = 0x0020;
constexpr uint16_t kPreloadRoot = 0x0040;
constexpr uint16_t kPreloadParent = 0x0080;
constexpr uint16_t kPreloadGlobal = 0x0100;
constexpr uint16_t kReserved = 0xFE00;
}

namespace avm1_try_flag {
constexpr uint8_t kCatchBlock = 0x01;
constexpr uint8_t kFinallyBlock = 0x02;
constexpr uint8_t kCatchInRegister = 0x04;
}

namespace avm1_clip_event {
constexpr uint32_t kLoad = 0x00000001;
constexpr uint32_t kEnterFrame = 0x00000002;
constexpr uint32_t kUnload = 0x00000004;
constexpr uint32_t kMouseMove = 0x00000008;
constexpr uint32_t kMouseDown = 0x00000010;
constexpr uint32_t kMouseUp = 0x00000020;
constexpr uint32_t kKeyDown = 0x00000040;
constexpr uint32_t kKeyUp = 0x00000080;
constexpr uint32_t kData = 0x00000100;
constexpr uint32_t kInitialize = 0x00000200;
constexpr uint32_t kPress = 0x00000400;
constexpr uint32_t kRelease = 0x00000800;
constexpr uint32_t kReleaseOutside = 0x00001000;
constexpr uint32_t kRollOver = 0x00002000;
constexpr uint32_t kRollOut = 0x00004000;
constexpr uint32_t kDragOver = 0x00008000;
constexpr uint32_t kDragOut = 0x00010000;
constexpr uint32_t kKeyPress = 0x00020000;
constexpr uint32_t kConstruct = 0x00040000;
}

std::string avm1_clip_event_names(uint32_t p_flags);

enum class Avm1PushType : uint8_t {
	String = 0,
	Float = 1,
	Null = 2,
	Undefined = 3,
	Register = 4,
	Boolean = 5,
	Double = 6,
	Integer = 7,
	Constant8 = 8,
	Constant16 = 9,
};

struct Avm1Value {
	Avm1PushType type = Avm1PushType::Undefined;
	std::string text;
	double number = 0.0;
	uint32_t index = 0;
	bool resolved = false;

	bool is_string() const { return type == Avm1PushType::String || ((type == Avm1PushType::Constant8 || type == Avm1PushType::Constant16) && resolved); }
	bool is_number() const;
};

struct Avm1Param {
	uint8_t reg = 0;
	std::string name;
};

inline constexpr uint32_t kAvm1NoTarget = 0xFFFFFFFFu;

struct Avm1Action {
	uint32_t offset = 0;
	uint8_t op = 0;
	uint16_t length = 0;
	uint32_t scope = 0;

	std::vector<Avm1Value> values;
	std::vector<std::string> strings;

	std::vector<Avm1Param> params;
	uint8_t register_count = 0;
	uint16_t function_flags = 0;
	uint16_t code_size = 0;

	uint16_t frame = 0;
	uint8_t skip_count = 0;
	uint8_t reg = 0;
	uint8_t mode = 0;
	uint16_t scene_bias = 0;

	int16_t branch_offset = 0;
	uint32_t target = kAvm1NoTarget;

	uint16_t block_size = 0;
	uint8_t try_flags = 0;
	uint16_t try_size = 0;
	uint16_t catch_size = 0;
	uint16_t finally_size = 0;

	uint32_t size() const { return op >= 0x80 ? 3u + length : 1u; }
	uint32_t end() const { return offset + size(); }
};

struct Avm1Scope {
	uint32_t begin = 0;
	uint32_t end = 0;
	uint32_t action = kAvm1NoTarget;
	uint32_t parent = 0;
	uint32_t depth = 0;

	uint32_t bytes() const { return end - begin; }
};

struct Avm1Code {
	uint32_t base = 0;
	uint32_t bytes = 0;
	std::vector<Avm1Action> actions;
	std::vector<Avm1Scope> scopes;
	uint32_t unresolved_constants = 0;
	uint32_t registers_out_of_range = 0;

	uint32_t record_bytes() const;
	uint32_t find(uint32_t p_offset) const;
	const Avm1Action *function_action(uint32_t p_scope) const;
};

Result<Avm1Code> decode_avm1(std::span<const uint8_t> p_bytes, uint32_t p_base = 0);

enum class Avm1BodyKind : uint8_t {
	DoAction,
	DoInitAction,
	ClipAction,
};
const char *avm1_body_kind_name(Avm1BodyKind p_kind);

struct Avm1Body {
	Avm1BodyKind kind = Avm1BodyKind::DoAction;
	uint16_t timeline = 0;
	uint32_t frame = 0;
	uint16_t sprite = 0;
	uint16_t depth = 0;
	std::string instance_name;
	uint32_t event_flags = 0;
	uint8_t key_code = 0;
	uint32_t tag_offset = 0;
	uint32_t tag_length = 0;
	uint32_t offset = 0;
	uint32_t length = 0;
	Avm1Code code;
};

struct Avm1Member {
	std::string name;
	uint32_t scope = 0;
	uint32_t bytes = 0;
};

struct Avm1Field {
	std::string name;
	std::string value;
};

struct Avm1Class {
	std::string name;
	std::string base;
	uint32_t body = 0;
	uint32_t constructor_scope = 0;
	uint32_t constructor_bytes = 0;
	std::vector<Avm1Member> methods;
	std::vector<Avm1Member> static_methods;
	std::vector<Avm1Field> fields;
	std::vector<Avm1Field> static_fields;
	std::vector<std::string> properties;
	std::vector<std::string> interfaces;
};

struct Avm1Registration {
	std::string linkage;
	std::string class_name;
	uint32_t body = 0;
};

struct Avm1Movie {
	std::vector<Avm1Body> bodies;
	std::vector<std::string> packages;
	std::vector<Avm1Class> classes;
	std::vector<Avm1Registration> registrations;
	uint64_t do_action_bytes = 0;
	uint64_t do_init_action_bytes = 0;
	uint64_t clip_action_bytes = 0;
	uint32_t do_action_tags = 0;
	uint32_t do_init_action_tags = 0;
	uint32_t clip_action_tags = 0;

	const Avm1Class *find_class(std::string_view p_name) const;
	uint32_t class_body_bytes(const Avm1Class &p_class) const;
};

Result<Avm1Movie> read_avm1(const SwfMovie &p_movie);

std::string format_avm1_code(const Avm1Code &p_code);
std::string format_avm1_class(const Avm1Class &p_class);
std::string format_avm1_body_header(const SwfMovie &p_movie, const Avm1Movie &p_avm1, uint32_t p_body);

struct Avm1MovieCensus {
	std::string name;
	std::string error;
	uint32_t bodies = 0;
	uint64_t actions = 0;
	uint64_t do_action_bytes = 0;
	uint64_t do_init_action_bytes = 0;
	uint64_t clip_action_bytes = 0;
	uint32_t do_action_tags = 0;
	uint32_t do_init_action_tags = 0;
	uint32_t clip_action_tags = 0;
	uint32_t records_mismatched = 0;
	uint32_t functions = 0;
	uint32_t classes = 0;
	uint32_t class_bodies = 0;
	uint64_t class_bytes = 0;
	uint32_t package_bodies = 0;
	uint32_t package_bodies_matched = 0;
	uint32_t registrations = 0;
	uint32_t unresolved_constants = 0;
	uint32_t registers_out_of_range = 0;
	std::array<uint32_t, 256> ops{};
};

struct Avm1Census {
	std::vector<Avm1MovieCensus> movies;
	uint32_t errors = 0;
	uint64_t actions = 0;
	uint64_t do_action_bytes = 0;
	uint64_t do_init_action_bytes = 0;
	uint64_t clip_action_bytes = 0;
	uint32_t do_action_tags = 0;
	uint32_t do_init_action_tags = 0;
	uint32_t clip_action_tags = 0;
	uint32_t records_mismatched = 0;
	uint32_t functions = 0;
	uint32_t classes = 0;
	uint32_t package_bodies = 0;
	uint32_t package_bodies_matched = 0;
	uint32_t registrations = 0;
	uint32_t unresolved_constants = 0;
	uint32_t registers_out_of_range = 0;
	std::array<uint32_t, 256> ops{};
	double seconds = 0.0;

	uint64_t tag_action_bytes() const { return do_action_bytes + do_init_action_bytes; }
	const Avm1MovieCensus *find(std::string_view p_name) const;
};

Result<Avm1Census> run_avm1_census(const SwfLibrary &p_library, unsigned p_threads = 0);

std::string format_avm1_census(const Avm1Census &p_census);

}
