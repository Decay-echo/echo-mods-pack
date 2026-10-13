#pragma once
#include "host.h"
#include "share.h"
#include <winrt/base.h>
#include <winrt/Windows.Data.Json.h>
#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

// Discord through the desktop client's own local pipe (discord.cpp): the "RPC" every Rich
// Presence game uses. It answers only an application the user made themselves on Discord's
// developer site (free): its id and secret come from echo_tweaks.ini [discord]. After a
// one-time Authorize click in Discord on the desktop, the pipe tells us the voice channel's
// people, who is speaking, mute/deafen, lets us join and leave channels, and pushes message
// notifications (incoming calls come as a call message in a DM).

struct DiscordUser {
    std::wstring id, name, avatar;  // avatar: the CDN hash ("" = Discord's default picture)
};
struct DiscordMember {
    DiscordUser user;
    bool speaking = false, mute = false, deaf = false, self = false;
};
struct DiscordNote {  // a message notification, or an incoming call
    DiscordUser from;
    std::wstring text, channelId, title;
    uint64_t at = 0;
    bool call = false;
};
struct DiscordRef {  // a guild or a channel
    std::wstring id, name;
};
struct DiscordState {
    bool pipe = false;      // Discord's pipe answered
    bool authed = false;    // commands allowed (signed in)
    bool busy = false;      // the Authorize step is under way
    std::wstring status;    // one line for the page
    DiscordUser me;
    std::wstring channelId, channelName;  // the voice channel I am in ("" = none)
    std::vector<DiscordMember> members;
    std::deque<DiscordNote> notes;        // newest first, at most 30
    int unread = 0;
    bool selfMute = false, selfDeaf = false;
    DiscordNote call;                     // rings while call.at != 0
    uint64_t serial = 0;                  // bumps on any change
};

class Discord {
public:
    Discord();
    ~Discord();
    void configure(const std::wstring& clientId, const std::wstring& clientSecret, const std::wstring& loginPath);
    bool configured() const { return !clientId_.empty() && !clientSecret_.empty(); }
    void start();
    void stop();
    DiscordState state() const;
    uint64_t serial() const { std::lock_guard<std::mutex> lock(stateLock_); return state_.serial; }
    // Actions, run on the pipe thread.
    void authorize();   // AUTHORIZE (Discord asks on the desktop), the token exchange, AUTHENTICATE
    void setMute(bool on);
    void setDeaf(bool on);
    void joinChannel(const std::wstring& id);
    void leaveChannel();
    void listGuilds(std::function<void(std::vector<DiscordRef>, std::wstring)> done);
    void listVoiceChannels(const std::wstring& guildId, std::function<void(std::vector<DiscordRef>, std::wstring)> done);
    void markRead();
    void dismissCall();
    void signOut();
    // Avatars: fetched from Discord's CDN on worker threads (null until there; a failed one
    // stays null). `size` 32..256.
    std::shared_ptr<const SharePicture> avatar(const DiscordUser& user, int size);

private:
    using JsonObject = winrt::Windows::Data::Json::JsonObject;
    using Reply = std::function<void(const JsonObject& data, const std::wstring& error)>;
    void run();
    bool connect();
    bool handshake();
    bool readFrame(uint32_t& op, std::wstring& json, int timeoutMs);
    bool writeFrame(uint32_t op, const std::string& json);
    void send(const std::wstring& cmd, const std::wstring& argsJson, Reply reply);
    void subscribe(const std::wstring& evt, const std::wstring& argsJson);
    void unsubscribe(const std::wstring& evt, const std::wstring& argsJson);
    void onFrame(uint32_t op, const std::wstring& json);
    void onEvent(const std::wstring& evt, const JsonObject& data);
    bool loadLogin(std::wstring& access, std::wstring& refresh, uint64_t& expires);
    void saveLogin(const std::wstring& access, const std::wstring& refresh, int expiresIn);
    bool tokenRequest(const std::string& body, std::wstring& access, std::wstring& refresh, int& expiresIn, std::wstring& error);
    void authenticate(const std::wstring& access);
    void afterAuth();
    void watchChannel(const std::wstring& channelId);
    void setStatus(const std::wstring& s);
    void changed();
    void avatarFetch(const std::wstring& key, const std::wstring& url);

    std::wstring clientId_, clientSecret_, loginPath_;
    std::atomic<bool> running_{false}, stopping_{false};
    std::thread thread_;
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    std::mutex writeLock_;
    std::mutex actionsLock_;
    std::deque<std::function<void()>> actions_;  // run on the pipe thread
    std::map<std::wstring, Reply> pending_;      // by nonce
    uint64_t nonce_ = 0;
    std::string readBuffer_;
    std::wstring watched_;                       // the channel whose voice events we get
    std::wstring access_;
    uint64_t authFailures_ = 0;

    mutable std::mutex stateLock_;
    DiscordState state_;

    std::mutex avatarLock_;
    std::map<std::wstring, std::shared_ptr<const SharePicture>> avatars_;
    std::set<std::wstring> avatarLoading_;
    std::vector<std::thread> avatarThreads_;
};
