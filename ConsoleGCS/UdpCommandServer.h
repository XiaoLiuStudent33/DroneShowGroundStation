/**
 * @brief 旧 GCS1 文本命令链的 UDP Server。
 *
 * 分类：Legacy / Compatibility。
 * 当前 Console Production 入口没有实例化该 Server；保留用于旧链兼容路径。
 */
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
