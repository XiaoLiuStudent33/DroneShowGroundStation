#include "UdpClient.h"

#include <iostream>

#include "MavlinkDatagramSender.h"

#pragma comment(lib,"ws2_32.lib")

//==================================================
// 构造函数
//==================================================
UdpClient::UdpClient()
{
    sock = INVALID_SOCKET;
    printedSource = false;
    wsaStarted = false;
    sharedEndpoint = nullptr;
    sharedTargetPort = 0;

    memset(&targetAddr, 0, sizeof(targetAddr));
}

//==================================================
// 析构函数
//==================================================
UdpClient::~UdpClient()
{
    Close();
}

//==================================================
// 初始化 UDP
//==================================================
bool UdpClient::Init(const std::string& targetIP,
    int targetPort,
    const std::string& localIP,
    int localPort)
{
    Close();

    //--------------------------------------------------
    // 初始化 Winsock
    //--------------------------------------------------
    WSADATA wsa;

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
       
        return false;
    }

    wsaStarted = true;

    //--------------------------------------------------
    // 创建 Socket
    //--------------------------------------------------
    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

    if (sock == INVALID_SOCKET)
    {
      
        WSACleanup();
        return false;
    }

    //--------------------------------------------------
    // 非阻塞
    //--------------------------------------------------
    u_long mode = 1;

    ioctlsocket(sock, FIONBIO, &mode);

    //--------------------------------------------------
    // 本地绑定
    //--------------------------------------------------
    sockaddr_in localAddr;

    memset(&localAddr, 0, sizeof(localAddr));

    localAddr.sin_family = AF_INET;
    localAddr.sin_port = htons(localPort);
    if (inet_pton(
        AF_INET,
        localIP.c_str(),
        &localAddr.sin_addr) != 1)
    {
        Close();
        return false;
    }

    if (bind(sock,
        (sockaddr*)&localAddr,
        sizeof(localAddr)) == SOCKET_ERROR)
    {
      

        closesocket(sock);

        sock = INVALID_SOCKET;

        WSACleanup();

        return false;
    }

    //--------------------------------------------------
    // 设置目标地址
    //--------------------------------------------------
    memset(&targetAddr, 0, sizeof(targetAddr));

    targetAddr.sin_family = AF_INET;
    targetAddr.sin_port = htons(targetPort);

    if (inet_pton(
        AF_INET,
        targetIP.c_str(),
        &targetAddr.sin_addr) != 1)
    {
        Close();
        return false;
    }

    //--------------------------------------------------
    // 打印配置
    //--------------------------------------------------
  

    return true;
}

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

    if (sock == INVALID_SOCKET)
    {
        return false;
    }

    //------------------------------------------
    // 打印目标地址
    //------------------------------------------
    char ip[64];

    inet_ntop(
        AF_INET,
        &targetAddr.sin_addr,
        ip,
        sizeof(ip));



    //------------------------------------------
    // sendto
    //------------------------------------------
    int ret = sendto(
        sock,
        (const char*)data,
        len,
        0,
        (sockaddr*)&targetAddr,
        sizeof(targetAddr));

    if (ret == SOCKET_ERROR)
    {
       

        return false;
    }

    

    return true;
}

//==================================================
// 接收 UDP
//==================================================
int UdpClient::Receive(uint8_t* buffer,
    int maxLen)
{
    if (sharedEndpoint != nullptr)
    {
        // The central MavlinkUdpReceiver owns receive and routes decoded
        // messages to the matching Session through MavlinkReadOnlyIngress.
        return 0;
    }

    if (sock == INVALID_SOCKET || buffer == nullptr || maxLen <= 0)
    {
        return -1;
    }

    sockaddr_in from;

    int fromLen = sizeof(from);

    int ret = recvfrom(
        sock,
        (char*)buffer,
        maxLen,
        0,
        (sockaddr*)&from,
        &fromLen);

    //--------------------------------------------------
    // 非阻塞没有数据
    //--------------------------------------------------
    if (ret == SOCKET_ERROR)
    {
        int err = WSAGetLastError();

        if (err == WSAEWOULDBLOCK)
        {
            return 0;
        }

        //--------------------------------------------------
        // Windows UDP 常见错误，忽略
        //--------------------------------------------------
        if (err == WSAECONNRESET)
        {
            return 0;
        }

        

        return -1;
    }

    //--------------------------------------------------
    // 第一次收到数据打印来源
    //--------------------------------------------------
    if (!printedSource)
    {
        char ip[64];

        inet_ntop(AF_INET,
            &from.sin_addr,
            ip,
            sizeof(ip));

      

        printedSource = true;
    }

    return ret;
}

//==================================================
// 关闭
//==================================================
void UdpClient::Close()
{
    sharedEndpoint = nullptr;
    sharedTargetIp.clear();
    sharedTargetPort = 0;

    if (sock != INVALID_SOCKET)
    {
        closesocket(sock);

        sock = INVALID_SOCKET;
    }

    if (wsaStarted)
    {
        WSACleanup();
        wsaStarted = false;
    }
}
