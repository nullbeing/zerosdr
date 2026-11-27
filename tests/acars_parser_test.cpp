#include "acars/acars_parser.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

acars::Message makeMessage(const std::string& label,
                           const std::string& text) {
    acars::Message msg;
    msg.label = label;
    msg.text = text;
    msg.flight = "CSN3702";
    msg.msgno = "M34A";
    return msg;
}

void testPoxPosition() {
    acars::Message msg = makeMessage(
        "18",
        "POX,CSN3702,25NOV26,063230,ZBYN,ZGGG,N 23028576,E113248512,2791,1040"
        "\n0,284,-  968,   35,   4,  10,0641,----,");
    const acars::Decoded decoded = acars::decodeText(msg);
    check(decoded.kind == acars::Kind::Position, "pox kind");
    check(decoded.has_position, "pox has position");
    check(std::fabs(decoded.latitude - 23.028576) < 1e-6, "pox latitude");
    check(std::fabs(decoded.longitude - 113.248512) < 1e-6, "pox longitude");
    check(decoded.origin == "ZBYN", "pox origin");
    check(decoded.destination == "ZGGG", "pox destination");
    check(decoded.report_time == "0632Z", "pox time");
    check(!decoded.has_altitude, "pox altitude stays unset");
}

void testFreeTextPosition() {
    acars::Message msg = makeMessage(
        "14",
        "PRESENT POSITION REPORT \n"
        "DMY 26NOV26,UTC 142336,LAT N 24.541,LON E114.105,CAS 280,"
        "WD 26102,WS\n  104,ALT 32097,FOB 10200,ETA 1628");
    const acars::Decoded decoded = acars::decodeText(msg);
    check(decoded.kind == acars::Kind::Position, "pos report kind");
    check(std::fabs(decoded.latitude - 24.541) < 1e-6, "pos report latitude");
    check(std::fabs(decoded.longitude - 114.105) < 1e-6,
          "pos report longitude");
    check(decoded.has_altitude && decoded.altitude_ft == 32097,
          "pos report altitude");
    check(decoded.report_time == "1423Z", "pos report time");
    check(decoded.origin.empty() && decoded.destination.empty(),
          "pos report has no route");
}

void testSlashRoute() {
    acars::Message msg = makeMessage(
        "41",
        "RKSI/ZGSZ/26NOV26/----/24588/  32/   4/0250/0309/N 23.231/E113.311/"
        "10838/24480/");
    const acars::Decoded decoded = acars::decodeText(msg);
    check(decoded.kind == acars::Kind::Position, "slash kind");
    check(decoded.origin == "RKSI", "slash origin");
    check(decoded.destination == "ZGSZ", "slash destination");
    check(std::fabs(decoded.latitude - 23.231) < 1e-6, "slash latitude");
    check(std::fabs(decoded.longitude - 113.311) < 1e-6, "slash longitude");
}

void testSystemAdvisory() {
    acars::Message msg = makeMessage("SA", "0EV062350VS");
    const acars::Decoded decoded = acars::decodeText(msg);
    check(decoded.kind == acars::Kind::System, "advisory kind");
    check(decoded.subtype == "EV", "advisory media code");
    check(decoded.report_time == "06:23:50Z", "advisory time");
}

void testAts() {
    acars::Message msg = makeMessage(
        "H1",
        "#DFBA320,065592,1,1,TB000000/REP001,85,02,1/CCB-309K,NOV25,050359,"
        "ZBYN,ZGGG,3702/C0TFC050CZ060300");
    const acars::Decoded decoded = acars::decodeText(msg);
    check(decoded.kind == acars::Kind::Ats, "ats kind");
    check(decoded.subtype == "DFB", "ats subtype");
    check(!decoded.has_position, "ats does not guess a position");
}

void testAdsc() {
    acars::Message msg = makeMessage(
        "B6",
        "/SGNCAYA.ADS.9V-NCH07109C228715881353AC1D0D1133AA85348814C1BC1");
    const acars::Decoded decoded = acars::decodeText(msg);
    check(decoded.kind == acars::Kind::Adsc, "adsc kind");
    check(!decoded.has_position, "adsc has no decoded position");
}

void testFreeTextAndEmpty() {
    acars::Message msg = makeMessage("81", "MSG TO AOC\nSORRY/ IMPOSSIBLE");
    const acars::Decoded decoded = acars::decodeText(msg);
    check(decoded.kind == acars::Kind::FreeText, "free text kind");
    check(!decoded.has_position, "free text has no position");

    acars::Message empty;
    check(acars::decodeText(empty).kind == acars::Kind::None,
          "empty payload kind");
}

void testRejectsBadCoordinates() {
    // A bare three digit number is not a coordinate ("CAS 280"), and an
    // out-of-range latitude must not be accepted.
    acars::Message msg = makeMessage("14", "CAS 280 N 99999999 E113248512");
    check(!acars::decodeText(msg).has_position, "bad latitude rejected");

    msg.text = "POSITION N 24.541";
    check(!acars::decodeText(msg).has_position,
          "latitude without longitude rejected");
}

void testRepeatedFrames() {
    acars::Message first;
    first.flight = "CZ3702";
    first.msgno = "S93A";
    first.label = "Q0";
    first.timestamp = 1000.0;

    acars::Message second = first;
    second.timestamp = 1010.0;
    check(acars::isRepeatedFrame(first, second), "repeated frame");

    second.text = "payload";
    check(!acars::isRepeatedFrame(first, second), "different text is not repeat");

    second = first;
    second.msgno = "S94A";
    check(!acars::isRepeatedFrame(first, second), "different msgno");

    second = first;
    second.timestamp = 1000.0 + 1801.0;
    check(!acars::isRepeatedFrame(first, second), "outside repeat window");
}

void testMultipartMerge() {
    acars::Message base;
    base.flight = "CZ3702";
    base.label = "H1";
    base.msgno = "S93A";
    base.text = "FIRST ";
    base.timestamp = 500.0;

    acars::Message block;
    block.flight = base.flight;
    block.label = base.label;
    block.msgno = "S93B";
    block.text = "SECOND";
    block.timestamp = 505.0;

    check(acars::isContinuationBlockOf(base, block), "block B continues A");
    check(acars::appendContinuation(base, block), "append block B");
    check(base.text == "FIRST SECOND", "merged text");
    check(base.block_count == 2, "block count");
    check(base.last_block == 'B', "last block");

    // The link layer repeats block B: it must be recognised, not appended.
    check(acars::isMergedBlock(base, block), "repeated block B");

    acars::Message third = block;
    third.msgno = "S93C";
    third.text = " THIRD";
    check(acars::appendContinuation(base, third), "append block C");
    check(base.text == "FIRST SECOND THIRD", "three block text");

    // A block from another message number is never merged.
    acars::Message other = block;
    other.msgno = "S94B";
    check(!acars::isContinuationBlockOf(base, other), "other msgno");

    // Nor is a block from another flight.
    acars::Message foreign = block;
    foreign.flight = "CZ3703";
    check(!acars::isContinuationBlockOf(base, foreign), "other flight");
}

}  // namespace

int main() {
    testPoxPosition();
    testFreeTextPosition();
    testSlashRoute();
    testSystemAdvisory();
    testAts();
    testAdsc();
    testFreeTextAndEmpty();
    testRejectsBadCoordinates();
    testRepeatedFrames();
    testMultipartMerge();

    if (failures != 0) {
        std::printf("acars_parser_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("acars_parser_test: ok\n");
    return 0;
}
