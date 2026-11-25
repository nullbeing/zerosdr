#ifndef ACARS_CONTROLLER_H
#define ACARS_CONTROLLER_H

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace acars {

struct Message {
    uint64_t seq = 0;  // stable identity for selection while new rows arrive
    double timestamp = 0.0;
    float freq_mhz = 0.0f;
    int channel = 0;
    float level = 0.0f;
    float noise = 0.0f;
    int error = 0;
    bool ack = false;
    std::string mode;
    std::string label;
    std::string block_id;
    std::string msgno;
    std::string tail;
    std::string flight;
    std::string sublabel;
    std::string text;
};

/**
 * Runs the bundled acarsdec as a child process and reads its JSON output
 * from a loopback UDP socket. acarsdec is GPLv2-only while zeroSDR is
 * GPL-3.0, so it is invoked as a separate program and never linked in.
 */
class AcarsController {
public:
    AcarsController();
    ~AcarsController();

    AcarsController(const AcarsController&) = delete;
    AcarsController& operator=(const AcarsController&) = delete;

    bool start(const std::vector<float>& channels_mhz, float gain_db,
               int udp_port);
    void stop();
    bool isRunning() const { return running_.load(); }

    std::vector<Message> snapshot() const;
    uint64_t totalMessages() const { return total_.load(); }
    float messagesPerSecond() const { return rate_.load(); }
    const std::string& lastError() const { return last_error_; }
    const std::vector<float>& channels() const { return channels_; }
    float gainDb() const { return gain_db_; }
    std::string decoderPath() const;

private:
    void receiveLoop();
    void handleJson(const std::string& line);
    void loadHistory(int max_messages);
    void noteExit(int status);
    static std::string sanitizeText(const std::string& text);

    mutable std::mutex mutex_;
    std::deque<Message> messages_;
    std::atomic<uint64_t> total_{0};
    std::atomic<uint64_t> session_total_{0};  // excludes loaded history
    uint64_t seq_counter_ = 0;  // guarded by mutex_

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<float> rate_{0.0f};
    pid_t pid_ = -1;
    int udp_fd_ = -1;
    int udp_port_ = 5556;
    float gain_db_ = 40.0f;
    std::vector<float> channels_{131.550f, 131.725f, 131.825f};
    double started_at_ = 0.0;
    std::string last_error_;
};

}  // namespace acars

#endif  // ACARS_CONTROLLER_H
