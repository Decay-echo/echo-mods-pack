#pragma once
#include "../shared/arcade_ipc.h"

// Streams host frames into two textures on the GPU: the tablet's screen, and the
// lobby-poster screen used while the arcade is docked on a poster.
//
// Each texture is identified when the engine creates it: a 2D B8G8R8A8 resource of a
// size no stock texture uses (tablet ARCADE_TEX_W x ARCADE_TEX_H, poster POSTER_TEX_W x
// POSTER_TEX_H, where the frame goes at POSTER_X0, POSTER_Y0). Uploads are recorded on
// the game's own direct queue, just before one of its submissions, so they are ordered
// with the frames that sample them.
namespace stream {
bool install(arcade::Shared* shared);  // hooks D3D12; call from a normal thread (not DllMain)
void setVisible(bool visible);         // tablet: upload only while the ARCADE page is on screen
// Poster: upload only while docked. Turning it off takes effect before this returns, so
// the caller can let the engine release the texture right after.
void setPosterActive(bool active);
bool posterTextureReady();             // the engine has created the poster texture
void shutdown();
}
