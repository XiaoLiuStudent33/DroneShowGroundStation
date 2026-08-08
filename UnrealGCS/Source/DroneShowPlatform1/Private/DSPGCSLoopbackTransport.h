/**
 * @brief Unreal 侧同机 DSP-GCS UDP Transport。
 *
 * 分类：Production / Loopback。
 * 输入：127.0.0.1:12002 接收；输出：发往 Console 127.0.0.1:12001 的 DSP Frame。
 * 关键说明：当前 Production Telemetry/Command 正在使用，不能标记为 Test Only。
 */
#pragma once

#include "Common/UdpSocketReceiver.h"
#include "CoreMinimal.h"
#include "DSPGCSTransportConfig.h"
#include "IDSPGCSTransport.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "Serialization/ArrayReader.h"

#include <atomic>
#include <mutex>

class FSocket;
/** Localhost-only transport currently shared by Production and acceptance paths. */
class FDSPGCSLoopbackTransport final : public dspgcs::IDSPGCSTransport
{
public:
	explicit FDSPGCSLoopbackTransport(dspgcs::DSPGCSTransportConfig Config);
	virtual ~FDSPGCSLoopbackTransport() override;

	virtual bool Start() override;
	virtual void Stop() override;
	virtual bool Send(const dspgcs::TransportFrame& Frame) override;
	virtual bool Receive(dspgcs::TransportReceiveHandler Handler) override;
	virtual dspgcs::TransportState GetState() const override;

private:
	void HandleDatagram(const FArrayReaderPtr& Data, const FIPv4Endpoint& Endpoint);

	dspgcs::DSPGCSTransportConfig Config;
	FSocket* Socket = nullptr;
	TUniquePtr<FUdpSocketReceiver> Receiver;
	TUniquePtr<FIPv4Endpoint> RemoteEndpoint;
	std::atomic<dspgcs::TransportState> State{ dspgcs::TransportState::Stopped };
	std::mutex HandlerMutex;
	dspgcs::TransportReceiveHandler ReceiveHandler;
};
