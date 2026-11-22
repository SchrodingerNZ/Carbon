/*
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#include "CarbonEngine/Common.h"
#include "CarbonEngine/Core/CoreEvents.h"
#include "CarbonEngine/Core/EventManager.h"
#include "CarbonEngine/Game/MultiplayerEvents.h"
#include "CarbonEngine/Core/SharedLibrary.h"
#include "CarbonEngine/Game/ConnectionPlatformInterface.h"
#include "CarbonEngine/Game/SessionManifestCodec.hpp"
#include "CarbonEngine/Game/SessionController.hpp"

#if defined(WINDOWS) || defined(MACOS)
    #include "CarbonEngine/Core/FileSystem/FileSystem.h"
    #include "CarbonEngine/Core/FileSystem/LocalDirectoryFileSystemVolume.h"
    #include "CarbonEngine/Game/CloudSaveFileSystem.hpp"
#endif

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <cstdlib>
#include <vector>

namespace Carbon
{

bool ConnectionPlatformInterface::processEvent(const Event& e)
{
    if (e.as<UpdateEvent>())
        update();

    return true;
}

void MultiplayerInterface::dispatchSessionStateChanged(MultiplayerSessionState previous)
{
    // Notify applications only when the public session state actually changes.
    const auto current = session_.getState();
    if (previous != current)
        events().dispatchEvent(MultiplayerSessionStateChangedEvent(previous, current));
}

void MultiplayerInterface::dispatchSessionUpdated()
{
    // The public session snapshot is current before any application handler runs.
    events().dispatchEvent(MultiplayerSessionUpdatedEvent());
}

void MultiplayerInterface::dispatchPlayerConnectionStateChanged(PlayerID playerID, MultiplayerConnectionState previous,
                                                                 MultiplayerConnectionState current)
{
    // Only report a real state transition, after the public session has been updated.
    if (previous != current)
        events().dispatchEvent(MultiplayerPlayerConnectionStateChangedEvent(playerID, previous, current));
}

void MultiplayerInterface::dispatchSessionEnded(uint64_t sessionID, MultiplayerSessionEndReason reason)
{
    // Report the ended session and its outcome through the application event chain.
    events().dispatchEvent(MultiplayerSessionEndedEvent(sessionID, reason));
}

void MultiplayerInterface::dispatchOperationCompleted(MultiplayerOperation operation, bool succeeded)
{
    // Asynchronous results belong to the normal application event chain.
    events().dispatchEvent(MultiplayerOperationCompletedEvent(operation, succeeded));
}

void MultiplayerInterface::dispatchOperationCompleted(MultiplayerOperation operation,
                                                        MultiplayerOperationFailureReason reason)
{
    events().dispatchEvent(MultiplayerOperationCompletedEvent(operation, reason));
}

void MultiplayerInterface::dispatchNetFrame(PlayerID sender, const NetFrame& frame)
{
    // The event owns a copy of the decoded frame for the duration of dispatch.
    events().dispatchEvent(NetFrameReceivedEvent(sender, frame));
}

MultiplayerInterface& multiplayer()
{
    return InterfaceRegistry<ConnectionPlatformInterface>::getActiveInstance()->getMultiplayer();
}

CARBON_DEFINE_INTERFACE_REGISTRY(ConnectionPlatformInterface)
{
    return i->isAvailable();
}

/** Non-persistent achievement capability for the Null connection platform. */
class NullAchievements : public AchievementInterface
{
public:
    bool setProgress(const String& id, unsigned int progress, unsigned int maxProgress) override
    {
        auto& current = progress_[id.cStr()];
        if (progress > current)
            current = progress;

        return true;
    }

    unsigned int getProgress(const String& id, unsigned int maxProgress) const override
    {
        const auto it = progress_.find(id.cStr());
        return it == progress_.end() ? 0 : it->second;
    }

private:
    std::unordered_map<std::string, unsigned int> progress_;
};

/** The Null connection has no network lobbies. */
class NullMultiplayer : public MultiplayerInterface {};

/** Always available unless a client explicitly requests an alternative implementation. */
class NullConnectionPlatform : public ConnectionPlatformInterface
{
public:
    AchievementInterface& getAchievements() override { return achievements_; }
    MultiplayerInterface& getMultiplayer() override { return multiplayer_; }

private:
    NullAchievements achievements_;
    NullMultiplayer multiplayer_;
};

// The Null provider remains the default even when an optional native provider is registered.
CARBON_REGISTER_INTERFACE_IMPLEMENTATION(ConnectionPlatformInterface, NullConnectionPlatform, 1)

#if defined(WINDOWS) || defined(MACOS)

namespace CloudFS = CloudSaveFileSystem;

// Keep the Steam flat API calling convention platform-neutral without exposing Steamworks headers.
#ifdef WINDOWS
    #define CARBON_STEAM_CALL __cdecl
#else
    #define CARBON_STEAM_CALL
#endif

// Flat Steamworks exports let Carbon keep the SDK optional at build and runtime.
// Private callback payload mirrors must match the Steamworks ABI for the target architecture.
struct SteamCallbackMessage
{
    int user;
    int callback;
    unsigned char* data;
    int size;
};

// Private mirror of Steamworks LobbyChatMsg_t; keep Steamworks optional at build time.
struct SteamLobbyChatMessage
{
    uint64_t lobbyID;
    uint64_t senderID;
    int32_t entryType;
    int32_t chatID;
};
static_assert(sizeof(SteamLobbyChatMessage) == 24, "Unexpected Steam lobby chat callback size");
static_assert(offsetof(SteamLobbyChatMessage, chatID) == 20, "Unexpected Steam lobby chat ID offset");

/** Steam achievement storage, with requests retained until the asynchronous stats callback arrives. */
class SteamAchievements : public AchievementInterface
{
public:
    using UserStatsFunction = void* (CARBON_STEAM_CALL *)();
    using GetAchievementFunction = bool (CARBON_STEAM_CALL *)(void*, const char*, bool*);
    using GetStatFunction = bool (CARBON_STEAM_CALL *)(void*, const char*, int32_t*);
    using SetAchievementFunction = bool (CARBON_STEAM_CALL *)(void*, const char*);
    using SetStatFunction = bool (CARBON_STEAM_CALL *)(void*, const char*, int32_t);
    using StoreFunction = bool (CARBON_STEAM_CALL *)(void*);
    using GetNumAchievementsFunction = uint32_t (CARBON_STEAM_CALL *)(void*);
    using GetAchievementNameFunction = const char* (CARBON_STEAM_CALL *)(void*, uint32_t);

    bool initialize(const SharedLibrary& library)
    {
        // Map only the user-stats functions required by Carbon's achievement capability.
        auto getUserStats = library.mapFunction<UserStatsFunction>("SteamAPI_SteamUserStats_v013");
        if (!getUserStats)
            getUserStats = library.mapFunction<UserStatsFunction>("SteamAPI_SteamUserStats_v012");

        getAchievement_ = library.mapFunction<GetAchievementFunction>("SteamAPI_ISteamUserStats_GetAchievement");
        getStat_ = library.mapFunction<GetStatFunction>("SteamAPI_ISteamUserStats_GetStatInt32");
        setAchievement_ = library.mapFunction<SetAchievementFunction>("SteamAPI_ISteamUserStats_SetAchievement");
        setStat_ = library.mapFunction<SetStatFunction>("SteamAPI_ISteamUserStats_SetStatInt32");
        store_ = library.mapFunction<StoreFunction>("SteamAPI_ISteamUserStats_StoreStats");
        getNumAchievements_ = library.mapFunction<GetNumAchievementsFunction>("SteamAPI_ISteamUserStats_GetNumAchievements");
        getAchievementName_ = library.mapFunction<GetAchievementNameFunction>("SteamAPI_ISteamUserStats_GetAchievementName");

        // Name missing exports individually so an outdated Steamworks runtime is easy to diagnose.
        if (!getUserStats)
            LOG_WARNING_WITHOUT_CALLER << "Steam achievements: no supported SteamUserStats interface export";
        if (!getAchievement_)
            LOG_WARNING_WITHOUT_CALLER << "Steam achievements: missing GetAchievement export";
        if (!getStat_)
            LOG_WARNING_WITHOUT_CALLER << "Steam achievements: missing GetStatInt32 export";
        if (!setAchievement_)
            LOG_WARNING_WITHOUT_CALLER << "Steam achievements: missing SetAchievement export";
        if (!setStat_)
            LOG_WARNING_WITHOUT_CALLER << "Steam achievements: missing SetStatInt32 export";
        if (!store_)
            LOG_WARNING_WITHOUT_CALLER << "Steam achievements: missing StoreStats export";
        if (!getNumAchievements_ || !getAchievementName_)
            LOG_WARNING_WITHOUT_CALLER << "Steam achievements: missing readiness-probe exports";

        if (!getUserStats || !getAchievement_ || !getStat_ || !setAchievement_ || !setStat_ || !store_ || !getNumAchievements_ || !getAchievementName_)
            return false;

        userStats_ = getUserStats();
        if (!userStats_)
            LOG_WARNING_WITHOUT_CALLER << "Steam achievements: SteamUserStats interface returned null";

        return userStats_ != nullptr;
    }

    bool isReady() const override { return ready_; }

    bool setProgress(const String& id, unsigned int progress, unsigned int maxProgress) override
    {
        // Absolute progress never decreases, even before Steam stats are ready.
        auto& entry = entries_[id.cStr()];
        entry.maxProgress = maxProgress;
        entry.local = std::max(entry.local, progress);

        if (ready_)
            reconcile(id.cStr(), entry);

        return true;
    }

    unsigned int getProgress(const String& id, unsigned int maxProgress) const override
    {
        // An achievement queried for the first time after readiness still loads remote progress.
        auto& entry = entries_[id.cStr()];
        entry.maxProgress = maxProgress;

        if (ready_)
            const_cast<SteamAchievements*>(this)->reconcile(id.cStr(), entry);

        return entry.local;
    }

    void onStatsReceived(int result)
    {
        if (result != 1) // k_EResultOK
        {
            LOG_WARNING_WITHOUT_CALLER << "Steam achievement stats unavailable, result " << result;
            return;
        }

        markReady();
    }

    void onStatsStored(int result)
    {
        // A StoreStats request is only complete once Steam acknowledges it.
        if (!storePending_)
            return;

        storePending_ = false;

        if (result == 1) // k_EResultOK
        {
            // Changes made while the request was in flight still need storing.
            dirty_ = mutationVersion_ != submittedVersion_;
        }
        else
        {
            dirty_ = true;
            LOG_WARNING_WITHOUT_CALLER << "Steam achievement stats store failed, result " << result;
        }
    }

    void update()
    {
        // The current-user stats cache is loaded by Steam before game launch. New Steamworks
        // runtimes may not emit an initial UserStatsReceived_t callback at all.
        // Probe an actual published achievement instead of waiting indefinitely or using a timer
        // to assume readiness. A successful GetAchievement proves that reads are available.
        if (!ready_ && std::chrono::steady_clock::now() - lastProbe_ >= std::chrono::milliseconds(500))
        {
            lastProbe_ = std::chrono::steady_clock::now();
            const auto count = getNumAchievements_(userStats_);
            if (count)
            {
                const auto firstID = getAchievementName_(userStats_, 0);
                bool granted = false;
                if (firstID && getAchievement_(userStats_, firstID, &granted))
                {
                    LOG_INFO << "Steam achievements: local cache available (" << count << " published achievements)";
                    markReady();
                }
            }
        }

        // Batch ordinary progress writes; Steam recommends StoreStats on major changes, not per-frame.
        if (dirty_ && std::chrono::steady_clock::now() - lastStore_ >= std::chrono::minutes(1))
            flush(true);
    }

    void flush(bool force)
    {
        if (!ready_ || !dirty_ || storePending_ || !force)
            return;

        if (store_(userStats_))
        {
            // Remember which changes this asynchronous request covers.
            submittedVersion_ = mutationVersion_;
            storePending_ = true;
            lastStore_ = std::chrono::steady_clock::now();
        }
    }

private:
    void markReady()
    {
        if (ready_)
            return;

        ready_ = true;
        LOG_INFO << "Steam achievements ready";

        // Apply locally queued requests only after remote values can be read.
        for (auto& entry : entries_)
            reconcile(entry.first, entry.second);

        flush(true);
    }

    struct Entry
    {
        unsigned int maxProgress = 0;
        unsigned int local = 0;
        unsigned int remote = 0;
        bool loaded = false;
        bool warned = false;
    };

    void reconcile(const std::string& id, Entry& entry)
    {
        if (!entry.loaded)
        {
            // Read Steam's unlock flag and, for progress achievements, its paired integer stat.
            bool granted = false;
            const auto achievementOk = getAchievement_(userStats_, id.c_str(), &granted);
            const auto statName = id + "_progress";
            auto stat = int32_t();
            const auto statOk = !entry.maxProgress || getStat_(userStats_, statName.c_str(), &stat);

            if (!achievementOk || !statOk)
            {
                if (!entry.warned)
                {
                    LOG_WARNING_WITHOUT_CALLER << "Steam achievement or stat not configured: " << id.c_str();
                    entry.warned = true;
                }
                return;
            }

            const auto remoteProgress = granted ? (entry.maxProgress ? entry.maxProgress : 1U)
                                                : (entry.maxProgress ? unsigned(std::max(0, stat)) : 0U);
            entry.remote = remoteProgress;
            entry.local = std::max(entry.local, remoteProgress);
            entry.loaded = true;
        }

        // Don't overwrite a higher remote value with an earlier local request.
        if (entry.local <= entry.remote)
            return;

        if (entry.maxProgress)
        {
            const auto statName = id + "_progress";
            if (entry.local > unsigned(std::numeric_limits<int32_t>::max()) ||
                !setStat_(userStats_, statName.c_str(), int32_t(entry.local)))
                return;
        }

        // Full progress unlocks the Steam achievement; partial progress updates only the stat.
        if ((!entry.maxProgress || entry.local >= entry.maxProgress) && !setAchievement_(userStats_, id.c_str()))
            return;

        entry.remote = entry.local;
        dirty_ = true;
        ++mutationVersion_;

        if (!entry.maxProgress || entry.local >= entry.maxProgress)
            flush(true);
    }

    mutable std::unordered_map<std::string, Entry> entries_;
    void* userStats_ = nullptr;
    GetAchievementFunction getAchievement_ = nullptr;
    GetStatFunction getStat_ = nullptr;
    SetAchievementFunction setAchievement_ = nullptr;
    SetStatFunction setStat_ = nullptr;
    StoreFunction store_ = nullptr;
    GetNumAchievementsFunction getNumAchievements_ = nullptr;
    GetAchievementNameFunction getAchievementName_ = nullptr;
    std::chrono::steady_clock::time_point lastProbe_ = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    bool ready_ = false;
    bool dirty_ = false;
    bool storePending_ = false;
    uint64_t mutationVersion_ = 0;
    uint64_t submittedVersion_ = 0;
    std::chrono::steady_clock::time_point lastStore_ = std::chrono::steady_clock::now();
};

/** Steam lobby discovery, sharing the Steam session and manual callback dispatcher. */
class SteamMultiplayer : public MultiplayerInterface
{
public:
    using GetRemoteStorageFunction = void* (CARBON_STEAM_CALL *)();
    using CloudEnabledFunction = bool (CARBON_STEAM_CALL *)(void*);
    using CloudFileWriteFunction = bool (CARBON_STEAM_CALL *)(void*, const char*, const void*, int32_t);
    using CloudFileReadFunction = int32_t (CARBON_STEAM_CALL *)(void*, const char*, void*, int32_t);
    using CloudFileExistsFunction = bool (CARBON_STEAM_CALL *)(void*, const char*);
    using CloudFileSizeFunction = int32_t (CARBON_STEAM_CALL *)(void*, const char*);
    using CloudFileTimestampFunction = int64_t (CARBON_STEAM_CALL *)(void*, const char*);
    using CloudFileCountFunction = int32_t (CARBON_STEAM_CALL *)(void*);
    using CloudFileNameAndSizeFunction = const char* (CARBON_STEAM_CALL *)(void*, int, int32_t*);

    using GetMatchmakingFunction = void* (CARBON_STEAM_CALL *)();
    using AddFilterFunction = void (CARBON_STEAM_CALL *)(void*, const char*, const char*, int);
    using RequestListFunction = uint64_t (CARBON_STEAM_CALL *)(void*);
    using GetLobbyByIndexFunction = uint64_t (CARBON_STEAM_CALL *)(void*, int);
    using CreateLobbyFunction = uint64_t (CARBON_STEAM_CALL *)(void*, int, int);
    using JoinLobbyFunction = uint64_t (CARBON_STEAM_CALL *)(void*, uint64_t);
    using LeaveLobbyFunction = void (CARBON_STEAM_CALL *)(void*, uint64_t);
    using SetLobbyDataFunction = bool (CARBON_STEAM_CALL *)(void*, uint64_t, const char*, const char*);
    using GetLobbyDataFunction = const char* (CARBON_STEAM_CALL *)(void*, uint64_t, const char*);
    using GetAPICallResultFunction = bool (CARBON_STEAM_CALL *)(int, uint64_t, void*, int, int, bool*);

    // The Steam flat interface keeps Steamworks headers out of Carbon's public SDK.
    using GetSocketsFunction = void* (CARBON_STEAM_CALL *)();
    using CreateListenSocketFunction = uint32_t (CARBON_STEAM_CALL *)(void*, int, int, const void*);
    using ConnectP2PFunction = uint32_t (CARBON_STEAM_CALL *)(void*, const void*, int, int, const void*);
    using AcceptConnectionFunction = int (CARBON_STEAM_CALL *)(void*, uint32_t);
    using CloseConnectionFunction = bool (CARBON_STEAM_CALL *)(void*, uint32_t, int, const char*, bool);
    using CloseListenSocketFunction = bool (CARBON_STEAM_CALL *)(void*, uint32_t);
    using SendMessageFunction = int (CARBON_STEAM_CALL *)(void*, uint32_t, const void*, uint32_t, int, int64_t*);
    using ReceiveMessagesFunction = int (CARBON_STEAM_CALL *)(void*, uint32_t, void**, int);
    using ReleaseMessageFunction = void (CARBON_STEAM_CALL *)(void*);
    using GetConnectionInfoFunction = bool (CARBON_STEAM_CALL *)(void*, uint32_t, void*);
    using GetLobbyOwnerFunction = uint64_t (CARBON_STEAM_CALL *)(void*, uint64_t);
    using GetLobbyMemberCountFunction = int (CARBON_STEAM_CALL *)(void*, uint64_t);
    using GetLobbyMemberFunction = uint64_t (CARBON_STEAM_CALL *)(void*, uint64_t, int);
    using SetLobbyJoinableFunction = bool (CARBON_STEAM_CALL *)(void*, uint64_t, bool);
    using SendLobbyChatFunction = bool (CARBON_STEAM_CALL *)(void*, uint64_t, const void*, int);
    using GetLobbyChatFunction = int (CARBON_STEAM_CALL *)(void*, uint64_t, int, uint64_t*, void*, int, int*);
    using GetSteamUserFunction = void* (CARBON_STEAM_CALL *)();
    using GetSteamIDFunction = uint64_t (CARBON_STEAM_CALL *)(void*);
    using GetFriendsFunction = void* (CARBON_STEAM_CALL *)();
    using GetPersonaNameFunction = const char* (CARBON_STEAM_CALL *)(void*);
    using GetFriendPersonaNameFunction = const char* (CARBON_STEAM_CALL *)(void*, uint64_t);
    using SetRichPresenceFunction = bool (CARBON_STEAM_CALL *)(void*, const char*, const char*);

    // SteamNetworkingIdentity: 32-bit type + 32-bit size + 128 bytes of identity data.
    // SteamID identities have type 16 and place the 64-bit user ID at byte offset 8.
    struct SteamIdentity
    {
        int32_t type = 16;
        int32_t size = 8;
        uint64_t steamID = 0;
        unsigned char reserved[120] = {};
    };
    static_assert(sizeof(SteamIdentity) == 136, "Unexpected SteamNetworkingIdentity layout");

    // SteamNetConnectionInfo_t's initial fields. The full Steam structure contains
    // additional debugging strings and reserved fields, so GetConnectionInfo is
    // given a separate, generously sized buffer rather than this prefix alone.
    // The layout follows Steam's documented types, including the explicit IP
    // address padding field. Do not decode the nested callback payload directly.
    struct SteamConnectionInfoPrefix
    {
        SteamIdentity remote;
        int64_t userData;
        uint32_t listenSocket;
        unsigned char remoteAddress[18];
        uint16_t padding;
        uint32_t remotePOP;
        uint32_t relayPOP;
        int32_t state;
    };
    static_assert(offsetof(SteamConnectionInfoPrefix, listenSocket) == 144, "Unexpected Steam listen socket offset");
    static_assert(offsetof(SteamConnectionInfoPrefix, state) == 176, "Unexpected Steam connection state offset");

    struct PeerConnection
    {
        uint32_t handle = 0;
        bool connected = false;
    };

    void initialize(const SharedLibrary& library)
    {
        // Cloud storage is optional and does not participate in matchmaking availability.
        initializeCloudStorage(library);

        // The same matchmaking interface owns discovery and lobby lifetime.
        auto getMatchmaking = library.mapFunction<GetMatchmakingFunction>("SteamAPI_SteamMatchmaking_v009");
        if (!getMatchmaking)
            getMatchmaking = library.mapFunction<GetMatchmakingFunction>("SteamAPI_SteamMatchmaking_v008");

        addFilter_ = library.mapFunction<AddFilterFunction>("SteamAPI_ISteamMatchmaking_AddRequestLobbyListStringFilter");
        requestList_ = library.mapFunction<RequestListFunction>("SteamAPI_ISteamMatchmaking_RequestLobbyList");
        getLobbyByIndex_ = library.mapFunction<GetLobbyByIndexFunction>("SteamAPI_ISteamMatchmaking_GetLobbyByIndex");
        createLobby_ = library.mapFunction<CreateLobbyFunction>("SteamAPI_ISteamMatchmaking_CreateLobby");
        joinLobby_ = library.mapFunction<JoinLobbyFunction>("SteamAPI_ISteamMatchmaking_JoinLobby");
        leaveLobby_ = library.mapFunction<LeaveLobbyFunction>("SteamAPI_ISteamMatchmaking_LeaveLobby");
        setLobbyData_ = library.mapFunction<SetLobbyDataFunction>("SteamAPI_ISteamMatchmaking_SetLobbyData");
        getLobbyData_ = library.mapFunction<GetLobbyDataFunction>("SteamAPI_ISteamMatchmaking_GetLobbyData");
        getCallResult_ = library.mapFunction<GetAPICallResultFunction>("SteamAPI_ManualDispatch_GetAPICallResult");
        getLobbyOwner_ = library.mapFunction<GetLobbyOwnerFunction>("SteamAPI_ISteamMatchmaking_GetLobbyOwner");
        getLobbyMemberCount_ = library.mapFunction<GetLobbyMemberCountFunction>("SteamAPI_ISteamMatchmaking_GetNumLobbyMembers");
        getLobbyMember_ = library.mapFunction<GetLobbyMemberFunction>("SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex");
        setLobbyJoinable_ = library.mapFunction<SetLobbyJoinableFunction>("SteamAPI_ISteamMatchmaking_SetLobbyJoinable");
        sendLobbyChat_ = library.mapFunction<SendLobbyChatFunction>("SteamAPI_ISteamMatchmaking_SendLobbyChatMsg");
        getLobbyChat_ = library.mapFunction<GetLobbyChatFunction>("SteamAPI_ISteamMatchmaking_GetLobbyChatEntry");

        // Carbon's session-player identities are translated from Steam accounts privately.
        auto getSteamUser = library.mapFunction<GetSteamUserFunction>("SteamAPI_SteamUser_v023");
        if (!getSteamUser)
            getSteamUser = library.mapFunction<GetSteamUserFunction>("SteamAPI_SteamUser_v022");
        getSteamID_ = library.mapFunction<GetSteamIDFunction>("SteamAPI_ISteamUser_GetSteamID");
        steamUser_ = getSteamUser ? getSteamUser() : nullptr;
        localSteamID_ = steamUser_ && getSteamID_ ? getSteamID_(steamUser_) : 0;

        auto getFriends = library.mapFunction<GetFriendsFunction>("SteamAPI_SteamFriends_v017");
        if (!getFriends)
            getFriends = library.mapFunction<GetFriendsFunction>("SteamAPI_SteamFriends_v018");
        getPersonaName_ = library.mapFunction<GetPersonaNameFunction>("SteamAPI_ISteamFriends_GetPersonaName");
        getFriendPersonaName_ = library.mapFunction<GetFriendPersonaNameFunction>("SteamAPI_ISteamFriends_GetFriendPersonaName");
        setRichPresence_ = library.mapFunction<SetRichPresenceFunction>("SteamAPI_ISteamFriends_SetRichPresence");
        steamFriends_ = getFriends ? getFriends() : nullptr;

        // Steam Networking Sockets are resolved dynamically like the matchmaking API.
        auto getSockets = library.mapFunction<GetSocketsFunction>("SteamAPI_SteamNetworkingSockets_SteamAPI_v012");
        if (!getSockets)
            getSockets = library.mapFunction<GetSocketsFunction>("SteamAPI_SteamNetworkingSockets_SteamAPI_v011");

        createListenSocket_ = library.mapFunction<CreateListenSocketFunction>("SteamAPI_ISteamNetworkingSockets_CreateListenSocketP2P");
        connectP2P_ = library.mapFunction<ConnectP2PFunction>("SteamAPI_ISteamNetworkingSockets_ConnectP2P");
        acceptConnection_ = library.mapFunction<AcceptConnectionFunction>("SteamAPI_ISteamNetworkingSockets_AcceptConnection");
        closeConnection_ = library.mapFunction<CloseConnectionFunction>("SteamAPI_ISteamNetworkingSockets_CloseConnection");
        closeListenSocket_ = library.mapFunction<CloseListenSocketFunction>("SteamAPI_ISteamNetworkingSockets_CloseListenSocket");
        sendMessage_ = library.mapFunction<SendMessageFunction>("SteamAPI_ISteamNetworkingSockets_SendMessageToConnection");
        receiveMessages_ = library.mapFunction<ReceiveMessagesFunction>("SteamAPI_ISteamNetworkingSockets_ReceiveMessagesOnConnection");
        releaseMessage_ = library.mapFunction<ReleaseMessageFunction>("SteamAPI_SteamNetworkingMessage_t_Release");
        getConnectionInfo_ = library.mapFunction<GetConnectionInfoFunction>("SteamAPI_ISteamNetworkingSockets_GetConnectionInfo");
        sockets_ = getSockets ? getSockets() : nullptr;

        socketsAvailable_ = sockets_ && getLobbyOwner_ && getLobbyMemberCount_ && getLobbyMember_ &&
                            createListenSocket_ && connectP2P_ && acceptConnection_ && closeConnection_ &&
                            closeListenSocket_ && sendMessage_ && receiveMessages_ && releaseMessage_ &&
                            getConnectionInfo_;
        matchmaking_ = getMatchmaking ? getMatchmaking() : nullptr;

        available_ = matchmaking_ && addFilter_ && requestList_ && getLobbyByIndex_ && createLobby_ &&
                     joinLobby_ && leaveLobby_ && setLobbyData_ && getLobbyData_ && getCallResult_ &&
                     sendLobbyChat_ && getLobbyChat_;

        if (available_)
            LOG_INFO << "Steam multiplayer: lobby discovery/hosting/joining available";
        else
            LOG_WARNING_WITHOUT_CALLER << "Steam multiplayer: required matchmaking exports unavailable";

        if (!socketsAvailable_)
            LOG_WARNING_WITHOUT_CALLER << "Steam multiplayer: Networking Sockets exports unavailable";
        if (!localSteamID_)
            LOG_WARNING_WITHOUT_CALLER << "Steam multiplayer: local Steam identity unavailable for session registry";
    }

    ~SteamMultiplayer() override = default;

    void cloudPersist() override { synchronizeCloud(true); }
    void cloudUpdate() override { synchronizeCloud(false); }

    bool isAvailable() const override { return available_; }

    String getLocalPlayerName() const override
    {
        // Persona names are display text, independent of the current network session.
        if (steamFriends_ && getPersonaName_)
        {
            const auto name = getPersonaName_(steamFriends_);
            if (name && *name)
                return name;
        }

        return "Player";
    }

    bool setPlatformValue(const String& key, const String& value) override
    {
        // The provider understands the key; Carbon does not translate either string.
        if (!steamFriends_ || !setRichPresence_ || !key.length())
            return false;

        const bool accepted = setRichPresence_(steamFriends_, key.cStr(), value.cStr());
        if (!accepted)
            return false;

        // Remember only successfully applied keys for session teardown.
        platformValueKeys_.insert(std::string(key.cStr(), key.length()));
        return true;
    }

    bool clearPlatformValue(const String& key) override
    {
        if (!steamFriends_ || !setRichPresence_ || !key.length())
            return false;

        // Steam removes this key when its value is null; other keys are untouched.
        const bool accepted = setRichPresence_(steamFriends_, key.cStr(), nullptr);
        if (!accepted)
            return false;

        platformValueKeys_.erase(std::string(key.cStr(), key.length()));
        return true;
    }

    void clearPlatformValues()
    {
        // Clear even when leaving a lobby fails or this provider is shutting down.
        if (steamFriends_ && setRichPresence_)
        {
            for (const auto& key : platformValueKeys_)
            {
                const bool accepted = setRichPresence_(steamFriends_, key.c_str(), nullptr);
                if (!accepted)
                    LOG_WARNING_WITHOUT_CALLER << "Steam multiplayer: could not clear platform value " << key.c_str();
            }
        }

        platformValueKeys_.clear();
    }

    bool isSearching() const override
    {
        return searching_ || (pendingRole_ == Role::Guest && pendingGuestOperation_ == MultiplayerOperation::FindLobby);
    }
    bool isInLobby() const override { return lobbyID_ != 0; }
    bool isLobbyPending() const override { return searching_ || pendingLobbyCall_ != 0 || pendingRole_ != Role::None; }
    uint64_t getLobbyID() const override { return lobbyID_; }

    std::string getManifestPlayerName(uint64_t steamID) const
    {
        const auto key = std::string("carbon_player_name_") + std::to_string(steamID);
        const auto value = getLobbyData_(matchmaking_, lobbyID_, key.c_str());
        return value && *value ? value : "Steam Player";
    }

    // The manifest is ordered by Carbon PlayerID, independent of Steam lobby order.
    // Each entry records the Carbon ID, the provider identity and the display name.
    std::string buildManifest(const std::map<uint32_t, uint64_t>& members) const
    {
        return SessionDetail::SessionManifestCodec::buildRoster(
            members, [](uint64_t member) { return std::to_string(member); },
            [this](uint64_t member) { return getManifestPlayerName(member); });
    }

    // Read the manifest and revision as one authoritative Steam lobby-data value.
    // A single write prevents guests from pairing a new body with an old revision.
    bool readManifestSnapshot(uint64_t& revision, std::string& manifest) const
    {
        revision = 0;
        manifest.clear();

        if (!lobbyID_ || !getLobbyData_)
            return false;

        const auto rawSnapshot = getLobbyData_(matchmaking_, lobbyID_, "carbon_manifest_snapshot");
        if (!rawSnapshot)
            return false;

        return SessionDetail::SessionManifestCodec::decodeSnapshot(rawSnapshot, revision, manifest);
    }

    // Announcements are advisory; lobby metadata is always the authoritative source.
    bool announceManifestRevision(uint64_t revision)
    {
        const auto message = std::string("CARBON_MANIFEST_UPDATED|") + std::to_string(revision);
        return sendLobbyChat_ && sendLobbyChat_(matchmaking_, lobbyID_, message.data(), int(message.size()));
    }

    // Optional metadata override allows explicit publication without exposing uncommitted edits.
    bool publishManifest(const MultiplayerSessionMetadata* metadata = nullptr)
    {
        if (!lobbyID_ || sessionController_.state() != MultiplayerSessionState::Lobby ||
            getLobbyOwner_(matchmaking_, lobbyID_) != localSteamID_)
            return false;

        // Wait for Carbon's existing host-assigned registry to be complete.
        const auto count = getLobbyMemberCount_(matchmaking_, lobbyID_);
        std::map<uint32_t, uint64_t> members;
        for (int index = 0; index < count; ++index)
        {
            const auto steamID = getLobbyMember_(matchmaking_, lobbyID_, index);
            const auto found = sessionController_.registry().assignments().find(steamID);
            if (found == sessionController_.registry().assignments().end())
                return false;
            members[found->second] = steamID;
        }
        if (int(members.size()) != count)
            return false;

        const auto manifest = SessionDetail::SessionManifestCodec::appendSessionMetadata(
            buildManifest(members), metadata ? *metadata : getSession().getMetadata());
        if (manifest.size() + std::to_string(sessionController_.agreement().revision() + 1).size() + 1 > 4096)
        {
            LOG_WARNING_WITHOUT_CALLER << "Steam multiplayer: player manifest exceeds lobby-message limit";
            return false;
        }
        if (manifest == sessionController_.agreement().manifest())
            return true;

        // Publish revision and manifest atomically through one Steam lobby-data key.
        // Failed writes leave the previously committed snapshot and confirmations intact.
        const auto nextRevision = sessionController_.agreement().revision() + 1;
        const auto snapshot = std::to_string(nextRevision) + "|" + manifest;
        if (!setLobbyData_(matchmaking_, lobbyID_, "carbon_manifest_snapshot", snapshot.c_str()))
        {
            LOG_WARNING_WITHOUT_CALLER << "Steam multiplayer: failed to publish player manifest";
            return false;
        }

        sessionController_.agreement().publish(nextRevision, manifest, localSteamID_);
        announceManifestRevision(sessionController_.agreement().revision());
        LOG_INFO << "Steam multiplayer: ManifestUpdated revision " << sessionController_.agreement().revision()
                 << ", members=" << members.size();
        return true;
    }

    // Clients confirm their locally reconstructed registry, not a blind copy of metadata.
    void confirmManifest()
    {
        if (!lobbyID_ || (sessionController_.state() != MultiplayerSessionState::Lobby &&
                          sessionController_.state() != MultiplayerSessionState::Syncing))
            return;

        const auto host = getLobbyOwner_(matchmaking_, lobbyID_);
        if (host == localSteamID_)
            return;

        uint64_t revision = 0;
        std::string receivedManifest;
        if (!readManifestSnapshot(revision, receivedManifest))
            return;

        // Validate the host snapshot against the local Carbon roster and metadata codec.
        MultiplayerSessionMetadata receivedMetadata;
        std::string localManifest;
        using Validation = SessionDetail::SessionController<uint64_t>::ManifestValidation;
        const auto validation = sessionController_.validateManifest(
            getSession(), receivedManifest, [](uint64_t member) { return std::to_string(member); },
            [this](uint64_t member) { return getManifestPlayerName(member); }, receivedMetadata, localManifest);
        if (validation != Validation::Valid)
        {
            if (validation == Validation::InvalidMetadata)
                LOG_WARNING_WITHOUT_CALLER << "Steam multiplayer: invalid session metadata for manifest revision " << revision;
            else if (validation == Validation::DifferentManifest)
                LOG_WARNING_WITHOUT_CALLER << "Steam multiplayer: locally reconstructed manifest differs from host revision " << revision;
            return;
        }

        // Reject a snapshot that was superseded while we reconstructed the registry.
        uint64_t currentRevision = 0;
        std::string currentManifest;
        if (!readManifestSnapshot(currentRevision, currentManifest) ||
            currentRevision != revision || currentManifest != receivedManifest)
            return;

        // Expose only complete, validated snapshots to the application.
        if (getSession().getMetadata() != receivedMetadata)
        {
            mutableSession().setMetadata(receivedMetadata);
            dispatchSessionUpdated();

            // An application handler can leave or replace the session. Resume
            // confirmation on the next update rather than using stale state.
            return;
        }

        // Re-send if the revision changed or the synchronization phase was entered.
        if (sessionController_.agreement().hasLocalConfirmation(revision, sessionController_.state()))
            return;

        const auto message = sessionController_.confirmationMessage(revision, localManifest);
        if (sendLobbyChat_ && sendLobbyChat_(matchmaking_, lobbyID_, message.data(), int(message.size())))
        {
            sessionController_.agreement().markLocalConfirmation(revision, sessionController_.state());
            LOG_INFO << "Steam multiplayer: ManifestConfirm sent for revision " << revision;
        }
    }

    // Only the host can finish synchronization; confirmation and readiness are independent.
    void beginPlayingIfReady()
    {
        if (!lobbyID_ || !sessionController_.canBeginPlaying(getLobbyOwner_(matchmaking_, lobbyID_) == localSteamID_))
            return;

        // Readiness records alone cannot establish current lobby membership.
        // A frozen participant may have left after confirming and becoming ready.
        for (const auto steamID : sessionController_.frozenMembers())
        {
            if (!isLobbyMember(steamID))
                return;
        }

        // Publish the phase and frozen roster together. Never expose Playing
        // without the same authoritative members and manifest revision.
        std::string roster;
        for (const auto steamID : sessionController_.frozenMembers())
        {
            if (!roster.empty())
                roster += ",";
            roster += std::to_string(steamID);
        }

        const auto snapshot = std::string("P|") + std::to_string(sessionController_.agreement().revision()) + "|" + roster;
        if (!setLobbyData_(matchmaking_, lobbyID_, "carbon_session_start", snapshot.c_str()))
        {
            LOG_WARNING_WITHOUT_CALLER << "Steam multiplayer: failed to publish Playing phase";
            return;
        }

        const auto previousState = sessionController_.enterPlaying(mutableSession(), lobbyID_);
        const auto generation = sessionController_.generation();
        dispatchSessionStateChanged(previousState);

        // An event handler may have ended this session while dispatching.
        if (generation != sessionController_.generation() || sessionController_.state() != MultiplayerSessionState::Playing)
            return;

        const auto message = std::string("CARBON_BEGIN_PLAYING|") + std::to_string(sessionController_.agreement().revision());
        if (sendLobbyChat_)
            sendLobbyChat_(matchmaking_, lobbyID_, message.data(), int(message.size()));

        LOG_INFO << "Steam multiplayer: all manifest confirmations and game readiness received; Playing";
    }

    // Application readiness follows the validated Syncing notification.
    // Still defer outgoing readiness until our locked-manifest confirmation is submitted.
    void sendPendingReadiness()
    {
        if (sessionController_.state() != MultiplayerSessionState::Syncing || sessionController_.agreement().requestedReadiness() < 0 ||
            getLobbyOwner_(matchmaking_, lobbyID_) == localSteamID_ || !sessionController_.agreement().hasPendingReadiness())
            return;

        uint64_t revision = 0;
        std::string manifest;
        if (!readManifestSnapshot(revision, manifest) || revision != sessionController_.agreement().confirmedRevision())
            return;

        const auto message = sessionController_.readinessMessage(revision);
        if (sendLobbyChat_ && sendLobbyChat_(matchmaking_, lobbyID_, message.data(), int(message.size())))
        {
            sessionController_.agreement().markLocalReadinessSent(revision);
            LOG_INFO << "Steam multiplayer: PlayerReady " << sessionController_.agreement().requestedReadiness() << " sent for revision " << revision;
        }
    }

    bool setReady(bool ready) override
    {
        if (!lobbyID_ || sessionController_.state() != MultiplayerSessionState::Syncing ||
            !getSession().getLocalPlayerID().isValid())
            return false;

        // The host's own readiness is tracked by the same barrier as remote clients.
        if (getLobbyOwner_(matchmaking_, lobbyID_) == localSteamID_)
        {
            sessionController_.agreement().setMemberReady(localSteamID_, ready);
            beginPlayingIfReady();
            return true;
        }

        sessionController_.agreement().requestLocalReadiness(ready);
        sendPendingReadiness();
        return true;
    }

    // Steam supplies the authenticated sender separately from untrusted message bytes.
    void onLobbyChat(const unsigned char* data, int size)
    {
        if (!lobbyID_ || !data || size < int(sizeof(SteamLobbyChatMessage)) || !getLobbyChat_)
            return;

        // Copy the callback to avoid assuming Steam's buffer alignment.
        SteamLobbyChatMessage callback = {};
        memcpy(&callback, data, sizeof(callback));
        if (callback.lobbyID != lobbyID_)
            return;

        char buffer[8192] = {};
        uint64_t sender = 0;
        int entryType = 0;
        const auto length = getLobbyChat_(matchmaking_, lobbyID_, callback.chatID, &sender, buffer, sizeof(buffer), &entryType);
        if (length <= 0 || length > int(sizeof(buffer)) || !isLobbyMember(sender))
            return;

        const std::string message(buffer, buffer + length);

        // A host announcement signals an updated metadata snapshot. The regular update
        // will reconstruct it and send an explicit confirmation when it is complete.
        const std::string updatedPrefix = "CARBON_MANIFEST_UPDATED|";
        if (message.compare(0, updatedPrefix.size(), updatedPrefix) == 0)
        {
            if (sender == getLobbyOwner_(matchmaking_, lobbyID_))
                LOG_INFO << "Steam multiplayer: ManifestUpdated received from host";
            return;
        }

        // A readiness message is valid only from a locked member for the active revision.
        const std::string readyPrefix = "CARBON_PLAYER_READY|";
        if (message.compare(0, readyPrefix.size(), readyPrefix) == 0)
        {
            std::string value;
            if (!sessionController_.acceptReadinessMessage(
                    sender, getLobbyOwner_(matchmaking_, lobbyID_) == localSteamID_, message, value))
                return;

            LOG_INFO << "Steam multiplayer: PlayerReady " << value.c_str() << " from " << sender;
            beginPlayingIfReady();
            return;
        }

        // Guests use the metadata phase as authoritative; this is only a prompt to update.
        const std::string beginPrefix = "CARBON_BEGIN_PLAYING|";
        if (message.compare(0, beginPrefix.size(), beginPrefix) == 0)
        {
            if (sender == getLobbyOwner_(matchmaking_, lobbyID_))
                LOG_INFO << "Steam multiplayer: BeginPlaying received from host";
            return;
        }

        using Confirmation = SessionDetail::SessionController<uint64_t>::ConfirmationResult;
        uint64_t revision = 0;
        const auto result = sessionController_.acceptConfirmationMessage(
            sender, getLobbyOwner_(matchmaking_, lobbyID_) == localSteamID_, message, revision);
        if (result == Confirmation::Invalid)
        {
            LOG_WARNING_WITHOUT_CALLER << "Steam multiplayer: invalid ManifestConfirm from member " << sender;
            return;
        }

        if (result == Confirmation::Accepted)
        {
            LOG_INFO << "Steam multiplayer: ManifestConfirm accepted from " << sender
                     << " for revision " << revision;
            if (sessionController_.state() == MultiplayerSessionState::Syncing &&
                sessionController_.agreement().confirmationCount() == sessionController_.frozenMembers().size())
                LOG_INFO << "Steam multiplayer: all locked members confirmed manifest revision " << revision;
            beginPlayingIfReady();
        }
    }

    bool startGame() override
    {
        // Only the host can freeze the roster; wait until all lobby members have IDs.
        if (!lobbyID_ || !localSteamID_ || !getLobbyOwner_ || !getLobbyMemberCount_ || !getLobbyMember_ ||
            getLobbyOwner_(matchmaking_, lobbyID_) != localSteamID_ ||
            sessionController_.state() != MultiplayerSessionState::Lobby || sessionController_.metadataDirty() ||
            mutableSession().getPlayerCount() != unsigned(getLobbyMemberCount_(matchmaking_, lobbyID_)))
            return false;

        // Record every participant before publishing the game phase. Order is by PlayerID.
        std::map<uint32_t, uint64_t> ordered;
        const auto count = getLobbyMemberCount_(matchmaking_, lobbyID_);
        for (int index = 0; index < count; ++index)
        {
            const auto steamID = getLobbyMember_(matchmaking_, lobbyID_, index);
            const auto found = sessionController_.registry().assignments().find(steamID);
            if (found != sessionController_.registry().assignments().end())
                ordered[found->second] = steamID;
        }

        std::string publishedRoster;
        for (const auto& player : ordered)
        {
            if (!publishedRoster.empty())
                publishedRoster += ",";
            publishedRoster += std::to_string(player.second);
        }

        // Lock only the exact current published snapshot; failed metadata publication cannot start a game.
        publishManifest();
        if (!sessionController_.agreement().revision() || sessionController_.agreement().manifest() !=
            SessionDetail::SessionManifestCodec::appendSessionMetadata(buildManifest(ordered), getSession().getMetadata()))
            return false;

        // The frozen roster, phase and manifest revision form one lobby-data
        // snapshot. A failed write leaves the lobby in its previous phase.
        const auto startSnapshot = std::string("S|") + std::to_string(sessionController_.agreement().revision()) + "|" + publishedRoster;
        if (!setLobbyData_(matchmaking_, lobbyID_, "carbon_session_start", startSnapshot.c_str()))
            return false;

        sessionController_.frozenMembers().clear();
        for (const auto& player : ordered)
            sessionController_.frozenMembers().push_back(player.second);

        if (setLobbyJoinable_ && !setLobbyJoinable_(matchmaking_, lobbyID_, false))
            LOG_WARNING_WITHOUT_CALLER << "Steam multiplayer: unable to close the lobby to new members";

        sessionController_.beginSynchronization(localSteamID_);
        updateSessionRegistry();
        announceManifestRevision(sessionController_.agreement().revision());
        LOG_INFO << "Steam multiplayer: session player roster locked for synchronization";
        return true;
    }

    unsigned int getConnectedPlayerCount() const override
    {
        // Count only authenticated remote participants in the current Carbon registry.
        unsigned int count = 0;
        for (const auto& player : sessionController_.registry().destinations())
        {
            const auto peer = peers_.find(player.second);
            if (player.second != localSteamID_ && peer != peers_.end() && peer->second.connected &&
                isLobbyMember(player.second))
                ++count;
        }
        return count;
    }

    PlayerID getConnectedPlayer(unsigned int index) const override
    {
        // Return stable Carbon identities, in PlayerID order.
        for (const auto& player : sessionController_.registry().destinations())
        {
            const auto peer = peers_.find(player.second);
            if (player.second != localSteamID_ && peer != peers_.end() && peer->second.connected &&
                isLobbyMember(player.second) && index-- == 0)
                return PlayerID(player.first);
        }
        return PlayerID();
    }

    bool send(PlayerID recipient, const NetFrame& frame) override
    {
        // Resolve a remote Carbon participant to its authenticated Steam connection.
        if (!recipient.isValid())
        {
            LOG_WARNING_WITHOUT_CALLER << "Steam multiplayer: rejected NetFrame send to invalid PlayerID";
            return false;
        }

        const auto destination = sessionController_.registry().destinations().find(recipient.getValue());
        if (destination == sessionController_.registry().destinations().end() || destination->second == localSteamID_)
        {
            LOG_WARNING_WITHOUT_CALLER << "Steam multiplayer: rejected NetFrame send to unknown or local PlayerID "
                                       << recipient.getValue();
            return false;
        }

        const auto peer = peers_.find(destination->second);
        if (!socketsAvailable_ || !lobbyID_ || !sendMessage_ || peer == peers_.end() ||
            !peer->second.connected || !isLobbyMember(destination->second))
        {
            LOG_WARNING_WITHOUT_CALLER << "Steam multiplayer: rejected NetFrame send to PlayerID "
                                       << recipient.getValue() << ": transport or authenticated peer unavailable";
            return false;
        }

        // Encode the game frame before submitting it to Steam's reliable transport.
        Vector<byte_t> data;
        if (!frame.encode(data) || data.empty() || data.size() > 512 * 1024)
        {
            LOG_WARNING_WITHOUT_CALLER << "Steam multiplayer: rejected NetFrame send: encoding failed or invalid encoded size "
                                       << data.size();
            return false;
        }

        // k_nSteamNetworkingSend_Reliable = 8; 1 means k_EResultOK.
        const auto status = sendMessage_(sockets_, peer->second.handle, data.getData(),
                                         uint32_t(data.size()), 8, nullptr);
        if (status != 1)
            LOG_WARNING_WITHOUT_CALLER << "Steam multiplayer: failed to send NetFrame to PlayerID "
                                       << recipient.getValue() << ", Steam result " << status;

        return status == 1;
    }

    bool broadcast(const NetFrame& frame) override
    {
        // Discover authenticated remote participants from the Carbon registry.
        if (!socketsAvailable_ || !lobbyID_ || !sendMessage_)
        {
            LOG_WARNING_WITHOUT_CALLER << "Steam multiplayer: rejected NetFrame broadcast: transport or lobby unavailable";
            return false;
        }

        std::vector<uint32_t> recipients;
        for (const auto& player : sessionController_.registry().destinations())
        {
            const auto peer = peers_.find(player.second);
            if (player.second != localSteamID_ && peer != peers_.end() && peer->second.connected &&
                isLobbyMember(player.second))
                recipients.push_back(player.first);
        }

        if (recipients.empty())
        {
            LOG_WARNING_WITHOUT_CALLER << "Steam multiplayer: rejected NetFrame broadcast: no connected remote recipients";
            return false;
        }

        // Encode once, then submit the same bytes to each connected remote recipient.
        Vector<byte_t> data;
        if (!frame.encode(data) || data.empty() || data.size() > 512 * 1024)
        {
            LOG_WARNING_WITHOUT_CALLER << "Steam multiplayer: rejected NetFrame broadcast: encoding failed or invalid encoded size "
                                       << data.size();
            return false;
        }

        bool allSubmitted = true;
        for (const auto playerID : recipients)
        {
            const auto destination = sessionController_.registry().destinations().find(playerID);
            if (destination == sessionController_.registry().destinations().end())
            {
                LOG_WARNING_WITHOUT_CALLER << "Steam multiplayer: skipped NetFrame broadcast to missing PlayerID " << playerID;
                allSubmitted = false;
                continue;
            }

            const auto peer = peers_.find(destination->second);
            if (peer == peers_.end() || !peer->second.connected || !isLobbyMember(destination->second))
            {
                LOG_WARNING_WITHOUT_CALLER << "Steam multiplayer: skipped NetFrame broadcast to PlayerID "
                                           << playerID << ": authenticated peer unavailable";
                allSubmitted = false;
                continue;
            }

            // A failed submission does not prevent attempts to reach other participants.
            const auto status = sendMessage_(sockets_, peer->second.handle, data.getData(),
                                             uint32_t(data.size()), 8, nullptr);
            if (status != 1)
            {
                LOG_WARNING_WITHOUT_CALLER << "Steam multiplayer: failed to broadcast NetFrame to PlayerID "
                                           << playerID << ", Steam result " << status;
                allSubmitted = false;
            }
        }

        return allSubmitted;
    }

    /** Reconcile Steam membership with Carbon's shared, host-assigned player registry. */
    void updateSessionRegistry()
    {
        if (!lobbyID_ || !localSteamID_ || !getLobbyOwner_ || !getLobbyMemberCount_ || !getLobbyMember_)
            return;

        const auto hostSteamID = getLobbyOwner_(matchmaking_, lobbyID_);

        // The initial owner may not yet be visible at lobby-entry callback time.
        if (!sessionHostSteamID_ && hostSteamID)
            sessionHostSteamID_ = hostSteamID;

        // Steam may migrate lobby ownership when its owner leaves. Carbon V1 does
        // not support host migration: end the original session instead of silently
        // accepting a replacement authority with incompatible player assignments.
        if (sessionHostSteamID_ && hostSteamID && hostSteamID != sessionHostSteamID_)
        {
            LOG_WARNING_WITHOUT_CALLER << "Steam multiplayer: original session host left; ending session";
            endSession(MultiplayerSessionEndReason::HostLost);
            return;
        }

        const bool isHost = pendingEntryWasHost_ && hostSteamID == localSteamID_;
        const auto memberCount = getLobbyMemberCount_(matchmaking_, lobbyID_);
        if (!hostSteamID || memberCount < 1)
            return;

        // The host publishes the frozen phase, revision and ordered members
        // in one value, so clients cannot combine different startup revisions.
        const auto rawStart = getLobbyData_(matchmaking_, lobbyID_, "carbon_session_start");
        if (rawStart && *rawStart)
        {
            const std::string snapshot(rawStart);
            const auto firstSeparator = snapshot.find('|');
            const auto secondSeparator = firstSeparator == std::string::npos ? std::string::npos
                                                                                : snapshot.find('|', firstSeparator + 1);
            if (firstSeparator != 1 || secondSeparator == std::string::npos ||
                secondSeparator + 1 == snapshot.size() || (snapshot[0] != 'S' && snapshot[0] != 'P'))
                return;

            const auto revisionText = snapshot.substr(firstSeparator + 1, secondSeparator - firstSeparator - 1);
            char* revisionTail = nullptr;
            const auto startRevision = std::strtoull(revisionText.c_str(), &revisionTail, 10);
            if (!startRevision || !revisionTail || *revisionTail ||
                std::to_string(startRevision) != revisionText)
                return;

            // A phase change is meaningful only for the manifest it freezes.
            uint64_t manifestRevision = 0;
            std::string manifest;
            if (!readManifestSnapshot(manifestRevision, manifest) || manifestRevision != startRevision)
                return;

            if (sessionController_.frozenMembers().empty())
            {
                const auto encoded = snapshot.substr(secondSeparator + 1);
                size_t start = 0;
                std::vector<uint64_t> parsed;
                while (start < encoded.size())
                {
                    const auto end = encoded.find(',', start);
                    const auto item = encoded.substr(start, end == std::string::npos ? end : end - start);
                    char* tail = nullptr;
                    const auto number = std::strtoull(item.c_str(), &tail, 10);
                    if (!number || !tail || *tail || std::to_string(number) != item ||
                        std::find(parsed.begin(), parsed.end(), uint64_t(number)) != parsed.end())
                        return;
                    parsed.push_back(uint64_t(number));
                    if (end == std::string::npos)
                        break;
                    start = end + 1;
                }
                if (parsed.empty() || parsed.front() != hostSteamID)
                    return;
                sessionController_.frozenMembers() = parsed;
            }

            // A later phase must never silently replace the already frozen roster.
            std::string frozenRoster;
            for (const auto steamID : sessionController_.frozenMembers())
            {
                if (!frozenRoster.empty())
                    frozenRoster += ",";
                frozenRoster += std::to_string(steamID);
            }
            if (snapshot.substr(secondSeparator + 1) != frozenRoster)
                return;

            // Guests validate their locked manifest before accepting Playing.
            sessionController_.acceptPublishedPhase(snapshot[0] == 'P', startRevision, hostSteamID == localSteamID_);
        }

        // Assign IDs only on the host, in join order. PlayerID 1 always belongs to the host.
        if (isHost && sessionController_.registry().assignments().empty() && sessionController_.state() == MultiplayerSessionState::Lobby)
        {
            sessionController_.registry().assign(hostSteamID);
        }

        std::map<uint32_t, MultiplayerPlayer> orderedRoster;
        std::map<uint32_t, uint64_t> destinations;
        PlayerID localID;
        PlayerID hostID;

        const int registryCount = (sessionController_.state() == MultiplayerSessionState::Syncing ||
                                   sessionController_.state() == MultiplayerSessionState::Playing)
                                      ? int(sessionController_.frozenMembers().size()) : memberCount;
        for (int index = 0; index < registryCount; ++index)
        {
            const auto steamID = (sessionController_.state() == MultiplayerSessionState::Syncing ||
                                  sessionController_.state() == MultiplayerSessionState::Playing)
                                     ? sessionController_.frozenMembers()[index]
                                     : getLobbyMember_(matchmaking_, lobbyID_, index);
            if (!steamID)
                continue;

            const auto idKey = std::string("carbon_player_id_") + std::to_string(steamID);
            const auto nameKey = std::string("carbon_player_name_") + std::to_string(steamID);

            if (isHost && sessionController_.state() == MultiplayerSessionState::Lobby)
            {
                const auto isNewMember = sessionController_.registry().assignments().count(steamID) == 0;
                const auto assignedID = sessionController_.registry().assign(steamID);
                if (isNewMember)
                {
                    LOG_INFO << "Steam multiplayer: assigned Carbon PlayerID " << assignedID << " to Steam member " << steamID;
                }

                // Publish the shared ID and display name through the owner-managed lobby data.
                const auto published = std::to_string(assignedID);
                const auto previousID = getLobbyData_(matchmaking_, lobbyID_, idKey.c_str());
                if (!previousID || published != previousID)
                    setLobbyData_(matchmaking_, lobbyID_, idKey.c_str(), published.c_str());

                const auto name = getSteamPlayerName(steamID);
                const auto previousName = getLobbyData_(matchmaking_, lobbyID_, nameKey.c_str());
                if (!previousName || name != previousName)
                    setLobbyData_(matchmaking_, lobbyID_, nameKey.c_str(), name.c_str());
            }

            // All clients read the owner's published mapping, never independently number members.
            const auto rawID = getLobbyData_(matchmaking_, lobbyID_, idKey.c_str());
            if (!rawID || !*rawID)
                continue;

            char* tail = nullptr;
            const auto number = std::strtoul(rawID, &tail, 10);
            if (!tail || *tail || !number || number > UINT32_MAX)
                continue;

            const PlayerID playerID{uint32_t(number)};
            const auto rawName = getLobbyData_(matchmaking_, lobbyID_, nameKey.c_str());
            const auto name = rawName && *rawName ? String(rawName) : String("Steam Player");

            MultiplayerConnectionState connection = MultiplayerConnectionState::Disconnected;
            if (steamID == localSteamID_)
                connection = MultiplayerConnectionState::Connected;
            else
            {
                const auto peer = peers_.find(steamID);
                if (peer != peers_.end())
                    connection = peer->second.connected ? MultiplayerConnectionState::Connected
                                                         : MultiplayerConnectionState::Connecting;
            }

            // Keep iteration order identical everywhere and reject duplicate published IDs.
            if (destinations.find(playerID.getValue()) != destinations.end())
                continue;
            orderedRoster.insert({playerID.getValue(), MultiplayerPlayer(playerID, name, connection)});
            destinations[playerID.getValue()] = steamID;

            if (steamID == localSteamID_)
                localID = playerID;
            if (steamID == hostSteamID)
                hostID = playerID;
        }

        Vector<MultiplayerPlayer> roster;
        for (const auto& entry : orderedRoster)
            roster.append(entry.second);

        auto& session = mutableSession();
        const auto reconciliation = sessionController_.reconcileRoster(session, lobbyID_, isHost, hostID, localID,
                                                                      roster, destinations);

        // Publish and validate the full registry before notifying application handlers.
        const auto generation = sessionController_.generation();
        publishManifest();
        confirmManifest();

        // A metadata notification may have ended or replaced the session.
        if (generation != sessionController_.generation() || lobbyID_ == 0)
            return;

        if (reconciliation.enteringGuestSync)
        {
            uint64_t revision = 0;
            std::string manifest;
            const bool manifestConfirmed = readManifestSnapshot(revision, manifest) &&
                                           sessionController_.agreement().confirmedRevision() &&
                                           sessionController_.agreement().hasLocalConfirmation(revision, MultiplayerSessionState::Syncing);

            // If a manifest is incomplete, stay publicly in Lobby and retry next update.
            if (manifestConfirmed)
                session.setIdentity(lobbyID_, MultiplayerSessionState::Syncing, hostID, localID);
        }

        // The application can now read the validated metadata in its Syncing handler.
        if (!sessionController_.dispatchRosterEvents(
                reconciliation, generation,
                [this](MultiplayerSessionState previous) { dispatchSessionStateChanged(previous); },
                [this]() { dispatchSessionUpdated(); },
                [this](PlayerID player, MultiplayerConnectionState previous, MultiplayerConnectionState current)
                {
                    dispatchPlayerConnectionStateChanged(player, previous, current);
                }))
            return;

        sendPendingReadiness();
        beginPlayingIfReady();

        if (reconciliation.rosterChanged)
        {
            LOG_INFO << "Carbon multiplayer: session " << lobbyID_ << " registry has " << roster.size()
                     << " players (local=" << localID.getValue() << ", host=" << hostID.getValue() << ")";
            for (const auto& player : roster)
                LOG_INFO << "Carbon multiplayer: PlayerID=" << player.getID().getValue()
                         << " name=" << player.getName();
        }
    }

    /** Forget a failed connection only if its handle still belongs to a tracked peer. */
    bool forgetPeerConnection(uint32_t handle)
    {
        for (auto it = peers_.begin(); it != peers_.end(); ++it)
        {
            if (it->second.handle != handle)
                continue;

            // Preserve lobby membership and PlayerID; registry reconciliation reports
            // the connectivity transition through the application event system.
            LOG_INFO << "Steam multiplayer: peer disconnected " << it->first;
            peers_.erase(it);
            return true;
        }

        // An obsolete callback must not affect a newer connection to this peer.
        return false;
    }

    // Called from the connection platform's existing main-thread manual callback loop.
    void onConnectionStatus(const unsigned char* data, int size)
    {
        // SteamNetConnectionStatusChangedCallback_t starts with the connection handle.
        // Endeavor's working Steam implementation queries the current state and
        // authenticated identity with GetConnectionInfo rather than decoding the
        // nested SteamNetConnectionInfo_t from callback memory.
        if (!socketsAvailable_ || !data || size < int(sizeof(uint32_t)))
            return;

        uint32_t handle = 0;
        memcpy(&handle, data, sizeof(handle));
        if (!handle)
            return;

        // Steam writes the complete connection info, not just its prefix.
        // Reserve enough aligned space for the current Steam ABI structure.
        alignas(8) unsigned char storage[1024] = {};
        if (!getConnectionInfo_(sockets_, handle, storage))
        {
            // Steam may have already disposed of a closed handle before this callback
            // is processed. Drop a matching tracked connection instead of leaving it
            // indefinitely marked Connected; ignore handles that have been replaced.
            const bool tracked = forgetPeerConnection(handle);
            LOG_WARNING_WITHOUT_CALLER << "Steam multiplayer: GetConnectionInfo failed for handle " << handle
                                       << (tracked ? " (tracked peer disconnected)" : " (stale callback)");
            return;
        }

        SteamConnectionInfoPrefix info = {};
        memcpy(&info, storage, sizeof(info));

        const auto peer = info.remote.steamID;
        const auto identityType = info.remote.type;
        const auto listen = info.listenSocket;
        const auto state = info.state;

        LOG_INFO << "Steam multiplayer: connection handle=" << handle
                 << " state=" << state << " peer=" << peer
                 << " identityType=" << identityType << " listen=" << listen
                 << " localListen=" << listenSocket_;

        // k_ESteamNetworkingConnectionState_Connecting = 1.
        if (state == 1 && listen != 0)
        {
            // Reject unsolicited connections and peers not belonging to our current lobby.
            if (listen != listenSocket_ || identityType != 16 || !isLobbyMember(peer) ||
                peers_.find(peer) != peers_.end())
            {
                closeConnection_(sockets_, handle, 0, "Not a permitted lobby peer", false);
                return;
            }

            if (acceptConnection_(sockets_, handle) != 1)
            {
                closeConnection_(sockets_, handle, 0, "Failed to accept connection", false);
                return;
            }

            PeerConnection connection;
            connection.handle = handle;
            peers_[peer] = connection;
            LOG_INFO << "Steam multiplayer: accepting connection from peer " << peer;
        }

        // k_ESteamNetworkingConnectionState_Connected = 3.
        if (state == 3)
        {
            const auto found = peers_.find(peer);
            if (identityType != 16 || !isLobbyMember(peer) || found == peers_.end() ||
                found->second.handle != handle)
            {
                closeConnection_(sockets_, handle, 0, "Unexpected network connection", false);
                return;
            }

            found->second.connected = true;
            LOG_INFO << "Steam multiplayer: peer connected " << peer;
        }

        // ClosedByPeer = 4, ProblemDetectedLocally = 5.
        if (state == 4 || state == 5)
        {
            // Match the connection handle, not just the Steam identity, so a late
            // close notification cannot tear down a replacement connection.
            forgetPeerConnection(handle);
            closeConnection_(sockets_, handle, 0, "Connection ended", false);
        }
    }

    void updateSockets()
    {
        if (!socketsAvailable_ || !lobbyID_)
            return;

        // Snapshot connection handles because callbacks can leave the lobby during dispatch.
        const auto connections = peers_;
        for (const auto& peer : connections)
        {
            if (!peer.second.connected)
                continue;

            for (unsigned int batch = 0; batch < 32; ++batch)
            {
                const auto live = peers_.find(peer.first);
                if (!lobbyID_ || live == peers_.end() || live->second.handle != peer.second.handle)
                    break;

                void* messages[16] = {};
                const auto count = receiveMessages_(sockets_, peer.second.handle, messages, 16);
                if (count <= 0)
                    break;

                for (int index = 0; index < count; ++index)
                {
                    // SteamNetworkingMessage_t begins with payload pointer and 32-bit size.
                    void* payload = nullptr;
                    int32_t length = 0;
                    memcpy(&payload, messages[index], sizeof(payload));
                    memcpy(&length, static_cast<const unsigned char*>(messages[index]) + sizeof(payload), sizeof(length));

                    NetFrame frame;
                    // Only deliver frames from authenticated peers with a published Carbon PlayerID.
                    PlayerID sender;
                    for (const auto& player : sessionController_.registry().destinations())
                    {
                        if (player.second == peer.first)
                        {
                            sender = PlayerID(player.first);
                            break;
                        }
                    }

                    if (payload && length > 0 && length <= 512 * 1024 && lobbyID_ && sender.isValid() &&
                        peers_.find(peer.first) != peers_.end() && isLobbyMember(peer.first) &&
                        NetFrame::decode(static_cast<const byte_t*>(payload), uint32_t(length), frame))
                    {
                        dispatchNetFrame(sender, frame);
                    }
                    else
                        LOG_WARNING_WITHOUT_CALLER << "Steam multiplayer: rejected unauthenticated or malformed NetFrame";

                    releaseMessage_(messages[index]);
                }
            }
        }
    }

    bool findLobby(const String& networkVersion) override
    {
        if (!available_ || searching_ || isLobbyPending() || isInLobby())
            return false;

        // A single exact-version search prevents accidental cross-protocol discovery.
        addFilter_(matchmaking_, "network_version", networkVersion.cStr(), 0);
        pendingSearchCall_ = requestList_(matchmaking_);
        if (!pendingSearchCall_)
        {
            LOG_WARNING_WITHOUT_CALLER << "Steam multiplayer: RequestLobbyList failed";
            return false;
        }

        searching_ = true;
        networkVersion_ = networkVersion;
        LOG_INFO << "Steam multiplayer: requested compatible lobbies, network version " << networkVersion;
        return true;
    }

    // Metadata is host-authored and mutable only while the session is in Lobby.
    bool canEditSessionMetadata() const
    {
        return lobbyID_ && sessionController_.state() == MultiplayerSessionState::Lobby &&
               getLobbyOwner_ && getLobbyOwner_(matchmaking_, lobbyID_) == localSteamID_;
    }

    bool setSessionMetadata(const String& key, const String& value) override
    {
        if (!canEditSessionMetadata() || !SessionDetail::SessionManifestCodec::validMetadataKey(key) || value.length() > 256)
            return false;

        // Start editing from the last published snapshot; never expose staged values through getSession().
        auto candidate = sessionController_.metadataDirty() ? sessionController_.stagedMetadata() : getSession().getMetadata();
        candidate[key] = value;
        if (!SessionDetail::SessionManifestCodec::validMetadata(candidate))
            return false;

        sessionController_.stageMetadata(candidate);
        return true;
    }

    bool removeSessionMetadata(const String& key) override
    {
        if (!canEditSessionMetadata() || !SessionDetail::SessionManifestCodec::validMetadataKey(key))
            return false;

        auto candidate = sessionController_.metadataDirty() ? sessionController_.stagedMetadata() : getSession().getMetadata();
        if (!candidate.erase(key))
            return false;

        sessionController_.stageMetadata(candidate);
        return true;
    }

    bool publishMetadata() override
    {
        if (!canEditSessionMetadata())
            return false;

        // No pending edits: publishing again is a harmless no-op.
        if (!sessionController_.metadataDirty())
            return true;

        // Publish the complete manifest before changing the host's public session snapshot.
        if (!SessionDetail::SessionManifestCodec::validMetadata(sessionController_.stagedMetadata()) ||
            !publishManifest(&sessionController_.stagedMetadata()))
            return false;

        // Finish the commit before dispatching. A handler may stage fresh
        // edits, which must not be discarded after the event returns.
        const auto publishedMetadata = sessionController_.stagedMetadata();
        sessionController_.discardMetadata();

        if (getSession().getMetadata() != publishedMetadata)
        {
            mutableSession().setMetadata(publishedMetadata);
            dispatchSessionUpdated();
        }

        return true;
    }

    bool createLobby(const String& networkVersion, unsigned int maxMembers) override
    {
        if (!available_ || isInLobby() || isLobbyPending() || !maxMembers || maxMembers > 250)
            return false;

        networkVersion_ = networkVersion;

        // k_ELobbyTypePublic = 2. This creates only the lobby, not a peer socket.
        pendingLobbyCall_ = createLobby_(matchmaking_, 2, int(maxMembers));
        if (!pendingLobbyCall_)
        {
            LOG_WARNING_WITHOUT_CALLER << "Steam multiplayer: CreateLobby failed to submit";
            return false;
        }

        pendingRole_ = Role::Host;
        LOG_INFO << "Steam multiplayer: creating public lobby for network version " << networkVersion;
        return true;
    }

    bool joinLobby(uint64_t lobbyID, const String& networkVersion) override
    {
        return beginLobbyJoin(lobbyID, networkVersion, MultiplayerOperation::JoinLobby);
    }

    bool beginLobbyJoin(uint64_t candidate, const String& networkVersion, MultiplayerOperation operation)
    {
        if (!available_ || !candidate || isInLobby() || isLobbyPending())
            return false;

        // Direct invitations may not have cached lobby metadata. Validate the
        // network version on entry, before exposing a session to the game.
        networkVersion_ = networkVersion;
        pendingLobbyCall_ = joinLobby_(matchmaking_, candidate);
        if (!pendingLobbyCall_)
        {
            LOG_WARNING_WITHOUT_CALLER << "Steam multiplayer: JoinLobby failed to submit";
            return false;
        }

        // Keep the original operation identity through asynchronous lobby entry.
        pendingRole_ = Role::Guest;
        pendingGuestOperation_ = operation;
        requestedLobbyID_ = candidate;
        LOG_INFO << "Steam multiplayer: joining lobby " << candidate;
        return true;
    }

    void leaveLobby() override
    {
        endSession(MultiplayerSessionEndReason::Left);
    }

    /** Close a session without converting transport disconnects into termination. */
    void endSession(MultiplayerSessionEndReason reason, bool notify = true)
    {
        // Platform values belong to the ending session, not the next lobby or game.
        // Do this before notifying applications so they can set new values safely.
        clearPlatformValues();

        // Invalidate all work resumed after application event dispatch.
        sessionController_.invalidateGeneration();

        // Discard any uncommitted host configuration from the previous session.
        sessionController_.discardMetadata();

        // Close all peer sockets before leaving the lobby.
        closeSockets();

        // Invalidate pending operations; late entry callbacks are rejected below.
        pendingRole_ = Role::None;
        pendingLobbyCall_ = 0;
        searching_ = false;
        pendingSearchCall_ = 0;
        requestedLobbyID_ = 0;

        if (lobbyID_ && leaveLobby_)
        {
            leaveLobby_(matchmaking_, lobbyID_);
            LOG_INFO << "Steam multiplayer: left lobby " << lobbyID_;
        }

        lobbyID_ = 0;
        sessionHostSteamID_ = 0;
        networkVersion_ = String();
        sessionController_.clearParticipants();
        localSteamID_ = steamUser_ && getSteamID_ ? getSteamID_(steamUser_) : 0;
        // Preserve the completed session's identity and outcome for the application.
        const auto ended = sessionController_.finishSession(mutableSession());
        const auto previousState = ended.previousState;
        const auto previousID = ended.previousID;
        const bool hadPlayers = ended.hadPlayers;

        // Public state is now Ended. Emit one terminal notification for the
        // previous active session, not for a cancelled or never-entered lobby.
        if (notify && previousState != MultiplayerSessionState::Ended &&
            sessionController_.state() == MultiplayerSessionState::Ended && previousID)
            dispatchSessionEnded(previousID, reason);

        // Handlers may react to termination, including leaving again. Only
        // dispatch additional state/roster events if this outcome still applies.
        const auto generation = sessionController_.generation();
        if (notify && mutableSession().getID() == previousID && mutableSession().getState() == sessionController_.state())
        {
            dispatchSessionStateChanged(previousState);

            // Termination handlers can open a new session; never report the
            // old roster update to that new session.
            if (generation == sessionController_.generation() && hadPlayers)
                dispatchSessionUpdated();
        }
    }

    /** Suppress application notifications while the connection provider is destroyed. */
    void shutdownSession() { endSession(MultiplayerSessionEndReason::Left, false); }

    void onLobbyEntered(const unsigned char* data, int size)
    {
        // LobbyEnter_t: uint64 lobby ID, uint32 permissions, bool locked, uint32 response.
        if (!available_ || size < 20)
            return;

        uint64_t enteredID = 0;
        uint32_t response = 0;
        memcpy(&enteredID, data, sizeof(enteredID));
        memcpy(&response, data + 16, sizeof(response));

        // Ignore stale callbacks, leaving unexpected lobbies so they don't leak.
        if (enteredID && enteredID == lobbyID_)
            return;

        if (pendingRole_ == Role::None || !enteredID ||
            (pendingRole_ == Role::Guest && enteredID != requestedLobbyID_))
        {
            if (enteredID)
                leaveLobby_(matchmaking_, enteredID);
            return;
        }

        if (response != 1) // k_EChatRoomEnterResponseSuccess
        {
            LOG_WARNING_WITHOUT_CALLER << "Steam multiplayer: lobby entry failed, response " << response;
            pendingLobbyCall_ = 0;
            const auto operation = pendingRole_ == Role::Host ? MultiplayerOperation::CreateLobby
                                                              : pendingGuestOperation_;
            pendingRole_ = Role::None;
            requestedLobbyID_ = 0;
            dispatchOperationCompleted(operation, MultiplayerOperationFailureReason::LobbyEntryFailed);
            return;
        }

        if (pendingRole_ == Role::Host)
        {
            if (!setLobbyData_(matchmaking_, enteredID, "network_version", networkVersion_.cStr()))
            {
                LOG_WARNING_WITHOUT_CALLER << "Steam multiplayer: unable to publish lobby network version";
                leaveLobby_(matchmaking_, enteredID);
                pendingLobbyCall_ = 0;
                pendingRole_ = Role::None;
                dispatchOperationCompleted(MultiplayerOperation::CreateLobby,
                                           MultiplayerOperationFailureReason::LobbyConfigurationFailed);
                return;
            }
        }
        else
        {
            const auto published = getLobbyData_(matchmaking_, enteredID, "network_version");
            if (!published || String(published) != networkVersion_)
            {
                LOG_WARNING_WITHOUT_CALLER << "Steam multiplayer: rejected incompatible lobby on entry";
                leaveLobby_(matchmaking_, enteredID);
                pendingLobbyCall_ = 0;
                pendingRole_ = Role::None;
                requestedLobbyID_ = 0;
                dispatchOperationCompleted(pendingGuestOperation_, MultiplayerOperationFailureReason::IncompatibleNetworkVersion);
                return;
            }
        }

        pendingEntryWasHost_ = pendingRole_ == Role::Host;
        const auto completedOperation = pendingEntryWasHost_ ? MultiplayerOperation::CreateLobby
                                                            : pendingGuestOperation_;
        lobbyID_ = enteredID;
        sessionController_.invalidateGeneration();
        sessionHostSteamID_ = getLobbyOwner_(matchmaking_, enteredID);
        pendingLobbyCall_ = 0;
        pendingRole_ = Role::None;
        requestedLobbyID_ = 0;
        const auto previousState = sessionController_.enterLobby(mutableSession(), lobbyID_);
        const auto generation = sessionController_.generation();
        dispatchSessionStateChanged(previousState);

        // Do not create sockets for a session the application just left.
        if (generation != sessionController_.generation() || lobbyID_ != enteredID)
            return;

        LOG_INFO << "Steam multiplayer: entered lobby " << lobbyID_ << " (network " << networkVersion_ << ")";

        // The host listens; guests initiate their connection to the authoritative lobby owner.
        if (socketsAvailable_)
        {
            const auto owner = getLobbyOwner_(matchmaking_, lobbyID_);
            if (owner && pendingEntryWasHost_)
            {
                listenSocket_ = createListenSocket_(sockets_, 0, 0, nullptr);
                if (!listenSocket_)
                    LOG_WARNING_WITHOUT_CALLER << "Steam multiplayer: could not open P2P listen socket";
                else
                    LOG_INFO << "Steam multiplayer: listening for P2P peers";
            }
            else if (owner)
            {
                SteamIdentity identity;
                identity.steamID = owner;
                const auto handle = connectP2P_(sockets_, &identity, 0, 0, nullptr);
                if (handle)
                {
                    PeerConnection connection;
                    connection.handle = handle;
                    peers_[owner] = connection;
                    LOG_INFO << "Steam multiplayer: connecting to lobby owner " << owner;
                }
                else
                    LOG_WARNING_WITHOUT_CALLER << "Steam multiplayer: ConnectP2P failed";
            }
        }

        // The session is now publicly visible; report entry success once.
        // An application may leave the lobby from its completion handler.
        if (lobbyID_ == enteredID)
            dispatchOperationCompleted(completedOperation, true);
    }

    void onCallCompleted(int pipe, uint64_t call, int callbackID, int payloadSize)
    {
        // 510 = LobbyMatchList_t; 513 = LobbyCreated_t; 504 = LobbyEnter_t.
        if (searching_ && call == pendingSearchCall_ && callbackID == 510)
        {
            uint32_t count = 0;
            bool ioFailed = false;
            const auto success = getCallResult_(pipe, call, &count, sizeof(count), 510, &ioFailed);
            pendingSearchCall_ = 0;
            searching_ = false;
            if (!success || ioFailed)
            {
                LOG_WARNING_WITHOUT_CALLER << "Steam multiplayer: lobby discovery call failed";
                dispatchOperationCompleted(MultiplayerOperation::FindLobby, MultiplayerOperationFailureReason::ServiceFailure);
                return;
            }

            // Steam returns its preferred candidates first. Try each in order
            // until a join request is accepted; entry reports the final outcome.
            LOG_INFO << "Steam multiplayer: found " << count << " compatible lobbies";
            for (uint32_t index = 0; index < count; ++index)
            {
                const auto candidate = getLobbyByIndex_(matchmaking_, int(index));
                if (beginLobbyJoin(candidate, networkVersion_, MultiplayerOperation::FindLobby))
                    return;
            }

            // No candidate could be joined. The game decides whether to retry.
            dispatchOperationCompleted(MultiplayerOperation::FindLobby,
                                       count ? MultiplayerOperationFailureReason::LobbyEntryFailed
                                             : MultiplayerOperationFailureReason::NoCompatibleLobbies);
            return;
        }

        if (pendingLobbyCall_ != call)
            return;

        if (callbackID == 513 && pendingRole_ == Role::Host)
        {
            // LobbyCreated_t: int EResult, 4 bytes padding, uint64 lobby ID.
            unsigned char result[16] = {};
            bool ioFailed = false;
            const auto success = getCallResult_(pipe, call, result, sizeof(result), 513, &ioFailed);
            int status = 0;
            memcpy(&status, result, sizeof(status));
            pendingLobbyCall_ = 0;
            if (!success || ioFailed || status != 1)
            {
                LOG_WARNING_WITHOUT_CALLER << "Steam multiplayer: lobby creation failed, result " << status;
                pendingRole_ = Role::None;
                dispatchOperationCompleted(MultiplayerOperation::CreateLobby,
                                           !success || ioFailed ? MultiplayerOperationFailureReason::ServiceFailure
                                                                : MultiplayerOperationFailureReason::LobbyCreationFailed);
                return;
            }

            LOG_INFO << "Steam multiplayer: lobby created; awaiting lobby entry callback";
            return;
        }

        if (callbackID == 504 && pendingRole_ == Role::Guest)
        {
            // Consume the JoinLobby call result to detect IO failure; LobbyEnter_t drives entry.
            unsigned char result[24] = {};
            bool ioFailed = false;
            const auto success = getCallResult_(pipe, call, result, sizeof(result), 504, &ioFailed);
            pendingLobbyCall_ = 0;
            if (!success || ioFailed)
            {
                LOG_WARNING_WITHOUT_CALLER << "Steam multiplayer: JoinLobby callback failed";
                pendingRole_ = Role::None;
                requestedLobbyID_ = 0;
                dispatchOperationCompleted(pendingGuestOperation_, MultiplayerOperationFailureReason::ServiceFailure);
            }
        }
    }

private:
    // Use the mounted SAVE volume, not an assumed application data directory. This also avoids syncing
    // a different directory if the application has replaced the default volume.
    std::string cloudSaveRoot() const
    {
        const auto volume = dynamic_cast<const LocalDirectoryFileSystemVolume*>(fileSystem().getVolume("SAVE"));
        if (!volume)
            return {};
        const auto text = volume->getLocalDirectory().toUTF8();
        std::string root(text.as<char>());
        std::replace(root.begin(), root.end(), '\\', '/');
        while (!root.empty() && root.back() == '/')
            root.pop_back();
#ifdef WINDOWS
        if (root.size() <= 3 || (root[1] != ':' && root.compare(0, 2, "//") != 0))
            return {};
        if (root[1] == ':' && root[2] != '/')
            return {};
#else
        if (root.empty() || root.front() != '/')
            return {};
#endif
        return root;
    }

    bool enumerateCloudSaves(std::vector<std::string>& files) const
    {
        const auto count = cloudFileCount_(remoteStorage_);
        if (count < 0)
            return false;
        const std::string prefix = CloudFS::Prefix;
        for (int32_t i = 0; i < count; ++i)
        {
            int32_t size = 0;
            const auto name = cloudFileNameAndSize_(remoteStorage_, i, &size);
            if (!name || !*name || size < 0)
                return false;
            const std::string key(name);
            if (key.compare(0, prefix.size(), prefix) != 0)
                continue;
            const auto relative = key.substr(prefix.size());
            if (CloudFS::validRelativePath(relative))
                files.push_back(relative);
            else
                LOG_WARNING_WITHOUT_CALLER << "Steam cloud: ignoring unsupported cloud path " << key.c_str();
        }
        return true;
    }

    bool readCloudSave(const std::string& key, CloudFS::Bytes& bytes, CloudFS::Info& version) const
    {
        if (!cloudFileExists_(remoteStorage_, key.c_str()))
            return false;
        const auto size = cloudFileSize_(remoteStorage_, key.c_str());
        const auto stamp = cloudFileTimestamp_(remoteStorage_, key.c_str());
        if (size < static_cast<int32_t>(CloudFS::HeaderSize) ||
            size > static_cast<int32_t>(CloudFS::MaxObjectSize) || stamp <= 0)
            return false;
        bytes.resize(static_cast<size_t>(size));
        // Never interpret a failed or partial read as a valid empty save.
        return cloudFileRead_(remoteStorage_, key.c_str(), bytes.data(), size) == size &&
               cloudFileExists_(remoteStorage_, key.c_str()) &&
               cloudFileSize_(remoteStorage_, key.c_str()) == size &&
               cloudFileTimestamp_(remoteStorage_, key.c_str()) == stamp && CloudFS::decode(bytes, version);
    }

    void persistCloudSave(const std::string& root, const std::string& relative, bool listedInCloud)
    {
        if (!isCloudStorageAvailable())
            return;
        const auto path = CloudFS::join(root, relative);
        const auto local = CloudFS::inspectRelative(root, relative);
        CloudFS::Bytes payload;
        if (!CloudFS::read(path, local, payload))
        {
            LOG_WARNING_WITHOUT_CALLER << "Steam cloud: could not read local save " << relative.c_str();
            return;
        }
        const auto key = std::string(CloudFS::Prefix) + relative;
        if (listedInCloud || cloudFileExists_(remoteStorage_, key.c_str()))
        {
            CloudFS::Bytes existing;
            CloudFS::Info version;
            if (!readCloudSave(key, existing, version))
            {
                LOG_WARNING_WITHOUT_CALLER << "Steam cloud: existing cloud save unreadable, preserving both copies: "
                                           << relative.c_str();
                return;
            }
            if (CloudFS::samePayload(payload, existing, version))
                return;
            if (!CloudFS::newer(local, version))
            {
                LOG_WARNING_WITHOUT_CALLER << "Steam cloud: cloud save is newer or equally dated; not publishing "
                                           << relative.c_str();
                return;
            }
        }
        const auto data = CloudFS::encode(local, payload);
        if (!CloudFS::sameVersion(local, CloudFS::inspectRelative(root, relative)) || !isCloudStorageAvailable())
        {
            LOG_WARNING_WITHOUT_CALLER << "Steam cloud: local save changed or cloud disabled; not publishing "
                                       << relative.c_str();
            return;
        }
        if (cloudFileWrite_(remoteStorage_, key.c_str(), data.data(), static_cast<int32_t>(data.size())))
            LOG_INFO << "Steam cloud: published " << relative.c_str();
        else
            LOG_WARNING_WITHOUT_CALLER << "Steam cloud: could not publish " << relative.c_str();
    }

    void updateCloudSave(const std::string& root, const std::string& relative)
    {
        if (!isCloudStorageAvailable())
            return;
        const auto local = CloudFS::inspectRelative(root, relative);
        if (local.kind != CloudFS::Kind::Missing && local.kind != CloudFS::Kind::File)
        {
            LOG_WARNING_WITHOUT_CALLER << "Steam cloud: unsafe local save path " << relative.c_str();
            return;
        }
        CloudFS::Bytes data;
        CloudFS::Info version;
        const auto key = std::string(CloudFS::Prefix) + relative;
        if (!readCloudSave(key, data, version))
        {
            LOG_WARNING_WITHOUT_CALLER << "Steam cloud: could not read cloud save " << relative.c_str();
            return;
        }
        if (local.kind == CloudFS::Kind::File)
        {
            if (!CloudFS::validTime(local) || CloudFS::newer(local, version))
                return;
            CloudFS::Bytes payload;
            if (!CloudFS::read(CloudFS::join(root, relative), local, payload))
            {
                LOG_WARNING_WITHOUT_CALLER << "Steam cloud: could not verify local save " << relative.c_str();
                return;
            }
            if (CloudFS::samePayload(payload, data, version))
                return;
            if (!CloudFS::newer(version, local))
            {
                LOG_WARNING_WITHOUT_CALLER << "Steam cloud: equally dated saves differ; preserving both copies: "
                                           << relative.c_str();
                return;
            }
        }
        if (!isCloudStorageAvailable())
            return;
        if (CloudFS::replace(root, relative, local, version, data.data() + CloudFS::HeaderSize,
                             static_cast<size_t>(version.size)))
            LOG_INFO << "Steam cloud: restored " << relative.c_str();
        else
            LOG_WARNING_WITHOUT_CALLER << "Steam cloud: could not restore save safely " << relative.c_str();
    }

    void synchronizeCloud(bool persist)
    {
        if (!isCloudStorageAvailable())
            return;
        // Both calls are synchronous and are intended for quiescent application save/load points.
        // No session state, callbacks or multiplayer transport participate in this operation.
        try
        {
            const auto root = cloudSaveRoot();
            std::vector<std::string> localFiles;
            std::vector<std::string> cloudFiles;
            if (root.empty() || !CloudFS::enumerate(root, {}, localFiles) || !enumerateCloudSaves(cloudFiles))
            {
                LOG_WARNING_WITHOUT_CALLER << "Steam cloud: save directory enumeration failed; skipping synchronization";
                return;
            }
            CloudFS::PathIndex index;
            for (const auto& name : localFiles)
                index.add(name);
            for (const auto& name : cloudFiles)
                index.add(name);
            const std::set<std::string> remoteNames(cloudFiles.begin(), cloudFiles.end());
            const auto& candidates = persist ? localFiles : cloudFiles;
            for (const auto& name : candidates)
            {
                if (!index.accepts(name))
                {
                    LOG_WARNING_WITHOUT_CALLER << "Steam cloud: ambiguous case for save path " << name.c_str();
                    continue;
                }
                if (persist)
                    persistCloudSave(root, name, remoteNames.count(name) != 0);
                else
                    updateCloudSave(root, name);
            }
        }
        catch (...)
        {
            // Optional persistence must not interrupt ordinary game progress or multiplayer.
            LOG_WARNING_WITHOUT_CALLER << "Steam cloud: save synchronization failed";
        }
    }

    void initializeCloudStorage(const SharedLibrary& library)
    {
        auto getRemoteStorage = library.mapFunction<GetRemoteStorageFunction>("SteamAPI_SteamRemoteStorage_v016");

        cloudEnabledForAccount_ = library.mapFunction<CloudEnabledFunction>("SteamAPI_ISteamRemoteStorage_IsCloudEnabledForAccount");
        cloudEnabledForApp_ = library.mapFunction<CloudEnabledFunction>("SteamAPI_ISteamRemoteStorage_IsCloudEnabledForApp");
        cloudFileWrite_ = library.mapFunction<CloudFileWriteFunction>("SteamAPI_ISteamRemoteStorage_FileWrite");
        cloudFileRead_ = library.mapFunction<CloudFileReadFunction>("SteamAPI_ISteamRemoteStorage_FileRead");
        cloudFileExists_ = library.mapFunction<CloudFileExistsFunction>("SteamAPI_ISteamRemoteStorage_FileExists");
        cloudFileSize_ = library.mapFunction<CloudFileSizeFunction>("SteamAPI_ISteamRemoteStorage_GetFileSize");
        cloudFileTimestamp_ = library.mapFunction<CloudFileTimestampFunction>("SteamAPI_ISteamRemoteStorage_GetFileTimestamp");
        cloudFileCount_ = library.mapFunction<CloudFileCountFunction>("SteamAPI_ISteamRemoteStorage_GetFileCount");
        cloudFileNameAndSize_ = library.mapFunction<CloudFileNameAndSizeFunction>("SteamAPI_ISteamRemoteStorage_GetFileNameAndSize");

        remoteStorage_ = getRemoteStorage ? getRemoteStorage() : nullptr;
        if (!cloudEnabledForAccount_ || !cloudEnabledForApp_ || !cloudFileWrite_ || !cloudFileRead_ ||
            !cloudFileExists_ || !cloudFileSize_ || !cloudFileTimestamp_ || !cloudFileCount_ || !cloudFileNameAndSize_)
            remoteStorage_ = nullptr;

        if (!remoteStorage_)
            LOG_INFO << "Steam cloud: Remote Storage interface unavailable";
        else if (!isCloudStorageAvailable())
            LOG_INFO << "Steam cloud: disabled for account or application";
        else
            LOG_INFO << "Steam cloud: Remote Storage available";
    }

    bool isCloudStorageAvailable() const
    {
        // Settings may change after initialization; check them at each operation.
        return remoteStorage_ && cloudEnabledForAccount_(remoteStorage_) && cloudEnabledForApp_(remoteStorage_);
    }

    void* remoteStorage_ = nullptr;
    CloudEnabledFunction cloudEnabledForAccount_ = nullptr;
    CloudEnabledFunction cloudEnabledForApp_ = nullptr;
    CloudFileWriteFunction cloudFileWrite_ = nullptr;
    CloudFileReadFunction cloudFileRead_ = nullptr;
    CloudFileExistsFunction cloudFileExists_ = nullptr;
    CloudFileSizeFunction cloudFileSize_ = nullptr;
    CloudFileTimestampFunction cloudFileTimestamp_ = nullptr;
    CloudFileCountFunction cloudFileCount_ = nullptr;
    CloudFileNameAndSizeFunction cloudFileNameAndSize_ = nullptr;

    enum class Role { None, Host, Guest };

    std::string getSteamPlayerName(uint64_t steamID) const
    {
        // Friends persona names are presentation data, not authentication identities.
        const char* name = nullptr;
        if (steamFriends_ && steamID == localSteamID_ && getPersonaName_)
            name = getPersonaName_(steamFriends_);
        else if (steamFriends_ && getFriendPersonaName_)
            name = getFriendPersonaName_(steamFriends_, steamID);

        return name && *name ? name : "Steam Player";
    }

    bool isLobbyMember(uint64_t peer) const
    {
        if (!lobbyID_ || !peer || !getLobbyMemberCount_ || !getLobbyMember_)
            return false;

        const auto count = getLobbyMemberCount_(matchmaking_, lobbyID_);
        for (int index = 0; index < count; ++index)
        {
            if (getLobbyMember_(matchmaking_, lobbyID_, index) == peer)
                return true;
        }

        return false;
    }

    void closeSockets()
    {
        if (sockets_ && closeConnection_)
        {
            for (const auto& peer : peers_)
                closeConnection_(sockets_, peer.second.handle, 0, "Leaving lobby", false);
        }
        peers_.clear();

        if (listenSocket_ && sockets_ && closeListenSocket_)
            closeListenSocket_(sockets_, listenSocket_);
        listenSocket_ = 0;
        pendingEntryWasHost_ = false;
    }
    void* matchmaking_ = nullptr;
    AddFilterFunction addFilter_ = nullptr;
    RequestListFunction requestList_ = nullptr;
    GetLobbyByIndexFunction getLobbyByIndex_ = nullptr;
    CreateLobbyFunction createLobby_ = nullptr;
    JoinLobbyFunction joinLobby_ = nullptr;
    LeaveLobbyFunction leaveLobby_ = nullptr;
    SetLobbyDataFunction setLobbyData_ = nullptr;
    GetLobbyDataFunction getLobbyData_ = nullptr;
    GetAPICallResultFunction getCallResult_ = nullptr;
    String networkVersion_;
    uint64_t lobbyID_ = 0;
    uint64_t sessionHostSteamID_ = 0;
    uint64_t requestedLobbyID_ = 0;
    uint64_t pendingSearchCall_ = 0;
    uint64_t pendingLobbyCall_ = 0;
    Role pendingRole_ = Role::None;
    MultiplayerOperation pendingGuestOperation_ = MultiplayerOperation::JoinLobby;
    bool available_ = false;
    bool searching_ = false;
    bool socketsAvailable_ = false;
    bool pendingEntryWasHost_ = false;
    void* sockets_ = nullptr;
    uint32_t listenSocket_ = 0;
    std::map<uint64_t, PeerConnection> peers_;
    GetLobbyOwnerFunction getLobbyOwner_ = nullptr;
    GetLobbyMemberCountFunction getLobbyMemberCount_ = nullptr;
    GetLobbyMemberFunction getLobbyMember_ = nullptr;
    SetLobbyJoinableFunction setLobbyJoinable_ = nullptr;
    SendLobbyChatFunction sendLobbyChat_ = nullptr;
    GetLobbyChatFunction getLobbyChat_ = nullptr;
    CreateListenSocketFunction createListenSocket_ = nullptr;
    ConnectP2PFunction connectP2P_ = nullptr;
    AcceptConnectionFunction acceptConnection_ = nullptr;
    CloseConnectionFunction closeConnection_ = nullptr;
    CloseListenSocketFunction closeListenSocket_ = nullptr;
    SendMessageFunction sendMessage_ = nullptr;
    ReceiveMessagesFunction receiveMessages_ = nullptr;
    ReleaseMessageFunction releaseMessage_ = nullptr;
    GetConnectionInfoFunction getConnectionInfo_ = nullptr;
    void* steamUser_ = nullptr;
    void* steamFriends_ = nullptr;
    uint64_t localSteamID_ = 0;
    GetSteamIDFunction getSteamID_ = nullptr;
    GetPersonaNameFunction getPersonaName_ = nullptr;
    GetFriendPersonaNameFunction getFriendPersonaName_ = nullptr;
    SetRichPresenceFunction setRichPresence_ = nullptr;
    std::set<std::string> platformValueKeys_;
    SessionDetail::SessionController<uint64_t> sessionController_;
};

/** Steam lifecycle with manual dispatch so callbacks reach Carbon without Steam SDK linkage. */
class SteamConnectionPlatform : public ConnectionPlatformInterface
{
public:
    SteamConnectionPlatform()
    {
        // Load Steamworks only for clients that explicitly select the Steam provider.
#ifdef WINDOWS
        const auto libraryName = sizeof(void*) == 8 ? "steam_api64.dll" : "steam_api.dll";
        if (!steamLibrary_.load(libraryName))
        {
            LOG_WARNING_WITHOUT_CALLER << "Steam connection unavailable: could not load " << libraryName;
            return;
        }
#else
        // Match a Steamworks runtime linked by a client application (e.g. Endeavor).
        // Also support Carbon's SDK packaging layout and developer dyld search paths.
        if (!steamLibrary_.load("@executable_path/libsteam_api.dylib") &&
            !steamLibrary_.load("@executable_path/../Frameworks/libsteam_api.dylib") &&
            !steamLibrary_.load("libsteam_api.dylib"))
        {
            LOG_WARNING_WITHOUT_CALLER << "Steam connection unavailable: could not load libsteam_api.dylib";
            return;
        }
#endif
        LOG_INFO << "Steam connection: loaded " << steamLibrary_.getName();

        initialize_ = steamLibrary_.mapFunction<InitializeFunction>("SteamAPI_InitFlat");
        shutdown_ = steamLibrary_.mapFunction<ShutdownFunction>("SteamAPI_Shutdown");
        manualInit_ = steamLibrary_.mapFunction<ManualInitFunction>("SteamAPI_ManualDispatch_Init");
        getPipe_ = steamLibrary_.mapFunction<GetPipeFunction>("SteamAPI_GetHSteamPipe");
        runFrame_ = steamLibrary_.mapFunction<RunFrameFunction>("SteamAPI_ManualDispatch_RunFrame");
        nextCallback_ = steamLibrary_.mapFunction<NextCallbackFunction>("SteamAPI_ManualDispatch_GetNextCallback");
        freeCallback_ = steamLibrary_.mapFunction<FreeCallbackFunction>("SteamAPI_ManualDispatch_FreeLastCallback");

        // Report exact missing lifecycle exports instead of one combined warning.
        if (!initialize_)
            LOG_WARNING_WITHOUT_CALLER << "Steam connection: missing SteamAPI_InitFlat";
        if (!shutdown_)
            LOG_WARNING_WITHOUT_CALLER << "Steam connection: missing SteamAPI_Shutdown";
        if (!manualInit_)
            LOG_WARNING_WITHOUT_CALLER << "Steam connection: missing SteamAPI_ManualDispatch_Init";
        if (!getPipe_)
            LOG_WARNING_WITHOUT_CALLER << "Steam connection: missing SteamAPI_GetHSteamPipe";
        if (!runFrame_)
            LOG_WARNING_WITHOUT_CALLER << "Steam connection: missing SteamAPI_ManualDispatch_RunFrame";
        if (!nextCallback_)
            LOG_WARNING_WITHOUT_CALLER << "Steam connection: missing SteamAPI_ManualDispatch_GetNextCallback";
        if (!freeCallback_)
            LOG_WARNING_WITHOUT_CALLER << "Steam connection: missing SteamAPI_ManualDispatch_FreeLastCallback";

        if (!initialize_ || !shutdown_ || !manualInit_ || !getPipe_ || !runFrame_ || !nextCallback_ || !freeCallback_)
            return;

        // SteamAPI_InitFlat expects a writable SteamErrMsg buffer, including on failure.
        char error[1024] = {};
        const auto initResult = initialize_(error);
        LOG_INFO << "Steam connection: SteamAPI_InitFlat result " << initResult;
        if (initResult != 0)
        {
            LOG_WARNING_WITHOUT_CALLER << "Steam connection initialization failed: " << error;
            return;
        }

        initialized_ = true;
        manualInit_();

        steamPipe_ = getPipe_();
        LOG_INFO << "Steam connection: callback pipe " << steamPipe_;
        if (!steamPipe_)
        {
            LOG_WARNING_WITHOUT_CALLER << "Steam connection unavailable: callback pipe is invalid";
            shutdown_();
            initialized_ = false;
            return;
        }

        if (!achievements_.initialize(steamLibrary_))
        {
            LOG_WARNING_WITHOUT_CALLER << "Steam connection unavailable: achievement stats interface initialization failed";
            shutdown_();
            initialized_ = false;
            return;
        }

        // Current-user stats are fetched automatically after SteamAPI_InitFlat succeeds.
        // Keep achievements pending until the UserStatsReceived_t callback confirms readiness.
        multiplayer_.initialize(steamLibrary_);
        LOG_INFO << "Steam connection initialized; waiting for achievement cache readiness";
    }

    ~SteamConnectionPlatform() override
    {
        // Persist pending progress, then shut down before the SharedLibrary unloads Steamworks.
        if (initialized_)
        {
            multiplayer_.shutdownSession();
            achievements_.flush(true);
            shutdown_();
            LOG_INFO << "Steam connection shut down";
        }
    }

    bool isAvailable() const override { return initialized_; }
    AchievementInterface& getAchievements() override { return achievements_; }
    MultiplayerInterface& getMultiplayer() override { return multiplayer_; }

    void update() override
    {
        if (!initialized_)
            return;

        // Manual dispatch is mutually exclusive with SteamAPI_RunCallbacks.
        runFrame_(steamPipe_);
        auto callback = SteamCallbackMessage();

        while (nextCallback_(steamPipe_, &callback))
        {
            // 1101 = UserStatsReceived_t; 1102 = UserStatsStored_t.
            if ((callback.callback == 1101 || callback.callback == 1102) && callback.size >= 12 && callback.data)
            {
                // Both payloads store EResult at byte offset 8, regardless of x86/x64 struct packing.
                auto result = int();
                memcpy(&result, callback.data + 8, sizeof(result));

                if (callback.callback == 1101)
                    achievements_.onStatsReceived(result);
                else
                    achievements_.onStatsStored(result);
            }

            // 703 = SteamAPICallCompleted_t, containing a call handle, callback ID and size.
            // Route call results through Carbon so clients never need their own Steam callbacks.
            if (callback.callback == 703 && callback.size >= 16 && callback.data)
            {
                uint64_t call = 0;
                int callbackID = 0;
                int payloadSize = 0;
                memcpy(&call, callback.data, sizeof(call));
                memcpy(&callbackID, callback.data + 8, sizeof(callbackID));
                memcpy(&payloadSize, callback.data + 12, sizeof(payloadSize));
                multiplayer_.onCallCompleted(steamPipe_, call, callbackID, payloadSize);
            }

            // SteamNetConnectionStatusChangedCallback_t = 1221.
            if (callback.callback == 1221 && callback.data)
                multiplayer_.onConnectionStatus(callback.data, callback.size);

            // LobbyChatMsg_t (507) carries manifest control messages with Steam-authenticated senders.
            if (callback.callback == 507 && callback.data)
                multiplayer_.onLobbyChat(callback.data, callback.size);

            // LobbyEnter_t (504) is delivered as an ordinary callback as well as a call result.
            if (callback.callback == 504 && callback.data)
                multiplayer_.onLobbyEntered(callback.data, callback.size);

            freeCallback_(steamPipe_);
        }

        multiplayer_.updateSessionRegistry();
        multiplayer_.updateSockets();
        achievements_.update();
    }

private:
    using InitializeFunction = int (CARBON_STEAM_CALL *)(char*);
    using ShutdownFunction = void (CARBON_STEAM_CALL *)();
    using ManualInitFunction = void (CARBON_STEAM_CALL *)();
    using GetPipeFunction = int (CARBON_STEAM_CALL *)();
    using RunFrameFunction = void (CARBON_STEAM_CALL *)(int);
    using NextCallbackFunction = bool (CARBON_STEAM_CALL *)(int, SteamCallbackMessage*);
    using FreeCallbackFunction = void (CARBON_STEAM_CALL *)(int);

    SharedLibrary steamLibrary_;
    InitializeFunction initialize_ = nullptr;
    ShutdownFunction shutdown_ = nullptr;
    ManualInitFunction manualInit_ = nullptr;
    GetPipeFunction getPipe_ = nullptr;
    RunFrameFunction runFrame_ = nullptr;
    NextCallbackFunction nextCallback_ = nullptr;
    FreeCallbackFunction freeCallback_ = nullptr;
    int steamPipe_ = 0;
    bool initialized_ = false;
    SteamAchievements achievements_;
    SteamMultiplayer multiplayer_;
};

CARBON_REGISTER_INTERFACE_IMPLEMENTATION(ConnectionPlatformInterface, SteamConnectionPlatform, 0)

#undef CARBON_STEAM_CALL
#endif

}
