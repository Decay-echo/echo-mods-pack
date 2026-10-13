#pragma once
// Discord on the HUD: ArcadeHost writes what the HUD overlay shows (the voice channel's people
// with their avatars and who is talking, the newest message's sender and a count, an incoming
// call) into this block; EchoHudPlus reads it. Fixed layout, versioned; a seqlock (serial odd
// while the host writes) keeps a reader from seeing half a picture.
#include <cstdint>
#include <windows.h>

namespace arcade {

constexpr const wchar_t* DISCORD_SHARED_NAME = L"Local\\EchoArcade.Discord.v1";
constexpr uint32_t DISCORD_MAGIC = 0x43534944;  // 'DISC'
constexpr uint32_t DISCORD_VERSION = 1;
constexpr int DISCORD_AVATAR = 48;              // avatar pictures, RGBA, 48 x 48
constexpr int DISCORD_MAX_MEMBERS = 8;

struct DiscordAvatar {
    uint8_t rgba[DISCORD_AVATAR * DISCORD_AVATAR * 4];
};

struct DiscordMemberShared {
    wchar_t name[32];
    LONG speaking, self, mute, deaf, hasPicture;
    DiscordAvatar picture;
};

struct DiscordShared {
    uint32_t magic, version;
    volatile LONG serial;           // seqlock: odd while the host writes; bumps twice per write
    volatile LONG64 heartbeat;      // GetTickCount64 of the host's last write or touch
    volatile LONG connected;        // 1 = the host talks to Discord
    volatile LONG inVoice;          // 1 = in a voice channel
    volatile LONG memberCount;      // people in the channel shown (<= DISCORD_MAX_MEMBERS)
    DiscordMemberShared members[DISCORD_MAX_MEMBERS];
    volatile LONG unread;           // messages not yet looked at on the tablet
    volatile LONG64 lastMessageAt;  // tick of the newest message (0 = none)
    wchar_t lastFrom[32];
    LONG lastHasPicture;
    DiscordAvatar lastPicture;
    volatile LONG ringing;          // 1 = an incoming call
    wchar_t caller[32];
    LONG callerHasPicture;
    DiscordAvatar callerPicture;
};

}  // namespace arcade
