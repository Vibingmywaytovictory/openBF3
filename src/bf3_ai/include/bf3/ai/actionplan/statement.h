#pragma once

#include "bf3/ai/main/bot.h"

#include <cstdint>

namespace bf3::ai {

enum class BAPStatementType : int {
	Parallel = 0,
	Serial = 1,
	PlanEnd = 2,
	If = 3,
	Wait = 4,
	While = 5,
	AimAt = 6,
	MoveTo = 7,
	MoveToDirection = 8,
	MoveToObject = 9,
	MoveAwayFromObject = 10,
	Trigger = 11,
	TriggerContinously = 12,
	Idle = 13,
	ResetControls = 14,
	InfoWrapper = 15,
	AimingWrapper = 16,
	FinishedWrapper = 17,
	WeaponWrapper = 18,
	LookAtWrapper = 19,
	Sense = 20,
	OverrideSensingAgent = 21,
	PlaceC4 = 22,
	LookAt = 23,
	Turn = 24,
	Roll3d = 25,
	SoldierPose = 26,
	UpdateVehicle = 27,
	ChangeWeapon = 28,
	ICChangeVehicle = 29,
	ICTurnLock = 31,
	Evaluate = 32,
	NotifyBot = 33,
	PhysicsEnable = 34,
	SetSensingAgentFocus = 35,
	MergeSoldierInputs = 36,
	SendRadioMessage = 37,
	Debug = 38,
	ForgetTarget = 39,
	UseFlareOnMissileLock = 40,
	RunOverObject = 41,
	HonkHorn = 42,
	EvaluateCondition = 43,
	Revive = 44,
};
inline constexpr int kBAPStatementTypeCount = 45;

enum class BAPConditionType : int {
	LookDeviation = 0,
	Precision = 1,
	Precision3d = 2,
	BombPrecision3d = 3,
	And = 4,
	EndOfPath = 5,
	False = 6,
	Generic1Arg = 7,
	Generic2Arg = 8,
	InsideBattleZone = 9,
	Iteration = 10,
	LineOfFire = 11,
	PointDistance = 12,
	PointInFront = 13,
	Not = 14,
	DistanceToProj = 15,
	StrategicObjectStatus = 16,
	ObjectBehind = 17,
	ObjectDistance = 18,
	ObjectDistanceToPoint = 19,
	ObjectEnemyOccupied = 20,
	ObjectExists = 21,
	ObjectHealth = 22,
	ObjectInFront = 23,
	ObjectInLandingZone = 24,
	ObjectInsideFrustum = 25,
	ObjectLineOfFire = 26,
	ObjectManDown = 27,
	ObjectMoved = 28,
	ObjectOccupied = 29,
	ObjectSpeed = 30,
	ObjectTouchingLand = 31,
	ObjectValidAiming = 32,
	ObjectValidPosition = 33,
	ObjectVisible = 34,
	Or = 35,
	PointAt = 36,
	Position = 37,
	SharedCondition = 38,
	Time = 39,
	True = 40,
	WeaponMagAmmo = 41,
	WeaponTotalAmmo = 42,
	TotalAmmo = 43,
	WeaponReadyToFire = 44,
	WeaponTargetReachable = 45,
	WeaponHeat = 46,
	WeaponHasLockOnTarget = 47,
	IsTriggered = 49,
	AllowFire = 50,
	IsSprinting = 51,
	WeaponIsZoomed = 52,
	WeaponDetonatorIsActive = 53,
	WeaponIsActive = 54,
	OpLessThan = 55,
	OkToThrowGrenade = 56,
	SplashDamage = 57,
	TriggerableActive = 58,
};

class InfantryBody;
class PlanInterpreter;

class PlanBot {
public:
	virtual ~PlanBot() = default;
	virtual const PlanInterpreter &plan_interpreter() const = 0;
	virtual bool is_just_spawned() = 0;
	virtual BotLodLevel lod_level() const = 0;
	virtual double game_time() const = 0;
	virtual InfantryBody *infantry() = 0;
};

class BotScript {
public:
	virtual ~BotScript() = default;
	virtual bool execute(PlanBot &r_bot, bool &r_ok, bool &r_finished) = 0;
};

class BAPStatement : public BotScript {
public:
	virtual uint64_t controls() const { return controls_; }
	virtual BAPStatementType id() const = 0;
	int pattern_id() const { return pattern_id_; }

protected:
	explicit BAPStatement(int p_pattern_id = 0, uint64_t p_controls = 0) :
			controls_(p_controls), pattern_id_(p_pattern_id) {}

	uint64_t controls_ = 0;
	int pattern_id_ = 0;
};

class BAPAction : public BAPStatement {
public:
	bool execute(PlanBot &r_bot, bool &r_ok, bool &r_finished) override;

protected:
	using BAPStatement::BAPStatement;
};

class BAPCondition {
public:
	virtual ~BAPCondition() = default;
	virtual bool evaluate(PlanBot &r_bot) = 0;
	virtual BAPConditionType condition_type() const = 0;
};

class PlanInterpreter {
public:
	virtual ~PlanInterpreter() = default;
	virtual bool execute(BAPStatement &r_statement, PlanBot &r_bot, bool &r_ok, bool &r_finished) const = 0;
};

}
