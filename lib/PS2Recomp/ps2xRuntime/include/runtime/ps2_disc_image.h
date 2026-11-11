#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

// A PS2 disc image read in place: .iso, raw .bin, or .chd when built with
// libchdr. Serves 2048-byte user data sectors and ISO 9660 path lookups, so
// cdrom0: files and raw sector reads both come from the same disc.
class DiscImage
{
public:
    static constexpr uint32_t kSectorSize = 2048;

    struct Extent
    {
        uint32_t lbn = 0;
        uint32_t size = 0;
        bool isDir = false;
    };

    // Frames of the underlying file: plain frames or decompressed CHD hunks.
    class FrameSource
    {
    public:
        virtual ~FrameSource() = default;
        virtual bool readFrame(uint32_t frame, uint8_t *dst) = 0;
        // `count` frames in a row; one read where the source allows it.
        virtual bool readFrames(uint32_t first, uint32_t count, uint8_t *dst)
        {
            for (uint32_t i = 0; i < count; ++i)
                if (!readFrame(first + i, dst + static_cast<size_t>(i) * frameSize))
                    return false;
            return true;
        }
        uint32_t frameSize = 0;
        uint32_t frameCount = 0;
    };

    static std::unique_ptr<DiscImage> open(const std::filesystem::path &path, std::string &error);
    ~DiscImage();

    bool readSectors(uint32_t lbn, uint32_t count, uint8_t *dst);
    // Bytes of a file's extent from offset; returns how many were read.
    size_t readExtent(const Extent &extent, uint64_t offset, uint8_t *dst, size_t size);
    // A path on the disc, case-insensitive, with any device prefix ("cdrom0:"),
    // backslashes and ";1" version suffix accepted.
    bool find(const std::string &path, Extent &out);
    uint32_t sectorCount() const { return m_frames->frameCount; }

private:
    DiscImage(std::unique_ptr<FrameSource> frames, uint32_t dataOffset);
    bool readSectorLocked(uint32_t lbn, uint8_t *dst);
    bool readSectorsLocked(uint32_t lbn, uint32_t count, uint8_t *dst);

    std::unique_ptr<FrameSource> m_frames;
    uint32_t m_dataOffset = 0;
    std::mutex m_mutex;
    // Sectors read ahead of a sequential reader, on a thread of their own.
    struct ReadAhead;
    std::unique_ptr<ReadAhead> m_readAhead;
};

// Microseconds callers spent waiting in DiscImage::readSectors since the
// last call (the disc reads a frame waited for), and how many of those
// reads the read-ahead already had.
struct DiscReadStats
{
    uint64_t waitedUs = 0;
    uint32_t reads = 0;
    uint32_t ahead = 0;
};
DiscReadStats ps2TakeDiscReadStats();

// The image named by PS2Runtime::IoPaths::cdImage, opened on first use, or
// null when none is configured or it cannot be opened.
DiscImage *ps2ConfiguredDisc();
