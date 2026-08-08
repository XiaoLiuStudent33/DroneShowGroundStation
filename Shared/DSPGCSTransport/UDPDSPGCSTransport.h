/**
 * @brief 预留的非 Loopback DSP-GCS UDP Transport 声明。
 *
 * 分类：Planned / Reserved；当前没有实现文件、Factory 注册或 Production 构造路径。
 * 本轮只记录真实状态，不代表该能力已经实现。
 */
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
