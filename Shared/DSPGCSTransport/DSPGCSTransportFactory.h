#pragma once

#include "DSPGCSTransportConfig.h"
#include "IDSPGCSTransport.h"

#include <memory>
#include <string>

namespace dspgcs
{
    /**
     * Platform factory declaration shared by ConsoleGCS and UnrealGCS.
     * Each executable supplies one implementation that constructs its private
     * Loopback transport. UDP is a reserved type and must return an error.
     */
    class DSPGCSTransportFactory final
    {
    public:
        static std::unique_ptr<IDSPGCSTransport> CreateTransport(
            const DSPGCSTransportConfig& config,
            std::string& error);
    };
}
