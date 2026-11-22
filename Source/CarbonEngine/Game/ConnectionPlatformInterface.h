/*
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

#include "CarbonEngine/Core/EventHandler.h"
#include "CarbonEngine/Core/InterfaceRegistry.h"
#include "CarbonEngine/Game/AchievementInterface.h"
#include "CarbonEngine/Game/NetFrame.h"
#include "CarbonEngine/Game/MultiplayerEvents.h"
#include "CarbonEngine/Game/MultiplayerSession.h"
#include <cstdint>

namespace Carbon
{

/** Game-independent lobby discovery and network frame delivery. */
class CARBON_API MultiplayerInterface : private Noncopyable
{
public:
    virtual ~MultiplayerInterface() {}

    /** Whether the selected provider supports multiplayer matchmaking. */
    virtual bool isAvailable() const { return false; }

    /** Local platform display name; available independently of lobby/session membership. */
    virtual String getLocalPlayerName() const { return "Player"; }

    /** Find and join a compatible lobby asynchronously. False means the request was not submitted. */
    virtual bool findLobby(const String& networkVersion) { return false; }

    /** True while automatic discovery or its subsequent join is in flight. */
    virtual bool isSearching() const { return false; }

    /** Create a public lobby. Successful submission is followed by an operation-completion event. */
    virtual bool createLobby(const String& networkVersion, unsigned int maxMembers) { return false; }

    /** Join a platform-identified lobby. False means no asynchronous operation was started. */
    virtual bool joinLobby(uint64_t lobbyID, const String& networkVersion) { return false; }

    /** Leave the current lobby and invalidate outstanding lobby operations. */
    virtual void leaveLobby() {}

    /** Returns whether a platform lobby is currently joined. */
    virtual bool isInLobby() const { return false; }
    /** Returns whether a lobby search, creation or join operation is pending. */
    virtual bool isLobbyPending() const { return false; }
    /** Returns the current platform lobby identifier, or zero if unavailable. */
    virtual uint64_t getLobbyID() const { return 0; }

    /** Current local session snapshot. Registry and metadata updates are reconciled across participants. */
    const MultiplayerSession& getSession() const { return session_; }

    /** Stage a host-owned metadata value. Changes remain private until publishMetadata(). Lobby only. */
    virtual bool setSessionMetadata(const String& key, const String& value) { return false; }

    /** Stage the removal of a host-owned metadata key. Lobby only. */
    virtual bool removeSessionMetadata(const String& key) { return false; }

    /** Publish all staged metadata as a single manifest revision. False leaves edits staged for retry. */
    virtual bool publishMetadata() { return false; }

    /** Set local platform-specific state; keys and values are forwarded unchanged to the provider.
        Successfully set keys are cleared when this multiplayer session ends. */
    virtual bool setPlatformValue(const String& key, const String& value) { return false; }

    /** Clear one previously set local platform value. False indicates unsupported or unsuccessful. */
    virtual bool clearPlatformValue(const String& key) { return false; }

    /** Publish persistent files from the $SAVE$/ volume to platform cloud storage.
        Independent of lobby membership. Local persistence remains the application's responsibility.
        Does not propagate deletions. Unsupported providers do nothing. */
    virtual void cloudPersist() {}

    /** Restore missing local save files or apply newer cloud copies to the $SAVE$/ volume.
        Does not propagate deletions or overwrite newer local files. Never discard local data on failure.
        Unsupported providers do nothing. */
    virtual void cloudUpdate() {}

    /** Host freezes the roster and published metadata. Gameplay begins after all participants are ready. */
    virtual bool startGame() { return false; }

    /** Report local preparation completed for the validated frozen session configuration. */
    virtual bool setReady(bool ready) { return false; }

    /** Send a reliable frame to a connected remote session participant. False means not submitted. */
    virtual bool send(PlayerID recipient, const NetFrame& frame) { return false; }

    /** Submit a reliable frame to all connected remote participants, without local echo.
        Returns true only if at least one recipient exists and every submission succeeds.
        False may indicate partial submission; successful submission does not acknowledge remote delivery. */
    virtual bool broadcast(const NetFrame& frame) { return false; }

    /** Connected, authenticated remote session participants (excluding the local instance). */
    virtual unsigned int getConnectedPlayerCount() const { return 0; }
    /** Returns the remote connected player at the given index, or an invalid ID if out of range. */
    virtual PlayerID getConnectedPlayer(unsigned int index) const { return PlayerID(); }

protected:
    /** Provides the backend with mutable access to its public session snapshot. */
    MultiplayerSession& mutableSession() { return session_; }

    /** Notify applications after the public session state has been updated. */
    void dispatchSessionStateChanged(MultiplayerSessionState previous);

    /** Notify applications after the public session registry has been updated. */
    void dispatchSessionUpdated();

    /** Notify applications when an existing participant changes transport connectivity. */
    void dispatchPlayerConnectionStateChanged(PlayerID playerID, MultiplayerConnectionState previous,
                                              MultiplayerConnectionState current);

    /** Notify applications after an active session has ended. */
    void dispatchSessionEnded(uint64_t sessionID, MultiplayerSessionEndReason reason);

    /** Notify applications when an accepted asynchronous lobby operation finishes. */
    void dispatchOperationCompleted(MultiplayerOperation operation, bool succeeded);

    /** Notify applications of a completed lobby operation with a provider-neutral outcome. */
    void dispatchOperationCompleted(MultiplayerOperation operation, MultiplayerOperationFailureReason reason);

    /** Providers call this only after authenticating the sender and decoding the frame. */
    void dispatchNetFrame(PlayerID sender, const NetFrame& frame);

private:
    MultiplayerSession session_;
};

/** The single selected external gaming connection; capabilities share its lifetime. */
class CARBON_API ConnectionPlatformInterface : public EventHandler, private Noncopyable
{
public:
    ~ConnectionPlatformInterface() override {}

    /** Synchronous selection check. Authentication and state loading may continue asynchronously. */
    virtual bool isAvailable() const { return true; }

    /** Achievement capability owned by this connection platform. */
    virtual AchievementInterface& getAchievements() = 0;

    /** Multiplayer capability owned by the same connection. */
    virtual MultiplayerInterface& getMultiplayer() = 0;

    /** Process external service callbacks once per engine update. */
    virtual void update() {}

    bool processEvent(const Event& e) override;
};

CARBON_DECLARE_INTERFACE_REGISTRY(ConnectionPlatformInterface);

/** Retrieve the selected platform's multiplayer capability after engine initialization. */
CARBON_API MultiplayerInterface& multiplayer();

}

// The client chooses an implementation at startup; without an override Null is selected.
// Example: CARBON_USE_CONNECTION_PLATFORM(Steam).
#define CARBON_USE_CONNECTION_PLATFORM(Name) \
    CARBON_USE_INTERFACE_IMPLEMENTATION(Carbon::ConnectionPlatformInterface, Name##ConnectionPlatform)
