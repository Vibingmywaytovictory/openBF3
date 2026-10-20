#pragma once

#include "bf3/core/ids.h"
#include "bf3/core/result.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace bf3::core {

enum class DbType : uint8_t {
	List = 0x01,
	Object = 0x02,
	HomoList = 0x03,
	Null = 0x04,
	Bool = 0x06,
	String = 0x07,
	Int = 0x08,
	Long = 0x09,
	VarInt = 0x0A,
	Float = 0x0B,
	Double = 0x0C,
	Timestamp = 0x0D,
	RecordId = 0x0E,
	Guid = 0x0F,
	Sha1 = 0x10,
	Matrix44 = 0x11,
	Blob = 0x13,
	Attachment = 0x14,
	Timespan = 0x15,
};

constexpr uint32_t kDbNoNode = 0xFFFFFFFFu;

struct DbNode {
	DbType type = DbType::Null;
	bool named = false;
	uint32_t name_at = 0;
	uint32_t name_len = 0;
	uint32_t next = kDbNoNode;
	uint32_t first_child = kDbNoNode;
	uint32_t data_at = 0;
	uint32_t data_len = 0;
	int64_t i = 0;
	double f = 0.0;
};

class DbDocument;

class DbRef {
public:
	DbRef() = default;
	DbRef(const DbDocument *p_doc, uint32_t p_index) :
			doc_(p_doc), index_(p_index) {}

	bool valid() const { return doc_ != nullptr && index_ != kDbNoNode; }
	DbType type() const;
	std::string_view name() const;
	bool is_object() const { return valid() && type() == DbType::Object; }
	bool is_list() const { return valid() && (type() == DbType::List || type() == DbType::HomoList); }

	DbRef first_child() const;
	DbRef next() const;
	size_t child_count() const;

	DbRef get(std::string_view p_key) const;

	std::optional<bool> as_bool() const;
	std::optional<std::string_view> as_str() const;
	std::optional<int64_t> as_i64() const;
	std::optional<Guid> as_guid() const;
	std::optional<Sha1> as_sha1() const;
	std::optional<std::span<const uint8_t>> as_blob() const;

	const DbDocument *document() const { return doc_; }
	uint32_t index() const { return index_; }

private:
	const DbDocument *doc_ = nullptr;
	uint32_t index_ = kDbNoNode;
};

class DbDocument {
public:

	static Result<DbDocument> parse(std::span<const uint8_t> p_bytes);

	static Result<DbDocument> parse_element(std::span<const uint8_t> p_bytes, size_t p_at, size_t &r_end);

	DbRef root() const { return DbRef(this, root_); }
	const DbNode &node(uint32_t p_index) const { return nodes_[p_index]; }
	std::span<const uint8_t> source() const { return bytes_; }
	size_t node_count() const { return nodes_.size(); }

private:
	friend class DbParser;
	std::span<const uint8_t> bytes_;
	std::vector<DbNode> nodes_;
	uint32_t root_ = kDbNoNode;
};

}
