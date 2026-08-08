/**
 * @brief 在 Console 平台按配置创建 DSP-GCS Transport 实现。
 *
 * 分类：Production。
 * 当前只注册 ConsoleDSPGCSLoopbackTransport；UDP Transport 声明仍是 Planned/Reserved。
 */
#include "DSPGCSTransportFactory.h"

#include "ConsoleDSPGCSLoopbackTransport.h"

#include <memory>

std::unique_ptr<dspgcs::IDSPGCSTransport>
dspgcs::DSPGCSTransportFactory::CreateTransport(
    const DSPGCSTransportConfig& config,
    std::string& error)
{
    error.clear();
    switch (config.type)
    {
    case TransportType::Loopback:
        if (config.bindAddress != DSPGCSDefaults::LoopbackAddress ||
            config.remoteAddress != DSPGCSDefaults::LoopbackAddress)
        {
            error = "Loopback Transport requires localhost endpoints";
            return nullptr;
        }
        if (config.bindPort == 0 || config.remotePort == 0)
        {
            error = "Loopback Transport requires non-zero ports";
            return nullptr;
        }
        if (config.receiveBufferBytes <
                DSPGCSDefaults::MinimumSocketBufferBytes ||
            config.receiveBufferBytes >
                DSPGCSDefaults::MaximumSocketBufferBytes ||
            config.receiverPollMs < DSPGCSDefaults::MinimumReceiverPollMs ||
            config.receiverPollMs > DSPGCSDefaults::MaximumReceiverPollMs)
        {
            error = "Loopback Transport performance configuration is invalid";
            return nullptr;
        }
        return std::make_unique<ConsoleDSPGCSLoopbackTransport>(config);
    case TransportType::UDP:
        error = "UDPDSPGCSTransport remains reserved";
        return nullptr;
    }

    error = "Unknown DSP-GCS TransportType";
    return nullptr;
}
