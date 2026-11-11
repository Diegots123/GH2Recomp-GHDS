#include "pcsx2gs_bridge.h"

#include "GS/GS.h"
#include "GS/GSUtil.h"
#include "GS/Renderers/Common/GSRenderer.h"
#include "GS/Renderers/HW/GSTextureCache.h"
#include "GS/Renderers/Common/GSDevice.h"
#include "GS/Renderers/OpenGL/GSDeviceOGL.h"
#ifdef _WIN32
#include "GS/Renderers/DX11/GSDevice11.h"
#endif
#include "Config.h"
#include "ShaderCacheVersion.h"
#include "common/AlignedMalloc.h"
#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "pcsx2gs_host.h"

#include <glad/gl.h>

#ifndef PCSX2GS_HAS_D3D12
#define PCSX2GS_HAS_D3D12 0
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

// The OpenGL device's game shader programs, which PCSX2 keeps to itself: a
// program is made the first time a draw needs it (from the shader cache on
// disk when it is there) and kept for the run. Reached here through the
// standard rule that explicit instantiations may name private members.
namespace
{
    using ProgramMap = std::unordered_map<GSDeviceOGL::ProgramSelector, GLProgram, GSDeviceOGL::ProgramSelectorHash>;

    struct ProgramsTag
    {
        using type = ProgramMap GSDeviceOGL::*;
        friend type member(ProgramsTag);
    };
    struct SetupPipelineTag
    {
        using type = void (GSDeviceOGL::*)(const GSDeviceOGL::ProgramSelector &);
        friend type member(SetupPipelineTag);
    };
    template <typename Tag, typename Tag::type M>
    struct Expose
    {
        friend typename Tag::type member(Tag) { return M; }
    };
    template struct Expose<ProgramsTag, &GSDeviceOGL::m_programs>;
    template struct Expose<SetupPipelineTag, &GSDeviceOGL::SetupPipeline>;

#ifdef _WIN32
    // The Direct3D 11 device's shaders, kept the same way: one pixel shader
    // per PSSelector, one vertex shader per VSSelector key.
    using PixelShaderMap = std::unordered_map<GSHWDrawConfig::PSSelector, wil::com_ptr_nothrow<ID3D11PixelShader>,
                                              GSHWDrawConfig::PSSelectorHash>;
    using VertexShaderMap = std::unordered_map<u32, GSVertexShader11>;
    struct PixelShadersTag
    {
        using type = PixelShaderMap GSDevice11::*;
        friend type member(PixelShadersTag);
    };
    struct VertexShadersTag
    {
        using type = VertexShaderMap GSDevice11::*;
        friend type member(VertexShadersTag);
    };
    template struct Expose<PixelShadersTag, &GSDevice11::m_ps>;
    template struct Expose<VertexShadersTag, &GSDevice11::m_vs>;
#endif
}

namespace pcsx2gs
{
    namespace
    {
        u8 *s_regs = nullptr; // the GS's view of the privileged registers
        bool s_open = false;
        Stalls s_stalls;      // GS thread only, as every call here

        // What can hold a frame up inside the GS, timed around the GL calls
        // that do it (PCSX2's renderer calls OpenGL through glad's function
        // pointers): compiling and linking shader programs, loading them
        // from the cache, and waiting on the GPU to read VRAM back.
        PFNGLCOMPILESHADERPROC s_compileShader = nullptr;
        PFNGLLINKPROGRAMPROC s_linkProgram = nullptr;
        PFNGLPROGRAMBINARYPROC s_programBinary = nullptr;
        PFNGLCLIENTWAITSYNCPROC s_clientWaitSync = nullptr;

        double millisSince(std::chrono::steady_clock::time_point t0)
        {
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        }

        void GLAD_API_PTR timedCompileShader(GLuint shader)
        {
            const auto t0 = std::chrono::steady_clock::now();
            s_compileShader(shader);
            s_stalls.compiledMs += millisSince(t0);
        }

        void GLAD_API_PTR timedLinkProgram(GLuint program)
        {
            const auto t0 = std::chrono::steady_clock::now();
            s_linkProgram(program);
            s_stalls.compiledMs += millisSince(t0);
            ++s_stalls.compiled;
        }

        void GLAD_API_PTR timedProgramBinary(GLuint program, GLenum format, const void *binary, GLsizei length)
        {
            const auto t0 = std::chrono::steady_clock::now();
            s_programBinary(program, format, binary, length);
            s_stalls.loadedMs += millisSince(t0);
            ++s_stalls.loaded;
        }

        GLenum GLAD_API_PTR timedClientWaitSync(GLsync sync, GLbitfield flags, GLuint64 timeout)
        {
            const auto t0 = std::chrono::steady_clock::now();
            const GLenum r = s_clientWaitSync(sync, flags, timeout);
            s_stalls.gpuWaitMs += millisSince(t0);
            ++s_stalls.gpuWaits;
            return r;
        }

        // The game's programs, made before the game needs them. Every
        // program a run used is listed in the cache folder
        // (gh1_programs.bin), and the next run makes them all as it opens:
        // they come from the shader cache in a moment, where making each
        // one as the game first draws with it would hold that frame up
        // (a new camera, a new effect). The list is PCSX2's own program
        // keys, so it only holds for the PCSX2 and the shader cache version
        // it was written with.
        struct ProgramListHeader
        {
            char magic[8] = {'G', 'H', '1', 'P', 'R', 'O', 'G', 'S'};
            u32 cacheVersion = SHADER_CACHE_VERSION;
            u32 selectorSize = sizeof(GSDeviceOGL::ProgramSelector);
        };
        size_t s_programsSaved = 0;
        uint32_t s_vsyncs = 0;

        std::string programListPath() { return Path::Combine(EmuFolders::Cache, "gh1_programs.bin"); }

        bool s_gl = false; // the OpenGL renderer is the one open

        GSDeviceOGL *glDevice() { return s_gl ? static_cast<GSDeviceOGL *>(g_gs_device.get()) : nullptr; }

        void savePrograms()
        {
            GSDeviceOGL *dev = glDevice();
            if (!dev || EmuFolders::Cache.empty())
                return;
            const ProgramMap &programs = dev->*member(ProgramsTag{});
            if (programs.size() == s_programsSaved)
                return;
            std::vector<u8> data(sizeof(ProgramListHeader));
            const ProgramListHeader header;
            std::memcpy(data.data(), &header, sizeof(header));
            for (const auto &[selector, program] : programs)
            {
                if (!program.IsValid())
                    continue;
                const u8 *p = reinterpret_cast<const u8 *>(&selector);
                data.insert(data.end(), p, p + sizeof(selector));
            }
            if (FileSystem::WriteBinaryFile(programListPath().c_str(), data.data(), data.size()))
                s_programsSaved = programs.size();
        }

        // Makes every program a list names; false when there is no such list.
        bool makeListed(const std::string &path)
        {
            GSDeviceOGL *dev = glDevice();
            if (!dev || path.empty())
                return false;
            std::optional<std::vector<u8>> data = FileSystem::ReadBinaryFile(path.c_str());
            const ProgramListHeader expected;
            if (!data || data->size() < sizeof(expected) ||
                std::memcmp(data->data(), &expected, sizeof(expected)) != 0 ||
                (data->size() - sizeof(expected)) % sizeof(GSDeviceOGL::ProgramSelector) != 0)
                return false;
            for (size_t at = sizeof(expected); at < data->size(); at += sizeof(GSDeviceOGL::ProgramSelector))
            {
                GSDeviceOGL::ProgramSelector selector;
                std::memcpy(&selector, data->data() + at, sizeof(selector));
                (dev->*member(SetupPipelineTag{}))(selector);
            }
            return true;
        }

        // The cache's list, and the one that comes with the game (the
        // programs GH1 draws with, so a first run makes them all here, once,
        // instead of one by one as it plays). A program in both is made once.
        void loadPrograms(const std::string &shipped)
        {
            GSDeviceOGL *dev = glDevice();
            if (!dev)
                return;
            const auto t0 = std::chrono::steady_clock::now();
            const Stalls before = s_stalls;
            const bool own = !EmuFolders::Cache.empty() && makeListed(programListPath());
            const bool given = makeListed(shipped);
            const size_t n = (dev->*member(ProgramsTag{})).size();
            // A list that grew from the shipped one is written to the cache.
            s_programsSaved = own ? n : 0;
            if (own || given)
                std::fprintf(stderr, "[gs] %zu shader programs ready in %.0f ms (%u from the cache, %u compiled)\n", n,
                             millisSince(t0), s_stalls.loaded - before.loaded, s_stalls.compiled - before.compiled);
        }

#ifdef _WIN32
        // Direct3D 11's list: the same idea as the OpenGL one, its own file
        // (the shaders a draw needs differ with the API's features). A first
        // run with no list of its own starts from the OpenGL list that comes
        // with the game, whose vertex and pixel selectors mostly hold here.
        struct ShaderListHeader
        {
            char magic[8] = {'G', 'H', '1', 'D', '3', 'D', '1', '1'};
            u32 cacheVersion = SHADER_CACHE_VERSION;
            u32 selectorSize = sizeof(GSHWDrawConfig::PSSelector);
        };
        bool s_d3d11 = false; // the Direct3D 11 renderer is the one open
        size_t s_d3d11Saved = 0;
        size_t s_d3d11Seen = 0; // shaders the device had at the last takeStalls

        GSDevice11 *d3d11Device() { return s_d3d11 ? static_cast<GSDevice11 *>(g_gs_device.get()) : nullptr; }
        std::string shaderListPath() { return Path::Combine(EmuFolders::Cache, "gh1_shaders_d3d11.bin"); }
        size_t d3d11Shaders(GSDevice11 *dev)
        {
            return (dev->*member(PixelShadersTag{})).size() + (dev->*member(VertexShadersTag{})).size();
        }

        void saveD3D11Shaders()
        {
            GSDevice11 *dev = d3d11Device();
            if (!dev || EmuFolders::Cache.empty())
                return;
            const PixelShaderMap &ps = dev->*member(PixelShadersTag{});
            const VertexShaderMap &vs = dev->*member(VertexShadersTag{});
            if (ps.size() + vs.size() == s_d3d11Saved)
                return;
            std::vector<u8> data(sizeof(ShaderListHeader));
            const ShaderListHeader header;
            std::memcpy(data.data(), &header, sizeof(header));
            auto put32 = [&data](u32 v) {
                const u8 *b = reinterpret_cast<const u8 *>(&v);
                data.insert(data.end(), b, b + 4);
            };
            put32(static_cast<u32>(vs.size()));
            for (const auto &[key, shader] : vs)
                put32(key);
            for (const auto &[selector, shader] : ps)
            {
                if (!shader)
                    continue;
                const u8 *b = reinterpret_cast<const u8 *>(&selector);
                data.insert(data.end(), b, b + sizeof(selector));
            }
            if (FileSystem::WriteBinaryFile(shaderListPath().c_str(), data.data(), data.size()))
                s_d3d11Saved = ps.size() + vs.size();
        }

        void makeD3D11(GSDevice11 *dev, const GSHWDrawConfig::VSSelector *vs, const GSHWDrawConfig::PSSelector *ps)
        {
            if (vs)
            {
                const GSHWDrawConfig::VSConstantBuffer cb{};
                dev->SetupVS(*vs, &cb);
            }
            if (ps)
                dev->SetupPS(*ps, nullptr, GSHWDrawConfig::SamplerSelector{});
        }

        bool makeD3D11Listed(GSDevice11 *dev)
        {
            std::optional<std::vector<u8>> data = FileSystem::ReadBinaryFile(shaderListPath().c_str());
            const ShaderListHeader expected;
            if (!data || data->size() < sizeof(expected) + 4 || std::memcmp(data->data(), &expected, sizeof(expected)) != 0)
                return false;
            size_t at = sizeof(expected);
            u32 nvs = 0;
            std::memcpy(&nvs, data->data() + at, 4);
            at += 4;
            if (data->size() < at + size_t(nvs) * 4u ||
                (data->size() - at - size_t(nvs) * 4u) % sizeof(GSHWDrawConfig::PSSelector) != 0)
                return false;
            for (u32 i = 0; i < nvs; ++i, at += 4)
            {
                u32 key = 0;
                std::memcpy(&key, data->data() + at, 4);
                const GSHWDrawConfig::VSSelector vs(static_cast<u8>(key));
                makeD3D11(dev, &vs, nullptr);
            }
            for (; at < data->size(); at += sizeof(GSHWDrawConfig::PSSelector))
            {
                GSHWDrawConfig::PSSelector ps;
                std::memcpy(&ps, data->data() + at, sizeof(ps));
                makeD3D11(dev, nullptr, &ps);
            }
            return true;
        }

        bool makeD3D11FromGlList(GSDevice11 *dev, const std::string &path)
        {
            std::optional<std::vector<u8>> data = path.empty() ? std::nullopt : FileSystem::ReadBinaryFile(path.c_str());
            const ProgramListHeader expected;
            if (!data || data->size() < sizeof(expected) ||
                std::memcmp(data->data(), &expected, sizeof(expected)) != 0 ||
                (data->size() - sizeof(expected)) % sizeof(GSDeviceOGL::ProgramSelector) != 0)
                return false;
            for (size_t at = sizeof(expected); at < data->size(); at += sizeof(GSDeviceOGL::ProgramSelector))
            {
                GSDeviceOGL::ProgramSelector selector;
                std::memcpy(&selector, data->data() + at, sizeof(selector));
                makeD3D11(dev, &selector.vs, &selector.ps);
            }
            return true;
        }

        void loadD3D11Shaders(const std::string &shippedGl)
        {
            GSDevice11 *dev = d3d11Device();
            if (!dev)
                return;
            const auto t0 = std::chrono::steady_clock::now();
            const bool own = !EmuFolders::Cache.empty() && makeD3D11Listed(dev);
            const bool seeded = !own && makeD3D11FromGlList(dev, shippedGl);
            const size_t n = d3d11Shaders(dev);
            s_d3d11Saved = own ? n : 0;
            s_d3d11Seen = n;
            if (own || seeded)
                std::fprintf(stderr, "[gs] %zu Direct3D 11 shaders ready in %.0f ms%s\n", n, millisSince(t0),
                             own ? "" : " (the first time: compiled from the OpenGL list that comes with the game)");
        }
#endif

        void timeGlCalls()
        {
            if (glad_glCompileShader && glad_glCompileShader != timedCompileShader)
            {
                s_compileShader = glad_glCompileShader;
                glad_glCompileShader = timedCompileShader;
            }
            if (glad_glLinkProgram && glad_glLinkProgram != timedLinkProgram)
            {
                s_linkProgram = glad_glLinkProgram;
                glad_glLinkProgram = timedLinkProgram;
            }
            if (glad_glProgramBinary && glad_glProgramBinary != timedProgramBinary)
            {
                s_programBinary = glad_glProgramBinary;
                glad_glProgramBinary = timedProgramBinary;
            }
            if (glad_glClientWaitSync && glad_glClientWaitSync != timedClientWaitSync)
            {
                s_clientWaitSync = glad_glClientWaitSync;
                glad_glClientWaitSync = timedClientWaitSync;
            }
        }
    }

    Stalls takeStalls()
    {
        Stalls s = s_stalls;
        s_stalls = {};
#ifdef _WIN32
        // Direct3D 11's shaders are made inside its draws: counted, not timed.
        if (GSDevice11 *dev = d3d11Device())
        {
            const size_t n = d3d11Shaders(dev);
            s.compiled += static_cast<uint32_t>(n - std::min(n, s_d3d11Seen));
            s_d3d11Seen = n;
        }
#endif
        return s;
    }

    namespace
    {
        GSRendererType s_renderer = GSRendererType::OGL;
    }

    VideoMemory videoMemory()
    {
        VideoMemory m;
        if (!s_open || !g_gs_device || s_renderer == GSRendererType::Null || s_renderer == GSRendererType::SW)
            return m;
        if (g_texture_cache)
        {
            m.sources = g_texture_cache->GetSourceMemoryUsage();
            m.targets = g_texture_cache->GetTargetMemoryUsage();
            m.hashed = g_texture_cache->GetTotalHashCacheMemoryUsage();
        }
        m.pool = g_gs_device->GetPoolMemoryUsage();
        return m;
    }

    namespace
    {

        const char *rendererName(GSRendererType renderer)
        {
            switch (renderer)
            {
            case GSRendererType::DX11:
                return "Direct3D 11";
            case GSRendererType::DX12:
                return "Direct3D 12";
            case GSRendererType::SW:
                return "software";
            case GSRendererType::Null:
                return "null";
            default:
                return "OpenGL";
            }
        }

        GSRendererType hardwareRenderer(Api api)
        {
#ifdef _WIN32
            switch (api)
            {
            case Api::Direct3D11:
                return GSRendererType::DX11;
            case Api::Direct3D12:
                return PCSX2GS_HAS_D3D12 ? GSRendererType::DX12 : GSRendererType::DX11;
            case Api::Auto:
            {
                // PCSX2's own choice by GPU maker and feature level
                // (pcsx2/GS/Renderers/DX11/D3D.cpp, GetPreferredRenderer).
                const GSRendererType preferred = GSUtil::GetPreferredRenderer();
                if (preferred == GSRendererType::DX12 && !PCSX2GS_HAS_D3D12)
                    return GSRendererType::DX11;
                return preferred == GSRendererType::DX11 || preferred == GSRendererType::DX12 ? preferred
                                                                                                : GSRendererType::OGL;
            }
            default:
                return GSRendererType::OGL;
            }
#else
            (void)api;
            return GSRendererType::OGL;
#endif
        }
    }

    std::string describe()
    {
        std::string out = rendererName(s_renderer);
        if (s_open && g_gs_device && s_renderer != GSRendererType::Null && s_renderer != GSRendererType::SW)
            out += " on " + g_gs_device->GetName();
        return out;
    }

    bool open(const NativeWindow &window, const Settings &settings, std::string &error)
    {
        // PCSX2's own messages into the log: its errors and warnings (why a
        // renderer did not open), everything with GH1_GS_LOG=1.
        Log::SetHostOutputLevel(std::getenv("GH1_GS_LOG") ? LOGLEVEL_INFO : LOGLEVEL_WARNING,
                                [](LOGLEVEL, ConsoleColors, std::string_view message) {
                                    std::fprintf(stderr, "[pcsx2] %.*s\n", static_cast<int>(message.size()),
                                                 message.data());
                                });
        WindowInfo wi;
        wi.type = window.type == NativeWindow::Type::Win32 ? WindowInfo::Type::Win32 : WindowInfo::Type::X11;
        wi.display_connection = window.display;
        wi.window_handle = window.handle;
        wi.surface_width = window.width;
        wi.surface_height = window.height;
        wi.surface_scale = window.scale;
        host().acquireWindow = [wi](bool) { return std::optional<WindowInfo>(wi); };

        // PCSX2 keeps the shaders it compiles in its cache folder; without
        // one, every effect is compiled again the first time each run shows
        // it, and the picture stalls for that moment.
        if (!settings.cacheDir.empty() && FileSystem::EnsureDirectoryExists(settings.cacheDir.c_str(), true))
            EmuFolders::Cache = settings.cacheDir;

        Pcsx2Config::GSOptions options;
        options.UpscaleMultiplier = settings.upscale;
        // A PS2 on a 4:3 television, and nothing on top of the picture.
        options.AspectRatio = AspectRatioType::R4_3;
        EmuConfig.GS = options;
        EmuConfig.CurrentAspectRatio = AspectRatioType::R4_3;

        if (!s_regs)
            s_regs = static_cast<u8 *>(_aligned_malloc(0x2000, 32));
        std::memset(s_regs, 0, 0x2000);
        GSRendererType renderer = settings.null       ? GSRendererType::Null
                                  : settings.software ? GSRendererType::SW
                                                      : hardwareRenderer(settings.api);
        if (!GSopen(options, renderer, s_regs, settings.vsync ? GSVSyncMode::FIFO : GSVSyncMode::Disabled, false))
        {
            error = renderer == GSRendererType::OGL
                        ? "the GS could not open its renderer (OpenGL 4.3 or newer is needed)"
                        : std::string("the GS could not open its renderer (") + rendererName(renderer) + ")";
            return false;
        }
        s_renderer = renderer;
        s_open = true;
        // From here on: the programs PCSX2 compiled while opening are its
        // fixed ones (copies, presentation), not the game's effects.
        s_gl = renderer == GSRendererType::OGL;
        if (s_gl)
        {
            timeGlCalls();
            loadPrograms(settings.programList);
        }
#ifdef _WIN32
        s_d3d11 = renderer == GSRendererType::DX11;
        loadD3D11Shaders(settings.programList);
#endif
        s_stalls = {};
        return true;
    }

    namespace
    {
        void releaseDepthCopies(); // below, with the late reads
    }

    void close()
    {
        if (s_open)
        {
            releaseDepthCopies(); // before the device that made them
            savePrograms();
#ifdef _WIN32
            saveD3D11Shaders();
#endif
            GSclose();
        }
        s_gl = false;
#ifdef _WIN32
        s_d3d11 = false;
#endif
        s_open = false;
    }

    void transfer(uint32_t path, const uint8_t *data, uint32_t qwc)
    {
        if (!s_open || qwc == 0)
            return;
        // Each path keeps its own GIFtag state, as on the GIF.
        switch (path)
        {
        case 1:
            g_gs_renderer->Transfer<0>(data, qwc);
            break;
        case 2:
            g_gs_renderer->Transfer<1>(data, qwc);
            break;
        default:
            g_gs_renderer->Transfer<2>(data, qwc);
            break;
        }
    }

    void vsync(const uint8_t *privRegs)
    {
        if (!s_open)
            return;
        std::memcpy(s_regs, privRegs, 0x2000);
        u32 csr;
        std::memcpy(&csr, s_regs + 0x1000, sizeof(csr));
        // CSR.FIELD (bit 13) is the field now being displayed, as PCSX2's MTGS passes it.
        GSvsync((csr & 0x2000) ? 0 : 1, true);
        // Every 10 s, the list of programs, when it grew (a run that ends
        // without closing keeps what it had used by then).
        if (++s_vsyncs % 600u == 0u)
        {
            savePrograms();
#ifdef _WIN32
            saveD3D11Shaders();
#endif
        }
    }

    void readLocal(uint8_t *dst, uint32_t qwc)
    {
        if (!s_open)
        {
            std::memset(dst, 0, static_cast<size_t>(qwc) * 16);
            return;
        }
        GSInitAndReadFIFO(dst, qwc);
        // GH1_READTRACE=1: each read's source, the Z and frame buffers drawn
        // to, and the first bytes read (the first 400 reads).
        static const bool trace = std::getenv("GH1_READTRACE") != nullptr;
        static unsigned traced = 0;
        if (trace && g_gs_renderer && traced < 400)
        {
            ++traced;
            const GSDrawingEnvironment &env = g_gs_renderer->m_env;
            const GIFRegZBUF z = env.CTXT[0].ZBUF;
            const GIFRegFRAME f = env.CTXT[0].FRAME;
            std::fprintf(stderr,
                         "[gs read] qwc %u from bp %x bw %u psm %x at %u,%u size %ux%u; ZBUF zbp %x psm %x zmsk %u; "
                         "FRAME fbp %x fbw %u psm %x; got %02x %02x %02x %02x\n",
                         qwc, env.BITBLTBUF.SBP, env.BITBLTBUF.SBW, env.BITBLTBUF.SPSM, env.TRXPOS.SSAX,
                         env.TRXPOS.SSAY, env.TRXREG.RRW, env.TRXREG.RRH, z.ZBP, z.PSM, z.ZMSK, f.FBP, f.FBW, f.PSM,
                         dst[0], dst[1], dst[2], dst[3]);
        }
    }

    namespace
    {
        // Local memory as it was at the end of the last frame whose late
        // reads (readLocalNow's callers) brought it up to date, from where
        // they read: what readLocalNow answers from. Local memory itself
        // will not do: by the time the EE reads, the GS may well be into
        // the next frame, whose clears PCSX2 writes to local memory too.
        std::mutex s_keptMutex;
        std::vector<u8> s_kept, s_keptBack;
        uint32_t s_keptStart = 0, s_keptBackStart = 0;
        bool s_keptValid = false;
        // The late reads since the last VBlank (GS thread).
        bool s_lateRead = false;
        uint32_t s_lateBp = 0, s_lateBw = 0;

        // Late reads of a 16-bit depth buffer with the hardware renderers:
        // a copy of the whole buffer from video memory, made at the read
        // (in the GS's order, after the draws before it) and taken from the
        // GPU at the VBlank after the next, when it has long been done, so
        // that neither the EE nor the GS thread waits for the GPU. Two
        // copies in turn: one being made while the other is collected.
        struct DepthCopy
        {
            std::unique_ptr<GSDownloadTexture> download;
            uint32_t width = 0, height = 0, bp = 0, bw = 0;
            bool pending = false;
            uint32_t age = 0; // VBlanks since it was made
            GLsync fence = nullptr; // OpenGL: signalled when the GPU has made the copy
#ifdef _WIN32
            wil::com_ptr_nothrow<ID3D11Query> query; // Direct3D 11: the same
#endif
        };
        // A copy the GPU has not finished by then (a slow or busy GPU, often
        // integrated graphics) is left for a later VBlank, the flares keeping
        // the values before, rather than the GS thread waiting for it; after
        // this many VBlanks it is waited for.
        constexpr uint32_t kDepthCopyPatience = 6;
        DepthCopy s_depthCopies[2];
        uint32_t s_depthNext = 0;
        bool s_depthCopiedThisFrame = false;
        // What readLocalNow answers depth reads from: the buffer's values,
        // row by row (guarded by s_keptMutex).
        std::vector<u16> s_depth;
        uint32_t s_depthW = 0, s_depthH = 0, s_depthBp = 0, s_depthBw = 0;
        bool s_depthValid = false;

        bool isZ16(uint32_t psm) { return psm == PSMZ16 || psm == PSMZ16S; }

        // After a copy is recorded: something to ask the GPU about it.
        void markDepthCopy(DepthCopy &c)
        {
            if (s_gl)
            {
                if (c.fence)
                    glDeleteSync(c.fence);
                c.fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
            }
#ifdef _WIN32
            if (GSDevice11 *dev = d3d11Device())
            {
                if (!c.query)
                {
                    const D3D11_QUERY_DESC desc{D3D11_QUERY_EVENT, 0};
                    dev->GetD3DDevice()->CreateQuery(&desc, c.query.put());
                }
                if (c.query)
                    dev->GetD3DContext()->End(c.query.get());
            }
#endif
        }

        // Whether the GPU has made the copy, without waiting for it.
        bool depthCopyDone(const DepthCopy &c)
        {
            if (s_gl && c.fence)
            {
                const auto wait = s_clientWaitSync ? s_clientWaitSync : glad_glClientWaitSync;
                return wait(c.fence, 0, 0) != GL_TIMEOUT_EXPIRED;
            }
#ifdef _WIN32
            if (GSDevice11 *dev = d3d11Device(); dev && c.query)
                return dev->GetD3DContext()->GetData(c.query.get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH) !=
                       S_FALSE;
#endif
            return true;
        }

        void dropDepthCopy(DepthCopy &c)
        {
            if (c.fence && s_gl)
                glDeleteSync(c.fence);
            c = DepthCopy{};
        }

        void collectDepth(DepthCopy &c)
        {
            static const bool trace = std::getenv("GH1_READTRACE") != nullptr;
            static unsigned traced = 0;
            if (trace && traced < 40)
            {
                ++traced;
                std::fprintf(stderr, "[gs read] depth copy collected %u VBlanks after it was made\n", c.age);
            }
            c.pending = false;
            if (!c.download || !c.download->Map(GSVector4i(0, 0, c.width, c.height)))
                return;
            std::vector<u16> image(static_cast<size_t>(c.width) * c.height);
            const u8 *rows = c.download->GetMapPointer();
            const u32 pitch = c.download->GetMapPitch();
            for (uint32_t y = 0; y < c.height; ++y)
                std::memcpy(image.data() + static_cast<size_t>(y) * c.width, rows + static_cast<size_t>(y) * pitch,
                            c.width * sizeof(u16));
            c.download->Unmap();
            std::lock_guard lock(s_keptMutex);
            s_depth.swap(image);
            s_depthW = c.width;
            s_depthH = c.height;
            s_depthBp = c.bp;
            s_depthBw = c.bw;
            s_depthValid = true;
        }
    }

    namespace
    {
        void releaseDepthCopies()
        {
            for (DepthCopy &c : s_depthCopies)
                dropDepthCopy(c);
            s_depthCopiedThisFrame = false;
            std::lock_guard lock(s_keptMutex);
            s_depthValid = false;
        }
    }

    bool copyDepthLate()
    {
        if (!s_open || !g_gs_renderer || !g_gs_device || !g_texture_cache || s_renderer == GSRendererType::SW ||
            s_renderer == GSRendererType::Null)
            return false;
        const GIFRegBITBLTBUF b = g_gs_renderer->m_env.BITBLTBUF;
        if (!isZ16(b.SPSM))
            return false;
        if (s_depthCopiedThisFrame)
            return true;
        // The draws so far reach the GPU first, as before any read.
        g_gs_renderer->Flush(GSState::DOWNLOADFIFO);
        GSTextureCache::Target *t = g_texture_cache->GetExactTarget(b.SBP, b.SBW, GSTextureCache::DepthStencil, b.SBP);
        if (!t || !t->m_texture || !isZ16(t->m_TEX0.PSM))
            return false;
        const int w = std::clamp(t->GetUnscaledWidth(), 1, 2048);
        const int h = std::clamp(t->GetUnscaledHeight(), 1, 2048);
        DepthCopy &c = s_depthCopies[s_depthNext];
        if (c.pending)
        {
            // The GPU still on the copy before: no new one this frame.
            if (!depthCopyDone(c) && c.age < kDepthCopyPatience)
            {
                s_depthCopiedThisFrame = true;
                return true;
            }
            collectDepth(c);
        }
        // As PCSX2's own read of a depth target: to 16-bit values at native
        // size, then into memory the CPU can read.
        GSTexture *tmp = g_gs_device->CreateRenderTarget(w, h, GSTexture::Format::UInt16, false);
        if (!tmp)
            return false;
        const GSVector4i drc(0, 0, w, h);
        const GSVector4 src(GSVector4(drc) * GSVector4(t->m_scale) / GSVector4(t->m_texture->GetSize()).xyxy());
        g_gs_device->StretchRect(t->m_texture, src, tmp, GSVector4(drc), ShaderConvert::DEPTH32_TO_16_BITS, Nearest);
        if (!c.download || c.width != static_cast<uint32_t>(w) || c.height != static_cast<uint32_t>(h))
        {
            c.download = g_gs_device->CreateDownloadTexture(w, h, GSTexture::Format::UInt16);
            c.width = static_cast<uint32_t>(w);
            c.height = static_cast<uint32_t>(h);
        }
        if (!c.download)
        {
            g_gs_device->Recycle(tmp);
            return false;
        }
        c.download->CopyFromTexture(drc, tmp, drc, 0, true);
        markDepthCopy(c);
        g_gs_device->Recycle(tmp);
        c.bp = b.SBP;
        c.bw = b.SBW;
        c.pending = true;
        c.age = 0;
        s_depthNext ^= 1u;
        s_depthCopiedThisFrame = true;
        static const bool trace = std::getenv("GH1_READTRACE") != nullptr;
        static unsigned traced = 0;
        if (trace && traced < 20)
        {
            ++traced;
            std::fprintf(stderr, "[gs read] depth copy of bp %x bw %u: %dx%d at scale %.2f\n", b.SBP, b.SBW, w, h,
                         t->m_scale);
        }
        return true;
    }

    void noteLateRead()
    {
        if (!s_open || !g_gs_renderer)
            return;
        s_lateRead = true;
        s_lateBp = g_gs_renderer->m_env.BITBLTBUF.SBP;
        s_lateBw = g_gs_renderer->m_env.BITBLTBUF.SBW;
    }

    void keepLateReads()
    {
        // The depth copies made a frame ago are done on the GPU by now.
        bool copying = false;
        for (DepthCopy &c : s_depthCopies)
        {
            if (c.pending && c.age >= 1 && (c.age >= kDepthCopyPatience || depthCopyDone(c)))
                collectDepth(c);
            if (c.pending)
            {
                ++c.age;
                copying = true;
            }
        }
        if (!copying && !s_depthCopiedThisFrame)
        {
            std::lock_guard lock(s_keptMutex);
            s_depthValid = false;
        }
        s_depthCopiedThisFrame = false;

        if (!s_lateRead || !s_open || !g_gs_renderer)
        {
            std::lock_guard lock(s_keptMutex);
            s_keptValid = false;
            return;
        }
        s_lateRead = false;
        // From the buffer's first block, 16 rows of pages (512 lines of a
        // 16-bit buffer, or more) of its width.
        const uint32_t start = std::min<uint32_t>(s_lateBp * 256u, GSLocalMemory::m_vmsize - 8192u);
        const uint32_t size = std::min<uint32_t>(std::max(1u, s_lateBw) * 16u * 8192u, GSLocalMemory::m_vmsize - start);
        const u8 *vm = g_gs_renderer->m_mem.vm8();
        s_keptBack.assign(vm + start, vm + start + size);
        s_keptBackStart = start;
        std::lock_guard lock(s_keptMutex);
        std::swap(s_kept, s_keptBack);
        std::swap(s_keptStart, s_keptBackStart);
        s_keptValid = true;
    }

    void readLocalNow(uint8_t *dst, uint32_t bytes, uint64_t bitbltbuf, uint64_t trxpos, uint64_t trxreg)
    {
        if (!s_open || !g_gs_renderer || bytes == 0)
        {
            std::memset(dst, 0, bytes);
            return;
        }
        GIFRegBITBLTBUF b{bitbltbuf};
        GIFRegTRXPOS p{trxpos};
        GIFRegTRXREG r{trxreg};
        const GSLocalMemory::psm_t &psm = GSLocalMemory::m_psm[b.SPSM];
        const uint32_t pixelBytes = psm.trbpp / 8u;
        {
            std::lock_guard lock(s_keptMutex);
            if (s_depthValid && isZ16(b.SPSM) && b.SBP == s_depthBp && b.SBW == s_depthBw && r.RRW != 0)
            {
                // From the depth buffer's last copy, by position.
                for (uint32_t i = 0, n = bytes / 2u; i < n; ++i)
                {
                    const uint32_t x = p.SSAX + i % r.RRW;
                    const uint32_t y = p.SSAY + i / r.RRW;
                    const u16 v = (x < s_depthW && y < s_depthH) ? s_depth[static_cast<size_t>(y) * s_depthW + x] : 0;
                    std::memcpy(dst + i * 2u, &v, 2u);
                }
                return;
            }
            if (s_keptValid && (psm.trbpp == 16 || psm.trbpp == 32) && r.RRW != 0)
            {
                // Pixel by pixel through the format's own swizzle.
                for (uint32_t i = 0, n = bytes / pixelBytes; i < n; ++i)
                {
                    const int x = static_cast<int>(p.SSAX + i % r.RRW);
                    const int y = static_cast<int>(p.SSAY + i / r.RRW);
                    const uint32_t at = psm.info.pa(x, y, b.SBP, b.SBW) * pixelBytes;
                    if (at >= s_keptStart && at - s_keptStart + pixelBytes <= s_kept.size())
                        std::memcpy(dst + i * pixelBytes, s_kept.data() + (at - s_keptStart), pixelBytes);
                    else
                        std::memcpy(dst + i * pixelBytes, g_gs_renderer->m_mem.vm8() + (at % GSLocalMemory::m_vmsize), pixelBytes);
                }
                return;
            }
        }
        // Whole qwords, as the GS sends them.
        std::vector<u8> whole((bytes + 15u) & ~15u);
        int tx = static_cast<int>(p.SSAX);
        int ty = static_cast<int>(p.SSAY);
        g_gs_renderer->m_mem.ReadImageX(tx, ty, whole.data(), static_cast<int>(whole.size()), b, p, r);
        std::memcpy(dst, whole.data(), bytes);
    }

    void resize(uint32_t width, uint32_t height, float scale)
    {
        if (s_open)
            GSResizeDisplayWindow(width, height, scale);
    }

    bool snapshot(std::vector<uint32_t> &rgba, uint32_t &width, uint32_t &height)
    {
        if (!s_open)
            return false;
        u32 w = 0, h = 0;
        if (!GSSaveSnapshotToMemory(0, 0, true, false, &w, &h, &rgba))
            return false;
        width = w;
        height = h;
        return true;
    }
}
