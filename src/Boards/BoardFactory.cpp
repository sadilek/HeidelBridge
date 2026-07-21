#include <Arduino.h>
#include "Board.h"
#include "BoardFactory.h"
#include "../Configuration/Settings.h"

// All supported boards
#include "ESP32/BoardESP32.h"
#include "Lilygo/BoardLilygo.h"
#include "Kupa5/BoardKupa5.h"

// Factory instance
BoardFactory *BoardFactory::Instance()
{
  static BoardFactory factoryInstance;
  return &factoryInstance;
}

// Gets the currently used board
Board *BoardFactory::GetBoard()
{
  static BoardESP32 genericBoard;
  static BoardLilygo lilygoBoard;
  static BoardKupa5 kupa5Board;

  if (Settings::Instance()->BoardType == "lilygo")
  {
    return &lilygoBoard;
  }
  if (Settings::Instance()->BoardType == "kupa5")
  {
    return &kupa5Board;
  }
  return &genericBoard;
}