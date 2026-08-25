#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace dronegcs::identity
{
    inline constexpr std::string_view DroneLogicalIdPrefix = "Drone";

    inline std::string FormatDroneLogicalId(const std::uint64_t numericId)
    {
        if (numericId == 0)
        {
            return {};
        }

        std::string digits = std::to_string(numericId);
        if (digits.size() < 3)
        {
            digits.insert(digits.begin(), 3 - digits.size(), '0');
        }
        return std::string(DroneLogicalIdPrefix) + digits;
    }

    inline bool TryParseDroneLogicalId(
        const std::string_view text,
        std::uint64_t& outNumericId)
    {
        outNumericId = 0;
        if (text.size() <= DroneLogicalIdPrefix.size() ||
            text.substr(0, DroneLogicalIdPrefix.size()) != DroneLogicalIdPrefix)
        {
            return false;
        }

        std::uint64_t value = 0;
        for (const char character : text.substr(DroneLogicalIdPrefix.size()))
        {
            if (character < '0' || character > '9')
            {
                return false;
            }
            const std::uint64_t digit = static_cast<std::uint64_t>(character - '0');
            if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10)
            {
                return false;
            }
            value = value * 10 + digit;
        }

        if (value == 0 || FormatDroneLogicalId(value) != text)
        {
            return false;
        }
        outNumericId = value;
        return true;
    }

    inline bool TryDroneLogicalIdToRouteSlot(
        const std::string_view text,
        const std::uint64_t routeSlotCount,
        std::uint64_t& outRouteSlotIndex)
    {
        outRouteSlotIndex = 0;
        std::uint64_t numericId = 0;
        if (!TryParseDroneLogicalId(text, numericId))
        {
            return false;
        }
        const std::uint64_t routeSlotIndex = numericId - 1;
        if (routeSlotIndex >= routeSlotCount)
        {
            return false;
        }
        outRouteSlotIndex = routeSlotIndex;
        return true;
    }
}
