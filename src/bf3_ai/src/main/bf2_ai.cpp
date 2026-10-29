#include "bf3/ai/main/ai_main.h"

namespace bf3::ai {

BF2AI::~BF2AI() {
	clear();
}

bool BF2AI::init(const AiServices &p_services) {
	const bool was_uninitialized = !initialized_;
	shutting_down_ = true;
	if (!was_uninitialized) {
		return false;
	}
	init_order_.clear();
	information_ = std::make_unique<InformationManager>();
	init_order_.push_back(Part::InformationManager);
	weapons_ = std::make_unique<WeaponInfoManager>();
	init_order_.push_back(Part::WeaponInfoManager);
	initialized_ = true;
	knowledge_ = KnowledgeContext{};
	knowledge_.information = information_.get();
	knowledge_.weapons = weapons_.get();
	knowledge_.timer = p_services.timer;
	knowledge_.altitude = p_services.altitude;
	knowledge_.infantry_map = p_services.infantry_map;
	factory_ = std::make_unique<InformationFactory>(knowledge_);
	knowledge_ = factory_->context();
	init_order_.push_back(Part::InformationFactory);
	settings_ = std::make_unique<AiSettings>(p_services.settings);
	init_order_.push_back(Part::Settings);
	events_ = std::make_unique<AIEventCourier>();
	init_order_.push_back(Part::EventCourier);
	behaviours_ = std::make_unique<BotBehaviourManager>(*settings_);
	init_order_.push_back(Part::BehaviourManager);
	random_ = std::make_unique<AIRandom>(AIRandom::seed_for(p_services.match_seed, 0));
	random_->init();
	init_order_.push_back(Part::Random);
	AiMainServices main;
	main.settings = settings_.get();
	main.timer = p_services.timer;
	main.information = information_.get();
	main.behaviours = behaviours_.get();
	main.game = p_services.game;
	main.factory = p_services.factory;
	main.pathfinding = p_services.pathfinding;
	main.squads = p_services.squads;
	main_ = std::make_unique<AIMain>(main);
	init_order_.push_back(Part::Main);
	events_->attach(main_.get(), p_services.squads, p_services.level);
	shutting_down_ = false;
	return true;
}

bool BF2AI::clear() {
	if (!initialized_) {
		return true;
	}
	initialized_ = false;
	clear_order_.clear();
	factory_.reset();
	clear_order_.push_back(Part::InformationFactory);
	events_.reset();
	clear_order_.push_back(Part::EventCourier);
	behaviours_.reset();
	clear_order_.push_back(Part::BehaviourManager);
	main_.reset();
	clear_order_.push_back(Part::Main);
	information_.reset();
	clear_order_.push_back(Part::InformationManager);
	weapons_.reset();
	clear_order_.push_back(Part::WeaponInfoManager);
	settings_.reset();
	clear_order_.push_back(Part::Settings);
	random_.reset();
	clear_order_.push_back(Part::Random);
	knowledge_ = KnowledgeContext{};
	return true;
}

bool BF2AI::shutdown() {
	clear();
	return true;
}

bool BF2AI::update() {
	if (!initialized_) {
		return false;
	}
	events_->deliver();
	return main_->action();
}

}
