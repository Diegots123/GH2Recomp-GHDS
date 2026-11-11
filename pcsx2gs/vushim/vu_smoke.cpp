// Runs a four-pair VU1 program through the microVU bridge: an integer add,
// a store, and an XGKICK of a one-qword packet. Exits 0 when all three show.

#include "pcsx2vu_bridge.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

int main()
{
    alignas(16) static uint8_t code[0x4000];
    alignas(16) static uint8_t data[0x4000];
    const uint32_t upperNop = 0x000002FFu, ebit = 0x40000000u, lowerNop = 0x8000033Cu;
    const uint32_t program[] = {
        0x10010005u, upperNop,        // IADDIU vi01, vi00, 5
        0x0B010010u, upperNop,        // ISW.x vi01, 0x10(vi00)
        0x800006FCu, upperNop | ebit, // XGKICK vi00, end
        lowerNop, upperNop,
    };
    std::memcpy(code, program, sizeof(program));
    const uint64_t tag[2] = {1u | 0x8000u | (1ull << 60), 0x0Eu}; // NLOOP 1, EOP, PACKED, A+D
    std::memcpy(data, tag, sizeof(tag));

    std::vector<uint32_t> kicks;
    pcsx2vu::configure(pcsx2vu::Settings{1}, [&](const uint8_t *, uint32_t size) { kicks.push_back(size); });
    pcsx2vu::Run run;
    run.code = code;
    run.data = data;
    run.startPC = 0;
    const pcsx2vu::Result result = pcsx2vu::run(run);

    uint32_t stored;
    std::memcpy(&stored, data + 0x100, sizeof(stored));
    std::printf("stored %u, kicks %zu (%u bytes), cycles %llu, vpu_stat %x\n", stored, kicks.size(),
                kicks.empty() ? 0u : kicks[0], static_cast<unsigned long long>(result.cycles), result.vpuStat);
    // Again, from the start, to use the cached program.
    std::memset(data + 0x100, 0, 16);
    pcsx2vu::run(run);
    std::memcpy(&stored, data + 0x100, sizeof(stored));
    std::printf("second run: stored %u, kicks %zu\n", stored, kicks.size());
    return (stored == 5u && kicks.size() == 2 && kicks[0] == 32u) ? 0 : 1;
}
