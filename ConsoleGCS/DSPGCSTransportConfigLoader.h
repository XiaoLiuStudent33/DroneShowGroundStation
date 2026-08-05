#pragma once

#include "DSPGCSTransportConfig.h"

#include <string>

bool LoadDSPGCSTransportConfig(
    const std::string& path,
    dspgcs::DSPGCSTransportConfig& config,
    std::string& error);
