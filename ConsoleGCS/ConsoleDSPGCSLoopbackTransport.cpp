#define WIN32_LEAN_AND_MEAN

#include "ConsoleDSPGCSLoopbackTransport.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <chrono>
#include <utility>

namespace
{
    constexpr std::uintptr_t InvalidSocketValue = ~std::uintptr_t{ 0 };
}

ConsoleDSPGCSLoopbackTransport::ConsoleDSPGCSLoopbackTransport(
    dspgcs::DSPGCSTransportConfig config)
    : config_(std::move(config))
{
}

ConsoleDSPGCSLoopbackTransport::~ConsoleDSPGCSLoopbackTransport()
{
    Stop();
}

bool ConsoleDSPGCSLoopbackTransport::Start()
{
    if (state_.load() == dspgcs::TransportState::Running)
    {
        return true;
    }
    state_.store(dspgcs::TransportState::Starting);

    WSADATA winsockData{};
    if (WSAStartup(MAKEWORD(2, 2), &winsockData) != 0)
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

    sockaddr_in localEndpoint{};
    localEndpoint.sin_family = AF_INET;
    localEndpoint.sin_port = htons(config_.bindPort);
    if (inet_pton(
        AF_INET,
        config_.bindAddress.c_str(),
        &localEndpoint.sin_addr) != 1 ||
        bind(
            socketHandle,
            reinterpret_cast<const sockaddr*>(&localEndpoint),
            sizeof(localEndpoint)) == SOCKET_ERROR)
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
    if (inet_pton(
        AF_INET,
        config_.remoteAddress.c_str(),
        &remoteEndpoint_->sin_addr) != 1)
    {
        Stop();
        state_.store(dspgcs::TransportState::Faulted);
        return false;
    }

    receiveRunning_.store(true);
    receiveThread_ =
        std::thread(&ConsoleDSPGCSLoopbackTransport::ReceiveLoop, this);
    state_.store(dspgcs::TransportState::Running);
    return true;
}

void ConsoleDSPGCSLoopbackTransport::Stop()
{
    if (state_.load() != dspgcs::TransportState::Stopped)
    {
        state_.store(dspgcs::TransportState::Stopping);
    }

    receiveRunning_.store(false);
    if (receiveThread_.joinable())
    {
        receiveThread_.join();
    }
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

bool ConsoleDSPGCSLoopbackTransport::Send(
    const dspgcs::TransportFrame& frame)
{
    if (state_.load() != dspgcs::TransportState::Running ||
        socketHandle_ == InvalidSocketValue ||
        remoteEndpoint_ == nullptr ||
        frame.bytes.empty())
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

bool ConsoleDSPGCSLoopbackTransport::Receive(
    dspgcs::TransportReceiveHandler handler)
{
    std::lock_guard<std::mutex> lock(handlerMutex_);
    receiveHandler_ = std::move(handler);
    return true;
}

dspgcs::TransportState ConsoleDSPGCSLoopbackTransport::GetState() const
{
    return state_.load();
}

void ConsoleDSPGCSLoopbackTransport::ReceiveLoop()
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

        if (length > 0 && remoteEndpoint_ &&
            sender.sin_addr.s_addr == remoteEndpoint_->sin_addr.s_addr &&
            sender.sin_port == remoteEndpoint_->sin_port)
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
