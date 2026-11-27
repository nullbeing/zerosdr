#include "acars/acars_controller.h"

#include "acars/acars_log.h"
#include "acars/acars_parser.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <iostream>
#include <netinet/in.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace acars {
namespace {

constexpr size_t kMaxMessages = 500;
constexpr int kMaxText = 2000;

double nowSeconds() {
    struct timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + ts.tv_nsec / 1e9;
}

bool isExecutable(const std::string& path) {
    struct stat st {};
    return stat(path.c_str(), &st) == 0 && (st.st_mode & S_IXUSR) != 0;
}

std::string joinArgs(const std::vector<std::string>& args) {
    std::string out;
    for (const auto& arg : args) {
        if (!out.empty()) out += ' ';
        out += arg;
    }
    return out;
}

}  // namespace

AcarsController::AcarsController() = default;

AcarsController::~AcarsController() {
    stop();
}

std::string AcarsController::decoderPath() const {
    const char* candidates[] = {
        "/usr/lib/zerosdr/acarsdec",
        "/usr/local/lib/zerosdr/acarsdec",
        "vendor/acarsdec/acarsdec",
    };
    for (const char* path : candidates) {
        if (isExecutable(path)) return path;
    }
    return "acarsdec";
}

bool AcarsController::start(const std::vector<float>& channels_mhz,
                            float gain_db, int udp_port) {
    stop();

    if (channels_mhz.empty()) {
        last_error_ = "no ACARS channels configured";
        return false;
    }
    channels_ = channels_mhz;
    gain_db_ = gain_db;
    udp_port_ = udp_port;

    udp_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_fd_ < 0) {
        last_error_ = std::string("socket: ") + std::strerror(errno);
        return false;
    }
    int reuse = 1;
    setsockopt(udp_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    // Do not leak the listening socket into acarsdec through exec().
    fcntl(udp_fd_, F_SETFD, FD_CLOEXEC);
    struct timeval timeout {};
    timeout.tv_sec = 0;
    timeout.tv_usec = 200000;
    setsockopt(udp_fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    struct sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<uint16_t>(udp_port_));
    if (bind(udp_fd_, reinterpret_cast<struct sockaddr*>(&addr),
             sizeof(addr)) != 0) {
        last_error_ = std::string("bind 127.0.0.1:") +
                      std::to_string(udp_port_) + ": " +
                      std::strerror(errno);
        close(udp_fd_);
        udp_fd_ = -1;
        return false;
    }

    float lo = channels_.front();
    float hi = channels_.front();
    for (float ch : channels_) {
        lo = std::min(lo, ch);
        hi = std::max(hi, ch);
    }
    const float span_mhz = std::max(0.025f, hi - lo);
    int mult = static_cast<int>(
        std::ceil((span_mhz * 2.0f * 1000.0f) / 12.0f));
    // The RTL-SDR Blog V4 rejects the low rates acarsdec would pick for a
    // single narrow channel; keep at least 1.536 Msps (128 * 12 kHz).
    mult = std::max(128, mult);
    if ((mult & 1) != 0) ++mult;
    const float center = (lo + hi) * 0.5f;

    const std::string output = "json:udp:host=127.0.0.1,port=" +
                               std::to_string(udp_port_);
    std::vector<std::string> args;
    args.push_back(decoderPath());
    args.push_back("--rtlsdr");
    args.push_back("0");
    if (gain_db_ > 0.0f) {
        char gain[16];
        std::snprintf(gain, sizeof(gain), "%.1f", gain_db_);
        args.push_back("-g");
        args.push_back(gain);
    }
    char mult_arg[16];
    std::snprintf(mult_arg, sizeof(mult_arg), "%d", mult);
    args.push_back("-m");
    args.push_back(mult_arg);
    char center_arg[24];
    std::snprintf(center_arg, sizeof(center_arg), "%.4f", center);
    args.push_back("-c");
    args.push_back(center_arg);
    args.push_back("--output");
    args.push_back(output);
    for (float ch : channels_) {
        char freq[24];
        std::snprintf(freq, sizeof(freq), "%.3f", ch);
        args.push_back(freq);
    }

    std::cout << "AcarsController: " << joinArgs(args) << std::endl;

    pid_ = fork();
    if (pid_ < 0) {
        last_error_ = std::string("fork: ") + std::strerror(errno);
        close(udp_fd_);
        udp_fd_ = -1;
        return false;
    }
    if (pid_ == 0) {
        setsid();
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (udp_fd_ >= 0) close(udp_fd_);
        // Keep acarsdec's own diagnostics out of the UI. The file helps when
        // the decoder cannot open the dongle.
        int fd = open("/tmp/zerosdr-acarsdec.log",
                      O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
        }
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
        argv.push_back(nullptr);
        execv(args[0].c_str(), argv.data());
        _exit(127);
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        messages_.clear();
        last_error_.clear();
    }
    total_.store(0);
    session_total_.store(0);
    rate_.store(0.0f);
    started_at_ = nowSeconds();
    loadHistory(static_cast<int>(kMaxMessages));
    running_.store(true);
    thread_ = std::thread(&AcarsController::receiveLoop, this);

    std::cout << "AcarsController: acarsdec pid " << pid_ << ", center "
              << center_arg << " MHz, " << channels_.size()
              << " channel(s), mult " << mult << std::endl;
    return true;
}

void AcarsController::stop() {
    const bool was_running = running_.exchange(false);
    // Always reap the receive thread: it can already have exited (decoder
    // died) while still being joinable, and destroying a joinable
    // std::thread calls std::terminate.
    if (thread_.joinable()) thread_.join();

    if (!was_running && pid_ <= 0) {
        if (udp_fd_ >= 0) {
            close(udp_fd_);
            udp_fd_ = -1;
        }
        return;
    }

    if (pid_ > 0) {
        kill(pid_, SIGTERM);
        bool reaped = false;
        for (int i = 0; i < 30; ++i) {
            int status = 0;
            const pid_t result = waitpid(pid_, &status, WNOHANG);
            if (result == pid_) {
                reaped = true;
                break;
            }
            usleep(50000);
        }
        if (!reaped) {
            kill(pid_, SIGKILL);
            waitpid(pid_, nullptr, 0);
        }
        pid_ = -1;
    }
    if (udp_fd_ >= 0) {
        close(udp_fd_);
        udp_fd_ = -1;
    }
}

void AcarsController::loadHistory(int max_messages) {
    auto loaded = loadRecentMessages(max_messages);
    if (loaded.empty()) return;

    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& msg : loaded) storeFrame(std::move(msg));
    total_.fetch_add(loaded.size(), std::memory_order_relaxed);
}

void AcarsController::storeFrame(Message message) {
    // ACARS repeats frames for reliability. Merge an identical transmission
    // instead of showing the same line two or three times.
    for (auto& held : messages_) {
        if (isRepeatedFrame(held, message)) {
            if (held.repeat_count < 255) ++held.repeat_count;
            if (message.timestamp > held.timestamp) {
                held.timestamp = message.timestamp;
            }
            return;
        }
        if (isMergedBlock(held, message)) return;
    }

    // Multi-block messages use msgno suffixes A, B, C...; append later blocks
    // to the earliest block still held.
    if (message.msgno.size() == 4 && message.msgno[3] > 'A') {
        Message* base = nullptr;
        for (auto& held : messages_) {
            if (!isContinuationBlockOf(held, message)) continue;
            if (base == nullptr || held.msgno[3] < base->msgno[3]) base = &held;
        }
        if (base != nullptr) {
            appendContinuation(*base, message);
            return;
        }
    }

    message.seq = ++seq_counter_;
    messages_.push_front(std::move(message));
    while (messages_.size() > kMaxMessages) messages_.pop_back();
}

void AcarsController::receiveLoop() {
    while (running_.load()) {
        char buffer[8192];
        struct sockaddr_in from {};
        socklen_t from_len = sizeof(from);
        const ssize_t length =
            recvfrom(udp_fd_, buffer, sizeof(buffer) - 1, 0,
                     reinterpret_cast<struct sockaddr*>(&from), &from_len);
        if (length > 0) {
            buffer[length] = '\0';
            std::string payload(buffer, static_cast<size_t>(length));
            size_t start = 0;
            while (start < payload.size()) {
                const size_t end = payload.find('\n', start);
                const std::string line = payload.substr(
                    start, end == std::string::npos ? std::string::npos
                                                    : end - start);
                if (!line.empty()) handleJson(line);
                if (end == std::string::npos) break;
                start = end + 1;
            }
        } else if (length < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
                   errno != EINTR) {
            break;
        }

        if (pid_ > 0) {
            int status = 0;
            const pid_t result = waitpid(pid_, &status, WNOHANG);
            if (result == pid_) {
                noteExit(status);
                break;
            }
        }
        const double elapsed = std::max(0.001, nowSeconds() - started_at_);
        rate_.store(static_cast<float>(session_total_.load() / elapsed));
    }
}

void AcarsController::noteExit(int status) {
    pid_ = -1;
    running_.store(false);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
        last_error_ = "cannot execute acarsdec";
    } else {
        last_error_ = "acarsdec stopped";
    }
    std::cerr << "AcarsController: " << last_error_ << std::endl;
}

void AcarsController::handleJson(const std::string& line) {
    nlohmann::json doc = nlohmann::json::parse(line, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) return;

    // acarsdec is not strict about JSON types: fields such as "ack" can be
    // a string, "level" can be null, and so on. Never let one odd message
    // take the whole page down.
    const auto number = [&doc](const char* key, double fallback) {
        const auto it = doc.find(key);
        if (it == doc.end() || it->is_null()) return fallback;
        if (it->is_number()) return it->get<double>();
        if (it->is_string()) {
            try {
                return std::stod(it->get<std::string>());
            } catch (...) {
                return fallback;
            }
        }
        return fallback;
    };
    const auto string = [&doc](const char* key) {
        const auto it = doc.find(key);
        if (it == doc.end() || it->is_null()) return std::string();
        if (it->is_string()) return it->get<std::string>();
        if (it->is_boolean()) return std::string(it->get<bool>() ? "1" : "0");
        return std::string();
    };

    Message msg;
    msg.timestamp = number("timestamp", 0.0);
    if (msg.timestamp <= 0.0) msg.timestamp = std::time(nullptr);
    msg.freq_mhz = static_cast<float>(number("freq", 0.0));
    msg.channel = static_cast<int>(number("channel", 0));
    msg.level = static_cast<float>(number("level", 0.0));
    msg.noise = static_cast<float>(number("noise", 0.0));
    msg.error = static_cast<int>(number("error", 0));

    const auto ack = doc.find("ack");
    if (ack != doc.end()) {
        if (ack->is_boolean()) {
            msg.ack = ack->get<bool>();
        } else if (ack->is_number()) {
            msg.ack = ack->get<double>() != 0.0;
        } else if (ack->is_string()) {
            const std::string value = ack->get<std::string>();
            msg.ack = !value.empty() && value != "0" && value != "false";
        }
    }

    msg.mode = string("mode");
    msg.label = string("label");
    msg.block_id = string("block_id");
    msg.msgno = string("msgno");
    msg.tail = string("tail");
    msg.flight = string("flight");
    msg.sublabel = string("sublabel");
    msg.text = sanitizeText(string("text"));
    if (msg.flight.empty() && msg.tail.empty() && msg.text.empty()) return;

    appendMessage(msg);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        storeFrame(std::move(msg));
    }
    total_.fetch_add(1);
    session_total_.fetch_add(1);
}

std::string AcarsController::sanitizeText(const std::string& text) {
    std::string out;
    out.reserve(std::min<size_t>(text.size(), kMaxText));
    bool last_space = false;
    for (char c : text) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (uc == '\r') continue;
        if (uc == '\n') {
            out.push_back('\n');
            last_space = false;
            continue;
        }
        if (uc < 0x20 || uc > 0x7e) {
            if (!last_space) out.push_back(' ');
            last_space = true;
            continue;
        }
        out.push_back(c);
        last_space = (c == ' ');
        if (out.size() >= kMaxText) break;
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '\n')) {
        out.pop_back();
    }
    return out;
}

std::vector<Message> AcarsController::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Message> out(messages_.begin(), messages_.end());
    // Newest first by message time, not arrival time, so a late/out-of-order
    // frame cannot split a day's group in the list. Equal timestamps keep
    // arrival order (stable_sort).
    std::stable_sort(out.begin(), out.end(),
                     [](const Message& a, const Message& b) {
                         return a.timestamp > b.timestamp;
                     });
    return out;
}

size_t AcarsController::messageCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return messages_.size();
}

}  // namespace acars
