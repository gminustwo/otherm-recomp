// Discord Rich Presence (discord_presence.h).
//
// Discord's desktop app takes commands from programs on the same PC through a local socket: the named pipe
// \\.\pipe\discord-ipc-<0..9> on Windows, the Unix socket discord-ipc-<0..9> in $XDG_RUNTIME_DIR, $TMPDIR, $TMP, $TEMP or
// /tmp elsewhere (the Flatpak and Snap builds of Discord put theirs in app/com.discordapp.Discord/ and snap.discord/
// there). A message is an 8-byte header, the opcode and then the JSON's length (both 32-bit little-endian), then the
// JSON. The program sends a handshake with its application ID, waits for the READY event, then sends SET_ACTIVITY
// commands; Discord answers each command and may send PING (answered with PONG). The status goes away when the
// connection closes, so a process that ends (quit, restart, crash) leaves nothing behind.
//
// All of it runs on one worker thread, with a timeout on every read and write, so a Discord that is busy, hung or not
// running never holds up the game. The thread is detached and its state is never freed: the process ends through
// TerminateProcess / _Exit (aurora_events.h), and a joinable std::thread at exit would call std::terminate.
// Log lines: [discord]. Nothing about the Discord account is read or logged (READY carries the user's name).
#include "discord_presence.h"

#if !defined(__ANDROID__)

#include "runtime_config.h"
#include "runtime_log.h"

#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace DiscordPresence {
namespace {

using PresenceClock = std::chrono::steady_clock;
using std::chrono::milliseconds;

constexpr uint32_t kOpHandshake = 0, kOpFrame = 1, kOpClose = 2, kOpPing = 3, kOpPong = 4;
constexpr milliseconds kRetryEvery{20000};    // no Discord, or the connection dropped: look again this often
constexpr milliseconds kReadyTimeout{5000};   // the handshake's READY
constexpr milliseconds kIoTimeout{250};       // one read or write; also the worker's tick while connected
constexpr milliseconds kUpdateSpacing{4000};  // SET_ACTIVITY: Discord takes 5 in 20 seconds
constexpr uint32_t kMaxMessage = 64u * 1024u;
constexpr size_t kMaxText = 128;              // Discord's limit for details and state
constexpr int kMaxStatusLines = 200;          // "[discord] status" lines in one session
constexpr int kMaxRefusedLines = 20;

struct Shared {
    std::mutex mutex;
    std::condition_variable wake;
    bool started = false;
    bool enabled = true;
    uint64_t toggles = 0;  // SetEnabled calls: wakes the worker out of a wait
    uint64_t version = 0;  // the text below changed
    std::string details, state;
    int64_t startUnix = 0;
};

Shared& State() {
    static Shared* shared = new Shared();  // never freed: the worker may still run while the process ends
    return *shared;
}

struct Endpoint {
    std::string path;
    std::string label;  // for the log: the name without the folder (which can be inside the home folder)
};

std::vector<Endpoint> Endpoints() {
    std::vector<Endpoint> out;
#if defined(_WIN32)
    for (int i = 0; i < 10; ++i)
        out.push_back({"\\\\.\\pipe\\discord-ipc-" + std::to_string(i), "discord-ipc-" + std::to_string(i)});
#else
    std::vector<std::string> roots;
    for (const char* name : {"XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP"})
        if (const char* value = std::getenv(name); value != nullptr && *value != '\0') roots.emplace_back(value);
    roots.emplace_back("/tmp");
    for (std::string root : roots) {
        while (root.size() > 1 && root.back() == '/') root.pop_back();
        for (const char* sub : {"", "app/com.discordapp.Discord/", "app/com.discordapp.DiscordCanary/",
                                "app/com.discordapp.DiscordPTB/", "snap.discord/", "snap.discord-canary/"}) {
            for (int i = 0; i < 10; ++i) {
                const std::string name = std::string(sub) + "discord-ipc-" + std::to_string(i);
                Endpoint endpoint{root + "/" + name, name};
                bool known = false;
                for (const Endpoint& other : out) known = known || other.path == endpoint.path;
                if (!known) out.push_back(std::move(endpoint));
            }
        }
    }
#endif
    return out;
}

uint32_t ProcessId() {
#if defined(_WIN32)
    return static_cast<uint32_t>(::GetCurrentProcessId());
#else
    return static_cast<uint32_t>(::getpid());
#endif
}

// The connection. Read: the bytes read (> 0), 0 when nothing came within the timeout, -1 when it is gone.
class IpcConnection {
public:
    IpcConnection() = default;
    IpcConnection(const IpcConnection&) = delete;
    IpcConnection& operator=(const IpcConnection&) = delete;
    ~IpcConnection() { Close(); }

#if defined(_WIN32)
    bool Open(const std::string& path) {
        Close();
        handle_ = ::CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) return false;
        event_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (event_ == nullptr) Close();
        return IsOpen();
    }
    bool IsOpen() const { return handle_ != INVALID_HANDLE_VALUE; }
    void Close() {
        if (handle_ != INVALID_HANDLE_VALUE) ::CloseHandle(handle_);
        if (event_ != nullptr) ::CloseHandle(event_);
        handle_ = INVALID_HANDLE_VALUE;
        event_ = nullptr;
    }
    int Read(uint8_t* data, size_t size, milliseconds timeout) { return Transfer(false, data, size, timeout); }
    bool Write(const uint8_t* data, size_t size, milliseconds timeout) {
        const auto deadline = PresenceClock::now() + timeout;
        while (size != 0) {
            const auto left = std::chrono::duration_cast<milliseconds>(deadline - PresenceClock::now());
            if (left.count() <= 0) return false;
            const int done = Transfer(true, const_cast<uint8_t*>(data), size, left);
            if (done <= 0) return false;
            data += done;
            size -= static_cast<size_t>(done);
        }
        return true;
    }

private:
    // One overlapped read or write, waited for up to `timeout` and canceled after it (waiting for the cancel, since
    // the OVERLAPPED is on this stack).
    int Transfer(bool write, uint8_t* data, size_t size, milliseconds timeout) {
        if (!IsOpen()) return -1;
        OVERLAPPED op{};
        ::ResetEvent(event_);
        op.hEvent = event_;
        DWORD done = 0;
        const DWORD length = static_cast<DWORD>(size > 0x10000u ? 0x10000u : size);
        const BOOL ok = write ? ::WriteFile(handle_, data, length, &done, &op) : ::ReadFile(handle_, data, length, &done, &op);
        if (!ok) {
            if (::GetLastError() != ERROR_IO_PENDING) return -1;
            if (::WaitForSingleObject(event_, static_cast<DWORD>(timeout.count())) != WAIT_OBJECT_0) {
                ::CancelIoEx(handle_, &op);
                if (!::GetOverlappedResult(handle_, &op, &done, TRUE))
                    return ::GetLastError() == ERROR_OPERATION_ABORTED ? 0 : -1;
            } else if (!::GetOverlappedResult(handle_, &op, &done, FALSE)) {
                return -1;
            }
        }
        return done > 0 ? static_cast<int>(done) : -1;
    }

    HANDLE handle_ = INVALID_HANDLE_VALUE;
    HANDLE event_ = nullptr;
#else
    bool Open(const std::string& path) {
        Close();
        sockaddr_un address{};
        if (path.size() >= sizeof(address.sun_path)) return false;
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
        fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd_ < 0) return false;
        ::fcntl(fd_, F_SETFD, FD_CLOEXEC);
        const int flags = ::fcntl(fd_, F_GETFL, 0);
        if (flags < 0 || ::fcntl(fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
            Close();
            return false;
        }
#if defined(SO_NOSIGPIPE)
        int one = 1;
        ::setsockopt(fd_, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        // A Unix socket connects at once or fails at once (a full backlog fails with EAGAIN: tried again later).
        if (::connect(fd_, reinterpret_cast<const sockaddr*>(&address), sizeof address) != 0) {
            Close();
            return false;
        }
        return true;
    }
    bool IsOpen() const { return fd_ >= 0; }
    void Close() {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }
    int Read(uint8_t* data, size_t size, milliseconds timeout) {
        if (!IsOpen()) return -1;
        pollfd descriptor{fd_, POLLIN, 0};
        const int ready = ::poll(&descriptor, 1, static_cast<int>(timeout.count()));
        if (ready == 0 || (ready < 0 && errno == EINTR)) return 0;
        if (ready < 0) return -1;
        const ssize_t count = ::recv(fd_, data, size, 0);
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return 0;
        return count > 0 ? static_cast<int>(count) : -1;
    }
    bool Write(const uint8_t* data, size_t size, milliseconds timeout) {
        if (!IsOpen()) return false;
        const auto deadline = PresenceClock::now() + timeout;
        while (size != 0) {
            const auto left = std::chrono::duration_cast<milliseconds>(deadline - PresenceClock::now());
            if (left.count() <= 0) return false;
            pollfd descriptor{fd_, POLLOUT, 0};
            const int ready = ::poll(&descriptor, 1, static_cast<int>(left.count()));
            if (ready < 0 && errno == EINTR) continue;
            if (ready <= 0) return false;
#if defined(MSG_NOSIGNAL)
            const ssize_t count = ::send(fd_, data, size, MSG_NOSIGNAL);
#else
            const ssize_t count = ::send(fd_, data, size, 0);
#endif
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
            if (count <= 0) return false;
            data += count;
            size -= static_cast<size_t>(count);
        }
        return true;
    }

private:
    int fd_ = -1;
#endif
};

uint32_t Le32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

bool SendIpc(IpcConnection& connection, uint32_t opcode, const std::string& json) {
    std::vector<uint8_t> bytes(8 + json.size());
    const uint32_t length = static_cast<uint32_t>(json.size());
    for (int i = 0; i < 4; ++i) {
        bytes[size_t(i)] = static_cast<uint8_t>(opcode >> (8 * i));
        bytes[size_t(4 + i)] = static_cast<uint8_t>(length >> (8 * i));
    }
    std::memcpy(bytes.data() + 8, json.data(), json.size());
    return connection.Write(bytes.data(), bytes.size(), kIoTimeout);
}

// Discord's messages as they come in, in pieces or several at once.
class MessageReader {
public:
    bool Append(const uint8_t* data, size_t size) {
        bytes_.insert(bytes_.end(), data, data + size);
        return bytes_.size() < 8 || Le32(bytes_.data() + 4) <= kMaxMessage;
    }
    // 1: a message, 0: not complete yet, -1: not a message (a length over the limit).
    int Pop(uint32_t& opcode, std::string& json) {
        if (bytes_.size() < 8) return 0;
        const uint32_t length = Le32(bytes_.data() + 4);
        if (length > kMaxMessage) return -1;
        if (bytes_.size() < 8u + length) return 0;
        opcode = Le32(bytes_.data());
        json.assign(reinterpret_cast<const char*>(bytes_.data() + 8), length);
        bytes_.erase(bytes_.begin(), bytes_.begin() + 8 + length);
        return 1;
    }

private:
    std::vector<uint8_t> bytes_;
};

std::string JsonText(std::string_view text) {
    std::string out = "\"";
    for (const unsigned char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += char(c);
        } else if (c < 0x20) {
            static constexpr char kHex[] = "0123456789abcdef";
            out += "\\u00";
            out += kHex[c >> 4];
            out += kHex[c & 15];
        } else {
            out += char(c);
        }
    }
    return out + "\"";
}

// The value of the first "key" in a message: a string's text, or a number / null / true as written. Enough for the
// few fields read here (evt, code, message); not a JSON parser.
std::string JsonField(std::string_view json, std::string_view key) {
    const std::string quoted = "\"" + std::string(key) + "\"";
    for (size_t at = json.find(quoted); at != std::string_view::npos; at = json.find(quoted, at + 1)) {
        size_t i = at + quoted.size();
        while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i]))) ++i;
        if (i >= json.size() || json[i] != ':') continue;
        ++i;
        while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i]))) ++i;
        std::string out;
        if (i < json.size() && json[i] == '"') {
            for (++i; i < json.size() && json[i] != '"'; ++i) {
                if (json[i] == '\\' && i + 1 < json.size()) ++i;
                out += json[i];
            }
        } else {
            while (i < json.size() && json[i] != ',' && json[i] != '}' && json[i] != ']' &&
                   !std::isspace(static_cast<unsigned char>(json[i])))
                out += json[i++];
        }
        return out.size() > 200 ? out.substr(0, 200) : out;
    }
    return {};
}

std::string Describe(const std::string& json) {
    const std::string code = JsonField(json, "code");
    const std::string message = JsonField(json, "message");
    if (code.empty() && message.empty()) return "no reason given";
    return "code " + (code.empty() ? std::string("?") : code) + (message.empty() ? "" : ", " + message);
}

std::string Clip(const std::string& text) {
    if (text.size() <= kMaxText) return text;
    size_t end = kMaxText;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0u) == 0x80u) --end;  // not inside a UTF-8 letter
    return text.substr(0, end);
}

std::string SetActivity(uint32_t pid, uint64_t nonce, const std::string& details, const std::string& state,
                        int64_t startUnix) {
    std::string activity = "{";
    // Discord refuses a details or state of fewer than 2 characters.
    if (details.size() >= 2) activity += "\"details\":" + JsonText(Clip(details)) + ",";
    if (state.size() >= 2) activity += "\"state\":" + JsonText(Clip(state)) + ",";
    activity += "\"timestamps\":{\"start\":" + std::to_string(startUnix) + "},\"instance\":false}";
    return "{\"cmd\":\"SET_ACTIVITY\",\"nonce\":\"" + std::to_string(nonce) + "\",\"args\":{\"pid\":" +
           std::to_string(pid) + ",\"activity\":" + activity + "}}";
}

std::string ClearActivity(uint32_t pid, uint64_t nonce) {
    return "{\"cmd\":\"SET_ACTIVITY\",\"nonce\":\"" + std::to_string(nonce) + "\",\"args\":{\"pid\":" +
           std::to_string(pid) + "}}";
}

// Waits up to `time`, or until Options turns the status on or off.
void WaitOrToggle(milliseconds time) {
    Shared& s = State();
    std::unique_lock<std::mutex> lock(s.mutex);
    const uint64_t toggles = s.toggles;
    s.wake.wait_for(lock, time, [&] { return s.toggles != toggles; });
}

void Run() {
    Shared& s = State();
    const std::vector<Endpoint> endpoints = Endpoints();
    const uint32_t pid = ProcessId();
    const std::string handshake = std::string("{\"v\":1,\"client_id\":\"") + kApplicationId + "\"}";
    uint64_t nonce = 0;
    bool absentLogged = false;
    int statusLines = 0;
    int refusedLines = 0;
    std::vector<uint8_t> buffer(4096);
    for (;;) {
        int64_t startUnix = 0;
        {
            std::unique_lock<std::mutex> lock(s.mutex);
            s.wake.wait(lock, [&] { return s.enabled; });
            startUnix = s.startUnix;
        }
        IpcConnection connection;
        const Endpoint* used = nullptr;
        for (const Endpoint& endpoint : endpoints) {
            if (connection.Open(endpoint.path)) {
                used = &endpoint;
                break;
            }
        }
        if (used == nullptr) {
            if (!absentLogged) {
                RT_LOGF(RT_TAG_RUNTIME, "[discord] no Discord app is running on this PC (%zu places looked at); looking "
                        "again every %lld s\n", endpoints.size(), static_cast<long long>(kRetryEvery.count() / 1000));
                absentLogged = true;
            }
            WaitOrToggle(kRetryEvery);
            continue;
        }

        // The handshake, then READY (or CLOSE with the reason, such as an unknown application ID).
        const auto readyBy = PresenceClock::now() + kReadyTimeout;
        MessageReader reader;
        bool ready = false;
        std::string problem;
        if (!SendIpc(connection, kOpHandshake, handshake)) problem = "the handshake could not be sent";
        while (!ready && problem.empty()) {
            if (PresenceClock::now() >= readyBy) {
                problem = "no answer within " + std::to_string(kReadyTimeout.count() / 1000) + " s";
                break;
            }
            const int count = connection.Read(buffer.data(), buffer.size(), kIoTimeout);
            if (count < 0) problem = "Discord closed the connection";
            else if (count > 0 && !reader.Append(buffer.data(), size_t(count))) problem = "an unreadable message";
            uint32_t opcode = 0;
            std::string json;
            int popped = 0;
            while (problem.empty() && (popped = reader.Pop(opcode, json)) == 1) {
                if (opcode == kOpClose) problem = "Discord refused it: " + Describe(json);
                else if (opcode == kOpPing) SendIpc(connection, kOpPong, json);
                else if (opcode == kOpFrame && JsonField(json, "evt") == "READY") ready = true;
            }
            if (popped < 0) problem = "an unreadable message";
        }
        if (!ready) {
            RT_LOGF(RT_TAG_RUNTIME, "[discord] could not connect through %s: %s; trying again in %lld s\n",
                    used->label.c_str(), problem.c_str(), static_cast<long long>(kRetryEvery.count() / 1000));
            connection.Close();
            WaitOrToggle(kRetryEvery);
            continue;
        }
        RT_LOGF(RT_TAG_RUNTIME, "[discord] connected to Discord through %s\n", used->label.c_str());
        absentLogged = false;

        // Connected: send the status when it changes (spaced out), answer PING, log what Discord refuses.
        uint64_t sentVersion = 0;
        bool sentOnce = false;
        auto nextSend = PresenceClock::now();
        bool enabled = true;
        problem.clear();
        while (problem.empty()) {
            std::string details, state;
            uint64_t version = 0;
            {
                std::lock_guard<std::mutex> lock(s.mutex);
                enabled = s.enabled;
                version = s.version;
                details = s.details;
                state = s.state;
            }
            if (!enabled) break;
            const auto now = PresenceClock::now();
            if ((!sentOnce || version != sentVersion) && now >= nextSend) {
                if (!SendIpc(connection, kOpFrame, SetActivity(pid, ++nonce, details, state, startUnix))) {
                    problem = "an update could not be sent";
                    break;
                }
                sentOnce = true;
                sentVersion = version;
                nextSend = now + kUpdateSpacing;
                if (statusLines < kMaxStatusLines) {
                    ++statusLines;
                    RT_LOGF(RT_TAG_RUNTIME, "[discord] status: \"%s\", \"%s\"%s\n", details.c_str(), state.c_str(),
                            statusLines == kMaxStatusLines ? " (no more of these lines this session)" : "");
                }
            }
            const int count = connection.Read(buffer.data(), buffer.size(), kIoTimeout);
            if (count < 0) {
                problem = "Discord closed the connection";
                break;
            }
            if (count > 0 && !reader.Append(buffer.data(), size_t(count))) {
                problem = "an unreadable message";
                break;
            }
            uint32_t opcode = 0;
            std::string json;
            int popped = 0;
            while (problem.empty() && (popped = reader.Pop(opcode, json)) == 1) {
                if (opcode == kOpClose) {
                    problem = "Discord closed it: " + Describe(json);
                } else if (opcode == kOpPing) {
                    if (!SendIpc(connection, kOpPong, json)) problem = "a PONG could not be sent";
                } else if (opcode == kOpFrame && JsonField(json, "evt") == "ERROR" && refusedLines < kMaxRefusedLines) {
                    ++refusedLines;
                    RT_LOGF(RT_TAG_RUNTIME, "[discord] Discord refused an update: %s\n", Describe(json).c_str());
                }
            }
            if (popped < 0) problem = "an unreadable message";
        }
        if (!enabled) {
            SendIpc(connection, kOpFrame, ClearActivity(pid, ++nonce));
            connection.Close();
            RT_LOGF(RT_TAG_RUNTIME, "[discord] status cleared and disconnected (turned off)\n");
            continue;
        }
        connection.Close();
        RT_LOGF(RT_TAG_RUNTIME, "[discord] disconnected: %s; trying again in %lld s\n", problem.c_str(),
                static_cast<long long>(kRetryEvery.count() / 1000));
        WaitOrToggle(kRetryEvery);
    }
}

}  // namespace

void Start() {
    Shared& s = State();
    bool enabled = false;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.started) return;
        s.started = true;
        s.enabled = enabled = RuntimeConfigFile::DiscordPresence(true);
        s.startUnix = std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::system_clock::now().time_since_epoch()).count();
    }
    RT_LOGF(RT_TAG_RUNTIME, "[discord] Rich Presence %s (application %s)\n", enabled ? "on" : "off", kApplicationId);
    try {
        std::thread(Run).detach();
    } catch (const std::exception& error) {
        RT_LOGF(RT_TAG_RUNTIME, "[discord] the worker thread could not start: %s\n", error.what());
    }
}

bool Enabled() {
    Shared& s = State();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.enabled;
}

void SetEnabled(bool on) {
    Shared& s = State();
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.enabled == on) return;
        s.enabled = on;
        ++s.toggles;
    }
    s.wake.notify_all();
    RuntimeConfigFile::SetDiscordPresence(on);
    RT_LOGF(RT_TAG_RUNTIME, "[discord] turned %s in Options\n", on ? "on" : "off");
}

void Publish(const std::string& details, const std::string& state) {
    Shared& s = State();
    // The worker holds the lock only to copy the text; when it has it, the next call (a moment later) gets through.
    std::unique_lock<std::mutex> lock(s.mutex, std::try_to_lock);
    if (!lock.owns_lock() || (s.details == details && s.state == state)) return;
    s.details = details;
    s.state = state;
    ++s.version;
}

}  // namespace DiscordPresence

#endif  // !__ANDROID__
