#include "DroneLogicalId.h"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
    void Expect(const bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }
}

int RunDroneLogicalIdTests()
{
    try
    {
        const std::vector<std::pair<std::uint64_t, std::string>> valid{
            {1, "Drone001"}, {9, "Drone009"}, {10, "Drone010"},
            {999, "Drone999"}, {1000, "Drone1000"}, {20000, "Drone20000"}};
        for (const auto& [numericId, text] : valid)
        {
            Expect(
                dronegcs::identity::FormatDroneLogicalId(numericId) == text,
                "DroneID formatter produced non-canonical text");
            std::uint64_t parsed = 0;
            Expect(
                dronegcs::identity::TryParseDroneLogicalId(text, parsed) &&
                    parsed == numericId,
                "DroneID parser round-trip failed");
            std::uint64_t slot = 0;
            Expect(
                dronegcs::identity::TryDroneLogicalIdToRouteSlot(
                    text, numericId, slot) && slot == numericId - 1,
                "DroneID to RouteSlot mapping failed");
        }

        const std::vector<std::string> invalidValues{
            std::string(), "Drone1", "Drone01", "Drone000", "Drone0",
            "Drone0001", "DroneABC", "drone001", "DRONE001",
            " Drone001", "Drone001 ", "Drone-1", "Drone001x",
            "Drone18446744073709551616"};
        for (const std::string& invalid : invalidValues)
        {
            std::uint64_t parsed = 123;
            Expect(
                !dronegcs::identity::TryParseDroneLogicalId(invalid, parsed) &&
                    parsed == 0,
                "DroneID parser accepted invalid text");
        }

        std::uint64_t slot = 0;
        Expect(
            !dronegcs::identity::TryDroneLogicalIdToRouteSlot("Drone1000", 999, slot),
            "out-of-range RouteSlot was accepted");
        std::cout << "PASS: canonical DroneID formatter/parser contract\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: canonical DroneID contract: " << error.what() << '\n';
        return 1;
    }
}
