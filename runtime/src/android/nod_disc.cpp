// The disc image the player picked, read through nod (MIT / Apache-2.0) from its file descriptor.
#include "nod_disc.h"

#include <nod.h>

#include <algorithm>
#include <cerrno>
#include <memory>
#include <stdexcept>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace NodDisc {
namespace {

int64_t ReadAt(void* user, uint64_t offset, void* out, size_t length) {
    const int fd = *static_cast<int*>(user);
    size_t done = 0;
    while (done < length) {
        const ssize_t n = pread64(fd, static_cast<char*>(out) + done, length - done, off64_t(offset + done));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return -1;
        if (n == 0) break;
        done += size_t(n);
    }
    return int64_t(done);
}

int64_t Length(void* user) {
    struct stat st {};
    return fstat(*static_cast<int*>(user), &st) == 0 ? int64_t(st.st_size) : -1;
}

void Close(void*) {}  // the descriptor belongs to the caller

std::string NodError(const char* what) {
    const char* message = nod_error_message();
    return std::string(what) + (message != nullptr && *message ? std::string(": ") + message : std::string());
}

struct Handle {
    NodHandle* handle = nullptr;
    ~Handle() { nod_free(handle); }
};

// The disc and its game partition.
struct Opened {
    Handle disc, partition;
    DiscImage::Info info;
};

// A provider that hands over a pipe (a cloud drive's file) can't be read at random places: that is said plainly.
void Open(int& fd, Opened& opened) {
    struct stat st {};
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode))
        throw std::runtime_error("The file can't be read in place (a cloud drive's file?): copy it to the device's storage first.");
    NodDiscStream stream{&fd, &ReadAt, &Length, &Close};
    NodDiscOptions options{};
    options.partition_encryption = NOD_PARTITION_ENCRYPTION_ORIGINAL;
    // Decompression runs ahead of the reads on the device's other cores.
    options.preloader_threads = std::max(1u, std::min(4u, std::thread::hardware_concurrency() / 2));
    if (nod_disc_open_stream(&stream, &options, &opened.disc.handle) != NOD_RESULT_OK)
        throw std::runtime_error(NodError("Not a disc image nod can read"));
    NodDiscHeader header{};
    if (nod_disc_header(opened.disc.handle, &header) != NOD_RESULT_OK) throw std::runtime_error(NodError("The disc header can't be read"));
    opened.info.id.assign(header.game_id, sizeof header.game_id);
    opened.info.revision = header.disc_version;
    size_t titleLength = 0;
    while (titleLength < sizeof header.game_title && header.game_title[titleLength] != '\0') ++titleLength;
    opened.info.title.assign(header.game_title, titleLength);
    if (nod_disc_open_partition_kind(opened.disc.handle, NOD_PARTITION_KIND_DATA, nullptr, &opened.partition.handle) != NOD_RESULT_OK)
        throw std::runtime_error(NodError("The image has no game data partition"));
}

} // namespace

bool Probe(int fd, DiscImage::Info& info, std::string& error) {
    try {
        Opened opened;
        Open(fd, opened);
        info = opened.info;
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

bool Extract(int fd, const std::filesystem::path& outDir, const std::function<void(int, int)>& progress, std::string& error) {
    try {
        Opened opened;
        Open(fd, opened);
        NodHandle* partition = opened.partition.handle;
        return DiscImage::ExtractPartition(
            [partition](int64_t offset, void* out, size_t size) {
                if (nod_seek(partition, offset, 0) != offset) throw std::runtime_error(NodError("Reading the disc failed"));
                auto* dst = static_cast<uint8_t*>(out);
                while (size > 0) {
                    const int64_t n = nod_read(partition, dst, size);
                    if (n <= 0) throw std::runtime_error(NodError("Reading the disc failed"));
                    dst += n;
                    size -= size_t(n);
                }
            },
            outDir, progress, error);
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

} // namespace NodDisc
