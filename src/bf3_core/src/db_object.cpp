#include "bf3/core/db_object.h"

#include "format_errors.h"

#include <cstring>

namespace bf3::core {

namespace {

constexpr size_t kMaxDepth = 64;
constexpr uint8_t kTagTypeMask = 0x1F;
constexpr uint8_t kTagAnonymous = 0x80;

template <class T>
T load_le(const uint8_t *p) {
	T v;
	std::memcpy(&v, p, sizeof(T));
	return v;
}

int64_t zigzag(uint64_t p_value) {
	return static_cast<int64_t>(p_value >> 1) ^ -static_cast<int64_t>(p_value & 1);
}

}

class DbParser {
public:
	DbParser(std::span<const uint8_t> p_bytes, size_t p_at, DbDocument &r_doc) :
			bytes_(p_bytes), at_(p_at), doc_(r_doc) {}

	Status element(size_t p_depth, uint32_t &r_index) {
		r_index = kDbNoNode;
		const size_t tag_at = at_;
		uint8_t tag = 0;
		BF3_TRY(byte(tag));
		const uint8_t kind = tag & kTagTypeMask;
		if (kind == 0) {
			return {};
		}
		DbNode node;
		if ((tag & kTagAnonymous) == 0) {
			size_t name_at = 0, name_len = 0;
			BF3_TRY(cstr(name_at, name_len));
			node.named = true;
			node.name_at = static_cast<uint32_t>(name_at);
			node.name_len = static_cast<uint32_t>(name_len);
		}
		node.type = static_cast<DbType>(kind);
		const uint32_t index = static_cast<uint32_t>(doc_.nodes_.size());
		switch (kind) {
			case 0x01:
			case 0x02:
			case 0x03: {
				doc_.nodes_.push_back(node);
				BF3_TRY(container(p_depth, index));
				r_index = index;
				return {};
			}
			case 0x04:
				break;
			case 0x06: {
				uint8_t b = 0;
				BF3_TRY(byte(b));
				node.i = b != 0 ? 1 : 0;
				break;
			}
			case 0x07: {
				size_t len = 0, at = 0;
				BF3_TRY(length(len));
				BF3_TRY(take(len, at));
				size_t end = len;
				while (end > 0 && bytes_[at + end - 1] == 0) {
					--end;
				}
				if (!detail::valid_utf8(bytes_.subspan(at, end))) {
					return detail::bad_utf8("DbObject string");
				}
				node.data_at = static_cast<uint32_t>(at);
				node.data_len = static_cast<uint32_t>(end);
				break;
			}
			case 0x08:
			case 0x09:
			case 0x0B:
			case 0x0C:
			case 0x0D: {
				const size_t n = (kind == 0x08 || kind == 0x0B) ? 4 : 8;
				size_t at = 0;
				BF3_TRY(take(n, at));
				const uint8_t *p = bytes_.data() + at;
				if (kind == 0x08) {
					node.i = load_le<int32_t>(p);
				} else if (kind == 0x0B) {
					node.f = load_le<float>(p);
				} else if (kind == 0x0C) {
					node.f = load_le<double>(p);
				} else {
					node.i = load_le<int64_t>(p);
				}
				break;
			}
			case 0x0A:
			case 0x15: {
				uint64_t v = 0;
				BF3_TRY(uleb(v));
				node.i = zigzag(v);
				break;
			}
			case 0x0E:
			case 0x0F:
			case 0x10:
			case 0x11:
			case 0x14: {
				const size_t n = kind == 0x0E ? 6 : kind == 0x0F ? 16 : kind == 0x11 ? 64 : 20;
				size_t at = 0;
				BF3_TRY(take(n, at));
				node.data_at = static_cast<uint32_t>(at);
				node.data_len = static_cast<uint32_t>(n);
				break;
			}
			case 0x13: {
				size_t len = 0, at = 0;
				BF3_TRY(length(len));
				BF3_TRY(take(len, at));
				node.data_at = static_cast<uint32_t>(at);
				node.data_len = static_cast<uint32_t>(len);
				break;
			}
			default:
				return make_error("unsupported DbObject tag " + detail::hex_prefixed(tag, 2) + " at byte " + std::to_string(tag_at));
		}
		doc_.nodes_.push_back(node);
		r_index = index;
		return {};
	}

	size_t at() const { return at_; }

private:
	Status container(size_t p_depth, uint32_t p_index) {
		if (p_depth >= kMaxDepth) {
			return detail::bad_db_object(at_, "nesting is too deep");
		}
		size_t len = 0;
		BF3_TRY(length(len));
		const size_t end = at_ + len;
		if (end > bytes_.size()) {
			return detail::truncated("DbObject container", end, bytes_.size());
		}
		bool terminated = false;
		uint32_t last = kDbNoNode;
		while (at_ < end) {
			uint32_t child = kDbNoNode;
			BF3_TRY(element(p_depth + 1, child));
			if (child == kDbNoNode) {
				terminated = true;
				break;
			}
			if (last == kDbNoNode) {
				doc_.nodes_[p_index].first_child = child;
			} else {
				doc_.nodes_[last].next = child;
			}
			last = child;
		}
		if (!terminated || at_ != end) {
			return detail::bad_db_object(at_, "container does not end with its terminator");
		}
		return {};
	}

	Status byte(uint8_t &r_byte) {
		if (at_ >= bytes_.size()) {
			return detail::truncated("DbObject", at_ + 1, bytes_.size());
		}
		r_byte = bytes_[at_++];
		return {};
	}

	Status take(size_t p_len, size_t &r_at) {
		if (p_len > bytes_.size() - at_) {
			return detail::truncated("DbObject value", at_ + p_len, bytes_.size());
		}
		r_at = at_;
		at_ += p_len;
		return {};
	}

	Status cstr(size_t &r_at, size_t &r_len) {
		const uint8_t *start = bytes_.data() + at_;
		const void *nul = std::memchr(start, 0, bytes_.size() - at_);
		if (nul == nullptr) {
			return detail::bad_db_object(at_, "name has no terminator");
		}
		const size_t len = static_cast<size_t>(static_cast<const uint8_t *>(nul) - start);
		if (!detail::valid_utf8(bytes_.subspan(at_, len))) {
			return detail::bad_utf8("DbObject name");
		}
		r_at = at_;
		r_len = len;
		at_ += len + 1;
		return {};
	}

	Status uleb(uint64_t &r_value) {
		const size_t start = at_;
		uint64_t value = 0;
		for (unsigned shift = 0; shift < 64; shift += 7) {
			uint8_t b = 0;
			BF3_TRY(byte(b));
			value |= static_cast<uint64_t>(b & 0x7F) << shift;
			if ((b & 0x80) == 0) {
				r_value = value;
				return {};
			}
		}
		return detail::bad_db_object(start, "varint is longer than 64 bits");
	}

	Status length(size_t &r_len) {
		const size_t at = at_;
		uint64_t len = 0;
		BF3_TRY(uleb(len));
		if (len > bytes_.size()) {
			return detail::bad_db_object(at, "length exceeds the buffer");
		}
		r_len = static_cast<size_t>(len);
		return {};
	}

	std::span<const uint8_t> bytes_;
	size_t at_;
	DbDocument &doc_;
};

Result<DbDocument> DbDocument::parse_element(std::span<const uint8_t> p_bytes, size_t p_at, size_t &r_end) {
	DbDocument doc;
	if (p_bytes.size() >= 0xFFFFFFFFu) {
		return make_error("DbObject buffer is larger than 4 GiB");
	}
	doc.bytes_ = p_bytes;
	doc.nodes_.reserve(p_bytes.size() / 24 + 16);
	DbParser parser(p_bytes, p_at, doc);
	uint32_t root = kDbNoNode;
	BF3_TRY(parser.element(0, root));
	if (root == kDbNoNode) {
		return detail::bad_db_object(p_at, "element is an end marker");
	}
	doc.root_ = root;
	r_end = parser.at();
	return doc;
}

Result<DbDocument> DbDocument::parse(std::span<const uint8_t> p_bytes) {
	size_t end = 0;
	Result<DbDocument> doc = parse_element(p_bytes, 0, end);
	if (doc && end != p_bytes.size()) {
		return detail::bad_db_object(end, "bytes follow the top-level element");
	}
	return doc;
}

DbType DbRef::type() const {
	return doc_->node(index_).type;
}

std::string_view DbRef::name() const {
	if (!valid()) {
		return {};
	}
	const DbNode &n = doc_->node(index_);
	if (!n.named) {
		return {};
	}
	return std::string_view(reinterpret_cast<const char *>(doc_->source().data()) + n.name_at, n.name_len);
}

DbRef DbRef::first_child() const {
	if (!valid()) {
		return {};
	}
	return DbRef(doc_, doc_->node(index_).first_child);
}

DbRef DbRef::next() const {
	if (!valid()) {
		return {};
	}
	return DbRef(doc_, doc_->node(index_).next);
}

size_t DbRef::child_count() const {
	size_t n = 0;
	for (DbRef c = first_child(); c.valid(); c = c.next()) {
		++n;
	}
	return n;
}

DbRef DbRef::get(std::string_view p_key) const {
	if (!is_object()) {
		return {};
	}
	for (DbRef c = first_child(); c.valid(); c = c.next()) {
		if (iequals_ascii(c.name(), p_key)) {
			return c;
		}
	}
	return {};
}

std::optional<bool> DbRef::as_bool() const {
	if (!valid() || type() != DbType::Bool) {
		return std::nullopt;
	}
	return doc_->node(index_).i != 0;
}

std::optional<std::string_view> DbRef::as_str() const {
	if (!valid() || type() != DbType::String) {
		return std::nullopt;
	}
	const DbNode &n = doc_->node(index_);
	return std::string_view(reinterpret_cast<const char *>(doc_->source().data()) + n.data_at, n.data_len);
}

std::optional<int64_t> DbRef::as_i64() const {
	if (!valid()) {
		return std::nullopt;
	}
	const DbType t = type();
	if (t != DbType::Int && t != DbType::Long && t != DbType::VarInt) {
		return std::nullopt;
	}
	return doc_->node(index_).i;
}

std::optional<Guid> DbRef::as_guid() const {
	if (!valid() || type() != DbType::Guid) {
		return std::nullopt;
	}
	return Guid::from_bytes(doc_->source().data() + doc_->node(index_).data_at);
}

std::optional<Sha1> DbRef::as_sha1() const {
	if (!valid() || type() != DbType::Sha1) {
		return std::nullopt;
	}
	return Sha1::from_bytes(doc_->source().data() + doc_->node(index_).data_at);
}

std::optional<std::span<const uint8_t>> DbRef::as_blob() const {
	if (!valid() || type() != DbType::Blob) {
		return std::nullopt;
	}
	const DbNode &n = doc_->node(index_);
	return doc_->source().subspan(n.data_at, n.data_len);
}

}
