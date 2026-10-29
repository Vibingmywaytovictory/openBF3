#pragma once

#include "bf3/ai/knowledge/knowledge_context.h"

#include <cstdint>
#include <memory>
#include <string>

namespace bf3::ai {

class Information;
class IPIArmament;
class IPIControlInfo;
class IPICover;
class IPIMobile;
class IPIPhysical;
class IPIUnit;

class InformationPlugIn {
public:
	virtual ~InformationPlugIn() = default;
	virtual PlugInType type() const = 0;
	virtual void set_time() = 0;
	virtual void set_time(sim::Tick p_time) = 0;

protected:
	explicit InformationPlugIn(Information *p_information) : information_(p_information) {}
	Information *information_ = nullptr;
};

class Information {
public:
	Information(AiObject *p_object, sim::Tick p_time, const KnowledgeContext &p_context);
	virtual ~Information();
	Information(const Information &) = delete;
	Information &operator=(const Information &) = delete;

	virtual int information_sort() const;
	virtual bool set_time(sim::Tick p_time) = 0;
	virtual bool set_time(sim::Tick p_time, float p_weight) = 0;
	virtual sim::Tick time_stamp() const = 0;
	virtual void add_contact() = 0;
	virtual void remove_contact() = 0;
	virtual void remove_all_contacts() = 0;
	virtual bool has_contact() const = 0;
	virtual float security(sim::Tick p_time) const = 0;
	virtual float security() const = 0;
	virtual const Vec3 &position() const = 0;
	virtual float altitude() const = 0;
	virtual float altitude(const Vec3 &p_direction) const = 0;
	virtual const Transform &transformation() const = 0;
	virtual Vec3 facing() const = 0;
	virtual int side() const = 0;

	bool remove_plug_in(PlugInType p_type);
	void set_time_on_plug_ins(sim::Tick p_time);
	bool set_armament(std::unique_ptr<IPIArmament> p_armament);
	void borrow_armament(Information *p_lender);
	void reset_armament();
	bool set_control_info(std::unique_ptr<IPIControlInfo> p_control_info);
	bool set_cover(std::unique_ptr<IPICover> p_cover);
	bool set_mobile(std::unique_ptr<IPIMobile> p_mobile);
	bool set_physical(std::unique_ptr<IPIPhysical> p_physical);
	bool set_unit(std::unique_ptr<IPIUnit> p_unit);

	const std::string &object_type_name() const;
	uint32_t object_id() const;
	int player_id() const;
	bool is_primary() const;
	uint32_t primary_object(int p_side) const;
	uint32_t secondary_object(int &r_iterator, int p_side) const;
	uint32_t first_secondary_object(int &r_iterator, int p_side) const;
	int first_secondary_object_iterator() const;
	const std::string &name() const;
	const std::string &ai_name() const;
	bool object_spawner_holding() const;
	uint32_t object_spawner(int p_side) const;
	uint32_t handle() const;
	int kit_type() const;
	bool is_root_object() const;
	SoldierPose soldier_pose() const;

	int booking_player_id() const { return booking_player_id_; }
	bool is_booked() const { return booked_; }
	bool is_booked_by(int p_player_id) const { return booking_player_id_ == p_player_id; }
	void book_vehicle(int p_player_id);
	void cancel_book_vehicle();

	IPIArmament *armament() const;
	IPIControlInfo *control_info() const { return control_info_.get(); }
	IPICover *cover() const { return cover_.get(); }
	IPIMobile *mobile() const { return mobile_.get(); }
	IPIPhysical *physical() const { return physical_.get(); }
	IPIUnit *unit() const { return unit_.get(); }
	bool has_armament() const { return armament() != nullptr; }
	bool has_mobile() const { return mobile_ != nullptr; }
	uint32_t object_type_flags() const { return object_type_flags_; }
	float strength() const { return strength_; }
	float basic_temperature() const { return basic_temperature_; }
	sim::Tick time() const { return time_; }
	AiObject *ai_object() const { return object_; }
	const KnowledgeContext &context() const { return context_; }

protected:
	uint32_t object_type_flags_ = 0;
	sim::Tick allowed_time_diff_ = 0;
	float strength_ = 0.0f;
	float basic_temperature_ = 0.0f;
	sim::Tick time_ = 0;
	AiObject *object_ = nullptr;
	bool booked_ = false;
	int booking_player_id_ = -1;
	KnowledgeContext context_;

private:
	std::unique_ptr<IPIArmament> armament_;
	IPIArmament *borrowed_armament_ = nullptr;
	std::unique_ptr<IPIControlInfo> control_info_;
	std::unique_ptr<IPICover> cover_;
	std::unique_ptr<IPIMobile> mobile_;
	std::unique_ptr<IPIPhysical> physical_;
	std::unique_ptr<IPIUnit> unit_;
};

class InformationReal final : public Information {
public:
	InformationReal(AiObject *p_object, uint32_t p_types, float p_basic_temperature, sim::Tick p_time,
			sim::Tick p_allowed_time_diff, const KnowledgeContext &p_context);

	int information_sort() const override;
	bool set_time(sim::Tick p_time) override;
	bool set_time(sim::Tick p_time, float p_weight) override;
	sim::Tick time_stamp() const override;
	void add_contact() override;
	void remove_contact() override;
	void remove_all_contacts() override;
	bool has_contact() const override;
	float security(sim::Tick p_time) const override;
	float security() const override;
	const Vec3 &position() const override;
	float altitude() const override;
	float altitude(const Vec3 &p_direction) const override;
	const Transform &transformation() const override;
	Vec3 facing() const override;
	int side() const override;
};

}
