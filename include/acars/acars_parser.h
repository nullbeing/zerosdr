#ifndef ACARS_PARSER_H
#define ACARS_PARSER_H

#include "acars/acars_controller.h"

#include <string>

namespace acars {

/**
 * Rough category of an ACARS text payload.
 *
 * The ACARS envelope (mode, label, block, message number, flight, tail) is
 * already structured by acarsdec. The text itself has no universal grammar:
 * a few formats are defined by ARINC, many labels are airline-defined free
 * text. Kind only describes the shape that was recognised; it never claims
 * that every field of the payload has been understood.
 */
enum class Kind {
    None,      // no text: link test, uplink acknowledge
    Position,  // position report with a decoded latitude/longitude
    Ats,       // ARINC 622 ATS datalink (#DFB, #CFB, ...)
    Adsc,      // ADS-C contract report (binary hex payload)
    System,    // media/system advisory (label SA, e.g. 0EV062350VS)
    FreeText,  // airline-defined free text
};

const char* kindTag(Kind kind);   // short list tag: POS, ATS, ADSC, SYS, TXT
const char* kindName(Kind kind);  // detail headline

/** Fields recovered from a text payload. Unset fields keep their default. */
struct Decoded {
    Kind kind = Kind::None;
    bool has_position = false;
    double latitude = 0.0;
    double longitude = 0.0;
    bool has_altitude = false;
    int altitude_ft = 0;
    std::string origin;       // departure airport (ICAO), when present
    std::string destination;  // arrival airport (ICAO), when present
    std::string report_time;  // "0632Z" (reports) or "06:23:50Z" (advisories)
    std::string subtype;      // ATS message type or advisory media code
};

/** Decodes what can be recovered from Message::text. */
Decoded decodeText(const Message& message);

/**
 * True when two frames are the same transmission repeated by the ACARS link
 * layer: same flight, message number, label and text, within 30 minutes.
 */
bool isRepeatedFrame(const Message& held, const Message& incoming);

/**
 * True when `part` is a later block (msgno suffix B..Z) of the multi-block
 * message `base` (suffix A). ACARS repeats the first three characters of the
 * message number for every block.
 */
bool isContinuationBlockOf(const Message& base, const Message& part);

/**
 * Appends the continuation text to `base`. Returns false when `part` is not a
 * continuation block of `base` (including a block already merged).
 */
bool appendContinuation(Message& base, const Message& part);

/**
 * True when `part` is a block that has already been merged into `base`, so the
 * link-layer repeat is not appended a second time.
 */
bool isMergedBlock(const Message& base, const Message& part);

}  // namespace acars

#endif  // ACARS_PARSER_H
