#define WIN32_LEAN_AND_MEAN

#include "LoopbackDSPGCSTransport.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <chrono>
#include <utility>
#include <vector>

namespace
{
    constexpr std::uintptr_t InvalidSocketValue = ~std::uintptr_t{ 0 };

    bool IsLoopbackAddress(const std::string& address)
    {
        return address == "127.0.0.1";
    }
}

LoopbackDSPGCSTransport::LoopbackDSPGCSTransport(dspgcs::DSPGCSTransportConfig config)
    : config_(std::move(config))
{
}

LoopbackDSPGCSTransport::~LoopbackDSPGCSTransport()
{
    Stop();
}

bool LoopbackDSPGCSTransport::Start()
{
    if (state_.load() == dspgcs::TransportState::Running) return true;
    if (!IsLoopbackAddress(config_.bindAddress) || !IsLoopbackAddress(config_.remoteAddress))
    {
        state_.store(dspgcs::TransportState::Faulted);
        return false;
    }

    state_.store(dspgcs::TransportState::Starting);
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        state_.store(dspgcs::TransportState::Faulted);
        return false;
    }
    winsockStarted_ = true;

    const SOCKET socketHandle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socketHandle == INVALID_SOCKET)
    {
        Stop();
        state_.store(dspgcs::TransportState::Faulted);
        return false;
    }
    socketHandle_ = static_cast<std::uintptr_t>(socketHandle);

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(config_.bindPort);
    inet_pton(AF_INET, config_.bindAddress.c_str(), &local.sin_addr);
    if (bind(socketHandle, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) == SOCKET_ERROR)
    {
        Stop();
        state_.store(dspgcs::TransportState::Faulted);
        return false;
    }

    u_long nonBlocking = 1;
    ioctlsocket(socketHandle, FIONBIO, &nonBlocking);

    remoteEndpoint_ = new sockaddr_in{};
    remoteEndpoint_->sin_family = AF_INET;
    remoteEndpoint_->sin_port = htons(config_.remotePort);
    inet_pton(AF_INET, config_.remoteAddress.c_str(), &remoteEndpoint_->sin_addr);

    receiveRunning_.store(true);
    receiveThread_ = std::thread(&LoopbackDSPGCSTransport::ReceiveLoop, this);
    state_.store(dspgcs::TransportState::Running);
    return true;
}

void LoopbackDSPGCSTransport::Stop()
{
    if (state_.load() != dspgcs::TransportState::Stopped)
    {
        state_.store(dspgcs::TransportState::Stopping);
    }
    receiveRunning_.store(false);
    if (receiveThread_.joinable()) receiveThread_.join();

    if (socketHandle_ != InvalidSocketValue)
    {
        closesocket(static_cast<SOCKET>(socketHandle_));
        socketHandle_ = InvalidSocketValue;
    }
    delete remoteEndpoint_;
    remoteEndpoint_ = nullptr;

    if (winsockStarted_)
    {
        WSACleanup();
        winsockStarted_ = false;
    }

    {
        std::lock_guard<std::mutex> lock(handlerMutex_);
        receiveHandler_ = {};
    }
    state_.store(dspgcs::TransportState::Stopped);
}

bool LoopbackDSPGCSTransport::Send(const dspgcs::TransportFrame& frame)
{
    if (GetState() != dspgcs::TransportState::Running
        || socketHandle_ == InvalidSocketValue
        || !remoteEndpoint_
        || frame.bytes.empty())
    {
        return false;
    }

    const int sent = sendto(
        static_cast<SOCKET>(socketHandle_),
        reinterpret_cast<const char*>(frame.bytes.data()),
        static_cast<int>(frame.bytes.size()),
        0,
        reinterpret_cast<const sockaddr*>(remoteEndpoint_),
        sizeof(*remoteEndpoint_));
    return sent == static_cast<int>(frame.bytes.size());
}

bool LoopbackDSPGCSTransport::Receive(dspgcs::TransportReceiveHandler handler)
{
    std::lock_guard<std::mutex> lock(handlerMutex_);
    receiveHandler_ = std::move(handler);
    return true;
}

dspgcs::TransportState LoopbackDSPGCSTransport::GetState() const
{
    return state_.load();
}

void LoopbackDSPGCSTransport::ReceiveLoop()
{
    while (receiveRunning_.load())
    {
        std::uint8_t buffer[65507];
        sockaddr_in sender{};
        int senderLength = sizeof(sender);
        const int length = recvfrom(
            static_cast<SOCKET>(socketHandle_),
            reinterpret_cast<char*>(buffer),
            sizeof(buffer),
            0,
            reinterpret_cast<sockaddr*>(&sender),
            &senderLength);

        if (length > 0
            && remoteEndpoint_
            && sender.sin_addr.s_addr == remoteEndpoint_->sin_addr.s_addr
            && sender.sin_port == remoteEndpoint_->sin_port)
        {
            dspgcs::TransportReceiveHandler handler;
            {
                std::lock_guard<std::mutex> lock(handlerMutex_);
                handler = receiveHandler_;
            }
            if (handler)
            {
                dspgcs::TransportFrame frame;
                frame.bytes.assign(buffer, buffer + length);
                handler(std::move(frame));
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}
