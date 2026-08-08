#include "UdpClient.h"

#include "MavlinkDatagramSender.h"

#pragma comment(lib,"ws2_32.lib")

/**
 * @brief 实现 MAVLink UDP 共享 Endpoint 发送适配。
 *
 * 分类：Production。
 * 多机路径不为每架无人机创建新 Socket，而是委托共享 IMavlinkDatagramSender。
 */

bool UdpClient::AttachSharedEndpoint(
    IMavlinkDatagramSender& endpoint,
    const std::string& targetIP,
    const int targetPort)
{
    Close();

    in_addr parsedAddress{};
    if (targetPort <= 0 ||
        targetPort > 65535 ||
        inet_pton(AF_INET, targetIP.c_str(), &parsedAddress) != 1)
    {
        return false;
    }

    sharedEndpoint = &endpoint;
    sharedTargetIp = targetIP;
    sharedTargetPort = targetPort;
    return true;
}

//==================================================
// 发送 UDP
//==================================================
bool UdpClient::Send(
    const uint8_t* data,
    int len)
{
    if (data == nullptr ||
        len <= 0)
    {
        return false;
    }

    if (sharedEndpoint != nullptr)
    {
        return sharedEndpoint->SendTo(
            sharedTargetIp,
            static_cast<std::uint16_t>(sharedTargetPort),
            data,
            len);
    }

    return false;
}

//==================================================
// 关闭
//==================================================
void UdpClient::Close()
{
    sharedEndpoint = nullptr;
    sharedTargetIp.clear();
    sharedTargetPort = 0;

}
