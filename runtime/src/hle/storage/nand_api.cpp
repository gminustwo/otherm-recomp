// NAND/ISFS HLE: the synchronous RVL NAND* library.
//
// Shared state and helpers live in nand_internal.h.

#include "nand_internal.h"

#include <atomic>
#include <cerrno>

#ifdef __linux__
#include <linux/fs.h>
#include <sys/syscall.h>
#endif

// ============================================================================
// Local helpers
// ============================================================================

// Unchecked NUL-terminated write into guest memory. The RVL NAND library hands
// out fixed-size caller buffers and never tells us how big they are, so this
// deliberately matches the library's own unbounded copy.
static void WriteGuestNandPath(uint32_t address, const std::string& value) {
    for (size_t i = 0; i < value.size(); ++i) {
        Memory::Write8(address + static_cast<uint32_t>(i), static_cast<uint8_t>(value[i]));
    }
    Memory::Write8(address + static_cast<uint32_t>(value.size()), 0);
}

// fileInfoPtr -> fd -> live handle. Logs and returns null when the guest hands
// us a descriptor that was never opened (or was already closed).
// Bring-up aid: trace every NAND call (the async APIs forward to these synchronous ones).
#define OTHERM_NAND_TRACE(...) RT_LOGF(RT_TAG_NAND, "[trace] " __VA_ARGS__)

static FileHandle* ResolveNandFileHandle(const char* who, uint32_t fileInfoPtr) {
    const int32_t fd = static_cast<int32_t>(Memory::Read32(fileInfoPtr));
    FileHandle* handle = GetHandle(fd);
    if (!handle || !handle->file) {
        LogNandError(who, "invalid fd=%d", fd);
        return nullptr;
    }
    return handle;
}

// ============================================================================
// The synchronous RVL NAND* library
// ============================================================================

static bool RenameNoReplace(const std::filesystem::path& from,
                            const std::filesystem::path& to,
                            std::error_code& error) {
#ifdef _WIN32
    if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH)) {
        error.clear();
        return true;
    }
    error = std::error_code(static_cast<int>(GetLastError()), std::system_category());
    return false;
#elif defined(__linux__)
    const int result = syscall(SYS_renameat2, AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), RENAME_NOREPLACE);
    if (result == 0) {
        error.clear();
        return true;
    }
    error = std::error_code(errno, std::generic_category());
    return false;
#else
    (void)from;
    (void)to;
    error = std::make_error_code(std::errc::operation_not_supported);
    return false;
#endif
}

// Moves `from` to `to` only while `to` does not exist. Uses an atomic no-replace primitive where
// the filesystem has one (link(2) for files, renameat2 RENAME_NOREPLACE); filesystems with
// neither, such as Android's shared storage (no hard links, no rename flags), get a
// check-then-rename, which only a second writer racing for the same name could defeat.
static bool PublishNoReplace(const std::filesystem::path& from, const std::filesystem::path& to,
                             bool isDirectory, std::error_code& error) {
    if (!isDirectory) {
        std::filesystem::create_hard_link(from, to, error);
        if (!error || error == std::errc::file_exists) {
            return !error;
        }
    }
    if (RenameNoReplace(from, to, error) || error == std::errc::file_exists) {
        return !error;
    }
    std::error_code existsEc;
    if (std::filesystem::exists(to, existsEc) || existsEc) {
        error = existsEc ? existsEc : std::make_error_code(std::errc::file_exists);
        return false;
    }
    std::filesystem::rename(from, to, error);
    return !error;
}

extern "C" int32_t NANDInit_HLE(void) {
    // IOS empties /tmp when it starts; the SDK's safe saves write there first and move the file into place.
    // A file left by an interrupted save would make the next NANDCreate of the same name answer EXISTS.
    static bool s_tmpCleared = false;
    if (!s_tmpCleared) {
        s_tmpCleared = true;
        std::error_code ec;
        const std::filesystem::path tmp = TranslateNandPath("/tmp");
        uintmax_t removed = 0;
        for (std::filesystem::directory_iterator it(tmp, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code removeEc;
            removed += std::filesystem::remove_all(it->path(), removeEc);
        }
        if (removed != 0) {
            LogNandWarning("NANDInit", "emptied /tmp (%ju entries left by an earlier run)", removed);
        }
    }

    // Initialize ISFS
    ISFS_OpenLib_Initialize(&GetPersistentCpuContext());

    // NAND home directory buffer: s_homeDir (0x802E1D20, 0x40 bytes; nandGetHomeDir 0x801070E0 returns it)
    WriteGuestNandPath(0x802E1D20, CurrentNandDataDir());
    // ...and the current directory, as Other M's NANDInit (0x80106A20) does after _ES_GetDataDir: strcpy(0x8022F7C0,
    // s_homeDir) at 0x80106AD8. nandGenerateAbsPath (0x80106950) resolves relative paths against 0x8022F7C0, whose
    // initial value is "/": without this the translated NANDReadDir("share") listed "/share" (ENOENT), the game saw
    // no save files after every write and rewrote share/save0.dat every 4 frames (boot 41).
    WriteGuestNandPath(0x8022F7C0, CurrentNandDataDir());

    // Mark NAND as initialized: s_libState = 2 (0x80332688)
    Memory::Write32(0x80332688, 2);

    return NAND_RESULT_OK;
}
PPC_NATIVE_OVERRIDE(80106A20, NANDInit_HLE, int32_t, (void), ());

extern "C" int32_t NANDCheck_HLE(uint32_t blockSize, uint32_t blockCount, uint32_t outResults) {
    const int32_t result = NandCheckContract::WriteHealthyResult(
        outResults,
        [](uint32_t address, size_t length) { return Memory::Contains(address, length); },
        [](uint32_t address, uint32_t value) {
            try {
                Memory::Write32(address, value);
                return true;
            } catch (const Memory::AccessViolation&) {
                return false;
            }
        });
    if (result != NAND_RESULT_OK) {
        LogNandError("NANDCheck",
                     "FAILED: invalid four-byte result buffer 0x%08X (blockSize=%u blockCount=%u)",
                     outResults, blockSize, blockCount);
    }
    return result;
}
// Other M has no NANDCheck at 0x80107290: that is NANDGetAvailableArea(u32* fsBlock, u32* inode) (the
// signature match "NAND::Check" was a collision). It takes ISFS_GetUsage of the home directory, nandCalcUsage over
// the whole NAND, and writes min(0x400 - home blocks, 0x4400 - NAND blocks) and min(0x20 - home inodes,
// 0xFA0 - NAND inodes) (disassembly 0x801072C0..0x8010738C). The game's save check (0x805C8D7C via 0x8017500C)
// passes two pointers and no third argument, so the NANDCheck HLE left both counts 0 and wrote through a stale r5:
// the game then reported "There is not enough available space in Wii system memory" (boot 27).

// NANDGetAvailableArea from the host save folder: 16 KiB file-system blocks and one inode per file or directory
// below the home directory (the home directory's own inode is not counted, as in the guest code). The whole-NAND
// limit (0x4400 blocks / 0xFA0 inodes) never binds on the host and is not modelled.
extern "C" int32_t NANDGetAvailableArea_HLE(uint32_t fsBlockPtr, uint32_t inodePtr) {
    constexpr uint64_t kHomeBlockLimit = 0x400, kHomeInodeLimit = 0x20, kFsBlockBytes = 0x4000;
    uint64_t blocks = 0, inodes = 0;
    std::error_code ec;
    const std::filesystem::path home = TranslateNandPath(CurrentNandDataDir().c_str());
    for (std::filesystem::recursive_directory_iterator it(home, std::filesystem::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        ++inodes;
        std::error_code sizeEc;
        if (it->is_regular_file(sizeEc)) {
            const uint64_t size = it->file_size(sizeEc);
            if (!sizeEc) blocks += (size + kFsBlockBytes - 1) / kFsBlockBytes;
        }
    }
    const uint32_t freeBlocks = static_cast<uint32_t>(blocks >= kHomeBlockLimit ? 0 : kHomeBlockLimit - blocks);
    const uint32_t freeInodes = static_cast<uint32_t>(inodes >= kHomeInodeLimit ? 0 : kHomeInodeLimit - inodes);
    try {
        if (fsBlockPtr) Memory::Write32(fsBlockPtr, freeBlocks);
        if (inodePtr) Memory::Write32(inodePtr, freeInodes);
    } catch (const Memory::AccessViolation&) {
        LogNandError("NANDGetAvailableArea", "invalid output pointers 0x%08X 0x%08X", fsBlockPtr, inodePtr);
        return NAND_RESULT_INVALID;
    }
    OTHERM_NAND_TRACE("NANDGetAvailableArea -> %u blocks, %u inodes free (home uses %llu blocks, %llu inodes)\n",
                     freeBlocks, freeInodes, static_cast<unsigned long long>(blocks), static_cast<unsigned long long>(inodes));
    return NAND_RESULT_OK;
}
PPC_NATIVE_OVERRIDE(80107290, NANDGetAvailableArea_HLE, int32_t, (uint32_t fsBlockPtr, uint32_t inodePtr), (fsBlockPtr, inodePtr));

extern "C" int32_t NANDOpen_HLE(uint32_t pathPtr, uint32_t fileInfoPtr, uint32_t mode) {
    const char* path = pathPtr ? (const char*)Memory::GetPointer(pathPtr) : nullptr;
    if (path) OTHERM_NAND_TRACE("NANDOpen '%s' mode %u\n", path, mode);
    if (!path || !fileInfoPtr) {
        LogNandError("NANDOpen", "invalid params: path=%p fileInfo=0x%08X", path, fileInfoPtr);
        return NAND_RESULT_INVALID;
    }

    const std::filesystem::path hostPath = TranslateNandPath(path);

    if (const auto result = NandCheckSystemSaveRead("NANDOpen", hostPath, mode))
        return *result;

    // Existing-file write opens go through a shadow copy seeded from the original, so a
    // crash between NANDWrite and NANDClose cannot leave a torn file (the game patches
    // sub-ranges, e.g. ghost saves at a non-zero offset). New files still create in place.
    if ((mode == 2 || mode == 3) && PathExists(hostPath) && !IsDirectory(hostPath)) {
        if (IsHostPathOpen(hostPath)) {
            // Another live handle already refers to this file. A shadow would hide the
            // writes from that handle, so stay in place for this open.
            LogNandWarning("NANDOpen", "WARNING: '%s' already has a live handle, writing in place",
                           HostPathText(hostPath).c_str());
        } else {
            const std::filesystem::path tempPath = SafeTempPathFor(hostPath);
            if (DiscardStaleSafeTemp(tempPath)) {
                std::error_code ec;
                std::filesystem::copy_file(hostPath, tempPath,
                                           std::filesystem::copy_options::overwrite_existing, ec);
                if (ec) {
                    LogNandWarning("NANDOpen", "WARNING: could not seed shadow '%s' (%s), writing in place",
                                   HostPathText(tempPath).c_str(), ec.message().c_str());
                    NandRemove(tempPath);
                } else {
                    FILE* shadow = NandFopen(tempPath, "r+b");
                    if (!shadow) {
                        LogNandWarning("NANDOpen", "WARNING: could not open shadow '%s', writing in place",
                                       HostPathText(tempPath).c_str());
                        NandRemove(tempPath);
                    } else {
                        const int32_t shadowFd = AllocateFd(tempPath, shadow, static_cast<int32_t>(mode));
                        {
                            std::lock_guard<std::mutex> lock(g_fdMutex);
                            auto it = g_fileHandles.find(shadowFd);
                            if (it != g_fileHandles.end()) {
                                it->second.safeCommitPath = hostPath;
                            }
                        }
                        Memory::Write32(fileInfoPtr, static_cast<uint32_t>(shadowFd));
                        Memory::Write8(fileInfoPtr + 0x8a, NAND_OPEN_FLAG_OPEN);
                        return NAND_RESULT_OK;
                    }
                }
            }
        }
    }

    const char* fopenMode = "rb";
    if (mode == 1) fopenMode = "rb";
    else if (mode == 2) fopenMode = "r+b";
    else if (mode == 3) fopenMode = "r+b";
    
    FILE* file = NandFopen(hostPath, fopenMode);
    if (!file && mode >= 2) {
        // Try creating for write modes
        file = NandFopen(hostPath, "w+b");
    }
    
    // Create parent directories and retry
    if (!file && CreateParentDirectories(hostPath)) {
        file = NandFopen(hostPath, mode >= 2 ? "w+b" : "rb");
    }

    if (!file) {
        if (IsFaceLibResourcePath(path) && SeedFaceLibResource(hostPath)) {
            file = NandFopen(hostPath, fopenMode);
        }
        if (!file) {
            LogNandError("NANDOpen", "FAILED to open '%s' (host '%s', mode %u)", path,
                         HostPathText(hostPath).c_str(), mode);
            return NAND_RESULT_NOEXISTS;
        }
    }

    int32_t fd = AllocateFd(hostPath, file, mode);
    OTHERM_NAND_TRACE("NANDOpen -> fd %d (fileInfo 0x%08X)\n", fd, fileInfoPtr);
    Memory::Write32(fileInfoPtr, static_cast<uint32_t>(fd));
    Memory::Write8(fileInfoPtr + 0x8a, NAND_OPEN_FLAG_OPEN);
    return NAND_RESULT_OK;
}
PPC_NATIVE_OVERRIDE(80105F00, NANDOpen_HLE, int32_t, (uint32_t pathPtr, uint32_t fileInfoPtr, uint32_t mode), (pathPtr, fileInfoPtr, mode));

extern "C" int32_t NANDClose_HLE(uint32_t fileInfoPtr) {
    OTHERM_NAND_TRACE("NANDClose fileInfo=0x%08X fd=%d\n", fileInfoPtr,
                     fileInfoPtr ? static_cast<int32_t>(Memory::Read32(fileInfoPtr)) : -1);
    if (!fileInfoPtr) {
        return NAND_RESULT_INVALID;
    }
    
    uint8_t openFlag = Memory::Read8(fileInfoPtr + 0x8a);
    if (openFlag == NAND_OPEN_FLAG_SAFE_OPEN || openFlag == NAND_OPEN_FLAG_SAFE_OPEN_ASYNC) {
        // Console behaviour (nandClose @ 0x801D9850): only openFlag==1 is accepted, a safe
        // handle falls through to -8. It is neither closed nor committed here.
        LogNandWarning("NANDClose", "WARNING: refusing safe-open handle (flag=%u), NANDSafeClose is required", openFlag);
        return NAND_RESULT_INVALID;
    }
    if (openFlag != NAND_OPEN_FLAG_OPEN) {
        LogNandWarning("NANDClose", "file not open (flag=%u)", openFlag);
        return NAND_RESULT_INVALID;
    }

    int32_t fd = static_cast<int32_t>(Memory::Read32(fileInfoPtr));

    // Flush, fsync and (for shadowed write handles) atomically publish. A failure here is
    // reported to the guest instead of being swallowed, so NandUtil_close surfaces it as a
    // save error rather than a silent success on top of a half-written file.
    const int32_t result = CommitAndCloseFd("NANDClose", fd, /*missingFdIsError=*/false);
    if (result != NAND_RESULT_OK) {
        return result;
    }

    Memory::Write8(fileInfoPtr + 0x8a, NAND_OPEN_FLAG_CLOSED); // Mark as closed
    return NAND_RESULT_OK;
}
PPC_NATIVE_OVERRIDE(801061A0, NANDClose_HLE, int32_t, (uint32_t fileInfoPtr), (fileInfoPtr));

extern "C" int32_t NANDRead_HLE(uint32_t fileInfoPtr, uint32_t bufferPtr, uint32_t length) {
    OTHERM_NAND_TRACE("NANDRead fileInfo=0x%08X fd=%d len=%u\n", fileInfoPtr,
                     fileInfoPtr ? static_cast<int32_t>(Memory::Read32(fileInfoPtr)) : -1, length);
    if (!fileInfoPtr) {
        return NAND_RESULT_INVALID;
    }

    FileHandle* handle = ResolveNandFileHandle("NANDRead", fileInfoPtr);
    if (!handle) {
        return NAND_RESULT_INVALID;
    }

    uint8_t* buffer = (uint8_t*)Memory::GetPointer(bufferPtr);
    if (!buffer) {
        return NAND_RESULT_INVALID;
    }

    size_t bytesRead = std::fread(buffer, 1, length, handle->file);
    return static_cast<int32_t>(bytesRead);
}
PPC_NATIVE_OVERRIDE(80104DD0, NANDRead_HLE, int32_t, (uint32_t fileInfoPtr, uint32_t bufferPtr, uint32_t length), (fileInfoPtr, bufferPtr, length));

extern "C" int32_t NANDWrite_HLE(uint32_t fileInfoPtr, uint32_t bufferPtr, uint32_t length) {
    OTHERM_NAND_TRACE("NANDWrite fileInfo=0x%08X fd=%d len=%u\n", fileInfoPtr,
                     fileInfoPtr ? static_cast<int32_t>(Memory::Read32(fileInfoPtr)) : -1, length);
    if (!fileInfoPtr) {
        return NAND_RESULT_INVALID;
    }

    FileHandle* handle = ResolveNandFileHandle("NANDWrite", fileInfoPtr);
    if (!handle) {
        return NAND_RESULT_INVALID;
    }

    const uint8_t* buffer = (const uint8_t*)Memory::GetPointer(bufferPtr);
    if (!buffer) {
        return NAND_RESULT_INVALID;
    }

    size_t bytesWritten = std::fwrite(buffer, 1, length, handle->file);
    std::fflush(handle->file);
    return static_cast<int32_t>(bytesWritten);
}
PPC_NATIVE_OVERRIDE(80104EC0, NANDWrite_HLE, int32_t, (uint32_t fileInfoPtr, uint32_t bufferPtr, uint32_t length), (fileInfoPtr, bufferPtr, length));

extern "C" int32_t NANDSeek_HLE(uint32_t fileInfoPtr, int32_t offset, int32_t whence) {
    if (!fileInfoPtr) {
        return NAND_RESULT_INVALID;
    }

    FileHandle* handle = ResolveNandFileHandle("NANDSeek", fileInfoPtr);
    if (!handle) {
        return NAND_RESULT_INVALID;
    }

    if (std::fseek(handle->file, offset, NandSeekOrigin(whence)) != 0) {
        return NAND_RESULT_UNKNOWN;
    }

    return static_cast<int32_t>(std::ftell(handle->file));
}

extern "C" int32_t NANDGetLength_HLE(uint32_t fileInfoPtr, uint32_t outLengthPtr) {
    if (!fileInfoPtr || !outLengthPtr) {
        return NAND_RESULT_INVALID;
    }

    FileHandle* handle = ResolveNandFileHandle("NANDGetLength", fileInfoPtr);
    if (!handle) {
        return NAND_RESULT_INVALID;
    }

    const NandFileExtent extent = NandProbeFileExtent(handle->file);
    Memory::Write32(outLengthPtr, static_cast<uint32_t>(extent.size));
    return NAND_RESULT_OK;
}

extern "C" int32_t NANDCreate_HLE(uint32_t pathPtr, uint32_t perm, uint32_t attr) {
    const char* path = pathPtr ? (const char*)Memory::GetPointer(pathPtr) : nullptr;
    if (path) OTHERM_NAND_TRACE("NANDCreate '%s' perm 0x%X attr 0x%X\n", path, perm, attr);
    if (!path) {
        return NAND_RESULT_INVALID;
    }
    
    const std::filesystem::path hostPath = TranslateNandPath(path);
    CreateParentDirectories(hostPath);

    // Check if file already exists
    if (PathExists(hostPath)) {
        return NAND_RESULT_EXISTS;
    }
    
    // Create empty file
    FILE* f = NandFopen(hostPath, "wb");
    if (!f) {
        return NAND_RESULT_UNKNOWN;
    }
    std::fclose(f);
    
    return NAND_RESULT_OK;
}

extern "C" int32_t NANDDelete_HLE(uint32_t pathPtr) {
    const char* path = pathPtr ? (const char*)Memory::GetPointer(pathPtr) : nullptr;
    if (path) OTHERM_NAND_TRACE("NANDDelete '%s'\n", path);
    if (!path) {
        return NAND_RESULT_INVALID;
    }

    const std::filesystem::path hostPath = TranslateNandPath(path);

    if (!PathExists(hostPath)) {
        return NAND_RESULT_NOEXISTS;
    }
    
    if (NandRemove(hostPath)) {
        return NAND_RESULT_OK;
    }
    
    return NAND_RESULT_UNKNOWN;
}
PPC_NATIVE_OVERRIDE(80104B90, NANDDelete_HLE, int32_t, (uint32_t pathPtr), (pathPtr));

extern "C" int32_t NANDCreateDir_HLE(uint32_t pathPtr, uint32_t perm, uint32_t attr) {
    const char* path = pathPtr ? (const char*)Memory::GetPointer(pathPtr) : nullptr;
    if (path) OTHERM_NAND_TRACE("NANDCreateDir '%s'\n", path);
    if (!path) {
        return NAND_RESULT_INVALID;
    }

    const std::filesystem::path hostPath = TranslateNandPath(path);

    if (PathExists(hostPath)) {
        if (IsDirectory(hostPath)) {
            return NAND_RESULT_OK; // Already exists
        }
        return NAND_RESULT_EXISTS;
    }
    
    if (CreateDirectoryPath(hostPath)) {
        return NAND_RESULT_OK;
    }
    
    return NAND_RESULT_UNKNOWN;
}
// Other M: NANDCreateDir (sync, "NAND::CreateDir" by signature; wrapper 0x80174EEC)
PPC_NATIVE_OVERRIDE(80105280, NANDCreateDir_HLE, int32_t, (uint32_t pathPtr, uint32_t perm, uint32_t attr), (pathPtr, perm, attr));

extern "C" int32_t NANDMove_HLE(uint32_t srcPathPtr, uint32_t dstPathPtr) {
    // A cross-mount move is implemented as several host operations. Keep two
    // guest moves from interleaving those operations and corrupting recovery.
    static std::mutex moveMutex;
    std::lock_guard<std::mutex> lock(moveMutex);

    const char* srcPath = srcPathPtr ? (const char*)Memory::GetPointer(srcPathPtr) : nullptr;
    const char* dstPath = dstPathPtr ? (const char*)Memory::GetPointer(dstPathPtr) : nullptr;
    
    if (!srcPath || !dstPath) {
        return NAND_RESULT_INVALID;
    }
    
    const std::filesystem::path srcHost = TranslateNandPath(srcPath);
    const std::filesystem::path dstDirectoryHost = TranslateNandPath(dstPath);
    const std::filesystem::path srcName = srcHost.filename();
    if (srcName.empty()) {
        return NAND_RESULT_INVALID;
    }

    // RVL SDK nandMove always appends the source entry's relative name to the
    // second argument. The latter is a directory, not a complete destination
    // filename (for example /tmp/banner.bin -> <title home>/banner.bin).
    const std::filesystem::path dstHost = dstDirectoryHost / srcName;

    if (!PathExists(srcHost)) {
        return NAND_RESULT_NOEXISTS;
    }
    if (!IsDirectory(dstDirectoryHost)) {
        return NAND_RESULT_NOEXISTS;
    }
    // IOS FS rename replaces an existing destination of the same kind, and the SDK's safe-save pattern depends on it
    // (write /tmp/<name>, then move it over the old copy; Dolphin's FS backend does the same). Other M rewrites
    // share/system.bin that way on every boot: refusing gave NAND_RESULT_EXISTS (-6) in its NAND result word from the
    // second boot on (boot 29). A file replaces a file here (std::filesystem::rename replaces atomically on Windows
    // and POSIX); a directory or a kind mismatch still answers EXISTS, as before (IOS behaviour there not checked).
    if (PathExists(dstHost) && (IsDirectory(dstHost) || IsDirectory(srcHost))) {
        return NAND_RESULT_EXISTS;
    }
    OTHERM_NAND_TRACE("NANDMove '%s' -> '%s'%s\n", srcPath, dstPath, PathExists(dstHost) ? " (replaces the existing file)" : "");

    std::error_code ec;
    std::filesystem::rename(srcHost, dstHost, ec);
    if (!ec) {
        return NAND_RESULT_OK;
    }

    // Flatpak can expose the managed NAND and an external Riivolution save
    // directory as separate mounts. Linux cannot rename across mounts, but
    // nandMove must still work for files such as banner.bin. Preserve the
    // operation's semantics with a copy followed by source removal.
    if (ec == std::errc::cross_device_link) {
        static std::atomic<uint64_t> moveSequence{0};
#ifdef _WIN32
        const auto processId = GetCurrentProcessId();
#else
        const auto processId = getpid();
#endif
        std::filesystem::path scratchHost;
        std::error_code scratchEc;
        for (unsigned attempt = 0; attempt < 128; ++attempt) {
            const auto name = ".nandmove-" + std::to_string(processId) + "-" +
                              std::to_string(moveSequence.fetch_add(1)) + "-" +
                              std::to_string(attempt);
            const auto candidate = dstDirectoryHost / name;
            scratchEc.clear();
            if (std::filesystem::create_directory(candidate, scratchEc)) {
                scratchHost = candidate;
                break;
            }
            if (scratchEc && scratchEc != std::errc::file_exists) {
                LogNandError("NANDMove", "failed to claim temporary directory '%s': %s",
                             HostPathText(candidate).c_str(), scratchEc.message().c_str());
                return NAND_RESULT_UNKNOWN;
            }
        }
        if (scratchHost.empty()) {
            LogNandError("NANDMove", "could not claim a unique temporary directory");
            return NAND_RESULT_UNKNOWN;
        }

        const bool sourceIsDirectory = IsDirectory(srcHost);
        const std::filesystem::path tempHost = scratchHost / srcName;
        const auto cleanupScratch = [&]() {
            std::error_code cleanupEc;
            std::filesystem::remove_all(scratchHost, cleanupEc);
            if (cleanupEc) {
                LogNandError("NANDMove", "failed to clean up temporary directory '%s': %s",
                             HostPathText(scratchHost).c_str(), cleanupEc.message().c_str());
            }
        };

        std::error_code copyEc;
        if (sourceIsDirectory) {
            std::filesystem::copy(srcHost, tempHost,
                                  std::filesystem::copy_options::recursive, copyEc);
        } else {
            std::filesystem::copy_file(srcHost, tempHost, copyEc);
        }

        if (copyEc) {
            LogNandError("NANDMove", "cross-mount copy failed: %s", copyEc.message().c_str());
            cleanupScratch();
            return NAND_RESULT_UNKNOWN;
        }

        // Both paths are already on the target filesystem, so this is a no-replace publication
        // (rename(2) on POSIX would replace an existing destination).
        std::error_code publishEc;
        PublishNoReplace(tempHost, dstHost, sourceIsDirectory, publishEc);
        if (publishEc) {
            LogNandError("NANDMove", "failed to publish cross-mount copy: %s",
                         publishEc.message().c_str());
            cleanupScratch();
            return NAND_RESULT_UNKNOWN;
        }
        cleanupScratch();

        std::error_code removeEc;
        std::filesystem::remove_all(srcHost, removeEc);
        if (!removeEc) {
            LogNandWarning("NANDMove", "used copy/remove fallback across mounts");
            return NAND_RESULT_OK;
        }

        // Keep the source as the authoritative copy when cleanup fails. The
        // destination was published atomically on its own mount; regular files
        // are rolled back below, while directories keep the complete copy when
        // their source removal was only partial. Cross-mount moves cannot
        // provide crash-atomicity, so this is best effort.
        LogNandError("NANDMove", "copy succeeded but source removal failed: %s",
                     removeEc.message().c_str());
        if (sourceIsDirectory) {
            // remove_all may have removed only part of a directory tree. Keep
            // the complete published copy rather than rolling it back to a
            // partially deleted source.
            LogNandWarning("NANDMove", "preserving published directory copy after partial source removal");
        } else {
            std::error_code rollbackEc;
            std::filesystem::remove_all(dstHost, rollbackEc);
            if (rollbackEc) {
                LogNandError("NANDMove", "failed to roll back destination '%s': %s",
                             HostPathText(dstHost).c_str(), rollbackEc.message().c_str());
            }
        }
        return NAND_RESULT_UNKNOWN;
    }

    LogNandError("NANDMove", "FAILED error=%d message='%s'", ec.value(), ec.message().c_str());
    return NAND_RESULT_UNKNOWN;
}
// Other M has no synchronous NANDMove at 0x80105530: it is NANDMoveAsync (nand_async.cpp), see there.

extern "C" int32_t NANDGetStatus_HLE(uint32_t pathPtr, uint32_t outStatusPtr) {
    const char* path = pathPtr ? (const char*)Memory::GetPointer(pathPtr) : nullptr;
    if (path) OTHERM_NAND_TRACE("NANDGetStatus '%s'\n", path);
    if (!path || !outStatusPtr) {
        return NAND_RESULT_INVALID;
    }

    const std::filesystem::path hostPath = TranslateNandPath(path);

    if (!PathExists(hostPath)) {
        return NAND_RESULT_NOEXISTS;
    }
    
    // NANDStatus structure - fill with fake values
    // This is typically used to check permissions and file type
    Memory::Write32(outStatusPtr, 0);      // Magic/type
    Memory::Write32(outStatusPtr + 4, 0);  // Permissions  
    
    return NAND_RESULT_OK;
}

extern "C" int32_t NANDGetType_HLE(uint32_t pathPtr, uint32_t outTypePtr) {
    const char* path = pathPtr ? (const char*)Memory::GetPointer(pathPtr) : nullptr;
    if (path) OTHERM_NAND_TRACE("NANDGetType '%s'\n", path);
    if (!path || !outTypePtr) {
        return NAND_RESULT_INVALID;
    }
    
    const std::filesystem::path hostPath = TranslateNandPath(path);
    
    if (!PathExists(hostPath)) {
        return NAND_RESULT_NOEXISTS;
    }
    
    // 1 = file, 2 = directory
    uint8_t type = IsDirectory(hostPath) ? 2 : 1;
    Memory::Write8(outTypePtr, type);
    return NAND_RESULT_OK;
}
// Other M: NANDGetType (sync, "NAND::GetType" by signature; wrapper 0x801750A4)
PPC_NATIVE_OVERRIDE(80106FC0, NANDGetType_HLE, int32_t, (uint32_t pathPtr, uint32_t outTypePtr), (pathPtr, outTypePtr));

// ============================================================================
// contentFastOpenNAND/contentReadNAND/contentCloseNAND: NAND-installed content (WADs) never
// exists here, everything comes from the DVD image, so refusing is the correct SDK answer.
// ============================================================================

