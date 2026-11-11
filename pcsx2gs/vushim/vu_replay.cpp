// Replays VU1 program starts dumped by the runtime (PS2X_VU1_DUMP) through
// the microVU bridge, for finding where microVU and the interpreter part.
//
//   pcsx2vu_replay <code.bin> <starts.bin>... [--out <dir>]
//
// Each starts file holds records of {u32 kind, u32 pc, u32 top, u32 itop,
// 16 KiB VU1 data, interpreter state}; each record's data is loaded before
// its start (so a record replays from the same memory the game had) unless
// --keep-data is given. With --out, VU1 data after each start is written
// to <dir>/after_<n>.bin.

#include "pcsx2vu_bridge.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static std::vector<uint8_t> readFile(const char *path)
{
    std::vector<uint8_t> bytes;
    if (std::FILE *f = std::fopen(path, "rb"))
    {
        uint8_t buf[65536];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
            bytes.insert(bytes.end(), buf, buf + n);
        std::fclose(f);
    }
    return bytes;
}

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: %s <code.bin> <starts.bin>... [--out dir] [--keep-data] [--state-size n]\n", argv[0]);
        return 2;
    }
    alignas(16) static uint8_t code[0x4000];
    alignas(16) static uint8_t data[0x4000];
    const std::vector<uint8_t> codeFile = readFile(argv[1]);
    if (codeFile.size() != sizeof(code))
    {
        std::fprintf(stderr, "code file must be 16 KiB\n");
        return 2;
    }
    std::memcpy(code, codeFile.data(), sizeof(code));

    std::vector<std::string> starts;
    std::string out;
    bool keepData = false;
    size_t stateSize = 664;
    for (int i = 2; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc)
            out = argv[++i];
        else if (std::strcmp(argv[i], "--keep-data") == 0)
            keepData = true;
        else if (std::strcmp(argv[i], "--state-size") == 0 && i + 1 < argc)
            stateSize = std::strtoul(argv[++i], nullptr, 0);
        else
            starts.push_back(argv[i]);
    }

    uint32_t kicks = 0, kickBytes = 0;
    pcsx2vu::configure(pcsx2vu::Settings{1}, [&](const uint8_t *, uint32_t size) {
        ++kicks;
        kickBytes += size;
    });

    const size_t recordSize = 16 + sizeof(data) + stateSize;
    unsigned n = 0;
    for (const std::string &path : starts)
    {
        const std::vector<uint8_t> file = readFile(path.c_str());
        for (size_t at = 0; at + recordSize <= file.size(); at += recordSize, ++n)
        {
            uint32_t head[4];
            std::memcpy(head, file.data() + at, sizeof(head));
            if (!keepData || n == 0)
                std::memcpy(data, file.data() + at + 16, sizeof(data));
            pcsx2vu::Run run;
            run.code = code;
            run.data = data;
            run.startPC = head[1];
            run.resume = head[0] == 1;
            run.top = head[2];
            run.itop = head[3];
            const uint32_t before = kicks;
            const pcsx2vu::Result result = pcsx2vu::run(run);
            std::printf("#%u %s pc %04x top %u: %llu cycles, %u kicks, vpu_stat %x\n", n, run.resume ? "mscnt" : "mscal",
                        head[1], head[2], static_cast<unsigned long long>(result.cycles), kicks - before, result.vpuStat);
            if (!out.empty())
            {
                const std::string name = out + "/after_" + std::to_string(n) + ".bin";
                if (std::FILE *f = std::fopen(name.c_str(), "wb"))
                {
                    std::fwrite(data, 1, sizeof(data), f);
                    std::fclose(f);
                }
            }
        }
    }
    std::printf("%u starts, %u kicks, %u bytes\n", n, kicks, kickBytes);
    return 0;
}
