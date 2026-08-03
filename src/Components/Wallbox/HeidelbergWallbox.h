#pragma once

#include "../../Configuration/Constants.h"
#include "IWallbox.h"

class HeidelbergWallbox : public IWallbox
{
private:
    HeidelbergWallbox() {};

public:
    static HeidelbergWallbox *Instance();

#pragma region IWallbox
    virtual void Init() override;
    virtual VehicleState GetState() override;
    virtual bool SetChargingCurrentLimit(float currentLimitA) override;
    virtual bool SetChargingEnabled(bool chargingEnabled) override;
    virtual bool SetStandbyEnabled(bool standbyEnabled) override;
    virtual float GetChargingCurrentLimit() override;
    virtual float GetEnergyMeterValue() override;
    virtual float GetFailsafeCurrent() override;
    virtual float GetChargingPower() override;
    virtual float GetTemperature() override;
    virtual bool GetChargingCurrents(float &c1A, float &c2A, float &c3A) override;
    virtual bool GetChargingVoltages(float &v1V, float &v2V, float &v3V) override;
    virtual bool IsChargingEnabled() override;
    virtual bool GetStandbyEnabled() override;

#pragma endregion IWallbox

private:
    VehicleState mState{VehicleState::Disconnected};
    // What we want the wallbox to do. Only ever changed by SetChargingCurrentLimit,
    // never by a telemetry read, so it always represents intent.
    float mRequestedChargingCurrentLimitA{Constants::HeidelbergWallbox::InitialChargingCurrentLimitA};
    // What the wallbox last reported (register 261). Observation, not intent.
    float mObservedChargingCurrentLimitA{0.0f};
    float mFailsafeCurrentA{0.0f};
    float mLastPowerMeterValueW{0.0f};
    float mLastEnergyMeterValueWh{0.0f};
    // Seeded from the wallbox in Init(); see the comment there.
    bool mChargingEnabled{true};
    bool mStandbyEnabled{true}; // default: standby enabled
};