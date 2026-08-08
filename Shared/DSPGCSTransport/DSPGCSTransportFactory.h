/**
 * @brief 声明由各平台分别实现的 DSP-GCS Transport 工厂入口。
 *
 * 分类：Production。
 * 输入：DSPGCSTransportConfig；输出：当前平台可用的 IDSPGCSTransport。
 */
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
