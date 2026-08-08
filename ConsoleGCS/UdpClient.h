#pragma once

/**
 * @brief 为 MavlinkCommand 提供独立 Socket 或共享 Endpoint 两种发送适配。
 *
 * 分类：Production / Compatibility。
 * 当前多机 Production 附着 MavlinkUdpReceiver；独立 bind/receive 仅服务旧单会话路径。
 * 关键约束：共享模式必须按 Session 保存的 Remote IP + Source Port 定向发送。
 */

#include <winsock2.h>
#include <ws2tcpip.h>

#include <string>

class IMavlinkDatagramSender;

class UdpClient
{
public:

    UdpClient();
    ~UdpClient();

    //--------------------------------------------------
    // 初始化UDP
    //--------------------------------------------------
    bool Init(
        const std::string& targetIP,
        int targetPort,
        const std::string& localIP,
        int localPort);

    // Attach to the shared Discovery MAVLink endpoint. This UdpClient keeps
    // only the per-Session target and does not create or own another socket.
    bool AttachSharedEndpoint(
        IMavlinkDatagramSender& endpoint,
        const std::string& targetIP,
        int targetPort);

    //--------------------------------------------------
    // 发送
    //--------------------------------------------------
    bool Send(
        const uint8_t* data,
        int len);

    //--------------------------------------------------
    // 接收
    //--------------------------------------------------
    int Receive(
        uint8_t* buffer,
        int maxLen);

    //--------------------------------------------------
    // 关闭
    //--------------------------------------------------
    void Close();

private:

    SOCKET sock;

    sockaddr_in targetAddr;

    // 是否已经打印来源地址
    bool printedSource;

    bool wsaStarted;

    IMavlinkDatagramSender* sharedEndpoint;
    std::string sharedTargetIp;
    int sharedTargetPort;
};
