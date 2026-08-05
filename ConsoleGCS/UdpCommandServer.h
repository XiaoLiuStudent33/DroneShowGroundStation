#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>

#include <string>

class UdpCommandServer
{
public:
    UdpCommandServer();
    ~UdpCommandServer();

    bool Init(
        const std::string& bindIp,
        int bindPort,
        const std::string& allowedClientIp);

    int Receive(
        char* buffer,
        int capacity,
        sockaddr_in& sender);

    bool Send(
        const std::string& response,
        const sockaddr_in& recipient);

    bool IsAllowedClient(const sockaddr_in& sender) const;
    void Close();

private:
    SOCKET socketHandle;
    in_addr allowedClientAddress;
    bool winsockStarted;
};
