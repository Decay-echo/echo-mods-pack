#pragma once
#include "../shared/arcade_ipc.h"

// DOCK: shows the arcade on the lobby poster nearest the player, by overriding that
// poster's texture the way the game applies server posters (CR15NetDynamicPosterCS), with
// a texture the D3D12 streamer fills (stream::setPosterActive).
//
// While docked the poster is a touch screen (fingertips from the touch-button system,
// hit-tested against the poster's picture face) and a light-gun screen (each shot from the
// player's gun is a ray from the muzzle; where it meets the face is where the bullet ends).
// Both arrive at the host as pixel events (arcade::PointDown/Move/Up, arcade::Shot).
//
// Poster positions and face shapes come from the level data at build time
// (tools/build_poster_table.py -> native/generated/posters.h).
namespace posters {
bool install(unsigned char* exe, arcade::Shared* shared);  // after MH_Initialize; false = disabled (logged)
void afterButtonUpdate(void* buttonSystem);                // engine thread, after its touch-button update; drives DOCK
void heartbeat(unsigned long long now);                    // runtime heartbeat thread, every ~100 ms
}
