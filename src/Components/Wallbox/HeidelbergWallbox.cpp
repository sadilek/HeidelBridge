#include <Arduino.h>
#include "../Logger/Logger.h"
#include "../Modbus/ModbusRTU.h"
#include "../../Configuration/Constants.h"
#include "HeidelbergWallbox.h"

HeidelbergWallbox *HeidelbergWallbox::Instance()
{
    static HeidelbergWallbox instance;
    return &instance;
}

void HeidelbergWallbox::Init()
{
    uint16_t rawCurrent = static_cast<uint16_t>(Constants::HeidelbergWallbox::FailSafeCurrentA / Constants::HeidelbergWallbox::CurrentFactor);
    Logger::Debug("Heidelberg wallbox: Initializing fail safe current with %d (raw)", rawCurrent);
    if (!ModbusRTU::Instance()->WriteHoldRegister16(Constants::HeidelbergRegisters::FailsafeCurrent, rawCurrent))
    {
        // Error writing modbus register
        Logger::Error("ERROR: Could not set fail safe current");
    }

    // Disable standby
    uint16_t standbyDisabled = Constants::HeidelbergWallbox::AllowStandby ? 0 : 4;
    Logger::Debug("Heidelberg wallbox: Initializing standby mode with %d", standbyDisabled);
    if (!ModbusRTU::Instance()->WriteHoldRegister16(Constants::HeidelbergRegisters::DisableStandby, standbyDisabled))
    {
        // Error writing modbus register
        Logger::Error("ERROR: Could not configure standby");
    }

    // Disable watchdog
    Logger::Debug("Heidelberg wallbox: Setting watch dog timeout to %d s", Constants::HeidelbergWallbox::WatchdogTimeoutS);
    if (!ModbusRTU::Instance()->WriteHoldRegister16(Constants::HeidelbergRegisters::WatchdogTimeout, Constants::HeidelbergWallbox::WatchdogTimeoutS))
    {
        // Error writing modbus register
        Logger::Error("ERROR: Could not set watchdog timeout");
    }

    // Seed our state from the wallbox rather than assuming it.
    //
    // Register 261 lives in the wallbox and keeps its value when this bridge
    // restarts. If charging was disabled at that moment the register holds 0 A,
    // while the defaults in the header would otherwise claim charging is enabled
    // at 16 A. That mismatch is invisible to a controller: the reported state
    // says "charging enabled" while the wallbox delivers nothing.
    uint16_t rawLimit[1];
    if (ModbusRTU::Instance()->ReadRegisters(Constants::HeidelbergRegisters::MaximalCurrent, 1, 0x3, rawLimit))
    {
        mObservedChargingCurrentLimitA = static_cast<float>(rawLimit[0] * Constants::HeidelbergWallbox::CurrentFactor);
        mChargingEnabled = mObservedChargingCurrentLimitA >= Constants::HeidelbergWallbox::MinChargingCurrentA;

        if (mChargingEnabled)
        {
            // Adopt the limit the wallbox is already applying, but do not trust
            // it blindly: register 261 is 16 bit and some other controller could
            // have left a value above the supported range in it.
            mRequestedChargingCurrentLimitA =
                mObservedChargingCurrentLimitA > Constants::HeidelbergWallbox::MaxChargingCurrentA
                    ? Constants::HeidelbergWallbox::MaxChargingCurrentA
                    : mObservedChargingCurrentLimitA;
        }
        // Otherwise keep the header default as the limit to apply once charging
        // is enabled again.

        Logger::Info("Heidelberg wallbox: seeded state from register: %f A, charging %s",
                     mObservedChargingCurrentLimitA, mChargingEnabled ? "enabled" : "disabled");
    }
    else
    {
        Logger::Error("Heidelberg wallbox: ERROR: Could not read charging current limit during init");
    }
}

VehicleState HeidelbergWallbox::GetState()
{
    uint16_t registerValue[1];
    if (ModbusRTU::Instance()->ReadRegisters(Constants::HeidelbergRegisters::ChargingState, 1, 0x4, registerValue))
    {
        Logger::Debug("Heidelberg wallbox: Read state: %d", registerValue[0]);
        if (registerValue[0] <= 3)
        {
            mState = VehicleState::Disconnected;
        }
        else if (registerValue[0] <= 5)
        {
            mState = VehicleState::Connected;
        }
        else if (registerValue[0] <= 7)
        {
            mState = VehicleState::Charging;
        }
    }
    else
    {
        // Error reading modbus register
        Logger::Error("Heidelberg wallbox: ERROR: Could not read plugged state");
    }

    return mState;
}

namespace
{
    // Writes the wallbox current limit register (Heidelberg reg. 261).
    // 0 A blocks charging, 6-16 A permits it at up to that current.
    bool WriteCurrentLimitRegister(float currentLimitA)
    {
        const uint16_t rawCurrent = static_cast<uint16_t>(currentLimitA / Constants::HeidelbergWallbox::CurrentFactor);
        if (!ModbusRTU::Instance()->WriteHoldRegister16(Constants::HeidelbergRegisters::MaximalCurrent, rawCurrent))
        {
            // Error writing modbus register
            Logger::Error("Heidelberg wallbox: ERROR: Could not set maximum charging current");
            return false;
        }
        return true;
    }
}

bool HeidelbergWallbox::SetChargingCurrentLimit(float currentLimitA)
{
    // The wallbox only accepts 0 A or 6-16 A. Clamp rather than pass anything
    // else through: a malformed MQTT payload parses to 0.0, and a negative value
    // would be undefined behaviour in the conversion to uint16_t. Values below
    // the minimum become 0 A ("blocked") because that is what a client writing
    // 0 to stop charging expects.
    if (currentLimitA < Constants::HeidelbergWallbox::MinChargingCurrentA)
    {
        currentLimitA = 0.0f;
    }
    else if (currentLimitA > Constants::HeidelbergWallbox::MaxChargingCurrentA)
    {
        currentLimitA = Constants::HeidelbergWallbox::MaxChargingCurrentA;
    }

    mRequestedChargingCurrentLimitA = currentLimitA;

    if (!mChargingEnabled)
    {
        // The register stays at 0 while charging is disabled. The request is
        // remembered and applied when charging is enabled again.
        Logger::Info("Heidelberg wallbox: charging is disabled. current limit %f A is not applied yet", currentLimitA);
        return true;
    }

    Logger::Info("Heidelberg wallbox: setting charging current limit to %f A", currentLimitA);
    return WriteCurrentLimitRegister(currentLimitA);
}

bool HeidelbergWallbox::SetChargingEnabled(bool chargingEnabled)
{
    Logger::Info("Heidelberg wallbox: %s charging", chargingEnabled ? "enabling" : "disabling");

    mChargingEnabled = chargingEnabled;

    if (!chargingEnabled)
    {
        // Write on every call rather than only on a transition. The flag above is
        // ours, the register belongs to the wallbox, and the two can disagree - so
        // a request that happens to match the flag must still reach the hardware.
        return WriteCurrentLimitRegister(0.0f);
    }

    // Enabling with a zero setpoint contradicts itself and would leave charging
    // blocked. That can happen after a client wrote 0 A to stop charging instead
    // of using the enable flag. Fall back to the configured default.
    if (mRequestedChargingCurrentLimitA < Constants::HeidelbergWallbox::MinChargingCurrentA)
    {
        mRequestedChargingCurrentLimitA = Constants::HeidelbergWallbox::InitialChargingCurrentLimitA;
    }

    return WriteCurrentLimitRegister(mRequestedChargingCurrentLimitA);
}

bool HeidelbergWallbox::IsChargingEnabled()
{
    return mChargingEnabled;
}

bool HeidelbergWallbox::SetStandbyEnabled(bool standbyEnabled)
{
    bool ok = true;

    // Write only if state changes
    if (mStandbyEnabled != standbyEnabled)
    {
        uint16_t value = standbyEnabled ? 0 : 4;

        Logger::Info("Heidelberg wallbox: %s standby",
                     standbyEnabled ? "enabling" : "disabling");

        ok = ModbusRTU::Instance()->WriteHoldRegister16(
            Constants::HeidelbergRegisters::DisableStandby, value);

        if (ok)
        {
            mStandbyEnabled = standbyEnabled;
        }
    }

    return ok;
}

bool HeidelbergWallbox::GetStandbyEnabled()
{
    uint16_t registerValue[1];

    if (!ModbusRTU::Instance()->ReadRegisters(
            Constants::HeidelbergRegisters::DisableStandby,
            1,
            0x3,
            registerValue))
    {
        Logger::Error("Heidelberg wallbox: Could not read standby state");
        return mStandbyEnabled; // last known
    }

    bool enabled = (registerValue[0] == 0); // 0 = standby allowed
    mStandbyEnabled = enabled;

    Logger::Debug("Heidelberg wallbox: Read standby enabled = %d", enabled);

    return enabled;
}

float HeidelbergWallbox::GetChargingCurrentLimit()
{
    // This is telemetry. It must not touch mRequestedChargingCurrentLimitA:
    // writing the observed value there would let a reading of 0 A silently
    // destroy the limit we intend to apply the next time charging is enabled.
    uint16_t registerValue[1];
    if (ModbusRTU::Instance()->ReadRegisters(Constants::HeidelbergRegisters::MaximalCurrent, 1, 0x3, registerValue))
    {
        mObservedChargingCurrentLimitA = static_cast<float>(registerValue[0] * Constants::HeidelbergWallbox::CurrentFactor);
        Logger::Debug("Heidelberg wallbox: Read max. charging current: %f", mObservedChargingCurrentLimitA);
    }
    else
    {
        // Error reading modbus register
        Logger::Error("Heidelberg wallbox: ERROR: Could not read max. charging current");
    }

    return mObservedChargingCurrentLimitA; // last known value if the read failed
}

float HeidelbergWallbox::GetEnergyMeterValue()
{
    uint16_t rawEnergy[2];

    if (ModbusRTU::Instance()->ReadRegisters(
            Constants::HeidelbergRegisters::Energy,
            2,
            0x4,
            rawEnergy))
    {
        uint32_t totalEnergyWh = static_cast<uint32_t>(rawEnergy[0]) << 16 | static_cast<uint32_t>(rawEnergy[1]);
        mLastEnergyMeterValueWh = static_cast<float>(totalEnergyWh);

        Logger::Debug("Heidelberg wallbox: Read energy meter value: %f Wh", mLastEnergyMeterValueWh);
    }
    else
    {
        Logger::Error("Heidelberg wallbox: ERROR: Could not read energy meter value");
    }

    return mLastEnergyMeterValueWh;
}

float HeidelbergWallbox::GetFailsafeCurrent()
{
    uint16_t registerValue[1];
    if (ModbusRTU::Instance()->ReadRegisters(Constants::HeidelbergRegisters::FailsafeCurrent, 1, 0x3, registerValue))
    {
        mFailsafeCurrentA = static_cast<float>(registerValue[0] * Constants::HeidelbergWallbox::CurrentFactor);
        Logger::Debug("Read Heidelberg failsafe current: %f", mFailsafeCurrentA);
    }
    else
    {
        // Error reading modbus register
        Logger::Error("Heidelberg wallbox: ERROR: Could not read failsafe current");
    }

    return mFailsafeCurrentA;
}

float HeidelbergWallbox::GetChargingPower()
{
    uint16_t registerValue[1];
    if (ModbusRTU::Instance()->ReadRegisters(Constants::HeidelbergRegisters::Power, 1, 0x4, registerValue))
    {
        mLastPowerMeterValueW = static_cast<float>(registerValue[0]);
        Logger::Debug("Reading power meter value: %f W", mLastPowerMeterValueW);
    }
    else
    {
        // Error reading modbus register
        Logger::Error("Heidelberg wallbox: ERROR: Could not read last power meter value");
    }

    return mLastPowerMeterValueW;
}

bool HeidelbergWallbox::GetChargingCurrents(float &c1A, float &c2A, float &c3A)
{
    uint16_t rawCurrents[3];

    if (ModbusRTU::Instance()->ReadRegisters(
            Constants::HeidelbergRegisters::Currents,
            3,
            0x4,
            rawCurrents))
    {
        c1A = static_cast<float>(rawCurrents[0] * Constants::HeidelbergWallbox::CurrentFactor);
        c2A = static_cast<float>(rawCurrents[1] * Constants::HeidelbergWallbox::CurrentFactor);
        c3A = static_cast<float>(rawCurrents[2] * Constants::HeidelbergWallbox::CurrentFactor);
        Logger::Debug("Reading currents: %f %f %f A", c1A, c2A, c3A);
        return true;
    }
    else
    {
        Logger::Error("Heidelberg wallbox: ERROR: Could not read currents");
        return false;
    }
}

bool HeidelbergWallbox::GetChargingVoltages(float &v1V, float &v2V, float &v3V)
{
    uint16_t rawVoltages[3];

    if (ModbusRTU::Instance()->ReadRegisters(
            Constants::HeidelbergRegisters::Voltages,
            3,
            0x4,
            rawVoltages))
    {
        v1V = static_cast<float>(rawVoltages[0] * Constants::HeidelbergWallbox::VoltageFactor);
        v2V = static_cast<float>(rawVoltages[1] * Constants::HeidelbergWallbox::VoltageFactor);
        v3V = static_cast<float>(rawVoltages[2] * Constants::HeidelbergWallbox::VoltageFactor);
        Logger::Debug("Reading voltages: %f %f %f V", v1V, v2V, v3V);
        return true;
    }
    else
    {
        Logger::Error("Heidelberg wallbox: ERROR: Could not read voltages");
        return false;
    }
}

float HeidelbergWallbox::GetTemperature()
{
    uint16_t registerValue[1];
    if (ModbusRTU::Instance()->ReadRegisters(Constants::HeidelbergRegisters::PcbTemperature, 1, 0x4, registerValue))
    {
        return static_cast<float>(registerValue[0]) * Constants::HeidelbergWallbox::TemperatureFactor;
    }
    else
    {
        // Error reading modbus register
        Logger::Error("Heidelberg wallbox: ERROR: Could not read PCB temperature");
        return 0.0f;
    }
}