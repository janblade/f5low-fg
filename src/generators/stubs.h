// generators/stubs.h - the generators that are registered but not built yet: fsr (docs/GENERATORS.md; dlssg and intel are real, generators/dlssg.h and intel.h).
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

private:
    GenCaps caps;
};

// fsr (lower case) as a stub; null for anything else (own is built by the host; dlssg and intel are routed by xefg_cuda.cpp).
inline std::unique_ptr<IGenerator> MakeStubGenerator(const std::string& id)
{
    if (id == "fsr")
        return std::make_unique<UnavailableGenerator>("FSR FG");
    return nullptr;
}

} // namespace xefgown
