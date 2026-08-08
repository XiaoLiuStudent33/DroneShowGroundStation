/**
 * @brief Console 侧同机 DSP-GCS UDP Transport。
 *
 * 分类：Production / Loopback。
 * 输入：127.0.0.1 本地/远端 Endpoint；输出：DSP-GCS Frame 双向收发。
 * 关键说明：名称含 Loopback，但当前 Console->UE 遥测及 UE->Console 命令都在使用，绝非 Test Only。
 */
#pragma once

#include "DSPGCSTransportConfig.h"
#include "IDSPGCSTransport.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>

struct sockaddr_in;

class ConsoleDSPGCSLoopbackTransport final : public dspgcs::IDSPGCSTransport
{
public:
    explicit ConsoleDSPGCSLoopbackTransport(
        dspgcs::DSPGCSTransportConfig config);
    ~ConsoleDSPGCSLoopbackTransport() override;

    bool Start() override;
    void Stop() override;
    bool Send(const dspgcs::TransportFrame& frame) override;
    bool Receive(dspgcs::TransportReceiveHandler handler) override;
    dspgcs::TransportState GetState() const override;

private:
    void ReceiveLoop();

    dspgcs::DSPGCSTransportConfig config_;
    std::atomic<dspgcs::TransportState> state_{
        dspgcs::TransportState::Stopped };
    std::atomic<bool> receiveRunning_{ false };
    std::thread receiveThread_;
    std::mutex handlerMutex_;
    dspgcs::TransportReceiveHandler receiveHandler_;
    std::uintptr_t socketHandle_ = ~std::uintptr_t{ 0 };
    sockaddr_in* remoteEndpoint_ = nullptr;
    bool winsockStarted_ = false;
};
