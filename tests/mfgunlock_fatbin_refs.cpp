// Ported from OptiScaler-NR tests/mfgunlock_fatbin_refs.cpp at 745e49431 (origin/release/0.1.36).
// Changes: nothing (it includes no unlock header).
// Evidence tool: how does nvngx_dlssg reach its CUDA kernel containers? Maps the DLL (no code runs) and, for every fatbin
// container, lists the 8-byte slots that hold its address and the RIP-relative lea instructions that load it.
// usage: mfgunlock_fatbin_refs <nvngx_dlssg.dll> [-v]
#include <windows.h>
#include <psapi.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2)
        return 2;
    const bool verbose = argc > 2;
    HMODULE module = LoadLibraryExW(argv[1], nullptr, DONT_RESOLVE_DLL_REFERENCES);
    MODULEINFO info {};
    if (module == nullptr || !GetModuleInformation(GetCurrentProcess(), module, &info, sizeof(info)))
        return 1;
    const auto* base = reinterpret_cast<const uint8_t*>(module);
    const size_t size = info.SizeOfImage;

    std::vector<size_t> containers;
    for (size_t at = 0; at + 16 < size; ++at)
    {
        uint32_t magic;
        std::memcpy(&magic, base + at, 4);
        uint16_t headerSize;
        uint64_t fatSize;
        std::memcpy(&headerSize, base + at + 6, 2);
        std::memcpy(&fatSize, base + at + 8, 8);
        if (magic == 0xBA55ED50 && headerSize == 0x10 && fatSize != 0 && fatSize < size - at)
            containers.push_back(at);
    }
    std::printf("containers=%zu\n", containers.size());

    std::map<uint64_t, size_t> byAddress;
    std::map<size_t, size_t> byRva;
    for (size_t i = 0; i < containers.size(); ++i)
    {
        byAddress[reinterpret_cast<uint64_t>(base + containers[i])] = i;
        byRva[containers[i]] = i;
    }

    std::vector<std::vector<size_t>> slotRefs(containers.size()), leaRefs(containers.size());
    for (size_t at = 0; at + 8 <= size; at += 8)
    {
        uint64_t value;
        std::memcpy(&value, base + at, 8);
        const auto found = byAddress.find(value);
        if (found != byAddress.end())
            slotRefs[found->second].push_back(at);
    }
    for (size_t at = 0; at + 7 <= size; ++at)
    {
        // lea r64, [rip+disp32]: REX.W (48/4C) 8D modrm(mod=00 rm=101)
        if ((base[at] == 0x48 || base[at] == 0x4C) && base[at + 1] == 0x8D && (base[at + 2] & 0xC7) == 0x05)
        {
            int32_t displacement;
            std::memcpy(&displacement, base + at + 3, 4);
            const size_t target = at + 7 + displacement;
            const auto found = byRva.find(target);
            if (found != byRva.end())
                leaRefs[found->second].push_back(at);
        }
    }

    std::map<size_t, unsigned> callTargets;
    for (size_t i = 0; i < containers.size(); ++i)
    {
        for (const size_t lea : leaRefs[i])
        {
            // first call rel32 within the next 0x80 bytes
            for (size_t at = lea; at + 5 < size && at < lea + 0x80; ++at)
            {
                if (base[at] == 0xE8)
                {
                    int32_t displacement;
                    std::memcpy(&displacement, base + at + 1, 4);
                    ++callTargets[at + 5 + displacement];
                    break;
                }
            }
        }
    }
    for (const auto& [target, count] : callTargets)
        std::printf("call target 0x%zX x%u\n", target, count);

    size_t withSlot = 0, withLea = 0, none = 0;
    for (size_t i = 0; i < containers.size(); ++i)
    {
        withSlot += !slotRefs[i].empty();
        withLea += !leaRefs[i].empty();
        none += slotRefs[i].empty() && leaRefs[i].empty();
        if (verbose || (slotRefs[i].empty() && leaRefs[i].empty()))
        {
            uint64_t fatSize;
            std::memcpy(&fatSize, base + containers[i] + 8, 8);
            std::printf("c%zu rva=0x%zX fat=0x%llX slots:", i, containers[i], static_cast<unsigned long long>(fatSize));
            for (const size_t slot : slotRefs[i])
            {
                uint64_t next, prev;
                std::memcpy(&next, base + slot + 8, 8);
                std::memcpy(&prev, base + slot - 8, 8);
                std::printf(" [0x%zX prev=0x%llX next=0x%llX]", slot, static_cast<unsigned long long>(prev),
                            static_cast<unsigned long long>(next));
            }
            std::printf(" lea:");
            for (const size_t lea : leaRefs[i])
                std::printf(" 0x%zX", lea);
            std::printf("\n");
        }
    }
    std::printf("with slot refs=%zu, with lea refs=%zu, no refs=%zu\n", withSlot, withLea, none);
    return 0;
}
