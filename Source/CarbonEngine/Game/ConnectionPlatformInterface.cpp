/*
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#include "CarbonEngine/Common.h"
#include "CarbonEngine/Core/CoreEvents.h"
#include "CarbonEngine/Core/SharedLibrary.h"
#include "CarbonEngine/Game/ConnectionPlatformInterface.h"

#include <chrono>
#include <cstdint>
#include <limits>

namespace Carbon
{

bool ConnectionPlatformInterface::processEvent(const Event& e)
{
    if (e.as<UpdateEvent>())
        update();

    return true;
}

MultiplayerInterface& connectionMultiplayer()
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

#ifdef WINDOWS

// Flat Steamworks exports let Carbon keep the SDK optional at build and runtime.
// The callback payload layouts below follow Steamworks' Windows x86/x64 ABI.
struct SteamCallbackMessage
{
    int user;
    int callback;
    unsigned char* data;
    int size;
};

/** Steam achievement storage, with requests retained until the asynchronous stats callback arrives. */
class SteamAchievements : public AchievementInterface
{
public:
    using UserStatsFunction = void* (__cdecl *)();
    using GetAchievementFunction = bool (__cdecl *)(void*, const char*, bool*);
    using GetStatFunction = bool (__cdecl *)(void*, const char*, int32_t*);
    using SetAchievementFunction = bool (__cdecl *)(void*, const char*);
    using SetStatFunction = bool (__cdecl *)(void*, const char*, int32_t);
    using StoreFunction = bool (__cdecl *)(void*);
    using GetNumAchievementsFunction = uint32_t (__cdecl *)(void*);
    using GetAchievementNameFunction = const char* (__cdecl *)(void*, uint32_t);

    bool initialize(const SharedLibrary& library)
    {
        // Map only the user-stats functions required by Carbon's achievement capability.
        auto getUserStats = library.mapFunction<UserStatsFunction>("SteamAPI_SteamUserStats_v013");
        if (getUserStats)
            LOG_INFO << "Steam achievements: user-stats interface v013 export found";
        else
        {
            getUserStats = library.mapFunction<UserStatsFunction>("SteamAPI_SteamUserStats_v012");
            if (getUserStats)
                LOG_INFO << "Steam achievements: user-stats interface v012 export found";
        }

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
        else
            LOG_INFO << "Steam achievements: user-stats interface acquired";

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
    using GetMatchmakingFunction = void* (__cdecl *)();
    using AddFilterFunction = void (__cdecl *)(void*, const char*, const char*, int);
    using RequestListFunction = uint64_t (__cdecl *)(void*);
    using GetLobbyByIndexFunction = uint64_t (__cdecl *)(void*, int);
    using CreateLobbyFunction = uint64_t (__cdecl *)(void*, int, int);
    using JoinLobbyFunction = uint64_t (__cdecl *)(void*, uint64_t);
    using LeaveLobbyFunction = void (__cdecl *)(void*, uint64_t);
    using SetLobbyDataFunction = bool (__cdecl *)(void*, uint64_t, const char*, const char*);
    using GetLobbyDataFunction = const char* (__cdecl *)(void*, uint64_t, const char*);
    using GetAPICallResultFunction = bool (__cdecl *)(int, uint64_t, void*, int, int, bool*);

    void initialize(const SharedLibrary& library)
    {
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
        matchmaking_ = getMatchmaking ? getMatchmaking() : nullptr;

        available_ = matchmaking_ && addFilter_ && requestList_ && getLobbyByIndex_ && createLobby_ &&
                     joinLobby_ && leaveLobby_ && setLobbyData_ && getLobbyData_ && getCallResult_;

        if (available_)
            LOG_INFO << "Steam multiplayer: lobby discovery/hosting/joining available";
        else
            LOG_WARNING_WITHOUT_CALLER << "Steam multiplayer: required matchmaking exports unavailable";
    }

    ~SteamMultiplayer() override { leaveLobby(); }

    bool isAvailable() const override { return available_; }
    bool isSearching() const override { return searching_; }
    bool hasSearchResults() const override { return completed_; }
    unsigned int getLobbyCount() const override { return lobbyCount_; }
    bool isInLobby() const override { return lobbyID_ != 0; }
    bool isLobbyPending() const override { return pendingLobbyCall_ != 0 || pendingRole_ != Role::None; }
    uint64_t getLobbyID() const override { return lobbyID_; }

    bool findLobbies(const String& networkVersion) override
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
        completed_ = false;
        lobbyCount_ = 0;
        LOG_INFO << "Steam multiplayer: requested compatible lobbies, network version " << networkVersion;
        return true;
    }

    bool createLobby(const String& networkVersion, unsigned int maxMembers) override
    {
        if (!available_ || isInLobby() || isLobbyPending() || !maxMembers || maxMembers > 250)
            return false;

        // An outstanding search may complete later; it must not affect this new session.
        searching_ = false;
        completed_ = false;
        pendingSearchCall_ = 0;
        lobbyCount_ = 0;
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

    bool joinLobby(unsigned int index, const String& networkVersion) override
    {
        if (!available_ || searching_ || !completed_ || index >= lobbyCount_ || isInLobby() || isLobbyPending())
            return false;

        const auto candidate = getLobbyByIndex_(matchmaking_, int(index));
        if (!candidate)
            return false;

        // Do not trust discovery alone; invites and stale lists can bypass the filter.
        const auto publishedVersion = getLobbyData_(matchmaking_, candidate, "network_version");
        if (!publishedVersion || networkVersion != String(publishedVersion))
        {
            LOG_WARNING_WITHOUT_CALLER << "Steam multiplayer: refusing lobby with incompatible network version";
            return false;
        }

        networkVersion_ = networkVersion;
        pendingLobbyCall_ = joinLobby_(matchmaking_, candidate);
        if (!pendingLobbyCall_)
        {
            LOG_WARNING_WITHOUT_CALLER << "Steam multiplayer: JoinLobby failed to submit";
            return false;
        }

        pendingRole_ = Role::Guest;
        requestedLobbyID_ = candidate;
        LOG_INFO << "Steam multiplayer: joining discovered lobby " << candidate;
        return true;
    }

    void leaveLobby() override
    {
        // Invalidate pending operations; late entry callbacks are rejected below.
        pendingRole_ = Role::None;
        pendingLobbyCall_ = 0;
        searching_ = false;
        pendingSearchCall_ = 0;
        completed_ = false;
        lobbyCount_ = 0;
        requestedLobbyID_ = 0;

        if (lobbyID_ && leaveLobby_)
        {
            leaveLobby_(matchmaking_, lobbyID_);
            LOG_INFO << "Steam multiplayer: left lobby " << lobbyID_;
        }

        lobbyID_ = 0;
        networkVersion_ = String();
    }

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
            pendingRole_ = Role::None;
            requestedLobbyID_ = 0;
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
                return;
            }
        }

        lobbyID_ = enteredID;
        pendingLobbyCall_ = 0;
        pendingRole_ = Role::None;
        requestedLobbyID_ = 0;
        LOG_INFO << "Steam multiplayer: entered lobby " << lobbyID_ << " (network " << networkVersion_ << ")";
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
            completed_ = success && !ioFailed;

            if (!completed_)
                LOG_WARNING_WITHOUT_CALLER << "Steam multiplayer: lobby discovery call failed";
            else
            {
                lobbyCount_ = count;
                LOG_INFO << "Steam multiplayer: found " << lobbyCount_ << " compatible lobbies";
            }
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
            }
        }
    }

private:
    enum class Role { None, Host, Guest };
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
    uint64_t requestedLobbyID_ = 0;
    uint64_t pendingSearchCall_ = 0;
    uint64_t pendingLobbyCall_ = 0;
    unsigned int lobbyCount_ = 0;
    Role pendingRole_ = Role::None;
    bool available_ = false;
    bool searching_ = false;
    bool completed_ = false;
};

/** Steam lifecycle with manual dispatch so callbacks reach Carbon without Steam SDK linkage. */
class SteamConnectionPlatform : public ConnectionPlatformInterface
{
public:
    SteamConnectionPlatform()
    {
        // Load Steamworks only for clients that explicitly select the Steam provider.
        const auto libraryName = sizeof(void*) == 8 ? "steam_api64.dll" : "steam_api.dll";
        if (!steamLibrary_.load(libraryName))
        {
            LOG_WARNING_WITHOUT_CALLER << "Steam connection unavailable: could not load " << libraryName;
            return;
        }

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
        LOG_INFO << "Steam connection: manual callback dispatch initialized";

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
            multiplayer_.leaveLobby();
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

            // LobbyEnter_t (504) is delivered as an ordinary callback as well as a call result.
            if (callback.callback == 504 && callback.data)
                multiplayer_.onLobbyEntered(callback.data, callback.size);

            freeCallback_(steamPipe_);
        }

        achievements_.update();
    }

private:
    using InitializeFunction = int (__cdecl *)(char*);
    using ShutdownFunction = void (__cdecl *)();
    using ManualInitFunction = void (__cdecl *)();
    using GetPipeFunction = int (__cdecl *)();
    using RunFrameFunction = void (__cdecl *)(int);
    using NextCallbackFunction = bool (__cdecl *)(int, SteamCallbackMessage*);
    using FreeCallbackFunction = void (__cdecl *)(int);

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

#endif

}
