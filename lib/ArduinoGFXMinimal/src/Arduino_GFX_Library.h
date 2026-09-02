#pragma once

// Minimal subset of Arduino_GFX 1.6.0 required by this board. Keeping only
// the P4 DSI backend avoids compiling unrelated SPI/parallel drivers.
#include "Arduino_DataBus.h"
#include "Arduino_GFX.h"
#include "databus/Arduino_ESP32DSIPanel.h"
#include "display/Arduino_DSI_Display.h"
