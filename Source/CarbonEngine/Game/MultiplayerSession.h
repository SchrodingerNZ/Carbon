/*
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

#include "CarbonEngine/Core/StringBase.h"
#include "CarbonEngine/Core/Vector.h"
#include <cstdint>
#include <map>

namespace Carbon
{

/** A stable, session-scoped Carbon player identity. Zero means invalid. */
class PlayerID
{
public:
    PlayerID() = default;
    /** Constructs a session-scoped ID from its numeric value; zero is invalid. */
    explicit PlayerID(uint32_t value) : value_(value) {}

    /** Returns the numeric Carbon player identifier. */
    uint32_t getValue() const { return value_; }
    /** Returns whether the identifier is nonzero. */
    bool isValid() const { return value_ != 0; }

    /** Tests whether the numeric identifiers are equal. */
    bool operator==(PlayerID other) const { return value_ == other.value_; }
    /** Tests whether the numeric identifiers differ. */
    bool operator!=(PlayerID other) const { return value_ != other.value_; }
    /** Orders identifiers by their numeric values. */
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
    /** Constructs a player snapshot with identity, display name and connectivity. */
    MultiplayerPlayer(PlayerID id, const String& name, MultiplayerConnectionState connectionState)
        : id_(id), name_(name), connectionState_(connectionState) {}

    /** Returns the session-scoped player identifier. */
    PlayerID getID() const { return id_; }
    /** Returns the player display name. */
    const String& getName() const { return name_; }
    /** Returns the locally observed transport connectivity. */
    MultiplayerConnectionState getConnectionState() const { return connectionState_; }

private:
    PlayerID id_;
    String name_;
    MultiplayerConnectionState connectionState_ = MultiplayerConnectionState::Disconnected;
};

/** Ordered, generic game-authored session properties; Carbon assigns no meaning to keys. */
using MultiplayerSessionMetadata = std::map<String, String>;

/** A read-only-to-games view of one lobby-and-game session and its shared player registry. */
class MultiplayerSession
{
public:
    /** Returns the session identifier. */
    uint64_t getID() const { return id_; }
    /** Returns the current session lifecycle state. */
    MultiplayerSessionState getState() const { return state_; }
    /** Returns the Carbon identifier of the session host. */
    PlayerID getHostPlayerID() const { return hostPlayerID_; }
    /** Returns the Carbon identifier of the local player. */
    PlayerID getLocalPlayerID() const { return localPlayerID_; }

    /** Locally accepted published metadata, including values replicated in the host manifest. */
    const MultiplayerSessionMetadata& getMetadata() const { return metadata_; }

    /** nullptr indicates an absent key (distinct from a present empty value). */
    const String* getMetadataValue(const String& key) const
    {
        const auto found = metadata_.find(key);
        return found == metadata_.end() ? nullptr : &found->second;
    }

    /** Returns the number of registered session players, regardless of connectivity. */
    unsigned int getPlayerCount() const { return players_.size(); }
    /** Returns the roster entry at the given index, or nullptr if out of range. */
    const MultiplayerPlayer* getPlayer(unsigned int index) const
    {
        return index < players_.size() ? &players_[index] : nullptr;
    }

    /** Returns the player with the supplied Carbon identifier, or nullptr if absent. */
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
    /** Resets the session identity, lifecycle state, roster and metadata. */
    void clear()
    {
        id_ = 0;
        state_ = MultiplayerSessionState::None;
        hostPlayerID_ = PlayerID();
        localPlayerID_ = PlayerID();
        players_.clear();
        metadata_.clear();
    }

    /** Updates the session identity, lifecycle state and host/local player identifiers. */
    void setIdentity(uint64_t id, MultiplayerSessionState state, PlayerID host, PlayerID local)
    {
        id_ = id;
        state_ = state;
        hostPlayerID_ = host;
        localPlayerID_ = local;
    }

    /** Replaces the session player registry. */
    void setPlayers(const Vector<MultiplayerPlayer>& players) { players_ = players; }

    // Providers publish only complete metadata snapshots, never in-progress edits.
    /** Replaces the accepted session metadata snapshot. */
    void setMetadata(const MultiplayerSessionMetadata& metadata) { metadata_ = metadata; }

private:
    uint64_t id_ = 0;
    MultiplayerSessionState state_ = MultiplayerSessionState::None;
    PlayerID hostPlayerID_;
    PlayerID localPlayerID_;
    Vector<MultiplayerPlayer> players_;
    MultiplayerSessionMetadata metadata_;
};

}
