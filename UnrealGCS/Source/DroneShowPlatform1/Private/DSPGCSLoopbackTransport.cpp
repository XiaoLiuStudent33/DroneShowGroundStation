/**
 * @brief 实现 Unreal 当前 Production DSP-GCS 的 localhost UDP 收发。
 *
 * 分类：Production / Loopback。
 * Wire Format 由 Shared DSPGCSProtocol 管理；本文件只负责 UE Socket 和接收线程边界。
 */
#include "DSPGCSLoopbackTransport.h"

#include "Common/UdpSocketBuilder.h"
#include "Common/UdpSocketReceiver.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Serialization/ArrayReader.h"
#include "SocketSubsystem.h"
#include "Sockets.h"

namespace
{
	constexpr int32 LoopbackReceiveBufferBytes = 4 * 1024 * 1024;
	const FIPv4Address LoopbackAddress(127, 0, 0, 1);
}

FDSPGCSLoopbackTransport::FDSPGCSLoopbackTransport(dspgcs::DSPGCSTransportConfig InConfig)
	: Config(MoveTemp(InConfig))
{
}

FDSPGCSLoopbackTransport::~FDSPGCSLoopbackTransport()
{
	Stop();
}

bool FDSPGCSLoopbackTransport::Start()
{
	if (GetState() == dspgcs::TransportState::Running)
	{
		return true;
	}

	State.store(dspgcs::TransportState::Starting);
	FIPv4Address BindAddress;
	FIPv4Address RemoteAddress;
	const FString BindAddressString = UTF8_TO_TCHAR(Config.bindAddress.c_str());
	const FString RemoteAddressString = UTF8_TO_TCHAR(Config.remoteAddress.c_str());
	if (!FIPv4Address::Parse(BindAddressString, BindAddress)
		|| !FIPv4Address::Parse(RemoteAddressString, RemoteAddress)
		|| BindAddress != LoopbackAddress
		|| RemoteAddress != LoopbackAddress)
	{
		State.store(dspgcs::TransportState::Faulted);
		return false;
	}

	RemoteEndpoint = MakeUnique<FIPv4Endpoint>(RemoteAddress, Config.remotePort);
	Socket = FUdpSocketBuilder(TEXT("DSPGCSLoopbackTransport"))
		.AsNonBlocking()
		.AsReusable()
		.BoundToAddress(BindAddress)
		.BoundToPort(Config.bindPort)
		.WithReceiveBufferSize(LoopbackReceiveBufferBytes)
		.WithSendBufferSize(LoopbackReceiveBufferBytes);
	if (!Socket)
	{
		RemoteEndpoint.Reset();
		State.store(dspgcs::TransportState::Faulted);
		return false;
	}

	Receiver = MakeUnique<FUdpSocketReceiver>(
		Socket,
		FTimespan::FromMilliseconds(20),
		TEXT("DSPGCSLoopbackTransportReceiver"));
	Receiver->OnDataReceived().BindRaw(this, &FDSPGCSLoopbackTransport::HandleDatagram);
	Receiver->Start();
	State.store(dspgcs::TransportState::Running);
	return true;
}

void FDSPGCSLoopbackTransport::Stop()
{
	if (GetState() != dspgcs::TransportState::Stopped)
	{
		State.store(dspgcs::TransportState::Stopping);
	}

	if (Receiver)
	{
		Receiver->Stop();
		Receiver.Reset();
	}

	{
		std::lock_guard<std::mutex> Lock(HandlerMutex);
		ReceiveHandler = {};
	}

	if (Socket)
	{
		Socket->Close();
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Socket);
		Socket = nullptr;
	}
	RemoteEndpoint.Reset();
	State.store(dspgcs::TransportState::Stopped);
}

bool FDSPGCSLoopbackTransport::Send(const dspgcs::TransportFrame& Frame)
{
	if (GetState() != dspgcs::TransportState::Running
		|| !Socket
		|| !RemoteEndpoint
		|| Frame.bytes.empty())
	{
		return false;
	}

	int32 BytesSent = 0;
	return Socket->SendTo(
		Frame.bytes.data(),
		static_cast<int32>(Frame.bytes.size()),
		BytesSent,
		*RemoteEndpoint->ToInternetAddr())
		&& BytesSent == static_cast<int32>(Frame.bytes.size());
}

bool FDSPGCSLoopbackTransport::Receive(dspgcs::TransportReceiveHandler Handler)
{
	std::lock_guard<std::mutex> Lock(HandlerMutex);
	ReceiveHandler = MoveTemp(Handler);
	return true;
}

dspgcs::TransportState FDSPGCSLoopbackTransport::GetState() const
{
	return State.load();
}

void FDSPGCSLoopbackTransport::HandleDatagram(
	const FArrayReaderPtr& Data,
	const FIPv4Endpoint& Endpoint)
{
	if (!Data.IsValid() || !RemoteEndpoint || Endpoint != *RemoteEndpoint)
	{
		return;
	}

	dspgcs::TransportReceiveHandler Handler;
	{
		std::lock_guard<std::mutex> Lock(HandlerMutex);
		Handler = ReceiveHandler;
	}
	if (!Handler)
	{
		return;
	}

	dspgcs::TransportFrame Frame;
	Frame.bytes.resize(Data->Num());
	if (!Frame.bytes.empty())
	{
		FMemory::Memcpy(Frame.bytes.data(), Data->GetData(), Data->Num());
	}
	Handler(MoveTemp(Frame));
}
