/*
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

#include "CarbonEngine/Core/StringBase.h"
#include "CarbonEngine/Core/Vector.h"
#include <cstdint>

namespace Carbon
{

/** A stable, session-scoped Carbon player identity. Zero means invalid. */
class PlayerID
{
public:
    PlayerID() = default;
    explicit PlayerID(uint32_t value) : value_(value) {}

    uint32_t getValue() const { return value_; }
    bool isValid() const { return value_ != 0; }

    bool operator==(PlayerID other) const { return value_ == other.value_; }
    bool operator!=(PlayerID other) const { return value_ != other.value_; }
    bool operator<(PlayerID other) const { return value_ < other.value_; }

private:
    uint32_t value_ = 0;
};

/** The status of the session, separate from its platform lobby's lifetime. */
enum class MultiplayerSessionState
{
    None,
    Lobby,
    Syncing,
    Playing,
    Ended
};

/** Whether this game instance can currently exchange messages with a player. */
enum class MultiplayerConnectionState
{
    Disconnected,
    Connecting,
    Connected
};

/** A named session member, not necessarily an individual game-controlled character. */
class MultiplayerPlayer
{
public:
    MultiplayerPlayer() = default;
    MultiplayerPlayer(PlayerID id, const String& name, MultiplayerConnectionState connectionState)
        : id_(id), name_(name), connectionState_(connectionState) {}

    PlayerID getID() const { return id_; }
    const String& getName() const { return name_; }
    MultiplayerConnectionState getConnectionState() const { return connectionState_; }

private:
    PlayerID id_;
    String name_;
    MultiplayerConnectionState connectionState_ = MultiplayerConnectionState::Disconnected;
};

/** A read-only-to-games view of one lobby-and-game session and its shared player registry. */
class MultiplayerSession
{
public:
    uint64_t getID() const { return id_; }
    MultiplayerSessionState getState() const { return state_; }
    PlayerID getHostPlayerID() const { return hostPlayerID_; }
    PlayerID getLocalPlayerID() const { return localPlayerID_; }

    unsigned int getPlayerCount() const { return players_.size(); }
    const MultiplayerPlayer* getPlayer(unsigned int index) const
    {
        return index < players_.size() ? &players_[index] : nullptr;
    }

    const MultiplayerPlayer* getPlayer(PlayerID id) const
    {
        for (const auto& player : players_)
        {
            if (player.getID() == id)
                return &player;
        }
        return nullptr;
    }

    // Providers update the registry; games access it as const through MultiplayerInterface.
    void clear()
    {
        id_ = 0;
        state_ = MultiplayerSessionState::None;
        hostPlayerID_ = PlayerID();
        localPlayerID_ = PlayerID();
        players_.clear();
    }

    void setIdentity(uint64_t id, MultiplayerSessionState state, PlayerID host, PlayerID local)
    {
        id_ = id;
        state_ = state;
        hostPlayerID_ = host;
        localPlayerID_ = local;
    }

    void setPlayers(const Vector<MultiplayerPlayer>& players) { players_ = players; }

private:
    uint64_t id_ = 0;
    MultiplayerSessionState state_ = MultiplayerSessionState::None;
    PlayerID hostPlayerID_;
    PlayerID localPlayerID_;
    Vector<MultiplayerPlayer> players_;
};

}
