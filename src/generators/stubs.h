// generators/stubs.h - the generators that are registered but not built yet: intel, dlssg, fsr (docs/GENERATORS.md).
// Each reports available = false; the host then logs one line and runs own.
#pragma once

#include "../generator.h"

#include <memory>
#include <string>

namespace xefgown
{

// Only Caps() means anything: the host never builds a generator that is not available.
class UnavailableGenerator : public IGenerator
{
public:
    explicit UnavailableGenerator(const char* name) { caps.name = name; caps.reason = "not available yet"; }
    GenCaps Caps() const override { return caps; }

    bool Build(const GenBuild&) override { return false; }
    bool Rebuild(const xefgpack::Sizes&, const GenBuild&) override { return false; }
    void Drain() override {}
    void Stop() override {}
    void Shutdown() override {}
    bool PushContext() override { return false; }
    void PopContext() override {}

    bool HasPack() const override { return false; }
    const xefgpack::Sizes& PackSize() const override { return sizes; }
    uint32_t PackVersion() const override { return 0; }
    bool PackHdr() const override { return false; }
    bool HasPassPatches() const override { return false; }
    bool HasUiInputs() const override { return false; }
    const xefgpack::Resource& Output() const override { return res; }
    const xefgpack::Resource& InputRes(GenSlot) const override { return res; }
    bool HasSlot(GenSlot) const override { return false; }
    GenBuf Buf(GenSlot) const override { return {}; }
    size_t DispatchCount() const override { return 0; }
    std::string VariantModules() const override { return {}; }
    bool InCommandLists() const override { return false; }
    int GpuSm() const override { return 0; }

    void Out(ID3D12GraphicsCommandList*, ID3D12Resource*, D3D12_RESOURCE_STATES) override {}
    void Back(ID3D12GraphicsCommandList*, ID3D12Resource*, D3D12_RESOURCE_STATES) override {}
    void MvScale(float&, float&) override {}
    void SwapInputsBgra(ID3D12GraphicsCommandList*, uint32_t) override {}
    void SetUiMode(uint32_t) override {}
    void FrameHandedOff() override {}

    void PreparePass(uint32_t, uint32_t, const float[2], const float[2], const float[2]) override {}
    bool RecordPass(ID3D12GraphicsCommandList*, uint32_t, size_t, ID3D12CommandQueue*, bool) override { return false; }
    void RecordPassInto(ID3D12GraphicsCommandList*, uint32_t, ID3D12Resource*, size_t, ID3D12CommandQueue*, bool, bool) override {}
    void ReadStamps(size_t) override {}

    bool EnsureRing(uint32_t, uint32_t) override { return false; }
    ID3D12Resource* RingFrame(uint32_t, uint32_t) const override { return nullptr; }
    ID3D12Fence* DoneFence() const override { return nullptr; }
    void Launch(ID3D12CommandQueue*, UINT64&, bool, bool, uint32_t, uint32_t, const float[2], const float[2], const float[2], uint32_t,
                UINT64[], UINT64, uint32_t) override {}
    bool WantsGateFence() const override { return false; }
    void ImportGateFence(HANDLE) override {}
    bool HasGateFence() const override { return false; }
    unsigned TakeHandoffFails() override { return 0; }

    bool DownloadInput(GenSlot, std::vector<uint8_t>&) override { return false; }
    bool DownloadOutput(std::vector<uint8_t>&, uint32_t, uint32_t) override { return false; }

private:
    GenCaps caps;
    xefgpack::Sizes sizes;
    xefgpack::Resource res;
};

// intel | dlssg | fsr (lower case) as a stub; null for anything else (own is built by the host).
inline std::unique_ptr<IGenerator> MakeStubGenerator(const std::string& id)
{
    if (id == "intel")
        return std::make_unique<UnavailableGenerator>("Intel XeFG");
    if (id == "dlssg")
        return std::make_unique<UnavailableGenerator>("DLSS-G");
    if (id == "fsr")
        return std::make_unique<UnavailableGenerator>("FSR FG");
    return nullptr;
}

} // namespace xefgown
