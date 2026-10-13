#pragma once
#include "../shared/arcade_ipc.h"

// Echo's voice chat mode switched from the tablet (SETTINGS > VOICE): its "voipmode" game
// setting, 0 muted, 1 open mic, 2 push to talk, 3 push to mute (as the settings loader
// numbers them). The game reads settings_mp_v2.json's "game" section into a settings block at
// start; the reader is hooked to learn that block's address, and the mode is written there
// when the host asks (the host also writes the file, so the choice survives a restart).
namespace voice {
void hookEarly(unsigned char* exe);                      // from the plugin's start(): before the game reads its settings
void attach(arcade::Shared* shared);                     // once the shared memory is up
void heartbeat(unsigned long long now);                  // runtime heartbeat thread, every ~100 ms
}
