#include "bf3/core/audio/sound_program.h"

#include "bf3/core/audio/sound_ports.h"

#include <algorithm>
#include <cmath>
#include <queue>
#include <string>
#include <unordered_map>

namespace bf3::core::audio {

namespace {

using gameplay::WorkBudget;

uint32_t u32_field(const EbxStruct &p_s, std::string_view p_field) {
	const std::optional<EbxValue> v = p_s.get(p_field);
	if (!v) {
		return 0;
	}
	if (std::optional<uint64_t> u = v->as_u64()) {
		return uint32_t(*u);
	}
	if (std::optional<int64_t> i = v->as_i64()) {
		return uint32_t(int32_t(*i));
	}
	return 0;
}

struct Compiler {
	const EbxResolver &db;
	const EbxPartition &part;
	const CompileOptions &options;
	WorkBudget &budget;
	CompileStats &stats;
	SoundProgram prog;
	bool master = false;
	std::vector<int32_t> node_of_instance;
	std::unordered_map<Guid, uint16_t, GuidHash> wave_index;
	std::vector<std::string> deps;
	uint32_t max_register = 0;

	Error error(const std::string &p_what) const { return make_error("sound patch " + (prog.name.empty() ? part.guid().to_string() : prog.name) + ": " + p_what); }

	void note_dependency(const Guid &p_partition) {
		if (p_partition == part.guid()) {
			return;
		}
		if (std::optional<std::string_view> n = db.name_of(p_partition)) {
			const std::string s(*n);
			if (std::find(deps.begin(), deps.end(), s) == deps.end()) {
				deps.push_back(s);
			}
		}
	}

	Result<SoundPort> read_port(const EbxStruct &p_s, std::string_view p_field) {
		Result<SoundPort> port = read_graph_port(p_s, p_field);
		if (!port) {
			return error(port.error().message);
		}
		if (port->connected()) {
			max_register = std::max(max_register, uint32_t(port->reg));
		}
		return port;
	}

	float read_float(const EbxStruct &p_s, std::string_view p_field, float p_default = 0.0f) {
		const std::optional<EbxValue> v = p_s.get(p_field);
		if (!v) {
			return p_default;
		}
		const float f = v->as_f32().value_or(float(v->as_f64().value_or(p_default)));
		return std::isfinite(f) ? f : p_default;
	}
	bool read_bool(const EbxStruct &p_s, std::string_view p_field) {
		const std::optional<EbxValue> v = p_s.get(p_field);
		return v && v->as_bool().value_or(false);
	}
	std::string_view read_enum(const EbxStruct &p_s, std::string_view p_field) {
		const std::optional<EbxValue> v = p_s.get(p_field);
		return v ? v->enum_name().value_or("") : std::string_view();
	}

	Result<SoundPluginRef> read_plugin_ref(const EbxStruct &p_s, std::string_view p_field) {
		const std::optional<EbxValue> v = p_s.get(p_field);
		const std::optional<EbxStruct> s = v ? v->as_struct() : std::nullopt;
		if (!s) {
			return SoundPluginRef{};
		}
		if (!(s->get("IsValid") && s->get("IsValid")->as_bool().value_or(false))) {
			return SoundPluginRef{};
		}
		const int64_t voice = s->get("VoiceIndex") ? s->get("VoiceIndex")->as_i64().value_or(-1) : -1;
		const int64_t plugin = s->get("PluginIndex") ? s->get("PluginIndex")->as_i64().value_or(-1) : -1;
		if (voice < 0 || voice >= int64_t(kMaxProgramVoices) || plugin < 0 || plugin >= int64_t(kMaxVoicePlugins)) {
			return error(std::string(p_field) + ": plugin ref (" + std::to_string(voice) + ", " + std::to_string(plugin) + ") out of range");
		}
		return SoundPluginRef{ uint16_t(voice), uint16_t(plugin) };
	}

	Result<std::optional<EbxInstance>> internal(const EbxValue &p_value) {
		BF3_TRY(budget.charge(1));
		const std::optional<EbxRef> ref = p_value.as_ref();
		if (!ref || ref->is_null()) {
			return std::optional<EbxInstance>();
		}
		if (ref->kind == EbxRef::Kind::Internal) {
			return part.resolve(*ref);
		}
		return db.resolve(part, *ref);
	}

	uint8_t option_of(SoundNodeType p_type, const EbxStruct &p_s, bool &r_unknown) {
		r_unknown = false;
		switch (p_type) {
			case SoundNodeType::Sampler: {
				const std::string_view e = read_enum(p_s, "Loop");
				SamplerLoop l = e == "LtNone"			  ? SamplerLoop::None
						: e == "LtForward"				  ? SamplerLoop::Forward
						: e == "LtRelease"				  ? SamplerLoop::Release
						: e == "LtInstantRelease"		  ? SamplerLoop::InstantRelease
														  : SamplerLoop::Unknown;
				r_unknown = l == SamplerLoop::Unknown;
				return uint8_t(l);
			}
			case SoundNodeType::SimpleTransform: {
				const SimpleTransformOp op = simple_transform_op(read_enum(p_s, "Operation"));
				r_unknown = op == SimpleTransformOp::Unknown;
				const bool degrees = read_enum(p_s, "AngleUnit") == "AngleUnit_Degrees";
				return uint8_t(uint8_t(op) | (degrees ? port::kSimpleTransformDegrees : 0));
			}
			case SoundNodeType::LogicalExpression: {
				const std::string_view e = read_enum(p_s, "Operator");
				LogicalOp op = e == "LogicalExpressionOperator_And"	  ? LogicalOp::And
						: e == "LogicalExpressionOperator_Or"		  ? LogicalOp::Or
						: e == "LogicalExpressionOperator_Nand"		  ? LogicalOp::Nand
						: e == "LogicalExpressionOperator_Nor"		  ? LogicalOp::Nor
																	  : LogicalOp::Unknown;
				r_unknown = op == LogicalOp::Unknown;
				return uint8_t(op);
			}
			case SoundNodeType::MultiCrossfaderGroup: {
				const std::string_view e = read_enum(p_s, "FadeType");
				FaderType f = e == "FaderType_Linear" ? FaderType::Linear : e == "FaderType_Cosine" ? FaderType::Cosine : FaderType::Unknown;
				r_unknown = f == FaderType::Unknown;
				return uint8_t(f);
			}
			case SoundNodeType::ConditionGroup: {
				static constexpr std::pair<std::string_view, ConditionOp> kOps[] = {
					{ "ConditionType_And", ConditionOp::And },
					{ "ConditionType_Or", ConditionOp::Or },
					{ "ConditionType_XOr", ConditionOp::XOr },
					{ "ConditionType_Equal", ConditionOp::Equal },
					{ "ConditionType_Less", ConditionOp::Less },
					{ "ConditionType_Greater", ConditionOp::Greater },
					{ "ConditionType_LessOrEqual", ConditionOp::LessOrEqual },
					{ "ConditionType_GreaterOrEqual", ConditionOp::GreaterOrEqual },
				};
				const std::string_view e = read_enum(p_s, "Condition");
				for (const auto &[name, op] : kOps) {
					if (name == e) {
						return uint8_t(op);
					}
				}
				r_unknown = true;
				return uint8_t(ConditionOp::Unknown);
			}
			case SoundNodeType::NumberGenerator: {
				const bool known = read_enum(p_s, "Mode") == "NumberGeneratorMode_RandomUniform";
				r_unknown = !known;
				return uint8_t(known ? NumberGeneratorMode::RandomUniform : NumberGeneratorMode::Unknown);
			}
			case SoundNodeType::Timer: {
				const std::string_view e = read_enum(p_s, "Mode");
				TimerMode m = e == "TimerMode_Repeating" ? TimerMode::Repeating : e == "TimerMode_SingleShot" ? TimerMode::SingleShot : TimerMode::Unknown;
				r_unknown = m == TimerMode::Unknown;
				return uint8_t(m);
			}
			default:
				return 0;
		}
	}

	uint8_t flags_of(SoundNodeType p_type, const EbxStruct &p_s) {
		uint8_t f = 0;
		switch (p_type) {
			case SoundNodeType::MultiCrossfader:
				f |= read_bool(p_s, "LockControlValue") ? kFlagLockControlValue : 0;
				break;
			case SoundNodeType::Sampler:
				f |= read_bool(p_s, "ShuffleSegments") ? kFlagShuffleSegments : 0;
				break;
			case SoundNodeType::WaveSwitcher:
				f |= read_bool(p_s, "IsRandom") ? kFlagIsRandom : 0;
				f |= read_bool(p_s, "RandomStartIndex") ? kFlagRandomStartIndex : 0;
				break;
			case SoundNodeType::Lfo:
				f |= read_bool(p_s, "StartAtRandomValue") ? kFlagStartAtRandomValue : 0;
				break;
			case SoundNodeType::DeMux:
				f |= read_bool(p_s, "WrapValue") ? kFlagWrapValue : 0;
				break;
			case SoundNodeType::ConeOutput:
			case SoundNodeType::FlatOutput:
			case SoundNodeType::DirectOutput:
				f |= read_bool(p_s, "EnableHdr") ? kFlagEnableHdr : 0;
				break;
			default:
				break;
		}
		return f;
	}

	Status read_body(SoundNodeType p_type, const EbxStruct &p_s, SoundNode &r_node) {
		const SoundNodeTypeInfo &info = sound_node_type_info(p_type);
		r_node.type = p_type;
		r_node.ports_begin = uint32_t(prog.ports.size());
		const uint32_t audio = port::audio_ports(p_type);
		for (size_t i = 0; i < info.ports.size(); ++i) {
			BF3_TRY(budget.charge(1));
			Result<SoundPort> p = read_port(p_s, info.ports[i]);
			if (!p) {
				return p.error();
			}
			if ((audio >> i) & 1u) {
				*p = SoundPort{};
			}
			prog.ports.push_back(*p);
		}
		r_node.ports_count = uint16_t(info.ports.size());
		r_node.floats_begin = uint32_t(prog.floats.size());
		for (std::string_view f : info.floats) {
			BF3_TRY(budget.charge(1));
			prog.floats.push_back(read_float(p_s, f));
		}
		r_node.floats_count = uint16_t(info.floats.size());
		bool unknown = false;
		r_node.option = option_of(p_type, p_s, unknown);
		if (unknown && !options.allow_unimplemented) {
			return error(std::string(info.ebx_name) + ": unknown enumerator of " + std::string(info.option));
		}
		r_node.flags = flags_of(p_type, p_s);
		return Status::success();
	}

	Result<uint16_t> wave_ref(const EbxValue &p_value) {
		BF3_TRY(budget.charge(1));
		const std::optional<EbxRef> ref = p_value.as_ref();
		if (!ref || ref->is_null()) {
			return kNoRegister;
		}
		const Guid partition = ref->kind == EbxRef::Kind::External ? ref->partition : part.guid();
		auto [it, fresh] = wave_index.emplace(partition, uint16_t(prog.waves.size()));
		if (fresh) {
			if (prog.waves.size() >= kMaxProgramWaves) {
				return error("more than " + std::to_string(kMaxProgramWaves) + " waves");
			}
			SoundWaveRef w;
			w.partition = partition;
			if (std::optional<std::string_view> n = db.name_of(partition)) {
				BF3_TRY(budget.charge_bytes(n->size()));
				w.name = std::string(*n);
			}
			prog.waves.push_back(std::move(w));
		}
		return it->second;
	}

	Result<MasterBus> bus_of(const EbxStruct &p_s, std::string_view p_field) {
		const std::optional<EbxValue> v = p_s.get(p_field);
		if (!v) {
			return MasterBus::Unknown;
		}
		BF3_TRY(budget.charge(1));
		const std::optional<EbxRef> ref = v->as_ref();
		if (!ref || ref->is_null()) {
			return MasterBus::Unknown;
		}
		std::optional<EbxInstance> bus = ref->kind == EbxRef::Kind::Internal ? part.resolve(*ref) : db.resolve(part, *ref);
		if (bus) {
			if (ref->kind == EbxRef::Kind::External) {
				note_dependency(ref->partition);
			}
			BF3_TRY(budget.charge_type(bus->data()));
			const std::string_view name = bus->get("BusName") ? bus->get("BusName")->as_str().value_or("") : std::string_view();
			return master_bus_of_name(name);
		}

		static constexpr std::pair<std::string_view, MasterBus> kGuids[] = {
			{ "b735bfcd-0afb-4210-8ca2-6f9c61989002", MasterBus::GameMixMain },
			{ "d72517d3-3816-421a-aca1-f75078942be9", MasterBus::Reverb },
			{ "9384b910-4438-438a-a545-27a827308df0", MasterBus::GameMixAlt },
			{ "84f7080e-f0bb-45fd-834f-2ae867d17175", MasterBus::DirectOut },
			{ "14706325-7045-4c3f-be6b-f6e8fee4d0e6", MasterBus::Dialog },
			{ "fcef9739-f135-4f90-9829-765512ce4a5e", MasterBus::Lfe },
			{ "8df9c7ba-5e20-462e-b93b-e7746a8c6afa", MasterBus::Music },
			{ "dda9bec8-169a-433f-85b2-41419cdc29ec", MasterBus::Movies },
			{ "49bdaefa-67e4-4781-9ef9-48569c55121b", MasterBus::LoadingMusic },
			{ "84003c1f-e9c7-4e29-bcb2-d37c49f59690", MasterBus::NarrVoice },
		};
		const std::string id = ref->instance.to_string();
		for (const auto &[g, b] : kGuids) {
			if (g == id) {
				return b;
			}
		}
		return MasterBus::Unknown;
	}

	Status read_output(SoundNodeType p_type, const EbxStruct &p_s, SoundNode &r_node) {
		SoundOutput out;
		out.kind = p_type == SoundNodeType::ConeOutput ? OutputKind::Cone : p_type == SoundNodeType::FlatOutput ? OutputKind::Flat : OutputKind::Direct;
		out.name_hash = u32_field(p_s, "OutputNameHash");
		out.gain_db = read_float(p_s, "Gain");
		out.min_distance = read_float(p_s, "MinDistance");
		out.hf_damping_distance = read_float(p_s, "HFDampingDistance");
		out.hf_damping_obstruction = read_float(p_s, "HFDampingObstruction");
		out.hf_damping_occlusion = read_float(p_s, "HFDampingOcclusion");
		out.hf_damping_angle = read_float(p_s, "HFDampingAngle");
		out.reverb_gain_db = p_s.get("ReverbGain") ? read_float(p_s, "ReverbGain") : -96.0f;
		out.pan_size = read_float(p_s, "PanSize");
		out.angle = read_float(p_s, "Angle");
		out.outside_gain_db = read_float(p_s, "OutsideGain");
		if (std::optional<EbxValue> d = p_s.get("Direction")) {
			if (std::optional<core::Vec3> v = d->as_vec3()) {
				out.direction[0] = std::isfinite((*v)[0]) ? (*v)[0] : 0.0f;
				out.direction[1] = std::isfinite((*v)[1]) ? (*v)[1] : 0.0f;
				out.direction[2] = std::isfinite((*v)[2]) ? (*v)[2] : 1.0f;
			}
		}
		out.enable_hdr = read_bool(p_s, "EnableHdr");
		const std::string_view ts = read_enum(p_s, "TransformSource");
		out.transform = ts == "OutputTransformSource_Sound"		 ? OutputTransformSource::Sound
				: ts == "OutputTransformSource_Listener"		 ? OutputTransformSource::Listener
				: ts == "OutputTransformSource_Output"			 ? OutputTransformSource::Output
																 : OutputTransformSource::Unknown;
		Result<MasterBus> main = bus_of(p_s, "MainSend");
		if (!main) {
			return main.error();
		}
		out.main_send = *main;
		Result<MasterBus> reverb = bus_of(p_s, "ReverbSend");
		if (!reverb) {
			return reverb.error();
		}
		out.reverb_send = *reverb;

		if (std::optional<EbxValue> c = p_s.get("AttenuationCurve"); c && c->as_struct()) {
			const std::optional<EbxValue> pts = c->as_struct()->get("Points");
			if (pts && pts->as_array()) {
				if (pts->as_array()->size() > kMaxCurvePoints) {
					return error("attenuation curve with " + std::to_string(pts->as_array()->size()) + " points");
				}
				for (EbxValue pv : *pts->as_array()) {
					BF3_TRY(budget.charge(1));
					const std::optional<EbxStruct> ps = pv.as_struct();
					if (!ps) {
						continue;
					}
					AudioCurvePoint pt{ read_float(*ps, "X"), read_float(*ps, "Y"), read_float(*ps, "K") };
					out.attenuation.points.push_back(pt);
				}
				std::stable_sort(out.attenuation.points.begin(), out.attenuation.points.end(),
						[](const AudioCurvePoint &a, const AudioCurvePoint &b) { return a.x < b.x; });
			}
		}
		const char *kRefs[port::kOutputPlugins] = { "LowPassPlugin", "PanPlugin", "MainSendPlugin", "ReverbSendPlugin", "VuPlugin" };
		r_node.plugins_begin = uint32_t(prog.plugin_refs.size());
		for (const char *f : kRefs) {
			BF3_TRY(budget.charge(1));
			Result<SoundPluginRef> r = read_plugin_ref(p_s, f);
			if (!r) {
				return r.error();
			}
			prog.plugin_refs.push_back(*r);
		}
		r_node.plugins_count = port::kOutputPlugins;
		out.low_pass = prog.plugin_refs[r_node.plugins_begin + port::OutLowPass];
		out.pan = prog.plugin_refs[r_node.plugins_begin + port::OutPan];
		out.main_send_plugin = prog.plugin_refs[r_node.plugins_begin + port::OutMainSend];
		out.reverb_send_plugin = prog.plugin_refs[r_node.plugins_begin + port::OutReverbSend];
		out.vu = prog.plugin_refs[r_node.plugins_begin + port::OutVu];
		if (prog.outputs.size() >= kMaxProgramVoices) {
			return error("too many output nodes");
		}
		r_node.output = uint16_t(prog.outputs.size());
		prog.outputs.push_back(std::move(out));
		return Status::success();
	}

	Status read_children(SoundNodeType p_type, const EbxStruct &p_s, SoundNode &r_node) {
		const SoundNodeType child = port::child_type(p_type);
		if (child == SoundNodeType::Count) {
			return Status::success();
		}
		const std::string_view field = sound_node_type_info(p_type).children;
		const std::optional<EbxValue> arr = p_s.get(field);
		r_node.children_begin = uint32_t(prog.children.size());
		if (!arr || !arr->as_array()) {
			return Status::success();
		}
		if (arr->as_array()->size() > kMaxNodeChildren) {
			return error(std::string(field) + " has " + std::to_string(arr->as_array()->size()) + " records");
		}
		for (EbxValue cv : *arr->as_array()) {
			Result<std::optional<EbxInstance>> ci = internal(cv);
			if (!ci) {
				return ci.error();
			}
			if (!*ci) {
				continue;
			}
			BF3_TRY(budget.charge_type((*ci)->data()));
			const std::optional<SoundNodeType> ct = sound_node_type((*ci)->type_name());
			if (!ct || *ct != child) {
				return error(std::string(field) + " holds a " + std::string((*ci)->type_name()) + ", expected " + std::string(sound_node_type_info(child).ebx_name));
			}
			SoundNode rec;
			rec.source_index = uint32_t(r_node.children_count);
			BF3_TRY(read_body(child, (*ci)->data(), rec));
			const EbxStruct cd = (*ci)->data();
			if (child == SoundNodeType::ReceiveEntry) {

				SoundPort source;
				if (std::optional<EbxValue> src = cd.get("Source")) {
					Result<std::optional<EbxInstance>> si = internal(*src);
					if (!si) {
						return si.error();
					}
					if (*si) {
						const int64_t index = (*si)->get("ValueIndex") ? (*si)->get("ValueIndex")->as_i64().value_or(-1) : -1;
						if (index < 0 || index >= int64_t(kMaxProgramValues)) {
							return error("ReceiveEntry source ValueIndex " + std::to_string(index));
						}
						source.reg = uint16_t(index);
						max_register = std::max(max_register, uint32_t(index));
					}
				}
				prog.ports.push_back(source);
				++rec.ports_count;
			} else if (child == SoundNodeType::MixerReceiveEntry) {
				SoundMixerInput mi;
				mi.out = prog.ports[rec.ports_begin + port::MixerEntryOut].reg;
				if (std::optional<EbxValue> m = cd.get("Mixer"); m && m->as_ref() && !m->as_ref()->is_null()) {
					BF3_TRY(budget.charge(1));
					if (m->as_ref()->kind == EbxRef::Kind::External) {
						mi.mixer = m->as_ref()->partition;
						note_dependency(mi.mixer);
					}
				}
				if (std::optional<EbxValue> src = cd.get("Source")) {
					Result<std::optional<EbxInstance>> si = internal(*src);
					if (!si) {
						return si.error();
					}
					if (*si) {
						mi.name_hash = u32_field((*si)->data(), "NameHash");
					}
				}
				rec.aux = uint32_t(prog.mixer_inputs.size());
				prog.mixer_inputs.push_back(mi);
			}
			prog.children.push_back(uint32_t(prog.child_nodes.size()));
			prog.child_nodes.push_back(rec);
			++r_node.children_count;
			++stats.child_records;
		}
		return Status::success();
	}

	Status read_node_extras(SoundNodeType p_type, const EbxStruct &p_s, SoundNode &r_node) {
		r_node.waves_begin = uint32_t(prog.wave_refs.size());
		r_node.plugins_begin = uint32_t(prog.plugin_refs.size());
		switch (p_type) {
			case SoundNodeType::Sampler: {
				const std::optional<EbxValue> w = p_s.get("Wave");
				Result<uint16_t> wi = w ? wave_ref(*w) : Result<uint16_t>(kNoRegister);
				if (!wi) {
					return wi.error();
				}
				prog.wave_refs.push_back(*wi);
				r_node.waves_count = 1;
				const std::optional<EbxValue> pl = p_s.get("Plugins");
				if (pl && pl->as_array()) {
					if (pl->as_array()->size() > 16) {
						return error("sampler with " + std::to_string(pl->as_array()->size()) + " voice slots");
					}
					for (EbxValue sv : *pl->as_array()) {
						BF3_TRY(budget.charge(1));
						const std::optional<EbxStruct> s = sv.as_struct();
						if (!s) {
							return error("sampler plugin slot is not a struct");
						}
						for (const char *f : { "SndPlayer", "Resample", "Pause", "Gain" }) {
							Result<SoundPluginRef> r = read_plugin_ref(*s, f);
							if (!r) {
								return r.error();
							}
							prog.plugin_refs.push_back(*r);
						}
						r_node.plugins_count = uint16_t(r_node.plugins_count + port::kSamplerSlotPlugins);
					}
				}
				r_node.floats_count = uint16_t(r_node.floats_count);
				break;
			}
			case SoundNodeType::WaveSwitcher: {
				const std::optional<EbxValue> arr = p_s.get("Waves");
				if (arr && arr->as_array()) {
					if (arr->as_array()->size() > kMaxProgramWaves) {
						return error("WaveSwitcher with " + std::to_string(arr->as_array()->size()) + " waves");
					}
					for (EbxValue wv : *arr->as_array()) {
						Result<uint16_t> wi = wave_ref(wv);
						if (!wi) {
							return wi.error();
						}
						prog.wave_refs.push_back(*wi);
						++r_node.waves_count;
					}
				}
				break;
			}
			case SoundNodeType::SimpleValueSelector: {
				const std::optional<EbxValue> arr = p_s.get("OutValues");
				if (arr && arr->as_array()) {
					if (arr->as_array()->size() > kMaxNodeChildren) {
						return error("SimpleValueSelector with " + std::to_string(arr->as_array()->size()) + " values");
					}
					for (EbxValue fv : *arr->as_array()) {
						BF3_TRY(budget.charge(1));
						const float f = fv.as_f32().value_or(0.0f);
						prog.floats.push_back(std::isfinite(f) ? f : 0.0f);
						++r_node.floats_count;
					}
				}
				break;
			}
			case SoundNodeType::Gain:
			case SoundNodeType::HighPassIir2:
			case SoundNodeType::LowPassIir2:
			case SoundNodeType::BandPassIir2:
			case SoundNodeType::PeakingIir2:
			case SoundNodeType::Delay: {
				BF3_TRY(budget.charge(1));
				Result<SoundPluginRef> r = read_plugin_ref(p_s, "Plugin");
				if (!r) {
					return r.error();
				}
				prog.plugin_refs.push_back(*r);
				r_node.plugins_count = 1;
				break;
			}
			case SoundNodeType::ConeOutput:
			case SoundNodeType::FlatOutput:
			case SoundNodeType::DirectOutput:
				return read_output(p_type, p_s, r_node);
			default:
				break;
		}
		return Status::success();
	}

	Status read_graph_info(const EbxStruct &p_graph) {
		const std::optional<EbxValue> iv = p_graph.get("Info");
		const std::optional<EbxStruct> info = iv ? iv->as_struct() : std::nullopt;
		if (!info) {
			return error("SoundGraphData has no Info");
		}

		const std::optional<EbxValue> conns = info->get("Connections");
		if (conns && conns->as_array()) {
			if (conns->as_array()->size() > kMaxProgramConnections) {
				return error(std::to_string(conns->as_array()->size()) + " connections");
			}
			for (EbxValue cv : *conns->as_array()) {
				BF3_TRY(budget.charge(1));
				const std::optional<EbxStruct> c = cv.as_struct();
				if (!c) {
					return error("connection is not a struct");
				}
				SoundConnection sc;
				const std::string_view kind = read_enum(*c, "ConnectionType");
				sc.to_bus = kind == "SoundGraphPluginConnectionType_Bus";
				if (sc.to_bus) {
					Result<MasterBus> b = bus_of(*c, "Bus");
					if (!b) {
						return b.error();
					}
					sc.bus = *b;
				} else {
					const int64_t voice = c->get("VoiceIndex") ? c->get("VoiceIndex")->as_i64().value_or(-1) : -1;
					if (voice < 0 || voice >= int64_t(kMaxProgramVoices)) {
						return error("connection to voice " + std::to_string(voice));
					}
					sc.voice = uint16_t(voice);
				}
				prog.connections.push_back(sc);
			}
		}
		const std::optional<EbxValue> cps = info->get("ConstructParams");
		if (cps && cps->as_array()) {
			if (cps->as_array()->size() > kMaxProgramVoices * kMaxVoicePlugins) {
				return error("too many construct params");
			}
			for (EbxValue cv : *cps->as_array()) {
				BF3_TRY(budget.charge(1));
				const std::optional<EbxStruct> c = cv.as_struct();
				prog.construct_params.push_back(c ? read_float(*c, "Value") : 0.0f);
			}
		}
		const std::optional<EbxValue> voices = info->get("Voices");
		if (voices && voices->as_array()) {
			if (voices->as_array()->size() > kMaxProgramVoices) {
				return error(std::to_string(voices->as_array()->size()) + " voices (at most " + std::to_string(kMaxProgramVoices) + ")");
			}
			std::vector<uint8_t> named(prog.connections.size(), 0);
			for (EbxValue vv : *voices->as_array()) {
				BF3_TRY(budget.charge(1));
				const std::optional<EbxStruct> v = vv.as_struct();
				if (!v) {
					return error("voice is not a struct");
				}
				SoundVoice sv;
				sv.stage = int8_t(std::clamp<int64_t>(v->get("ProcessingStage") ? v->get("ProcessingStage")->as_i64().value_or(0) : 0, -128, 127));
				sv.plugins_begin = uint32_t(prog.plugins.size());
				const std::optional<EbxValue> pl = v->get("Plugins");
				if (pl && pl->as_array()) {
					if (pl->as_array()->size() > kMaxVoicePlugins) {
						return error("voice with " + std::to_string(pl->as_array()->size()) + " plugins");
					}
					for (EbxValue pv : *pl->as_array()) {
						BF3_TRY(budget.charge(1));
						const std::optional<EbxStruct> p = pv.as_struct();
						if (!p) {
							return error("plugin is not a struct");
						}
						SoundVoicePlugin sp;
						sp.id = u32_field(*p, "Id");
						sp.channels = uint8_t(std::clamp<int64_t>(p->get("OutputChannelCount") ? p->get("OutputChannelCount")->as_i64().value_or(0) : 0, 0, 255));
						const int64_t ci = p->get("ConnectionIndex") ? p->get("ConnectionIndex")->as_i64().value_or(-1) : -1;
						if (ci < 0 || ci > int64_t(prog.connections.size())) {
							return error("ConnectionIndex " + std::to_string(ci) + " outside 0..." + std::to_string(prog.connections.size()) + " (1-based, 0 = none)");
						}
						if (ci != int64_t(kStoredConnectionNone)) {
							if (named[size_t(ci - 1)]) {
								return error("connection " + std::to_string(ci) + " is named by two plugins");
							}
							named[size_t(ci - 1)] = 1;
							sp.connection = uint16_t(ci - 1);
						}
						const int64_t cb = p->get("ConstructParamsIndex") ? p->get("ConstructParamsIndex")->as_i64().value_or(0) : 0;
						const int64_t cc = p->get("ConstructParamCount") ? p->get("ConstructParamCount")->as_i64().value_or(0) : 0;
						if (cb < 0 || cc < 0 || uint64_t(cb) + uint64_t(cc) > prog.construct_params.size()) {
							return error("construct params [" + std::to_string(cb) + ", +" + std::to_string(cc) + ") outside " +
									std::to_string(prog.construct_params.size()));
						}
						sp.construct_begin = uint32_t(cb);
						sp.construct_count = uint16_t(cc);
						sp.attribute_mask = u32_field(*p, "EnableAttributeReadMask");
						prog.plugins.push_back(sp);
						++sv.plugins_count;
					}
				}
				prog.voices.push_back(sv);
			}
			for (size_t i = 0; i < named.size(); ++i) {
				if (!named[i]) {
					return error("connection " + std::to_string(i + 1) + " is named by no plugin");
				}
			}
		} else if (!prog.connections.empty()) {
			return error("connections without voices");
		}
		stats.connections += prog.connections.size();

		prog.voice_order.resize(prog.voices.size());
		for (size_t i = 0; i < prog.voices.size(); ++i) {
			prog.voice_order[i] = uint16_t(i);
		}
		std::stable_sort(prog.voice_order.begin(), prog.voice_order.end(),
				[&](uint16_t a, uint16_t b) { return prog.voices[a].stage < prog.voices[b].stage; });

		const std::optional<EbxValue> links = info->get("LinkedPluginAttributes");
		if (links && links->as_array()) {
			if (links->as_array()->size() > kMaxProgramVoices * kMaxVoicePlugins) {
				return error("too many linked plugin attributes");
			}
			for (EbxValue lv : *links->as_array()) {
				BF3_TRY(budget.charge(1));
				const std::optional<EbxStruct> l = lv.as_struct();
				if (!l) {
					return error("linked attribute is not a struct");
				}
				SoundPluginLink link;
				link.node = UINT32_MAX;
				if (std::optional<EbxValue> nv = l->get("Node")) {
					Result<std::optional<EbxInstance>> ni = internal(*nv);
					if (!ni) {
						return ni.error();
					}
					if (*ni && (*ni)->partition().guid() == part.guid() && (*ni)->index() < node_of_instance.size() && node_of_instance[(*ni)->index()] >= 0) {
						link.node = uint32_t(node_of_instance[(*ni)->index()]);
					}
				}
				Result<SoundPluginRef> pr = read_plugin_ref(*l, "Plugin");
				if (!pr) {
					return pr.error();
				}
				link.plugin = *pr;
				const int64_t attr = l->get("AttributeIndex") ? l->get("AttributeIndex")->as_i64().value_or(-1) : -1;
				if (attr < 0 || attr >= int64_t(port::kMaxPluginAttributes)) {
					return error("linked attribute index " + std::to_string(attr));
				}
				link.attribute = uint8_t(attr);
				link.allow_extreme = read_bool(*l, "AllowExtremeValues");
				Result<SoundPort> p = read_port(*l, "Port");
				if (!p) {
					return p.error();
				}
				link.port = *p;
				if (link.node == UINT32_MAX && !options.allow_unimplemented) {
					return error("linked attribute names no node of the graph");
				}
				prog.links.push_back(link);
			}
		}
		return Status::success();
	}

	Result<std::vector<uint32_t>> sort_nodes() {
		const size_t n = prog.nodes.size();
		const uint32_t regs = prog.registers;

		std::vector<std::vector<uint16_t>> reads(n), writes(n);
		auto collect = [&](const SoundNode &p_node, size_t p_owner) {
			const uint32_t out = port::output_ports(p_node.type);
			for (uint16_t i = 0; i < p_node.ports_count; ++i) {
				const SoundPort &p = prog.ports[p_node.ports_begin + i];
				if (!p.connected()) {
					continue;
				}
				((out >> i) & 1u ? writes : reads)[p_owner].push_back(p.reg);
			}
		};
		for (size_t i = 0; i < n; ++i) {
			collect(prog.nodes[i], i);
			for (uint16_t c = 0; c < prog.nodes[i].children_count; ++c) {
				collect(prog.child_nodes[prog.children[prog.nodes[i].children_begin + c]], i);
			}
		}
		return order_by_register_dependencies(reads, writes, regs, budget, stats.feedback_edges);
	}

	Result<SoundProgram> run() {
		const EbxInstance primary = part.primary();
		BF3_TRY(budget.charge_type(primary.data()));
		if (std::optional<std::string_view> n = db.name_of(part.guid())) {
			BF3_TRY(budget.charge_bytes(n->size()));
			prog.name = std::string(*n);
		}
		prog.partition = part.guid();
		master = primary.is_a("SoundMasterPatchAsset");
		if (!master && !primary.is_a("SoundPatchAsset")) {
			return error("primary instance is " + std::string(primary.type_name()) + ", not a SoundPatchAsset");
		}
		const EbxStruct pd = primary.data();
		if (!master) {
			prog.loudness_db = read_float(pd, "Loudness");
			prog.radius = read_float(pd, "Radius");
			prog.master_pitch = read_float(pd, "MasterPitch", 1.0f);
			if (!(prog.master_pitch > 0.0f)) {
				prog.master_pitch = 1.0f;
			}
			prog.doppler_factor = read_float(pd, "DopplerFactor");
			prog.ai_loudness = read_float(pd, "AILoudness");
			prog.is_looping = read_bool(pd, "IsLooping");
			for (auto [field, out] : { std::pair{ "DefaultStartEvent", &prog.start_event }, std::pair{ "DefaultStopEvent", &prog.stop_event } }) {
				if (std::optional<EbxValue> e = pd.get(field)) {
					Result<std::optional<EbxInstance>> ei = internal(*e);
					if (!ei) {
						return ei.error();
					}
					if (*ei) {
						*out = u32_field((*ei)->data(), "NameHash");
					}
				}
			}
		}
		const std::optional<EbxValue> gv = pd.get("Graph");
		Result<std::optional<EbxInstance>> gi = gv ? internal(*gv) : Result<std::optional<EbxInstance>>(std::optional<EbxInstance>());
		if (!gi) {
			return gi.error();
		}
		if (!*gi || !(*gi)->is_a("SoundGraphData")) {
			return error("no SoundGraphData");
		}
		BF3_TRY(budget.charge_type((*gi)->data()));
		const EbxStruct graph = (*gi)->data();
		const int64_t value_count = graph.get("ValueCount") ? graph.get("ValueCount")->as_i64().value_or(0) : 0;
		if (value_count < 0 || value_count > int64_t(kMaxProgramValues)) {
			return error("ValueCount " + std::to_string(value_count));
		}

		for (auto [field, is_event] : { std::pair{ "InputParameters", false }, std::pair{ "InputEvents", true } }) {
			const std::optional<EbxValue> arr = graph.get(field);
			if (!arr || !arr->as_array()) {
				continue;
			}
			if (arr->as_array()->size() > kMaxProgramValues) {
				return error(std::string(field) + " too long");
			}
			for (EbxValue iv : *arr->as_array()) {
				Result<std::optional<EbxInstance>> ii = internal(iv);
				if (!ii) {
					return ii.error();
				}
				if (!*ii) {
					continue;
				}
				SoundInput in;
				in.event = is_event;
				in.name_hash = u32_field((*ii)->data(), "NameHash");
				const int64_t index = (*ii)->get("ValueIndex") ? (*ii)->get("ValueIndex")->as_i64().value_or(-1) : -1;
				if (index < 0 || index >= int64_t(kMaxProgramValues)) {
					return error("input ValueIndex " + std::to_string(index));
				}
				in.reg = uint16_t(index);
				max_register = std::max(max_register, uint32_t(index));
				in.default_value = (*ii)->get("DefaultValue") ? (*ii)->get("DefaultValue")->as_f32().value_or(0.0f) : 0.0f;
				if (!std::isfinite(in.default_value)) {
					in.default_value = 0.0f;
				}
				prog.inputs.push_back(in);
			}
		}

		const std::optional<EbxValue> nodes = graph.get("Nodes");
		if (!nodes || !nodes->as_array()) {
			return error("SoundGraphData has no Nodes");
		}
		if (nodes->as_array()->size() > kMaxProgramNodes) {
			return error(std::to_string(nodes->as_array()->size()) + " nodes (at most " + std::to_string(kMaxProgramNodes) + ")");
		}
		node_of_instance.assign(part.instance_count(), -1);
		std::vector<SoundNode> source_nodes;
		for (EbxValue nv : *nodes->as_array()) {
			Result<std::optional<EbxInstance>> ni = internal(nv);
			if (!ni) {
				return ni.error();
			}
			if (!*ni) {
				continue;
			}
			const EbxInstance inst = **ni;
			BF3_TRY(budget.charge_type(inst.data()));
			++stats.nodes;
			const std::optional<SoundNodeType> type = sound_node_type(inst.type_name());
			if (!type) {
				++stats.unknown_types;
				const std::string t(inst.type_name());
				if (std::find(stats.unknown_type_names.begin(), stats.unknown_type_names.end(), t) == stats.unknown_type_names.end()) {
					stats.unknown_type_names.push_back(t);
				}
				if (!options.allow_unimplemented) {
					return error("unknown node type " + t);
				}
				continue;
			}
			const SoundNodeTypeInfo &info = sound_node_type_info(*type);
			if (info.role == SoundNodeRole::Child) {
				return error(std::string(info.ebx_name) + " listed as a node");
			}
			SoundNode node;
			node.source_index = uint32_t(source_nodes.size());
			if (!info.a2) {
				++stats.unimplemented_nodes;
				if (!options.allow_unimplemented) {
					return error("node type " + std::string(info.ebx_name) + " is not implemented in A2");
				}
				node.type = *type;
			} else {
				BF3_TRY(read_body(*type, inst.data(), node));
				BF3_TRY(read_children(*type, inst.data(), node));
				BF3_TRY(read_node_extras(*type, inst.data(), node));
			}
			if (inst.partition().guid() == part.guid() && inst.index() < node_of_instance.size()) {
				node_of_instance[inst.index()] = int32_t(source_nodes.size());
			}
			source_nodes.push_back(node);
		}
		prog.nodes = std::move(source_nodes);
		BF3_TRY(read_graph_info(graph));

		prog.registers = std::max<uint32_t>(uint32_t(value_count), max_register + 1);
		if (prog.registers > kMaxProgramValues) {
			return error(std::to_string(prog.registers) + " registers");
		}

		auto valid_plugin = [&](const SoundPluginRef &p_ref) {
			return !p_ref.valid() || (p_ref.voice < prog.voices.size() && p_ref.plugin < prog.voices[p_ref.voice].plugins_count);
		};
		for (const SoundPluginRef &r : prog.plugin_refs) {
			if (!valid_plugin(r)) {
				return error("plugin ref (" + std::to_string(r.voice) + ", " + std::to_string(r.plugin) + ") names no plugin");
			}
		}
		for (const SoundPluginLink &l : prog.links) {
			if (!valid_plugin(l.plugin) || !l.plugin.valid()) {
				return error("linked attribute names no plugin");
			}
		}
		for (const SoundConnection &c : prog.connections) {
			if (!c.to_bus && c.voice >= prog.voices.size()) {
				return error("connection to voice " + std::to_string(c.voice) + " of " + std::to_string(prog.voices.size()));
			}
		}

		Result<std::vector<uint32_t>> order = sort_nodes();
		if (!order) {
			return order.error();
		}
		std::vector<uint32_t> new_index(prog.nodes.size());
		std::vector<SoundNode> sorted;
		sorted.reserve(prog.nodes.size());
		for (uint32_t i : *order) {
			new_index[i] = uint32_t(sorted.size());
			sorted.push_back(prog.nodes[i]);
		}
		prog.nodes = std::move(sorted);
		for (SoundPluginLink &l : prog.links) {
			if (l.node != UINT32_MAX) {
				l.node = new_index[l.node];
			}
		}
		for (size_t i = 0; i < prog.nodes.size(); ++i) {
			if (prog.nodes[i].output != kNoRegister) {
				prog.outputs[prog.nodes[i].output].node = uint32_t(i);
			}
		}
		std::sort(deps.begin(), deps.end());
		stats.registers = prog.registers;
		stats.voices = prog.voices.size();
		stats.plugins = prog.plugins.size();
		stats.dependencies = deps;
		BF3_TRY(budget.status());
		return std::move(prog);
	}
};

}

float AudioCurve::evaluate(float p_x) const {
	if (points.empty()) {
		return 0.0f;
	}
	if (!(p_x > points.front().x)) {
		return points.front().y;
	}
	if (!(p_x < points.back().x)) {
		return points.back().y;
	}
	size_t i = 0;
	while (i + 1 < points.size() && !(p_x < points[i + 1].x)) {
		++i;
	}
	const AudioCurvePoint &a = points[i];
	const AudioCurvePoint &b = points[i + 1];
	const float h = b.x - a.x;
	if (!(h > 0.0f)) {
		return b.y;
	}
	const float t = (p_x - a.x) / h;
	const float t2 = t * t, t3 = t2 * t;
	const float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
	const float h10 = t3 - 2.0f * t2 + t;
	const float h01 = -2.0f * t3 + 3.0f * t2;
	const float h11 = t3 - t2;
	return h00 * a.y + h10 * h * a.k + h01 * b.y + h11 * h * b.k;
}

std::optional<uint16_t> SoundProgram::input_register(uint32_t p_name_hash) const {
	for (const SoundInput &in : inputs) {
		if (in.name_hash == p_name_hash) {
			return in.reg;
		}
	}
	return std::nullopt;
}

Result<SoundPort> read_graph_port(const EbxStruct &p_node, std::string_view p_field) {
	const std::optional<EbxValue> v = p_node.get(p_field);
	const std::optional<EbxStruct> s = v ? v->as_struct() : std::nullopt;
	if (!s) {
		return make_error(std::string(p_node.type_name()) + " has no port " + std::string(p_field));
	}
	SoundPort port;
	const bool connected = s->get("IsConnected") && s->get("IsConnected")->as_bool().value_or(false);
	port.constant = s->get("UnconnectedValue") ? s->get("UnconnectedValue")->as_f32().value_or(0.0f) : 0.0f;
	if (!std::isfinite(port.constant)) {
		port.constant = 0.0f;
	}
	if (connected) {
		const int64_t index = s->get("ValueIndex") ? s->get("ValueIndex")->as_i64().value_or(-1) : -1;
		if (index < 0 || index >= int64_t(kMaxProgramValues)) {
			return make_error(std::string(p_node.type_name()) + "." + std::string(p_field) + ": ValueIndex " +
					std::to_string(index) + " out of range");
		}
		port.reg = uint16_t(index);
	}
	return port;
}

Result<std::vector<uint32_t>> order_by_register_dependencies(std::span<const std::vector<uint16_t>> p_reads,
		std::span<const std::vector<uint16_t>> p_writes, uint32_t p_registers, gameplay::WorkBudget &r_budget, size_t &r_feedback_edges) {
	if (p_reads.size() != p_writes.size()) {
		return make_error("sound program: node order: reads and writes differ in length");
	}
	const size_t n = p_reads.size();
	std::vector<std::vector<uint32_t>> writers(p_registers);
	for (size_t i = 0; i < n; ++i) {
		for (uint16_t r : p_writes[i]) {
			BF3_TRY(r_budget.charge(1));
			if (r >= p_registers) {
				return make_error("sound program: node order: register out of range");
			}
			if (writers[r].empty() || writers[r].back() != i) {
				writers[r].push_back(uint32_t(i));
			}
		}
	}
	std::vector<std::vector<uint32_t>> succ(n);
	std::vector<uint32_t> indegree(n, 0);
	std::vector<uint16_t> reads;
	for (size_t i = 0; i < n; ++i) {
		BF3_TRY(r_budget.charge(p_reads[i].size() + 1));
		reads.assign(p_reads[i].begin(), p_reads[i].end());
		std::sort(reads.begin(), reads.end());
		reads.erase(std::unique(reads.begin(), reads.end()), reads.end());
		for (uint16_t r : reads) {
			if (r >= p_registers) {
				return make_error("sound program: node order: register out of range");
			}
			for (uint32_t w : writers[r]) {
				BF3_TRY(r_budget.charge(1));
				if (w == i) {
					continue;
				}
				succ[w].push_back(uint32_t(i));
				++indegree[i];
			}
		}
	}
	std::priority_queue<uint32_t, std::vector<uint32_t>, std::greater<uint32_t>> ready;
	std::priority_queue<uint32_t, std::vector<uint32_t>, std::greater<uint32_t>> remaining;
	std::vector<uint8_t> done(n, 0);
	for (size_t i = 0; i < n; ++i) {
		if (indegree[i] == 0) {
			ready.push(uint32_t(i));
		}
		remaining.push(uint32_t(i));
	}
	std::vector<uint32_t> order;
	order.reserve(n);
	while (order.size() < n) {
		if (ready.empty()) {

			while (!remaining.empty() && done[remaining.top()]) {
				remaining.pop();
			}
			const uint32_t pick = remaining.top();
			r_feedback_edges += indegree[pick];
			indegree[pick] = 0;
			ready.push(pick);
		}
		const uint32_t at = ready.top();
		ready.pop();
		if (done[at]) {
			continue;
		}
		done[at] = 1;
		order.push_back(at);
		for (uint32_t s : succ[at]) {
			BF3_TRY(r_budget.charge(1));
			if (done[s] || indegree[s] == 0) {
				continue;
			}
			if (--indegree[s] == 0) {
				ready.push(s);
			}
		}
	}
	return order;
}

Result<SoundProgram> compile_sound_patch(const EbxResolver &p_db, const EbxPartition &p_partition, const CompileOptions &p_options,
		gameplay::WorkBudget &r_budget, CompileStats *r_stats) {
	CompileStats local;
	Compiler c{ p_db, p_partition, p_options, r_budget, r_stats ? *r_stats : local, {}, false, {}, {}, {}, 0 };
	return c.run();
}

}
