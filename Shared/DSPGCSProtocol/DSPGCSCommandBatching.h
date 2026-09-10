#pragma once
#include "DSPGCSProtocol.h"

namespace dspgcs
{
    // Existing wire format, independent child IDs for backend deduplication.
    // The target cap also bounds per-iteration backend dispatch work/results.
    inline std::vector<CommandRequestDTO> SplitCommandTargets(
        const MessageEnvelope& envelope, const CommandRequestDTO& request,
        const std::size_t maxBytes = 60000, const std::size_t maxTargets = 256)
    {
        if (request.targetDroneIds.empty() || maxTargets == 0) return {};
        if (request.targetDroneIds.size() <= maxTargets &&
            EncodeCommand(envelope, request).size() <= maxBytes) return {request};
        std::vector<CommandRequestDTO> parts;
        CommandRequestDTO prototype = request;
        prototype.targetDroneIds.clear();
        std::size_t offset = 0;
        while (offset < request.targetDroneIds.size())
        {
            CommandRequestDTO part = prototype;
            part.commandId += "/part-" + std::to_string(parts.size());
            part.targetDroneIds.clear();
            MessageEnvelope childEnvelope = envelope;
            childEnvelope.correlationId = part.commandId;
            std::size_t bytes = EncodeCommand(childEnvelope, part).size();
            while (offset < request.targetDroneIds.size() && part.targetDroneIds.size() < maxTargets)
            {
                const auto& id = request.targetDroneIds[offset];
                const std::size_t added = sizeof(std::uint32_t) + id.size();
                if (bytes > maxBytes || added > maxBytes - bytes) break;
                part.targetDroneIds.push_back(id);
                bytes += added;
                ++offset;
            }
            // Fail atomically: caller must not send any partial plan.
            if (part.targetDroneIds.empty()) return {};
            parts.push_back(std::move(part));
        }
        return parts;
    }
}
