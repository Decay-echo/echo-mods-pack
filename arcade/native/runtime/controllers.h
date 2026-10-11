// Touch controllers for the arcade (CONTROLLERS on the RetroArch screen): while the host asks,
// Echo VR gets neutral controllers, the player is held still, and the real controllers go to
// the host for RetroArch's RetroPad.
#pragma once
#include "../shared/arcade_ipc.h"

namespace controllers {

void install(unsigned char* exe, arcade::Shared* shared);  // after the tablet hooks; hooks lazily
void tick(uint64_t now);                                    // from the heartbeat loop (every 100 ms)
void levelChanged(const char* why);                         // ends FREEZE (Shared::levelSerial)

}  // namespace controllers
