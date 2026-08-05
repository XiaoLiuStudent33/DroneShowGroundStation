#pragma once

#include "DSPGCSTransportConfig.h"
#include "IDSPGCSTransport.h"

namespace dspgcs
{
    /**
     * Reserved production transport seam.
     *
     * Stage 4.3 intentionally provides declarations only: there is no source
     * implementation, factory registration, or runtime construction yet.
     */
    class UDPDSPGCSTransport : public IDSPGCSTransport
    {
    public:
        explicit UDPDSPGCSTransport(DSPGCSTransportConfig config);
        ~UDPDSPGCSTransport() override;

        bool Start() override;
        void Stop() override;
        bool Send(const TransportFrame& frame) override;
        bool Receive(TransportReceiveHandler handler) override;
        TransportState GetState() const override;

    private:
        class Impl;
        Impl* implementation_ = nullptr;
    };
}
