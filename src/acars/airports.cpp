#include "acars/airports.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace acars {
namespace {

std::vector<Airport> g_airports;  // sorted by ident
std::once_flag g_once;
bool g_loaded = false;

// Minimal CSV field splitter. The airport names contain commas and
// apostrophes ("Gwaunaru'u"), so quoted fields and doubled quotes must be
// handled even though the bundled file only needs the first three columns.
std::vector<std::string> parseCsvLine(const std::string& line) {
    std::vector<std::string> fields;
    std::string field;
    bool quoted = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (quoted) {
            if (c == '"') {
                if (i + 1 < line.size() && line[i + 1] == '"') {
                    field.push_back('"');
                    ++i;
                } else {
                    quoted = false;
                }
            } else {
                field.push_back(c);
            }
        } else if (c == '"') {
            quoted = true;
        } else if (c == ',') {
            fields.push_back(field);
            field.clear();
        } else if (c != '\r') {
            field.push_back(c);
        }
    }
    fields.push_back(field);
    return fields;
}

bool loadFile(const std::string& path) {
    std::ifstream in(path);
    if (!in) return false;

    std::vector<Airport> airports;
    airports.reserve(6000);
    std::string line;
    bool header = true;
    while (std::getline(in, line)) {
        if (header) {
            header = false;
            continue;
        }
        if (line.empty()) continue;
        const auto fields = parseCsvLine(line);
        if (fields.size() < 3) continue;
        Airport airport;
        airport.ident = fields[0];
        airport.name = fields[1];
        airport.city = fields[2];
        if (airport.ident.size() != 4 || airport.name.empty()) continue;
        airports.push_back(std::move(airport));
    }
    if (airports.empty()) return false;

    std::sort(airports.begin(), airports.end(),
              [](const Airport& a, const Airport& b) {
                  return a.ident < b.ident;
              });
    airports.erase(std::unique(airports.begin(), airports.end(),
                               [](const Airport& a, const Airport& b) {
                                   return a.ident == b.ident;
                               }),
                   airports.end());

    g_airports = std::move(airports);
    g_loaded = true;
    return true;
}

void loadDefault() {
    if (g_loaded) return;
    if (const char* env = std::getenv("ZEROSDR_AIRPORTS")) {
        if (*env && loadFile(env)) return;
    }
    const char* candidates[] = {
        "/usr/share/zerosdr/airports.csv",
        "/usr/local/share/zerosdr/airports.csv",
        "data/airports.csv",
    };
    for (const char* path : candidates) {
        if (loadFile(path)) return;
    }
}

}  // namespace

bool loadAirports(const std::string& path) {
    if (!loadFile(path)) return false;
    std::call_once(g_once, [] {});
    return true;
}

const Airport* findAirport(const std::string& ident) {
    std::call_once(g_once, loadDefault);
    if (ident.size() != 4 || g_airports.empty()) return nullptr;

    std::string key = ident;
    for (char& c : key) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    const auto it = std::lower_bound(
        g_airports.begin(), g_airports.end(), key,
        [](const Airport& airport, const std::string& value) {
            return airport.ident < value;
        });
    if (it == g_airports.end() || it->ident != key) return nullptr;
    return &*it;
}

size_t airportCount() { return g_airports.size(); }

}  // namespace acars
