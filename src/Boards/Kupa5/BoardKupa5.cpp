#include <Arduino.h>
#include "Components/Logger/Logger.h"
#include "../Board.h"
#include "BoardKupa5.h"

// Pin connections for the Kupa5 hardware:
// ESP32 GPIO16 -> MAXRS485 RO (Receiver Output)
// ESP32 GPIO17 -> MAXRS485 DI (Driver Input)
// DE+RE are driven automatically (RTS auto mode).
constexpr uint8_t PinRX = GPIO_NUM_16;
constexpr uint8_t PinTX = GPIO_NUM_17;
constexpr uint8_t PinRTS = -1; // Auto mode.

// Constructor
BoardKupa5::BoardKupa5()
    : Board(PinRX, PinTX, PinRTS)
{
  // Nothing to do
}

// Initializes the board
void BoardKupa5::Init()
{
  // Nothing to do
}

// Logs board name/information
void BoardKupa5::Print()
{
  Logger::Print("Kupa5");
}