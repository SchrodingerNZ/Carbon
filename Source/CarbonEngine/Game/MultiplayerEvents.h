/*
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

#include "CarbonEngine/Core/Event.h"
#include "CarbonEngine/Game/MultiplayerSession.h"
#include "CarbonEngine/Game/NetFrame.h"
#include <cstdint>

namespace Carbon
{

/** Sent after the public multiplayer session has entered a new state.
    In Syncing, the local frozen manifest has already been validated. */
class CARBON_API MultiplayerSessionStateChangedEvent : public Event
{
public:
    MultiplayerSessionStateChangedEvent(MultiplayerSessionState previous, MultiplayerSessionState current)
        : previous_(previous), current_(current) {}

    MultiplayerSessionState getPreviousState() const { return previous_; }
    MultiplayerSessionState getCurrentState() const { return current_; }

private:
    MultiplayerSessionState previous_;
    MultiplayerSessionState current_;
};

/** Sent after the public session roster, identity or accepted metadata changes.
    Query multiplayer().getSession() for the current local snapshot. */
class CARBON_API MultiplayerSessionUpdatedEvent : public Event
{
public:
    MultiplayerSessionUpdatedEvent() = default;
};

/** Sent when an existing session participant changes transport connectivity. */
class CARBON_API MultiplayerPlayerConnectionStateChangedEvent : public Event
{
public:
    MultiplayerPlayerConnectionStateChangedEvent(PlayerID playerID, MultiplayerConnectionState previous,
                                                 MultiplayerConnectionState current)
        : playerID_(playerID), previous_(previous), current_(current) {}

    PlayerID getPlayerID() const { return playerID_; }
    MultiplayerConnectionState getPreviousState() const { return previous_; }
    MultiplayerConnectionState getCurrentState() const { return current_; }

private:
    PlayerID playerID_;
    MultiplayerConnectionState previous_;
    MultiplayerConnectionState current_;
};

/** Why the current multiplayer session ended. */
enum class MultiplayerSessionEndReason
{
    Left,
    HostLost
};

/** Sent once when a previously active session ends. */
class CARBON_API MultiplayerSessionEndedEvent : public Event
{
public:
    MultiplayerSessionEndedEvent(uint64_t sessionID, MultiplayerSessionEndReason reason)
        : sessionID_(sessionID), reason_(reason) {}

    uint64_t getSessionID() const { return sessionID_; }
    MultiplayerSessionEndReason getReason() const { return reason_; }

private:
    uint64_t sessionID_;
    MultiplayerSessionEndReason reason_;
};

/** Which asynchronous lobby operation has completed. */
enum class MultiplayerOperation
{
    FindLobby,
    CreateLobby,
    JoinLobby
};

/** Sent when an accepted asynchronous lobby request completes or fails.
    A request rejected synchronously returns false and does not generate this event. */
class CARBON_API MultiplayerOperationCompletedEvent : public Event
{
public:
    MultiplayerOperationCompletedEvent(MultiplayerOperation operation, bool succeeded)
        : operation_(operation), succeeded_(succeeded) {}

    MultiplayerOperation getOperation() const { return operation_; }
    bool succeeded() const { return succeeded_; }

private:
    MultiplayerOperation operation_;
    bool succeeded_;
};

/** Sent after a remote NetFrame has been authenticated and decoded.
    The sender is a session-local PlayerID, not a platform identifier. */
class CARBON_API NetFrameReceivedEvent : public Event
{
public:
    NetFrameReceivedEvent(PlayerID sender, const NetFrame& frame) : sender_(sender), frame_(frame) {}

    PlayerID getSender() const { return sender_; }
    const NetFrame& getFrame() const { return frame_; }

private:
    PlayerID sender_;
    NetFrame frame_;
};

}
