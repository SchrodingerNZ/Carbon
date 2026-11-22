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
    /** Constructs an event describing a session lifecycle transition. */
    MultiplayerSessionStateChangedEvent(MultiplayerSessionState previous, MultiplayerSessionState current)
        : previous_(previous), current_(current) {}

    /** Returns the state before the transition. */
    MultiplayerSessionState getPreviousState() const { return previous_; }
    /** Returns the state after the transition. */
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
    /** Constructs an event describing a player connectivity transition. */
    MultiplayerPlayerConnectionStateChangedEvent(PlayerID playerID, MultiplayerConnectionState previous,
                                                 MultiplayerConnectionState current)
        : playerID_(playerID), previous_(previous), current_(current) {}

    /** Returns the identifier of the player whose connectivity changed. */
    PlayerID getPlayerID() const { return playerID_; }
    /** Returns the state before the transition. */
    MultiplayerConnectionState getPreviousState() const { return previous_; }
    /** Returns the state after the transition. */
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
    /** Constructs an event identifying the ended session and its termination reason. */
    MultiplayerSessionEndedEvent(uint64_t sessionID, MultiplayerSessionEndReason reason)
        : sessionID_(sessionID), reason_(reason) {}

    /** Returns the identifier of the session that ended. */
    uint64_t getSessionID() const { return sessionID_; }
    /** Returns the reason the session ended. */
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

/** Provider-neutral outcome details for an asynchronous lobby operation. */
enum class MultiplayerOperationFailureReason
{
    None,                       ///< Operation succeeded.
    Unknown,                    ///< No more specific reason is available.
    NoCompatibleLobbies,        ///< Discovery returned no compatible candidates.
    ServiceFailure,             ///< The provider could not deliver a valid call result.
    IncompatibleNetworkVersion, ///< The joined lobby advertised a different network version or none.
    LobbyEntryFailed,           ///< A candidate could not be joined or entry was rejected.
    LobbyCreationFailed,        ///< The provider rejected lobby creation.
    LobbyConfigurationFailed    ///< Required lobby configuration could not be published.
};

/** Sent when an accepted asynchronous lobby request completes or fails.
    A request rejected synchronously returns false and does not generate this event. */
class CARBON_API MultiplayerOperationCompletedEvent : public Event
{
public:
    /** Constructs an event containing the result of an asynchronous lobby operation. */
    MultiplayerOperationCompletedEvent(MultiplayerOperation operation, bool succeeded)
        : MultiplayerOperationCompletedEvent(operation, succeeded ? MultiplayerOperationFailureReason::None
                                                                  : MultiplayerOperationFailureReason::Unknown) {}

    /** Constructs an event with a provider-neutral outcome; None indicates success. */
    MultiplayerOperationCompletedEvent(MultiplayerOperation operation, MultiplayerOperationFailureReason reason)
        : operation_(operation), failureReason_(reason) {}

    /** Returns the asynchronous lobby operation that completed. */
    MultiplayerOperation getOperation() const { return operation_; }
    /** Returns whether the operation completed successfully. */
    bool succeeded() const { return failureReason_ == MultiplayerOperationFailureReason::None; }
    /** Returns None on success, or the reason the operation failed. */
    MultiplayerOperationFailureReason getFailureReason() const { return failureReason_; }

private:
    MultiplayerOperation operation_;
    MultiplayerOperationFailureReason failureReason_;
};

/** Sent after a remote NetFrame has been authenticated and decoded.
    The sender is a session-local PlayerID, not a platform identifier. */
class CARBON_API NetFrameReceivedEvent : public Event
{
public:
    /** Constructs an event containing the authenticated sender and received frame. */
    NetFrameReceivedEvent(PlayerID sender, const NetFrame& frame) : sender_(sender), frame_(frame) {}

    /** Returns the authenticated sender's session-scoped Carbon ID. */
    PlayerID getSender() const { return sender_; }
    /** Returns the received application frame. */
    const NetFrame& getFrame() const { return frame_; }

private:
    PlayerID sender_;
    NetFrame frame_;
};

}
