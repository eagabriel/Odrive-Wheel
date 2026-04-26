/*
 * EffectsCalculator.h
 *
 *  Created on: 27.01.2021
 *      Author: Yannick / Jon Lidgard / Vincent Manoukian
 *
 * ────── ODrive-FFB-Wheel fork ──────────────────────────────────────────────
 * Cópia forkada de OpenFFBoard upstream (v1.17.0, commit ac68fba).
 * Localização upstream: Firmware/FFBoard/Inc/EffectsCalculator.h
 *
 * Modificações locais (procurar "Phase 3" nos comentários):
 *   - public getGainStruct() — acesso direto ao gain struct (spring/damper/
 *     friction/inertia) pra cmd_table
 *   - public getFilterStruct() — retorna filter[CUSTOM_PROFILE_ID]
 *   - public getFilterStructDefault() — retorna filter[0]
 *   - public getGlobalGain() — leitura do master gain
 *   - public getFilterProfileId() / setFilterProfileToCustom() — controle de profile
 *   - public applyFilterChanges() — re-aplica filtros nos efeitos ativos
 *
 * Pra atualizar contra upstream:
 *   diff Firmware-V56-Stock/inc/EffectsCalculator.h \
 *        OpenFFBoard-master/OpenFFBoard-master/Firmware/FFBoard/Inc/EffectsCalculator.h
 * ───────────────────────────────────────────────────────────────────────────
 */

#ifndef EFFECTSCALCULATOR_H_
#define EFFECTSCALCULATOR_H_

//#include "ClassChooser.h"
#include "ffb_defs.h"
#include "PersistentStorage.h"
#include "CommandHandler.h"
#include "thread.hpp"
#include <vector>
#include <functional>
#include "FastAvg.h"

// Stubs do ChoosableClass do OpenFFBoard
#ifdef __cplusplus
enum class ClassType : uint32_t { Internal, External, Mainclass };
#endif
#define CLSID_EFFECTSCALC 0xA02

#define EFFECT_THREAD_MEM 128
#define EFFECT_THREAD_PRIO 20 // low priority for stat

#define INTERNAL_SCALER_DAMPER 40
#define INTERNAL_SCALER_FRICTION 45
#define INTERNAL_SCALER_INERTIA 4

#define CUSTOM_PROFILE_ID 1

class Axis;
struct metric_t;

// default effect gains
struct effect_gain_t {
	uint8_t friction = 254;
	uint8_t spring = 64;
	uint8_t damper = 64;
	uint8_t inertia = 127;
};

struct effect_scaler_t {
	float friction = 1.0; //0.4 * 40;
	float spring = 16.0;
	float damper = 4.0; //2 * 40 * 2
	float inertia = 2.0;//0.5 * 40;
};

struct effect_biquad_t {
	biquad_constant_t constant	= { 500, 70 };
	biquad_constant_t friction 	= { 50, 20 };
	biquad_constant_t damper 	= { 30, 40 };
	biquad_constant_t inertia	= { 15, 20 };
};

struct effect_stat_t {
	std::array<int16_t,MAX_AXIS> current={0};
	std::array<int16_t,MAX_AXIS> max={0};
	uint16_t nb=0;
};

enum class EffectsCalculator_commands : uint32_t {
	ffbfiltercf,ffbfiltercf_q,effects,spring,friction,damper,inertia,
	damper_f, damper_q, friction_f, friction_q, inertia_f, inertia_q, filterProfileId,
	frictionPctSpeedToRampup,
	monitorEffect, effectsDetails, effectsForces,
};

class EffectsCalculator: public PersistentStorage,
						 public CommandHandler,
						 public cpp_freertos::Thread {
public:
	EffectsCalculator();
	virtual ~EffectsCalculator();

	static ClassIdentifier info;
	const ClassIdentifier getInfo();
	const ClassType getClassType() {return ClassType::Internal;};

	void saveFlash();
	void restoreFlash();

	bool isActive();
	void setActive(bool active);
	void calculateEffects(std::vector<std::unique_ptr<Axis>> &axes);
	virtual void setFilters(FFB_Effect* effect);
	void setGain(uint8_t gain);
	uint8_t getGain();
	void logEffectType(uint8_t type,bool remove = false);
	//void setDirectionEnableMask(uint8_t mask);
	void calcStatsEffectType(uint8_t type, int32_t force,uint8_t axis);
	void logEffectState(uint8_t type,uint8_t state);
	void resetLoggedActiveEffects(bool reinit);

	int32_t find_free_effect(uint8_t type);
	void free_effect(uint16_t idx);

	CommandStatus command(const ParsedCommand& cmd,std::vector<CommandReply>& replies);
	virtual std::string getHelpstring() { return "Controls internal FFB effects"; }

	static const uint32_t max_effects = MAX_EFFECTS;
	std::array<FFB_Effect,max_effects> effects; // Main effects storage

	// Fase 7 — acesso direto pros gains do cmd_table (fx.spring/damper/friction/inertia).
	// OpenFFBoard original mantem gain private e usa CommandHandler protocolo;
	// no merge usamos CmdParser simples que precisa de acesso direto.
	effect_gain_t& getGainStruct() { return gain; }

	// Phase 3.4 — acesso direto aos coefs de filtro (cutoff freq + Q) por tipo
	// de efeito.
	//
	// IMPORTANTE: saveFlash() só grava friction/damper/inertia quando
	// filterProfileId == CUSTOM_PROFILE_ID (=1). Default é 0. Pra persistência
	// funcionar, nossos setters em ffb_task.cpp precisam SEMPRE retornar
	// filter[CUSTOM_PROFILE_ID] e força filterProfileId = CUSTOM_PROFILE_ID.
	// CF (constant) é gravado sempre em filter[0] pelo saveFlash, mas pra
	// runtime usamos filter[CUSTOM_PROFILE_ID].constant também (a struct é
	// idêntica e o setFilters lê de filter[filterProfileId]).
	effect_biquad_t& getFilterStruct() { return filter[CUSTOM_PROFILE_ID]; }
	effect_biquad_t& getFilterStructDefault() { return filter[0]; }
	void setFilterProfileToCustom() { filterProfileId = CUSTOM_PROFILE_ID; }
	uint8_t getFilterProfileId() const { return filterProfileId; }

	// Re-aplica os coefs atuais de filter[filterProfileId] em todos os efeitos
	// existentes (state != INACTIVE). Sem isso, mudar freq/Q só afeta efeitos
	// novos criados depois — efeitos já alocados pelo jogo continuam com o
	// filtro antigo.
	void applyFilterChanges() {
		for (auto &e : effects) {
			if (e.type != FFB_EFFECT_NONE) setFilters(&e);
		}
	}

	// Thread impl
	void Run();

	void updateSamplerate(float newSamplerate); // Must be called if update rate is changed to update filters and effects


protected:

public:
	// Diagnóstico — exposto pro ffb_task dump no comando 'E' do ASCII
	uint8_t getGlobalGain() const { return global_gain; }

private:
	//uint8_t directionEnableMask = 0;
	// Filters
	effect_biquad_t filter[2];		// 0 is the default profile and the custom for CFFilter, CUSTOM_PROFILE_ID is the custom slot
	uint8_t filterProfileId = 0;
	uint32_t calcfrequency = 1000; 		// HID frequency 1khz
	const float qfloatScaler = 0.01;

	// Rescale factor for conditional effect to boost or decrease the intensity
	uint8_t global_gain = 0xff;
	effect_gain_t gain;
	effect_scaler_t scaler;
	uint8_t frictionPctSpeedToRampup = 25;	// define the max value of the range (0..5% of maxspeed) where torque is rampup on friction

	// FFB status
	bool effects_active = false; // If FFB is on
	uint32_t effects_used = 0;
	std::array<effect_stat_t,12> effects_stats; // [0..12 effect types]
	std::array<effect_stat_t,12> effects_statslast; // [0..12 effect types]
	bool isMonitorEffect = false;

	int32_t calcComponentForce(FFB_Effect *effect, int32_t forceVector, std::vector<std::unique_ptr<Axis>> &axes, uint8_t axis);
	int32_t calcNonConditionEffectForce(FFB_Effect* effect);
	float speedRampupPct();
	int32_t calcConditionEffectForce(FFB_Effect *effect, float metric, uint8_t gain, uint8_t idx, float scale, float angle_ratio);
	int32_t getEnvelopeMagnitude(FFB_Effect *effect);
	std::string listEffectsUsed(bool details = false,uint8_t axis = 0);
	//std::string listForceEffects();
	void checkFilterCoeff(biquad_constant_t *filter, uint32_t freq,uint8_t q);
	void updateFilterSettingsForEffects(uint8_t type_effect);
};

/**
 * Helper interface class for common effects calculator related control functions
 */
class EffectsControlItf{
public:
	virtual void set_FFB(bool state) = 0; // Enables or disables FFB
	virtual void stop_FFB(){set_FFB(false);};
	virtual void start_FFB(){set_FFB(true);};
	virtual void reset_ffb() = 0;
	virtual uint32_t getConstantForceRate(); // Returns an estimate of the constant force effect update rate in hz
	virtual uint32_t getRate(); // Returns an estimate of the overall effect update speed in hz
	virtual bool getFfbActive() = 0;
	virtual void set_gain(uint8_t gain) = 0;
	virtual void cfUpdateEvent();
	virtual void fxUpdateEvent();
	virtual void updateSamplerate(float newSamplerate) = 0; // Should be called when update loop rate is changed

private:
	FastMovingAverage<float> fxPeriodAvg{5};
	FastMovingAverage<float> cfUpdatePeriodAvg{5};

	uint32_t lastFxUpdate = 0;
	uint32_t lastCfUpdate = 0;
};

#endif /* EFFECTSCALCULATOR_H_ */
