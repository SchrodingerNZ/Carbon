/*
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

#include "CarbonEngine/Common.h"
#include "CarbonEngine/Game/MultiplayerSession.h"
#include "CarbonEngine/Game/SessionManifestCodec.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

// Internal implementation shared by the native connection providers.
/** \cond CARBON_INTERNAL_SESSION_IMPLEMENTATION */
namespace Carbon
{
namespace SessionDetail
{

// Track manifest agreement independently of the provider's transport and identity type.
// The provider validates incoming messages and calls these methods only after successful publication.
template <typename MemberID>
class SessionManifestAgreement
{
public:
    uint64_t revision() const { return revision_; }
    const std::string& manifest() const { return manifest_; }
    size_t confirmationCount() const { return confirmedMembers_.size(); }
    int requestedReadiness() const { return localReadyRequested_; }
    uint64_t confirmedRevision() const { return confirmedRevision_; }

    void publish(uint64_t revision, const std::string& manifest, const MemberID& localMember)
    {
        // Commit the new snapshot and invalidate confirmations of the previous one.
        revision_ = revision;
        manifest_ = manifest;
        resetConfirmations(localMember);
    }

    void beginSynchronization(const MemberID& localMember)
    {
        // Require confirmations again for the locked phase, even at the same revision.
        resetConfirmations(localMember);
        readyMembers_.clear();
        localReadyRequested_ = -1;
        localReadySentRevision_ = 0;
        localReadySentValue_ = -1;
    }

    bool confirmMember(const MemberID& member)
    {
        return confirmedMembers_.insert(member).second;
    }

    void setMemberReady(const MemberID& member, bool ready)
    {
        if (ready)
            readyMembers_.insert(member);
        else
            readyMembers_.erase(member);
    }

    bool allMembersReady(const std::vector<MemberID>& members) const
    {
        if (members.empty())
            return false;

        // Every locked member must independently confirm the manifest and report readiness.
        for (const auto& member : members)
        {
            if (confirmedMembers_.count(member) == 0 || readyMembers_.count(member) == 0)
                return false;
        }

        return true;
    }

    bool hasLocalConfirmation(uint64_t revision, MultiplayerSessionState phase) const
    {
        return confirmedRevision_ == revision && confirmedPhase_ == phase;
    }

    void markLocalConfirmation(uint64_t revision, MultiplayerSessionState phase)
    {
        confirmedRevision_ = revision;
        confirmedPhase_ = phase;
    }

    void requestLocalReadiness(bool ready)
    {
        localReadyRequested_ = ready ? 1 : 0;
    }

    bool hasPendingReadiness() const
    {
        // Only send readiness after submitting the locked-manifest confirmation.
        return localReadyRequested_ >= 0 && confirmedPhase_ == MultiplayerSessionState::Syncing &&
               confirmedRevision_ &&
               !(localReadySentRevision_ == confirmedRevision_ && localReadySentValue_ == localReadyRequested_);
    }

    void markLocalReadinessSent(uint64_t revision)
    {
        localReadySentRevision_ = revision;
        localReadySentValue_ = localReadyRequested_;
    }

    void clear()
    {
        manifest_.clear();
        revision_ = 0;
        confirmedRevision_ = 0;
        confirmedMembers_.clear();
        readyMembers_.clear();
        localReadyRequested_ = -1;
        localReadySentRevision_ = 0;
        localReadySentValue_ = -1;
        confirmedPhase_ = MultiplayerSessionState::None;
    }

private:
    void resetConfirmations(const MemberID& localMember)
    {
        confirmedMembers_.clear();
        confirmedMembers_.insert(localMember);
    }

    uint64_t revision_ = 0;
    uint64_t confirmedRevision_ = 0;
    std::string manifest_;
    std::set<MemberID> confirmedMembers_;
    std::set<MemberID> readyMembers_;
    int localReadyRequested_ = -1;
    uint64_t localReadySentRevision_ = 0;
    int localReadySentValue_ = -1;
    MultiplayerSessionState confirmedPhase_ = MultiplayerSessionState::None;
};
// Keep host assignments separate from the currently published routing table.
// A member can leave the roster without losing its original host-assigned ID.
template <typename MemberID>
class SessionPlayerRegistry
{
public:
    typedef std::map<MemberID, uint32_t> AssignmentMap;
    typedef std::map<uint32_t, MemberID> DestinationMap;

    const AssignmentMap& assignments() const { return assignments_; }
    const DestinationMap& destinations() const { return destinations_; }

    uint32_t assign(const MemberID& member)
    {
        // Reuse existing IDs; new IDs are allocated in host-observed join order.
        const auto found = assignments_.find(member);
        if (found != assignments_.end())
            return found->second;

        const auto id = nextPlayerID_++;
        assignments_.insert({member, id});
        return id;
    }

    void setDestinations(const DestinationMap& destinations)
    {
        destinations_ = destinations;
    }

    void clear()
    {
        assignments_.clear();
        destinations_.clear();
        nextPlayerID_ = 1;
    }

private:
    AssignmentMap assignments_;
    DestinationMap destinations_;
    uint32_t nextPlayerID_ = 1;
};

// Own the Carbon protocol state independently of platform callbacks and transport.
// Providers retain responsibility for validating and publishing transitions before committing them.
template <typename MemberID>
class SessionController
{
public:
    struct EndedSession
    {
        MultiplayerSessionState previousState;
        uint64_t previousID;
        bool hadPlayers;
    };

    MultiplayerSessionState state() const { return state_; }
    uint64_t generation() const { return generation_; }
    void invalidateGeneration() { ++generation_; }

    SessionPlayerRegistry<MemberID>& registry() { return registry_; }
    const SessionPlayerRegistry<MemberID>& registry() const { return registry_; }
    SessionManifestAgreement<MemberID>& agreement() { return agreement_; }
    const SessionManifestAgreement<MemberID>& agreement() const { return agreement_; }
    std::vector<MemberID>& frozenMembers() { return frozenMembers_; }
    const std::vector<MemberID>& frozenMembers() const { return frozenMembers_; }

    enum class ManifestValidation
    {
        Valid, IncompleteRoster, InvalidFormat, InvalidMetadata, DifferentManifest
    };

    // Reconstruct the guest's local roster before accepting host-authored metadata.
    // The caller must still verify the snapshot has not been superseded and dispatch events.
    template <typename EncodeIdentity, typename GetName>
    ManifestValidation validateManifest(const MultiplayerSession& session, const std::string& receivedManifest,
                                        EncodeIdentity encodeIdentity, GetName getName,
                                        MultiplayerSessionMetadata& receivedMetadata, std::string& localManifest) const
    {
        if (!session.getLocalPlayerID().isValid() || !session.getHostPlayerID().isValid() ||
            session.getPlayerCount() == 0)
            return ManifestValidation::IncompleteRoster;

        const auto& members = registry_.destinations();
        if (members.size() != session.getPlayerCount())
            return ManifestValidation::IncompleteRoster;

        for (const auto& member : members)
        {
            const auto player = session.getPlayer(PlayerID(member.first));
            if (!player || player->getName() != String(getName(member.second).c_str()))
                return ManifestValidation::IncompleteRoster;
        }

        const auto marker = receivedManifest.find("|M1|");
        if (marker == std::string::npos || receivedManifest.find("|M1|", marker + 4) != std::string::npos)
            return ManifestValidation::InvalidFormat;

        if (!SessionManifestCodec::decodeSessionMetadata(receivedManifest.substr(marker + 4), receivedMetadata))
            return ManifestValidation::InvalidMetadata;

        localManifest = SessionManifestCodec::appendSessionMetadata(
            SessionManifestCodec::buildRoster(members, encodeIdentity, getName), receivedMetadata);
        if (localManifest != receivedManifest)
            return ManifestValidation::DifferentManifest;

        return ManifestValidation::Valid;
    }

    struct ConnectionTransition
    {
        PlayerID playerID;
        MultiplayerConnectionState previous;
        MultiplayerConnectionState current;
    };

    struct RosterReconciliation
    {
        bool rosterChanged = false;
        bool enteringGuestSync = false;
        MultiplayerSessionState previousState = MultiplayerSessionState::None;
        std::vector<ConnectionTransition> connectionChanges;
    };

    // Commit the complete roster before the provider publishes or confirms its manifest.
    RosterReconciliation reconcileRoster(MultiplayerSession& session, uint64_t sessionID, bool isHost,
                                         PlayerID hostID, PlayerID localID, const Vector<MultiplayerPlayer>& roster,
                                         const typename SessionPlayerRegistry<MemberID>::DestinationMap& destinations)
    {
        RosterReconciliation result;
        registry_.setDestinations(destinations);

        // Membership changes are independent of transport connectivity.
        result.rosterChanged = session.getID() != sessionID ||
                               session.getHostPlayerID() != hostID ||
                               session.getLocalPlayerID() != localID ||
                               session.getPlayerCount() != roster.size();
        if (!result.rosterChanged)
        {
            for (unsigned int index = 0; index < roster.size(); ++index)
            {
                const auto existing = session.getPlayer(index);
                if (!existing || existing->getID() != roster[index].getID() ||
                    existing->getName() != roster[index].getName())
                {
                    result.rosterChanged = true;
                    break;
                }
            }
        }

        // Record transitions only for participants already present in the public roster.
        // New participants are announced by MultiplayerSessionUpdatedEvent instead.
        for (const auto& player : roster)
        {
            const auto* existing = session.getPlayer(player.getID());
            if (existing && existing->getConnectionState() != player.getConnectionState())
            {
                result.connectionChanges.push_back({player.getID(), existing->getConnectionState(),
                                                    player.getConnectionState()});
            }
        }

        result.previousState = session.getState();

        // Guests must validate the frozen manifest before exposing the Syncing transition.
        // Keep the previous public state while Carbon reconstructs and confirms metadata.
        result.enteringGuestSync = !isHost && state() == MultiplayerSessionState::Syncing &&
                                   result.previousState != MultiplayerSessionState::Syncing;
        session.setIdentity(sessionID, result.enteringGuestSync ? result.previousState : state(), hostID, localID);
        session.setPlayers(roster);

        return result;
    }

    // Dispatch the reconciled snapshot in Carbon order, stopping if an application callback
    // ends or replaces the session, or changes the protocol phase.
    template <typename StateChanged, typename Updated, typename ConnectionChanged>
    bool dispatchRosterEvents(const RosterReconciliation& result, uint64_t generation,
                              StateChanged stateChanged, Updated updated, ConnectionChanged connectionChanged) const
    {
        const auto reconciledState = state();
        stateChanged(result.previousState);
        if (generation != this->generation() || state() != reconciledState)
            return false;

        if (result.rosterChanged)
            updated();
        if (generation != this->generation() || state() != reconciledState)
            return false;

        for (const auto& change : result.connectionChanges)
        {
            connectionChanged(change.playerID, change.previous, change.current);
            if (generation != this->generation() || state() != reconciledState)
                return false;
        }

        return true;
    }

    // The provider must authenticate the sender and verify current membership before calling.
    // Carbon owns the control-message grammar and revision/phase validation.
    bool acceptReadinessMessage(const MemberID& sender, bool isHost, const std::string& message, std::string& value)
    {
        const std::string readyPrefix = "CARBON_PLAYER_READY|";
        if (message.compare(0, readyPrefix.size(), readyPrefix) != 0)
            return false;

        if (state() != MultiplayerSessionState::Syncing ||
            !isHost ||
            std::find(frozenMembers_.begin(), frozenMembers_.end(), sender) == frozenMembers_.end())
            return false;

        const auto separator = message.find('|', readyPrefix.size());
        if (separator == std::string::npos)
            return false;

        const auto number = message.substr(readyPrefix.size(), separator - readyPrefix.size());
        char* end = nullptr;
        const auto revision = std::strtoull(number.c_str(), &end, 10);
        if (!revision || !end || *end || revision != agreement_.revision())
            return false;

        value = message.substr(separator + 1);
        if (value != "1" && value != "0")
            return false;

        agreement_.setMemberReady(sender, value == "1");

        return true;
    }

    enum class ConfirmationResult { Ignored, Invalid, Accepted };

    ConfirmationResult acceptConfirmationMessage(const MemberID& sender, bool isHost, const std::string& message,
                                                 uint64_t& revision)
    {
        const std::string prefix = "CARBON_MANIFEST_CONFIRM|";
        if (message.compare(0, prefix.size(), prefix) != 0 ||
            (state() != MultiplayerSessionState::Lobby && state() != MultiplayerSessionState::Syncing) ||
            !isHost)
            return ConfirmationResult::Ignored;

        const auto phaseSeparator = message.find('|', prefix.size());
        if (phaseSeparator == std::string::npos)
            return ConfirmationResult::Ignored;

        const auto expectedPhase = state() == MultiplayerSessionState::Syncing ? "S" : "L";
        if (message.substr(prefix.size(), phaseSeparator - prefix.size()) != expectedPhase)
            return ConfirmationResult::Ignored;

        const auto separator = message.find('|', phaseSeparator + 1);
        if (separator == std::string::npos)
            return ConfirmationResult::Ignored;

        const auto number = message.substr(phaseSeparator + 1, separator - phaseSeparator - 1);
        char* end = nullptr;
        revision = std::strtoull(number.c_str(), &end, 10);
        if (!revision || !end || *end || revision != agreement_.revision())
            return ConfirmationResult::Ignored;

        // Only members of the host's committed roster may confirm it.
        if (message.substr(separator + 1) != agreement_.manifest() ||
            registry_.assignments().find(sender) == registry_.assignments().end() ||
            (state() == MultiplayerSessionState::Syncing &&
             std::find(frozenMembers_.begin(), frozenMembers_.end(), sender) == frozenMembers_.end()))
        {
            return ConfirmationResult::Invalid;
        }

        return agreement_.confirmMember(sender) ? ConfirmationResult::Accepted : ConfirmationResult::Ignored;
    }

    bool canBeginPlaying(bool isHost) const
    {
        return state() == MultiplayerSessionState::Syncing && isHost &&
               agreement_.allMembersReady(frozenMembers_);
    }

    std::string confirmationMessage(uint64_t revision, const std::string& manifest) const
    {
        const auto phase = state() == MultiplayerSessionState::Syncing ? "S" : "L";
        return std::string("CARBON_MANIFEST_CONFIRM|") + phase + "|" + std::to_string(revision) + "|" + manifest;
    }

    std::string readinessMessage(uint64_t revision) const
    {
        return std::string("CARBON_PLAYER_READY|") + std::to_string(revision) +
               "|" + (agreement_.requestedReadiness() ? "1" : "0");
    }

    bool metadataDirty() const { return metadataDirty_; }
    const MultiplayerSessionMetadata& stagedMetadata() const { return stagedMetadata_; }

    void stageMetadata(const MultiplayerSessionMetadata& metadata)
    {
        stagedMetadata_ = metadata;
        metadataDirty_ = true;
    }

    void discardMetadata()
    {
        stagedMetadata_.clear();
        metadataDirty_ = false;
    }

    void clearParticipants()
    {
        registry_.clear();
        frozenMembers_.clear();
        agreement_.clear();
    }

    MultiplayerSessionState enterLobby(MultiplayerSession& session, uint64_t id)
    {
        // Prepare the public snapshot before any application event is dispatched.
        const auto previous = session.getState();
        state_ = MultiplayerSessionState::Lobby;
        session.clear();
        session.setIdentity(id, state_, PlayerID(), PlayerID());
        return previous;
    }

    void beginSynchronization(const MemberID& localMember)
    {
        state_ = MultiplayerSessionState::Syncing;
        agreement_.beginSynchronization(localMember);
    }

    void acceptPublishedPhase(bool playing, uint64_t revision, bool isLocalHost)
    {
        // A guest may accept Playing only after confirming the frozen manifest.
        if (playing)
        {
            if (isLocalHost || agreement_.hasLocalConfirmation(revision, MultiplayerSessionState::Syncing))
                state_ = MultiplayerSessionState::Playing;
        }
        else if (state_ != MultiplayerSessionState::Playing)
            state_ = MultiplayerSessionState::Syncing;
    }

    MultiplayerSessionState enterPlaying(MultiplayerSession& session, uint64_t id)
    {
        const auto previous = session.getState();
        state_ = MultiplayerSessionState::Playing;
        session.setIdentity(id, state_, session.getHostPlayerID(), session.getLocalPlayerID());
        return previous;
    }

    EndedSession finishSession(MultiplayerSession& session)
    {
        // Retain the identity of the ended session for application inspection.
        const EndedSession ended = {session.getState(), session.getID(),
                                      session.getPlayerCount() != 0 || !session.getMetadata().empty()};
        const auto previousHost = session.getHostPlayerID();
        const auto previousLocal = session.getLocalPlayerID();
        session.clear();

        state_ = ended.previousState == MultiplayerSessionState::None
                     ? MultiplayerSessionState::None : MultiplayerSessionState::Ended;
        if (state_ == MultiplayerSessionState::Ended)
            session.setIdentity(ended.previousID, state_, previousHost, previousLocal);

        return ended;
    }

private:
    SessionPlayerRegistry<MemberID> registry_;
    SessionManifestAgreement<MemberID> agreement_;
    std::vector<MemberID> frozenMembers_;
    MultiplayerSessionMetadata stagedMetadata_;
    bool metadataDirty_ = false;
    MultiplayerSessionState state_ = MultiplayerSessionState::None;
    uint64_t generation_ = 0;
};

}
}
/** \endcond */
