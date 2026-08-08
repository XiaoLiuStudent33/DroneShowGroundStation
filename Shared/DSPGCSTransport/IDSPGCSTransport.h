/**
 * @brief 定义平台无关的 DSP-GCS Frame 收发边界。
 *
 * 分类：Production。
 * Console 和 Unreal 各自实现平台 Socket，但共享 Start/Stop/Send/Receive 语义。
 */
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

namespace dspgcs
{
    enum class TransportState : std::uint8_t
    {
        Stopped = 0,
        Starting = 1,
        Running = 2,
        Stopping = 3,
        Faulted = 4
    };

    struct TransportFrame
    {
        std::vector<std::uint8_t> bytes;
    };

    using TransportReceiveHandler = std::function<void(TransportFrame)>;

    /**
     * Byte-frame transport boundary for DSP-GCS.
     *
     * This interface deliberately has no dependency on MessageEnvelope or any
     * DTO. Receive registers the asynchronous frame consumer; implementations
     * own their receive pump and invoke that consumer from a transport thread.
     */
    class IDSPGCSTransport
    {
    public:
        virtual ~IDSPGCSTransport() = default;

        virtual bool Start() = 0;
        virtual void Stop() = 0;
        virtual bool Send(const TransportFrame& frame) = 0;
        virtual bool Receive(TransportReceiveHandler handler) = 0;
        virtual TransportState GetState() const = 0;
    };
}
