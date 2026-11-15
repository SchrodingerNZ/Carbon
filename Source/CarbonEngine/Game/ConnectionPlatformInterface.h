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

    /** Start an asynchronous lobby search. False means the request was not submitted; no completion event follows. */
    virtual bool findLobbies(const String& networkVersion) { return false; }

    /** True while the most recently requested search is in flight. */
    virtual bool isSearching() const { return false; }

    /** Most recent search completed successfully. False also applies before the first search. */
    virtual bool hasSearchResults() const { return false; }

    virtual unsigned int getLobbyCount() const { return 0; }

    /** Create a public lobby. Successful submission is followed by an operation-completion event. */
    virtual bool createLobby(const String& networkVersion, unsigned int maxMembers) { return false; }

    /** Join a discovered lobby. False means no asynchronous operation was started. */
    virtual bool joinLobby(unsigned int index, const String& networkVersion) { return false; }

    /** Leave the current lobby and invalidate outstanding lobby operations. */
    virtual void leaveLobby() {}

    virtual bool isInLobby() const { return false; }
    virtual bool isLobbyPending() const { return false; }
    virtual uint64_t getLobbyID() const { return 0; }

    /** Current local session snapshot. Registry and metadata updates are reconciled across participants. */
    const MultiplayerSession& getSession() const { return session_; }

    /** Stage a host-owned metadata value. Changes remain private until publishMetadata(). Lobby only. */
    virtual bool setSessionMetadata(const String& key, const String& value) { return false; }

    /** Stage the removal of a host-owned metadata key. Lobby only. */
    virtual bool removeSessionMetadata(const String& key) { return false; }

    /** Publish all staged metadata as a single manifest revision. False leaves edits staged for retry. */
    virtual bool publishMetadata() { return false; }

    /** Host freezes the roster and published metadata. Gameplay begins after all participants are ready. */
    virtual bool startGame() { return false; }

    /** Report local preparation completed for the validated frozen session configuration. */
    virtual bool setReady(bool ready) { return false; }

    /** Send a reliable frame to a connected remote session participant. False means not submitted. */
    virtual bool send(PlayerID recipient, const NetFrame& frame) { return false; }

    /** Submit a reliable frame to all connected remote participants, without local echo.
        Returns true only if at least one recipient exists and every submission succeeds. */
    virtual bool broadcast(const NetFrame& frame) { return false; }

    /** Connected, authenticated remote session participants (excluding the local instance). */
    virtual unsigned int getConnectedPlayerCount() const { return 0; }
    virtual PlayerID getConnectedPlayer(unsigned int index) const { return PlayerID(); }

protected:
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

    /** Providers call this only after authenticating the sender and decoding the frame. */
    void dispatchNetFrame(PlayerID sender, const NetFrame& frame);

private:
    MultiplayerSession session_;
};

/** The single selected external gaming connection; capabilities share its lifetime. */
class CARBON_API ConnectionPlatformInterface : public EventHandler, private Noncopyable
{
public:
    virtual ~ConnectionPlatformInterface() {}

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
