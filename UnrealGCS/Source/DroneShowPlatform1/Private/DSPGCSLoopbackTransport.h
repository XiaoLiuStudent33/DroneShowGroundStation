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
/** Localhost-only Stage 4.3 transport used by UnrealGCS acceptance tests. */
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
