#include "acars/acars_log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

namespace acars {
namespace {

namespace fs = std::filesystem;

constexpr int kMaxText = 2000;

fs::path logDirectory() {
    const char* xdg = std::getenv("XDG_DATA_HOME");
    fs::path base;
    if (xdg && *xdg) {
        base = xdg;
    } else {
        const char* home = std::getenv("HOME");
        base = home ? fs::path(home) / ".local" / "share" : fs::path("/tmp");
    }
    return base / "zerosdr" / "acars";
}

std::string dateStamp(double timestamp) {
    time_t seconds = static_cast<time_t>(timestamp);
    if (seconds <= 0) seconds = std::time(nullptr);
    struct tm local {};
    localtime_r(&seconds, &local);
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d",
                  local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
    return buffer;
}

std::optional<Message> messageFromJson(const nlohmann::json& doc) {
    if (!doc.is_object()) return std::nullopt;

    const auto number = [&doc](const char* key, double fallback) {
        const auto it = doc.find(key);
        return (it != doc.end() && it->is_number()) ? it->get<double>()
                                                    : fallback;
    };
    const auto string = [&doc](const char* key) {
        const auto it = doc.find(key);
        return (it != doc.end() && it->is_string())
                   ? it->get<std::string>()
                   : std::string();
    };

    Message msg;
    msg.timestamp = number("timestamp", 0.0);
    if (msg.timestamp <= 0.0) return std::nullopt;
    msg.freq_mhz = static_cast<float>(number("freq", 0.0));
    msg.channel = static_cast<int>(number("channel", 0.0));
    msg.level = static_cast<float>(number("level", 0.0));
    msg.noise = static_cast<float>(number("noise", 0.0));
    msg.error = static_cast<int>(number("error", 0.0));

    const auto ack = doc.find("ack");
    if (ack != doc.end()) {
        if (ack->is_boolean()) {
            msg.ack = ack->get<bool>();
        } else if (ack->is_number()) {
            msg.ack = ack->get<double>() != 0.0;
        }
    }

    msg.mode = string("mode");
    msg.label = string("label");
    msg.block_id = string("block_id");
    msg.msgno = string("msgno");
    msg.tail = string("tail");
    msg.flight = string("flight");
    msg.sublabel = string("sublabel");
    msg.text = string("text");
    if (msg.text.size() > static_cast<size_t>(kMaxText)) {
        msg.text.resize(kMaxText);
    }
    if (msg.flight.empty() && msg.tail.empty() && msg.text.empty()) {
        return std::nullopt;
    }
    return msg;
}

}  // namespace

void appendMessage(const Message& message) {
    std::error_code ec;
    const fs::path dir = logDirectory();
    fs::create_directories(dir, ec);

    nlohmann::json doc;
    doc["timestamp"] = message.timestamp;
    doc["freq"] = message.freq_mhz;
    doc["channel"] = message.channel;
    doc["level"] = message.level;
    doc["noise"] = message.noise;
    doc["error"] = message.error;
    doc["ack"] = message.ack;
    doc["mode"] = message.mode;
    doc["label"] = message.label;
    doc["block_id"] = message.block_id;
    doc["msgno"] = message.msgno;
    doc["tail"] = message.tail;
    doc["flight"] = message.flight;
    doc["sublabel"] = message.sublabel;
    doc["text"] = message.text;

    const fs::path file = dir / (dateStamp(message.timestamp) + ".jsonl");
    std::ofstream out(file, std::ios::app);
    if (!out) return;
    out << doc.dump() << '\n';
}

std::vector<Message> loadRecentMessages(int max_messages) {
    std::vector<Message> newest_first;
    if (max_messages <= 0) return newest_first;

    std::error_code ec;
    const fs::path dir = logDirectory();
    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        std::error_code type_ec;
        if (!entry.is_regular_file(type_ec)) continue;
        const std::string name = entry.path().filename().string();
        if (name.size() == 16 && name.compare(10, 6, ".jsonl") == 0) {
            files.push_back(entry.path());
        }
    }
    if (files.empty()) return newest_first;

    // YYYY-MM-DD.jsonl sorts chronologically, so walk from newest to oldest.
    std::sort(files.begin(), files.end());
    for (auto file = files.rbegin(); file != files.rend(); ++file) {
        std::ifstream in(*file);
        if (!in) continue;
        std::vector<Message> day;
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            const auto doc = nlohmann::json::parse(line, nullptr, false);
            if (doc.is_discarded()) continue;
            auto parsed = messageFromJson(doc);
            if (parsed) day.push_back(std::move(*parsed));
        }
        for (auto msg = day.rbegin(); msg != day.rend(); ++msg) {
            newest_first.push_back(std::move(*msg));
            if (static_cast<int>(newest_first.size()) >= max_messages) {
                break;
            }
        }
        if (static_cast<int>(newest_first.size()) >= max_messages) break;
    }

    std::reverse(newest_first.begin(), newest_first.end());
    return newest_first;
}

}  // namespace acars
