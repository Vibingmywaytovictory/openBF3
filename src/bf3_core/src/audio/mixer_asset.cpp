#include "bf3/core/audio/mixer_asset.h"

#include "bf3/core/audio/sound_ports.h"
#include "bf3/core/audio/sound_types.h"
#include "bf3/core/gameplay/work_budget.h"

#include <algorithm>
#include <cmath>
#include <string_view>

namespace bf3::core::audio {

namespace {

struct Reader {
	const EbxResolver &db;
	const EbxPartition &part;
	MixerAsset asset;
	std::vector<Guid> input_params;

	Error error(const std::string &p_what) const {
		return make_error("mixer asset " + (asset.name.empty() ? part.guid().to_string() : asset.name) + ": " + p_what);
	}

	std::optional<EbxInstance> follow(const EbxValue &p_value) const {
		const std::optional<EbxRef> r = p_value.as_ref();
		if (!r || r->is_null()) {
			return std::nullopt;
		}
		return r->kind == EbxRef::Kind::Internal ? part.resolve(*r) : db.resolve(part, *r);
	}

	Result<std::vector<EbxInstance>> refs(const EbxStruct &p_s, std::string_view p_field) const {
		std::vector<EbxInstance> out;
		const std::optional<EbxValue> v = p_s.get(p_field);
		const OptionalEbxArray array = v ? v->as_array() : OptionalEbxArray();
		if (!array) {
			return out;
		}
		if (array->size() > kMaxMixerAssetEntries) {
			return error(std::string(p_s.type_name()) + "." + std::string(p_field) + " has " +
					std::to_string(array->size()) + " entries");
		}
		for (const EbxValue element : *array) {
			const std::optional<EbxInstance> i = follow(element);
			if (!i) {
				return error(
						std::string(p_s.type_name()) + "." + std::string(p_field) + " holds an unresolved reference");
			}
			out.push_back(*i);
		}
		return out;
	}

	Result<SoundPort> port(const EbxStruct &p_s, std::string_view p_field) const {
		Result<SoundPort> p = read_graph_port(p_s, p_field);
		if (!p) {
			return error(p.error().message);
		}
		if (p->connected() && p->reg >= asset.registers) {
			return error(std::string(p_s.type_name()) + "." + std::string(p_field) + " register " +
					std::to_string(p->reg) + " is past the graph's ValueCount " + std::to_string(asset.registers));
		}
		return p;
	}

	Result<uint16_t> out_port(const EbxStruct &p_s, std::string_view p_field) const {
		Result<SoundPort> p = port(p_s, p_field);
		if (!p) {
			return p.error();
		}
		return p->reg;
	}

	static uint32_t name_hash(const EbxInstance &p_param) {
		const std::optional<EbxValue> v = p_param.get("NameHash");
		return v ? v->as_u32().value_or(uint32_t(v->as_i64().value_or(0))) : 0u;
	}

	static float float_field(const EbxInstance &p_i, std::string_view p_field) {
		const std::optional<EbxValue> v = p_i.get(p_field);
		const float f = v ? v->as_f32().value_or(0.0f) : 0.0f;
		return std::isfinite(f) ? f : 0.0f;
	}

	Status input_node(const EbxInstance &p_node) {
		Result<std::vector<EbxInstance>> entries = refs(p_node.data(), "Entries");
		if (!entries) {
			return entries.error();
		}
		for (const EbxInstance &e : *entries) {
			const std::optional<EbxValue> source = e.get("Source");
			const std::optional<EbxInstance> param = source ? follow(*source) : std::nullopt;
			if (!param) {
				return error("a MixerInputEntry has no Source");
			}
			const auto at = std::find(input_params.begin(), input_params.end(), param->guid());
			if (at == input_params.end()) {
				return error("a MixerInputEntry names a parameter that is not a graph input");
			}
			MixerAssetInput &in = asset.inputs[size_t(at - input_params.begin())];
			const std::optional<EbxValue> mode = e.get("AccumulateMode");
			const std::string_view mode_name = mode ? mode->enum_name().value_or("") : std::string_view();
			if (mode_name == "MixerValueAccumulateMode_Max") {
				in.accumulate = MixerAccumulate::Max;
			} else if (mode_name == "MixerValueAccumulateMode_None") {
				in.accumulate = MixerAccumulate::None;
			} else {
				return error("accumulate mode '" + std::string(mode_name) + "' is not evaluated");
			}
			Result<uint16_t> out = out_port(e.data(), "Out");
			if (!out) {
				return out.error();
			}
			in.out = *out;
		}
		MixerAssetNode n;
		n.kind = MixerNodeKind::Input;
		asset.nodes.push_back(n);
		return Status::success();
	}

	Status output_node(const EbxInstance &p_node) {
		Result<std::vector<EbxInstance>> entries = refs(p_node.data(), "Entries");
		if (!entries) {
			return entries.error();
		}
		MixerAssetNode n;
		n.kind = MixerNodeKind::Output;
		n.entries_begin = uint32_t(asset.entries.size());
		for (const EbxInstance &e : *entries) {
			const std::optional<EbxValue> target = e.get("Target");
			const std::optional<EbxInstance> param = target ? follow(*target) : std::nullopt;
			if (!param) {
				return error("a MixerOutputEntry has no Target");
			}
			Result<SoundPort> in = port(e.data(), "In");
			if (!in) {
				return in.error();
			}
			asset.outputs.push_back(MixerAssetOutput{ name_hash(*param), *in });
			asset.entries.push_back(*in);
		}
		n.entries_count = uint32_t(asset.entries.size()) - n.entries_begin;
		asset.nodes.push_back(n);
		return Status::success();
	}

	Status ports(MixerAssetNode &r_node, const EbxStruct &p_s, std::initializer_list<std::string_view> p_in,
			std::initializer_list<std::string_view> p_out) const {
		size_t i = 0;
		for (const std::string_view f : p_in) {
			Result<SoundPort> p = port(p_s, f);
			if (!p) {
				return p.error();
			}
			r_node.in[i++] = *p;
		}
		i = 0;
		for (const std::string_view f : p_out) {
			Result<uint16_t> p = out_port(p_s, f);
			if (!p) {
				return p.error();
			}
			r_node.out[i++] = *p;
		}
		return Status::success();
	}

	Status node(const EbxInstance &p_node) {
		const std::string_view type = p_node.type_name();
		const EbxStruct s = p_node.data();
		if (type == "MixerInputNodeData") {
			return input_node(p_node);
		}
		if (type == "MixerOutputNodeData") {
			return output_node(p_node);
		}
		MixerAssetNode n;
		if (type == "SimpleTransformNodeData") {
			n.kind = MixerNodeKind::SimpleTransform;
			const std::optional<EbxValue> op = s.get("Operation");
			const SimpleTransformOp o = simple_transform_op(op ? op->enum_name().value_or("") : std::string_view());
			if (o == SimpleTransformOp::Unknown) {
				return error("a SimpleTransform has an unknown operation");
			}
			const std::optional<EbxValue> unit = s.get("AngleUnit");
			const bool degrees = unit && unit->enum_name().value_or("") == "AngleUnit_Degrees";
			n.option = uint8_t(uint8_t(o) | (degrees ? port::kSimpleTransformDegrees : 0));
			BF3_TRY(ports(n, s, { "X", "Y" }, { "Z" }));
		} else if (type == "ScaleClampNodeData") {
			n.kind = MixerNodeKind::ScaleClamp;
			BF3_TRY(ports(n, s, { "In", "InMin", "InMax", "OutMin", "OutMax" }, { "Out" }));
		} else if (type == "MinMaxValueSelectorNodeData") {
			n.kind = MixerNodeKind::MinMaxValueSelector;
			BF3_TRY(ports(n, s, {}, { "MaxIndex", "MaxValue", "MinIndex", "MinValue" }));
			Result<std::vector<EbxInstance>> entries = refs(s, "Inputs");
			if (!entries) {
				return entries.error();
			}
			if (entries->empty()) {
				return error("a MinMaxValueSelector has no inputs");
			}
			n.entries_begin = uint32_t(asset.entries.size());
			for (const EbxInstance &e : *entries) {
				Result<SoundPort> in = port(e.data(), "Input");
				if (!in) {
					return in.error();
				}
				asset.entries.push_back(*in);
			}
			n.entries_count = uint32_t(asset.entries.size()) - n.entries_begin;
		} else {
			return error("node type " + std::string(type) + " is not evaluated");
		}
		asset.nodes.push_back(n);
		return Status::success();
	}

	void reads_writes(
			const MixerAssetNode &p_n, std::vector<uint16_t> &r_reads, std::vector<uint16_t> &r_writes) const {
		const size_t ins = p_n.kind == MixerNodeKind::SimpleTransform ? 2
				: p_n.kind == MixerNodeKind::ScaleClamp				  ? 5
																	  : 0;
		for (size_t i = 0; i < ins; ++i) {
			if (p_n.in[i].connected()) {
				r_reads.push_back(p_n.in[i].reg);
			}
		}
		if (p_n.kind == MixerNodeKind::MinMaxValueSelector || p_n.kind == MixerNodeKind::Output) {
			for (uint32_t i = 0; i < p_n.entries_count; ++i) {
				const SoundPort &e = asset.entries[p_n.entries_begin + i];
				if (e.connected()) {
					r_reads.push_back(e.reg);
				}
			}
		}
		if (p_n.kind == MixerNodeKind::Input) {
			for (const MixerAssetInput &in : asset.inputs) {
				if (in.out != kNoRegister) {
					r_writes.push_back(in.out);
				}
			}
		}
		for (const uint16_t o : p_n.out) {
			if (o != kNoRegister) {
				r_writes.push_back(o);
			}
		}
	}

	Result<MixerAsset> run() {
		const EbxInstance primary = part.primary();
		if (!primary.is_a("MixerAsset")) {
			return error("primary instance is " + std::string(primary.type_name()) + ", not a MixerAsset");
		}
		if (const std::optional<std::string_view> n = db.name_of(part.guid())) {
			asset.name = std::string(*n);
		}
		asset.partition = part.guid();
		const std::optional<EbxValue> g = primary.get("Graph");
		const std::optional<EbxInstance> graph = g ? follow(*g) : std::nullopt;
		if (!graph || !graph->is_a("MixerGraphData")) {
			return error("no MixerGraphData");
		}
		const std::optional<EbxValue> count = graph->get("ValueCount");
		const int64_t values = count ? count->as_i64().value_or(-1) : -1;
		if (values < 0 || values > int64_t(kMaxProgramValues)) {
			return error("ValueCount " + std::to_string(values) + " out of range");
		}
		asset.registers = uint32_t(values);
		Result<std::vector<EbxInstance>> inputs = refs(graph->data(), "Inputs");
		if (!inputs) {
			return inputs.error();
		}
		for (const EbxInstance &p : *inputs) {
			asset.inputs.push_back(MixerAssetInput{ name_hash(p), float_field(p, "DefaultValue") });
			input_params.push_back(p.guid());
		}
		Result<std::vector<EbxInstance>> nodes = refs(graph->data(), "Nodes");
		if (!nodes) {
			return nodes.error();
		}
		if (nodes->size() > kMaxMixerAssetNodes) {
			return error(std::to_string(nodes->size()) + " nodes");
		}
		for (const EbxInstance &n : *nodes) {
			BF3_TRY(node(n));
		}
		std::vector<std::vector<uint16_t>> reads(asset.nodes.size()), writes(asset.nodes.size());
		for (size_t i = 0; i < asset.nodes.size(); ++i) {
			reads_writes(asset.nodes[i], reads[i], writes[i]);
		}
		gameplay::WorkBudget budget;
		size_t feedback = 0;
		Result<std::vector<uint32_t>> order =
				order_by_register_dependencies(reads, writes, asset.registers, budget, feedback);
		if (!order) {
			return error(order.error().message);
		}
		if (feedback != 0) {
			return error(std::to_string(feedback) + " feedback edges");
		}
		std::vector<MixerAssetNode> sorted;
		sorted.reserve(asset.nodes.size());
		for (const uint32_t i : *order) {
			sorted.push_back(asset.nodes[i]);
		}
		asset.nodes = std::move(sorted);
		return std::move(asset);
	}
};

float read(const SoundPort &p_port, std::span<const float> p_registers) {
	return p_port.connected() && p_port.reg < p_registers.size() ? p_registers[p_port.reg] : p_port.constant;
}

void write(uint16_t p_reg, float p_value, std::span<float> r_registers) {
	if (p_reg != kNoRegister && p_reg < r_registers.size()) {
		r_registers[p_reg] = p_value;
	}
}

}

std::optional<size_t> MixerAsset::input_of(uint32_t p_name_hash) const {
	for (size_t i = 0; i < inputs.size(); ++i) {
		if (inputs[i].name_hash == p_name_hash) {
			return i;
		}
	}
	return std::nullopt;
}

std::optional<size_t> MixerAsset::output_of(uint32_t p_name_hash) const {
	for (size_t i = 0; i < outputs.size(); ++i) {
		if (outputs[i].name_hash == p_name_hash) {
			return i;
		}
	}
	return std::nullopt;
}

float MixerAsset::accumulate(size_t p_input, std::span<const float> p_contributions) const {
	if (p_input >= inputs.size() || p_contributions.empty()) {
		return p_input < inputs.size() ? inputs[p_input].default_value : 0.0f;
	}
	if (inputs[p_input].accumulate == MixerAccumulate::Max) {
		return *std::max_element(p_contributions.begin(), p_contributions.end());
	}
	return p_contributions.back();
}

void MixerAsset::evaluate(
		std::span<const float> p_inputs, std::span<float> r_registers, std::span<float> r_outputs) const {
	std::fill(r_registers.begin(), r_registers.end(), 0.0f);
	for (const MixerAssetNode &n : nodes) {
		switch (n.kind) {
			case MixerNodeKind::Input:
				for (size_t i = 0; i < inputs.size(); ++i) {
					write(inputs[i].out, i < p_inputs.size() ? p_inputs[i] : inputs[i].default_value, r_registers);
				}
				break;
			case MixerNodeKind::SimpleTransform:
				write(n.out[0], simple_transform(n.option, read(n.in[0], r_registers), read(n.in[1], r_registers)),
						r_registers);
				break;
			case MixerNodeKind::ScaleClamp:
				write(n.out[0],
						scale_clamp(read(n.in[0], r_registers), read(n.in[1], r_registers), read(n.in[2], r_registers),
								read(n.in[3], r_registers), read(n.in[4], r_registers)),
						r_registers);
				break;
			case MixerNodeKind::MinMaxValueSelector: {
				uint32_t max_at = 0, min_at = 0;
				float max_v = read(entries[n.entries_begin], r_registers);
				float min_v = max_v;
				for (uint32_t i = 1; i < n.entries_count; ++i) {
					const float v = read(entries[n.entries_begin + i], r_registers);
					if (v > max_v) {
						max_v = v;
						max_at = i;
					}
					if (v < min_v) {
						min_v = v;
						min_at = i;
					}
				}
				write(n.out[0], float(max_at), r_registers);
				write(n.out[1], max_v, r_registers);
				write(n.out[2], float(min_at), r_registers);
				write(n.out[3], min_v, r_registers);
				break;
			}
			case MixerNodeKind::Output:
				break;
		}
	}
	for (size_t i = 0; i < outputs.size() && i < r_outputs.size(); ++i) {
		r_outputs[i] = read(outputs[i].value, r_registers);
	}
}

Result<MixerAsset> read_mixer_asset(const EbxResolver &p_db, const EbxPartition &p_partition) {
	Reader r{ p_db, p_partition, {}, {} };
	return r.run();
}

}
