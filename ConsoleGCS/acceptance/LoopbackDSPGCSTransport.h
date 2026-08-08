#pragma once

#include "DSPGCSTransportConfig.h"
#include "IDSPGCSTransport.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

struct sockaddr_in;

/** Localhost-only Stage 4.3 transport used by the Console acceptance host. */
class LoopbackDSPGCSTransport final : public dspgcs::IDSPGCSTransport
{
public:
    explicit LoopbackDSPGCSTransport(dspgcs::DSPGCSTransportConfig config);
    ~LoopbackDSPGCSTransport() override;

    bool Start() override;
    void Stop() override;
    bool Send(const dspgcs::TransportFrame& frame) override;
    bool Receive(dspgcs::TransportReceiveHandler handler) override;
    dspgcs::TransportState GetState() const override;

private:
    void ReceiveLoop();

    dspgcs::DSPGCSTransportConfig config_;
    std::atomic<dspgcs::TransportState> state_{ dspgcs::TransportState::Stopped };
    std::atomic<bool> receiveRunning_{ false };
    std::thread receiveThread_;
    std::mutex handlerMutex_;
    dspgcs::TransportReceiveHandler receiveHandler_;
    std::uintptr_t socketHandle_ = ~std::uintptr_t{ 0 };
    sockaddr_in* remoteEndpoint_ = nullptr;
    bool winsockStarted_ = false;
};
