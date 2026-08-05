#include "MavlinkUdpReceiver.h"

#include <array>

#pragma comment(lib, "ws2_32.lib")

MavlinkUdpReceiver::MavlinkUdpReceiver() = default;

MavlinkUdpReceiver::~MavlinkUdpReceiver() noexcept
{
    Stop();
}

bool MavlinkUdpReceiver::Start(
    const std::string& bindIp,
    const std::uint16_t bindPort,
    std::string& error)
{
    Stop();

    WSADATA winsockData{};
    if (WSAStartup(MAKEWORD(2, 2), &winsockData) != 0)
    {
        error = "WSAStartup failed";
        return false;
    }
    winsockStarted = true;

    socketHandle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socketHandle == INVALID_SOCKET)
    {
        error = "socket creation failed: " + std::to_string(WSAGetLastError());
        Stop();
        return false;
    }

    u_long nonBlocking = 1;
    if (ioctlsocket(socketHandle, FIONBIO, &nonBlocking) == SOCKET_ERROR)
    {
        error = "failed to set non-blocking mode: " +
            std::to_string(WSAGetLastError());
        Stop();
        return false;
    }

    sockaddr_in localAddress{};
    localAddress.sin_family = AF_INET;
    localAddress.sin_port = htons(bindPort);
    if (inet_pton(AF_INET, bindIp.c_str(), &localAddress.sin_addr) != 1)
    {
        error = "invalid MAVLink bind IP: " + bindIp;
        Stop();
        return false;
    }

    if (bind(
        socketHandle,
        reinterpret_cast<const sockaddr*>(&localAddress),
        sizeof(localAddress)) == SOCKET_ERROR)
    {
        error = "MAVLink UDP bind failed for " + bindIp + ":" +
            std::to_string(bindPort) + " error=" +
            std::to_string(WSAGetLastError());
        Stop();
        return false;
    }

    sockaddr_in actualAddress{};
    int actualAddressLength = sizeof(actualAddress);
    if (getsockname(
        socketHandle,
        reinterpret_cast<sockaddr*>(&actualAddress),
        &actualAddressLength) == SOCKET_ERROR)
    {
        error = "getsockname failed: " + std::to_string(WSAGetLastError());
        Stop();
        return false;
    }

    boundPort = ntohs(actualAddress.sin_port);
    return true;
}

void MavlinkUdpReceiver::Stop()
{
    if (socketHandle != INVALID_SOCKET)
    {
        closesocket(socketHandle);
        socketHandle = INVALID_SOCKET;
    }
    if (winsockStarted)
    {
        WSACleanup();
        winsockStarted = false;
    }
    boundPort = 0;
}

MavlinkUdpReceiveResult MavlinkUdpReceiver::Receive(
    MavlinkReceivedDatagram& datagram,
    std::string& error)
{
    datagram = MavlinkReceivedDatagram{};
    if (socketHandle == INVALID_SOCKET)
    {
        error = "MAVLink UDP receiver is not running";
        return MavlinkUdpReceiveResult::Error;
    }

    std::array<std::uint8_t, 65507> buffer{};
    sockaddr_in sourceAddress{};
    int sourceAddressLength = sizeof(sourceAddress);
    const int received = recvfrom(
        socketHandle,
        reinterpret_cast<char*>(buffer.data()),
        static_cast<int>(buffer.size()),
        0,
        reinterpret_cast<sockaddr*>(&sourceAddress),
        &sourceAddressLength);

    if (received == SOCKET_ERROR)
    {
        const int socketError = WSAGetLastError();
        if (socketError == WSAEWOULDBLOCK || socketError == WSAECONNRESET)
        {
            return MavlinkUdpReceiveResult::WouldBlock;
        }
        error = "MAVLink UDP receive failed: " + std::to_string(socketError);
        return MavlinkUdpReceiveResult::Error;
    }

    char sourceIp[INET_ADDRSTRLEN]{};
    if (inet_ntop(
        AF_INET,
        &sourceAddress.sin_addr,
        sourceIp,
        sizeof(sourceIp)) == nullptr)
    {
        error = "failed to format MAVLink source IP";
        return MavlinkUdpReceiveResult::Error;
    }

    datagram.bytes.assign(buffer.begin(), buffer.begin() + received);
    datagram.sourceIp = sourceIp;
    datagram.sourcePort = ntohs(sourceAddress.sin_port);
    return MavlinkUdpReceiveResult::Received;
}

bool MavlinkUdpReceiver::SendTo(
    const std::string& targetIp,
    const std::uint16_t targetPort,
    const std::uint8_t* data,
    const int length)
{
    if (socketHandle == INVALID_SOCKET ||
        targetPort == 0 ||
        data == nullptr ||
        length <= 0)
    {
        return false;
    }

    sockaddr_in targetAddress{};
    targetAddress.sin_family = AF_INET;
    targetAddress.sin_port = htons(targetPort);
    if (inet_pton(
        AF_INET,
        targetIp.c_str(),
        &targetAddress.sin_addr) != 1)
    {
        return false;
    }

    const int sent = sendto(
        socketHandle,
        reinterpret_cast<const char*>(data),
        length,
        0,
        reinterpret_cast<const sockaddr*>(&targetAddress),
        sizeof(targetAddress));
    return sent == length;
}

bool MavlinkUdpReceiver::IsRunning() const
{
    return socketHandle != INVALID_SOCKET;
}

std::uint16_t MavlinkUdpReceiver::GetBoundPort() const
{
    return boundPort;
}
