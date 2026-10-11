#pragma once
#include <string>

// Tablet size (SETTINGS slider / echo_tweaks.ini [tablet] scale).
//
// The tablet is a 0.3 x 0.3 m canvas (CCanvasUICR) plus touch hitboxes laid out in
// metres from its top-left corner (CR15ButtonInteractCR). We resize the canvas's
// in-memory world size; runtime.cpp scales the hitbox rows by appliedScale() so
// touches stay under the fingers.
namespace tablet {
void start(const std::wstring& pluginDir);  // reads the saved size; worker thread
void request(float scale);                  // desired size (from the SETTINGS tab)
float appliedScale();                       // size the canvas actually has now (1 if unchanged)
void tabletPresent(bool present);           // tablet canvases loaded / unloaded
}
