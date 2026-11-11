#pragma once

// A raster backend that draws nothing, for when an external GS renders from
// GS::setPacketTap. The frontend still tracks GS state (SIGNAL/FINISH/LABEL,
// transfers); local-to-host transfers are answered by the external GS.

#include "runtime/gs/gs_backend.h"

#include <functional>
#include <utility>

class GSNullBackend final : public GSRasterBackend
{
public:
    using LocalToHostFn = std::function<uint32_t(uint8_t *dst, uint32_t maxBytes)>;

    explicit GSNullBackend(LocalToHostFn localToHost = {}) : m_localToHost(std::move(localToHost)) {}

    void Initialize(uint8_t *, uint32_t) override {}
    void Reset() override {}
    void Submit(const GSPrimitiveBatch &) override {}
    void LoadClut(const GSTex0Reg &, const GSTexClutReg &) override {}
    void BeginTransfer(const GSTransferCommand &) override {}
    void UploadImage(const uint8_t *, uint32_t) override {}
    void Flush() override {}
    void TextureFlush() override {}
    void Sync(GSSyncReason) override {}
    PresentationFrame Present(const GSPresentationRequest &) override { return {}; }
    bool ClearFramebuffer(const GSContext &, uint32_t) override { return false; }
    uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) override
    {
        return m_localToHost ? m_localToHost(dst, maxBytes) : 0u;
    }
    uint32_t ReadVram(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) const override { return 0u; }
    void WriteVram(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) override {}
    void SnapshotVram(std::vector<uint8_t> &out) const override { out.clear(); }
    GSTransferSnapshot GetTransferSnapshot() const override { return {}; }

private:
    LocalToHostFn m_localToHost;
};
