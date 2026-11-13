/*
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

#include "CarbonEngine/Core/EventHandler.h"
#include "CarbonEngine/Core/InterfaceRegistry.h"
#include "CarbonEngine/Game/AchievementInterface.h"
#include <cstdint>

namespace Carbon
{

/** Game-independent lobby discovery. Peer sockets and game messages are a later capability. */
class CARBON_API MultiplayerInterface : private Noncopyable
{
public:
    virtual ~MultiplayerInterface() {}

    /** Whether this provider can perform Steam-style matchmaking requests. */
    virtual bool isAvailable() const { return false; }

    /** Start a filtered asynchronous lobby search. Returns false if it could not be submitted. */
    virtual bool findLobbies(const String& networkVersion) { return false; }

    /** True while the most recently requested search is in flight. */
    virtual bool isSearching() const { return false; }

    /** Most recent search completed successfully. False also applies before the first search. */
    virtual bool hasSearchResults() const { return false; }

    virtual unsigned int getLobbyCount() const { return 0; }

    /** Create a public lobby. Ownership, entry and metadata publication are asynchronous. */
    virtual bool createLobby(const String& networkVersion, unsigned int maxMembers) { return false; }

    /** Join a lobby returned by the most recent successful search. */
    virtual bool joinLobby(unsigned int index, const String& networkVersion) { return false; }

    /** Leave the current lobby and invalidate outstanding lobby operations. */
    virtual void leaveLobby() {}

    virtual bool isInLobby() const { return false; }
    virtual bool isLobbyPending() const { return false; }
    virtual uint64_t getLobbyID() const { return 0; }
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
CARBON_API MultiplayerInterface& connectionMultiplayer();

}

// The client chooses an implementation at startup; without an override Null is selected.
// Example: CARBON_USE_CONNECTION_PLATFORM(Steam) (once Steam support is implemented).
#define CARBON_USE_CONNECTION_PLATFORM(Name) \
    CARBON_USE_INTERFACE_IMPLEMENTATION(Carbon::ConnectionPlatformInterface, Name##ConnectionPlatform)
