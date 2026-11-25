#include "acars/acars_labels.h"

namespace acars {
namespace {

struct Entry {
    const char* code;
    const char* text;
};

// Common ACARS labels. Descriptions are deliberately short so they fit the
// 320 px detail header; airline-specific labels (10, 3x, 4x) only describe
// the usual traffic and may differ between operators.
constexpr Entry kLabels[] = {
    {"_", "UPLINK ACKNOWLEDGE"},
    {"_d", "UPLINK ACKNOWLEDGE"},
    {"00", "EMERGENCY"},
    {"10", "AIRLINE DEFINED"},
    {"11", "AIRLINE DEFINED"},
    {"14", "GA FREE TEXT"},
    {"15", "GA POSITION REPORT"},
    {"16", "GA WEATHER REQUEST"},
    {"17", "POSITION / WEATHER"},
    {"18", "WEATHER / POSITION"},
    {"19", "ATIS REPORT"},
    {"20", "POSITION REPORT"},
    {"21", "POSITION REPORT"},
    {"22", "PERIODIC POSITION"},
    {"24", "WEATHER REPORT"},
    {"27", "WEATHER REQUEST"},
    {"28", "WEATHER REQUEST"},
    {"2S", "WEATHER REQUEST"},
    {"30", "POSITION REPORT"},
    {"44", "OOOI + POSITION"},
    {"4J", "POSITION / WEATHER"},
    {"52", "UTC REQUEST / REPORT"},
    {"54", "VOICE CONTACT"},
    {"57", "ALTERNATE POSITION"},
    {"5D", "ATIS REQUEST"},
    {"5P", "TEMP SUSPENSION"},
    {"5R", "POSITION REPORT"},
    {"5U", "WEATHER REQUEST"},
    {"5Y", "ETA REVISION"},
    {"5Z", "AIRLINE DEFINED"},
    {"B1", "OCEANIC CLEARANCE"},
    {"B9", "FMS FLIGHT PLAN"},
    {"C1", "AIRLINE DEFINED"},
    {"H1", "POSITION / WEATHER"},
    {"H2", "WIND LAYERS"},
    {"Q0", "ACARS LINK TEST"},
    {"Q1", "ETA REPORT"},
    {"Q2", "ETA REPORT"},
    {"Q5", "UNABLE TO PROCESS"},
    {"Q6", "VOICE / ACARS CHANGE"},
    {"Q7", "DELAY MESSAGE"},
    {"QA", "DEPARTURE REPORT"},
    {"QB", "OFF REPORT"},
    {"QC", "ON REPORT"},
    {"QD", "ARRIVAL REPORT"},
    {"QE", "OUT / FUEL"},
    {"QF", "OFF / DESTINATION"},
    {"QX", "INTERCEPT"},
    {"RA", "COMMAND RESPONSE"},
    {"RB", "RESPONSE TO RA"},
    {"SA", "SYSTEM MESSAGE"},
    {"SQ", "SQUITTER"},
    {"S1", "SURVEILLANCE DATA"},
    {"X1", "PROVIDER DEFINED"},
};

}  // namespace

std::string labelDescription(const std::string& label) {
    if (label.empty()) return {};
    for (const auto& entry : kLabels) {
        if (label == entry.code) return entry.text;
    }
    return {};
}

}  // namespace acars
