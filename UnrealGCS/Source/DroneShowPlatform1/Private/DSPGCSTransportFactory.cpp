#include "DSPGCSTransportFactory.h"

#include "DSPGCSLoopbackTransport.h"

#include <memory>

std::unique_ptr<dspgcs::IDSPGCSTransport> dspgcs::DSPGCSTransportFactory::CreateTransport(
	const DSPGCSTransportConfig& config,
	std::string& error)
{
	error.clear();
	switch (config.type)
	{
	case TransportType::Loopback:
		if (config.bindAddress != "127.0.0.1" || config.remoteAddress != "127.0.0.1")
		{
			error = "Loopback Transport requires BindAddress and RemoteAddress to be 127.0.0.1";
			return nullptr;
		}
		if (config.bindPort == 0 || config.remotePort == 0)
		{
			error = "Loopback Transport requires non-zero BindPort and RemotePort";
			return nullptr;
		}
		return std::make_unique<FDSPGCSLoopbackTransport>(config);
	case TransportType::UDP:
		error = "UDPDSPGCSTransport is reserved and not implemented in Stage 4.4";
		return nullptr;
	}

	error = "Unknown DSP-GCS TransportType";
	return nullptr;
}
