#ifndef ACARS_AIRPORTS_H
#define ACARS_AIRPORTS_H

#include <cstddef>
#include <string>

namespace acars {

/**
 * One airport from the bundled OurAirports table.
 *
 * The table only keeps fields that fit the 320x170 display and that ACARS
 * route strings can name: the ICAO identifier, a short English name and the
 * municipality. Everything is ASCII because the UI font has no accents.
 */
struct Airport {
    std::string ident;  // ICAO, e.g. "ZGGG"
    std::string name;   // e.g. "Guangzhou Baiyun"
    std::string city;   // e.g. "Guangzhou"
};

/**
 * Looks up an ICAO identifier ("ZGGG"). Returns nullptr when the airport is
 * not in the table or the table could not be loaded. The table is loaded
 * lazily from /usr/share/zerosdr/airports.csv (or data/airports.csv in a
 * source tree) on first use.
 */
const Airport* findAirport(const std::string& ident);

/**
 * Loads (or replaces) the airport table from a CSV file with the header
 * ident,name,city,country,iata. Returns false when the file cannot be read.
 * Exposed for the regression test.
 */
bool loadAirports(const std::string& path);

/** Number of airports currently loaded (0 when unavailable). */
size_t airportCount();

}  // namespace acars

#endif  // ACARS_AIRPORTS_H
