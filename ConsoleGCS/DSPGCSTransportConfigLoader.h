/**
 * @brief 读取 Console 侧 DSP-GCS TransportType 与本地/远端 Endpoint 配置。
 *
 * 分类：Production。
 * 输入：gcs-config.ini 指向的 DSPGCSTransport.ini；输出：共享 DSPGCSTransportConfig。
 */
#pragma once

#include "DSPGCSTransportConfig.h"

#include <string>

bool LoadDSPGCSTransportConfig(
    const std::string& path,
    dspgcs::DSPGCSTransportConfig& config,
    std::string& error);
