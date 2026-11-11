// Launching GH2 Recomp by itself: the player's own Guitar Hero II (USA) disc
// image is required, and the game boots from the executable on it.
//
// With no ELF on the command line, the disc image is the first argument (a
// file dropped on the exe) or the first .iso in the exe's iso folder, beside
// the exe, or in an iso folder beside the exe's folder. Its boot executable
// (SLUS_214.47) must be byte-for-byte the retail v1.00 one this build was
// recompiled from, and the whole image Redump's: no modified file, since GH2
// Recomp is for playing the game as it shipped. The ELF is copied to
// data/SLUS_214.47 beside the exe; memory cards go in memcards/.
// A developer's command line (<elf> <disc> [options]) is left as it is.

#include "runtime/boot_args.h"

#if defined(GH1_PCSX2GS)
#include <SDL3/SDL.h>
#endif

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    constexpr const char *kElfName = "SLUS_214.47";
    constexpr const char *kElfSha1 = "aea7bbb01eb6e56056c3a33771ed046090d39168";
    // Redump: Guitar Hero II (USA), v1.00.
    constexpr const char *kDiscSha1 = "e1a3a81827aa4264984b4f3678de58ca3d881528";
    constexpr uintmax_t kDiscSize = 4646305792u;

    // Paths go to the runtime as the platform's narrow strings (what it
    // builds its paths from) and into messages as UTF-8.
    std::string utf8(const fs::path &p)
    {
        const std::u8string s = p.u8string();
        return std::string(s.begin(), s.end());
    }

    void tell(const std::string &message)
    {
        std::cerr << "[gh2] " << message << std::endl;
#if defined(GH1_PCSX2GS)
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "GH2 Recomp", message.c_str(), nullptr);
#endif
    }

    fs::path exeDirectory(const std::string &argv0)
    {
#if defined(GH1_PCSX2GS)
        if (const char *base = SDL_GetBasePath())
            return fs::path(std::u8string(reinterpret_cast<const char8_t *>(base)));
#endif
        std::error_code ec;
        const fs::path p = fs::absolute(fs::path(argv0), ec).parent_path();
        return ec ? fs::current_path() : p;
    }

    // --- SHA-1 (FIPS 180-4) --------------------------------------------------------

    class Sha1
    {
    public:
        void update(const uint8_t *data, size_t size)
        {
            m_length += size;
            if (m_used)
            {
                const size_t take = std::min(size, size_t(64) - m_used);
                std::memcpy(m_block + m_used, data, take);
                m_used += take, data += take, size -= take;
                if (m_used < 64)
                    return;
                compress(m_block);
                m_used = 0;
            }
            for (; size >= 64; data += 64, size -= 64)
                compress(data);
            std::memcpy(m_block, data, size);
            m_used = size;
        }

        std::string hex()
        {
            const uint64_t bits = m_length * 8u;
            const uint8_t pad = 0x80;
            update(&pad, 1);
            const uint8_t zero = 0;
            while (m_used != 56)
                update(&zero, 1);
            uint8_t len[8];
            for (int i = 0; i < 8; ++i)
                len[i] = static_cast<uint8_t>(bits >> ((7 - i) * 8));
            update(len, 8);
            char out[41];
            for (int i = 0; i < 5; ++i)
                std::snprintf(out + i * 8, 9, "%08x", m_h[i]);
            return out;
        }

    private:
        static uint32_t rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

        void compress(const uint8_t *p)
        {
            uint32_t w[80];
            for (int i = 0; i < 16; ++i)
                w[i] = (uint32_t(p[i * 4]) << 24) | (uint32_t(p[i * 4 + 1]) << 16) | (uint32_t(p[i * 4 + 2]) << 8) |
                       uint32_t(p[i * 4 + 3]);
            for (int i = 16; i < 80; ++i)
                w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
            uint32_t a = m_h[0], b = m_h[1], c = m_h[2], d = m_h[3], e = m_h[4];
            for (int i = 0; i < 80; ++i)
            {
                uint32_t f, k;
                if (i < 20)
                    f = (b & c) | (~b & d), k = 0x5A827999u;
                else if (i < 40)
                    f = b ^ c ^ d, k = 0x6ED9EBA1u;
                else if (i < 60)
                    f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDCu;
                else
                    f = b ^ c ^ d, k = 0xCA62C1D6u;
                const uint32_t t = rol(a, 5) + f + e + k + w[i];
                e = d, d = c, c = rol(b, 30), b = a, a = t;
            }
            m_h[0] += a, m_h[1] += b, m_h[2] += c, m_h[3] += d, m_h[4] += e;
        }

        uint32_t m_h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
        uint8_t m_block[64] = {};
        size_t m_used = 0;
        uint64_t m_length = 0;
    };

    std::string sha1(const std::vector<uint8_t> &data)
    {
        Sha1 h;
        h.update(data.data(), data.size());
        return h.hex();
    }

    std::optional<std::string> sha1File(const fs::path &path)
    {
        std::ifstream f(path, std::ios::binary);
        if (!f)
            return std::nullopt;
        Sha1 h;
        std::vector<char> buffer(4u << 20);
        while (f)
        {
            f.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            h.update(reinterpret_cast<const uint8_t *>(buffer.data()), static_cast<size_t>(f.gcount()));
        }
        if (!f.eof())
            return std::nullopt;
        return h.hex();
    }

    // The whole disc image must be Redump's Guitar Hero II (USA) v1.00: no
    // modified file on it. Hashing 2.3 GB takes some seconds, so a match is
    // remembered in data/disc.verified by the image's size and write time.
    bool discVerified(const fs::path &image, const fs::path &data, std::string &problem)
    {
        std::error_code ec;
        const uintmax_t size = fs::file_size(image, ec);
        if (ec)
        {
            problem = "cannot read it";
            return false;
        }
        const auto written = fs::last_write_time(image, ec).time_since_epoch().count();
        const std::string record = std::to_string(size) + " " + std::to_string(written) + " " + kDiscSha1;
        const fs::path cache = data / "disc.verified";
        {
            std::ifstream in(cache);
            std::string line;
            while (std::getline(in, line))
                if (line == record)
                    return true;
        }
        if (size != kDiscSize)
        {
            problem = "its size is " + std::to_string(size) + " bytes, the original's " + std::to_string(kDiscSize);
            return false;
        }
        std::cerr << "[gh2] checking the whole disc image against Redump (once)..." << std::endl;
        const std::optional<std::string> got = sha1File(image);
        if (!got)
        {
            problem = "cannot read it";
            return false;
        }
        if (*got != kDiscSha1)
        {
            problem = "its SHA-1 is " + *got + ", the original's " + kDiscSha1;
            return false;
        }
        std::ofstream(cache, std::ios::app) << record << "\n";
        return true;
    }

    // --- ISO 9660: a file in the root directory --------------------------------------

    uint32_t le32(const uint8_t *p)
    {
        return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
    }

    std::optional<std::vector<uint8_t>> readRootFile(const fs::path &image, const std::string &name)
    {
        std::ifstream f(image, std::ios::binary);
        if (!f)
            return std::nullopt;
        std::array<uint8_t, 2048> pvd{};
        f.seekg(16 * 2048);
        if (!f.read(reinterpret_cast<char *>(pvd.data()), pvd.size()) || std::memcmp(&pvd[1], "CD001", 5) != 0)
            return std::nullopt;
        const uint32_t rootLba = le32(&pvd[156 + 2]);
        const uint32_t rootSize = le32(&pvd[156 + 10]);
        if (rootSize == 0 || rootSize > 1u << 20)
            return std::nullopt;
        std::vector<uint8_t> dir(rootSize);
        f.seekg(static_cast<std::streamoff>(rootLba) * 2048);
        if (!f.read(reinterpret_cast<char *>(dir.data()), dir.size()))
            return std::nullopt;
        for (size_t i = 0; i < dir.size();)
        {
            const uint8_t n = dir[i];
            if (n == 0)
            {
                i = (i / 2048 + 1) * 2048;
                continue;
            }
            if (i + n > dir.size() || n < 34)
                break;
            const std::string entry(reinterpret_cast<const char *>(&dir[i + 33]), dir[i + 32]);
            if (entry.substr(0, entry.find(';')) == name)
            {
                const uint32_t lba = le32(&dir[i + 2]);
                const uint32_t size = le32(&dir[i + 10]);
                if (size > 64u << 20)
                    return std::nullopt;
                std::vector<uint8_t> data(size);
                f.seekg(static_cast<std::streamoff>(lba) * 2048);
                if (!f.read(reinterpret_cast<char *>(data.data()), data.size()))
                    return std::nullopt;
                return data;
            }
            i += n;
        }
        return std::nullopt;
    }

    bool isElf(const std::string &arg)
    {
        std::string ext = fs::path(arg).extension().string();
        for (char &c : ext)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return ext == ".elf" || fs::path(arg).filename().string() == kElfName;
    }

    std::vector<fs::path> candidateImages(const fs::path &base, const std::vector<std::string> &args)
    {
        std::vector<fs::path> found;
        if (args.size() >= 2 && !args[1].empty() && args[1][0] != '-')
            found.push_back(fs::path(args[1]));
        for (const fs::path &dir : {base / "iso", base, base.parent_path() / "iso"})
        {
            std::error_code ec;
            for (const auto &entry : fs::directory_iterator(dir, ec))
            {
                std::string ext = entry.path().extension().string();
                for (char &c : ext)
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (entry.is_regular_file(ec) && ext == ".iso")
                    found.push_back(entry.path());
            }
        }
        return found;
    }
}

void ps2xPrepareArguments(std::vector<std::string> &args)
{
    const bool developer = args.size() >= 2 && isElf(args[1]);
    const fs::path base = exeDirectory(args.empty() ? std::string() : args[0]);
#if defined(_WIN32)
    // Launched as a window program there is no console: the log goes to
    // GH2Recomp.log beside the exe.
    if (_fileno(stdout) < 0 || _fileno(stderr) < 0)
    {
        // Emptied, then both streams append: each write goes to the end,
        // so neither overwrites what the other wrote.
        // Wide paths: the folder's name may have letters the ANSI code page
        // does not (and the narrow calls would miss the file).
        const std::wstring log = (base / "GH2Recomp.log").wstring();
        if (std::FILE *f = _wfopen(log.c_str(), L"w"))
            std::fclose(f);
        _wfreopen(log.c_str(), L"a", stdout);
        _wfreopen(log.c_str(), L"a", stderr);
    }
#endif
    // Log lines go out as they are written, so a log ends where the run did.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    if (developer)
        return; // a developer's command line
    std::vector<std::string> options;
    size_t first = 1;
    if (args.size() >= 2 && !args[1].empty() && args[1][0] != '-')
        first = 2;
    for (size_t i = first; i < args.size(); ++i)
        options.push_back(args[i]);

    std::string rejected;
    for (const fs::path &image : candidateImages(base, args))
    {
        const std::optional<std::vector<uint8_t>> elf = readRootFile(image, kElfName);
        if (!elf)
        {
            rejected += "\n  " + utf8(image) + ": not Guitar Hero II (USA), SLUS-21447";
            continue;
        }
        if (sha1(*elf) != kElfSha1)
        {
            rejected += "\n  " + utf8(image) + ": not a clean Guitar Hero II (USA) v1.00 image";
            continue;
        }

        std::error_code ec;
        const fs::path data = base / "data";
        const fs::path elfPath = data / kElfName;
        const fs::path cards = base / "memcards";
        fs::create_directories(data, ec);
        fs::create_directories(cards, ec);
        std::string problem;
        if (!discVerified(image, data, problem))
        {
            rejected += "\n  " + utf8(image) + ": not the original Guitar Hero II (USA) v1.00 disc (" + problem +
                        "); GH2 Recomp runs the unmodified disc only";
            continue;
        }
        std::ofstream out(elfPath, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char *>(elf->data()), static_cast<std::streamsize>(elf->size()));
        out.close();
        if (!out)
        {
            tell("Could not write " + utf8(elfPath) + ".\nNão foi possível gravar " + utf8(elfPath) + ".");
            args.clear();
            return;
        }
        std::cerr << "[gh2] disc: " << utf8(image) << std::endl;
        std::vector<std::string> prepared{args.empty() ? std::string("GH2Recomp") : args[0], elfPath.string(),
                                          image.string(), "--mc", cards.string()};
        prepared.insert(prepared.end(), options.begin(), options.end());
        args.swap(prepared);
        return;
    }

    tell("GH2 Recomp needs your Guitar Hero II (USA) disc image (SLUS-21447, v1.00).\n"
         "Put the .iso in the \"iso\" folder next to GH2Recomp.exe, or drop it on the exe.\n\n"
         "O GH2 Recomp precisa da imagem do seu disco do Guitar Hero II (USA) (SLUS-21447, v1.00).\n"
         "Coloque o .iso na pasta \"iso\" ao lado do GH2Recomp.exe, ou arraste-o sobre o exe." +
         (rejected.empty() ? std::string() : "\n" + rejected));
    args.clear();
}
