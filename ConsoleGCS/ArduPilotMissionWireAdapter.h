#pragma once

#include "RouteMissionCodec.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace dronegcs::ardupilot_mission
{
    inline constexpr std::uint16_t ReservedHomeWireSequence = 0;
    inline constexpr std::uint16_t FirstRouteWireSequence = 1;
    inline constexpr std::size_t ReservedHomeSlotCount = 1;
    inline constexpr std::size_t MaximumWireMissionItems =
        std::numeric_limits<std::uint16_t>::max();
    inline constexpr std::size_t MaximumArtifactRouteItems =
        MaximumWireMissionItems - ReservedHomeSlotCount;

    inline std::uint16_t WireSequenceForArtifactRouteIndex(
        const std::size_t artifactRouteIndex)
    {
        return static_cast<std::uint16_t>(
            artifactRouteIndex + ReservedHomeSlotCount);
    }

    inline std::size_t ArtifactRouteIndexForWireSequence(
        const std::uint16_t wireSequence)
    {
        return static_cast<std::size_t>(wireSequence - FirstRouteWireSequence);
    }

    inline bool BuildWireMission(
        const std::vector<route_mission::MissionItemIntV1>& artifactRouteItems,
        std::vector<route_mission::MissionItemIntV1>& wireMission)
    {
        wireMission.clear();
        if (artifactRouteItems.empty() ||
            artifactRouteItems.size() > MaximumArtifactRouteItems)
            return false;

        wireMission.reserve(artifactRouteItems.size() + ReservedHomeSlotCount);

        // ArduPilot owns readback seq 0 as Home. This protocol placeholder is
        // deliberately copied from a valid route item; its coordinates are not
        // asserted to be the aircraft Home and are never route-verified.
        route_mission::MissionItemIntV1 homePlaceholder = artifactRouteItems.front();
        homePlaceholder.Sequence = ReservedHomeWireSequence;
        wireMission.push_back(homePlaceholder);

        for (std::size_t routeIndex = 0;
            routeIndex < artifactRouteItems.size(); ++routeIndex)
        {
            route_mission::MissionItemIntV1 wireItem =
                artifactRouteItems[routeIndex];
            wireItem.Sequence = WireSequenceForArtifactRouteIndex(routeIndex);
            wireMission.push_back(wireItem);
        }
        return true;
    }
}
