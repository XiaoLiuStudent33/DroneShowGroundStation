/**
 * @brief 实现旧 GCS1 Server 的 bind、client 过滤和文本 Datagram 收发。
 *
 * 分类：Legacy / Compatibility；当前没有 Production Socket 监听历史 10006 端口。
 */
#include "UdpCommandServer.h"

#pragma comment(lib, "ws2_32.lib")

UdpCommandServer::UdpCommandServer()
    : socketHandle(INVALID_SOCKET),
      allowedClientAddress{},
      winsockStarted(false)
{
}

UdpCommandServer::~UdpCommandServer()
{
    Close();
}

bool UdpCommandServer::Init(
    const std::string& bindIp,
    int bindPort,
    const std::string& allowedClientIp)
{
    Close();

    WSADATA wsa{};

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        return false;
    }

    winsockStarted = true;
    socketHandle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

    if (socketHandle == INVALID_SOCKET)
    {
        Close();
        return false;
    }

    u_long nonBlocking = 1;

    if (ioctlsocket(socketHandle, FIONBIO, &nonBlocking) == SOCKET_ERROR)
    {
        Close();
        return false;
    }

    sockaddr_in localAddress{};
    localAddress.sin_family = AF_INET;
    localAddress.sin_port = htons(static_cast<u_short>(bindPort));

    if (inet_pton(AF_INET, bindIp.c_str(), &localAddress.sin_addr) != 1 ||
        inet_pton(
            AF_INET,
            allowedClientIp.c_str(),
            &allowedClientAddress) != 1)
    {
        Close();
        return false;
    }

    if (bind(
        socketHandle,
        reinterpret_cast<const sockaddr*>(&localAddress),
        sizeof(localAddress)) == SOCKET_ERROR)
    {
        Close();
        return false;
    }

    return true;
}

int UdpCommandServer::Receive(
    char* buffer,
    int capacity,
    sockaddr_in& sender)
{
    if (socketHandle == INVALID_SOCKET ||
        buffer == nullptr ||
        capacity <= 0)
    {
        return -1;
    }

    int senderLength = sizeof(sender);
    const int received = recvfrom(
        socketHandle,
        buffer,
        capacity,
        0,
        reinterpret_cast<sockaddr*>(&sender),
        &senderLength);

    if (received == SOCKET_ERROR)
    {
        const int error = WSAGetLastError();

        if (error == WSAEWOULDBLOCK ||
            error == WSAECONNRESET)
        {
            return 0;
        }

        return -1;
    }

    return received;
}

bool UdpCommandServer::Send(
    const std::string& response,
    const sockaddr_in& recipient)
{
    if (socketHandle == INVALID_SOCKET)
    {
        return false;
    }

    const int sent = sendto(
        socketHandle,
        response.data(),
        static_cast<int>(response.size()),
        0,
        reinterpret_cast<const sockaddr*>(&recipient),
        sizeof(recipient));

    return sent == static_cast<int>(response.size());
}

bool UdpCommandServer::IsAllowedClient(
    const sockaddr_in& sender) const
{
    return sender.sin_family == AF_INET &&
        sender.sin_addr.s_addr == allowedClientAddress.s_addr;
}

void UdpCommandServer::Close()
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
}
