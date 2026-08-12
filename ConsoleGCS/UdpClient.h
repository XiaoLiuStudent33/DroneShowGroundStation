#pragma once

/**
 * @brief 为 MavlinkCommand 提供共享 MAVLink Endpoint 发送适配。
 *
 * 分类：Production。
 * 当前多机 Production 附着 MavlinkUdpReceiver，不创建或拥有 per-Session Socket。
 * 关键约束：共享模式必须按 Session 保存的 Remote IP + Source Port 定向发送。
 */

#include <winsock2.h>
#include <ws2tcpip.h>

#include <string>

class IMavlinkDatagramSender;

class UdpClient
{
public:

    UdpClient() = default;
    ~UdpClient() = default;

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

    std::string GetRemoteEndpoint() const;

    //--------------------------------------------------
    // 关闭
    //--------------------------------------------------
    void Close();

private:

    IMavlinkDatagramSender* sharedEndpoint = nullptr;
    std::string sharedTargetIp;
    int sharedTargetPort = 0;
};
