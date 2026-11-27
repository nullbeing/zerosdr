#include "acars/airports.h"

#include <cstdio>
#include <fstream>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

bool writeFile(const char* path, const char* text) {
    std::ofstream out(path);
    if (!out) return false;
    out << text;
    return true;
}

}  // namespace

int main() {
    const char* table = "/tmp/zerosdr-airports-test.csv";
    // The name of EGLL contains a comma inside quotes: the splitter must keep
    // it in one field.
    const char* csv =
        "ident,name,city,country,iata\n"
        "ZGGG,Guangzhou Baiyun,Guangzhou,CN,CAN\n"
        "EGLL,\"Heathrow, London\",London,GB,LHR\n"
        "LFPG,Charles de Gaulle,Paris,FR,CDG\n";
    check(writeFile(table, csv), "write test csv");
    check(acars::loadAirports(table), "load table");
    check(acars::airportCount() == 3, "three airports");

    const acars::Airport* airport = acars::findAirport("ZGGG");
    check(airport != nullptr, "find ZGGG");
    if (airport != nullptr) {
        check(airport->city == "Guangzhou", "ZGGG city");
        check(airport->name == "Guangzhou Baiyun", "ZGGG name");
    }

    // ACARS may deliver lower case; the lookup is case-insensitive.
    check(acars::findAirport("zggg") != nullptr, "lower case lookup");

    airport = acars::findAirport("EGLL");
    check(airport != nullptr, "find EGLL");
    if (airport != nullptr) {
        check(airport->name == "Heathrow, London", "quoted comma kept");
    }

    check(acars::findAirport("XXXX") == nullptr, "unknown airport");
    check(acars::findAirport("ZGG") == nullptr, "short ident");

    // A failed load must keep the current table intact.
    check(!acars::loadAirports("/nonexistent/airports.csv"),
          "missing file reports failure");
    check(acars::airportCount() == 3, "table survives failed load");

    // An explicit load replaces the table (used by tests/tools).
    const char* second = "/tmp/zerosdr-airports-test2.csv";
    check(writeFile(second, "ident,name,city,country,iata\n"
                            "ZBYN,Taiyuan Wusu,Taiyuan,CN,TYN\n"),
          "write second csv");
    check(acars::loadAirports(second), "load second table");
    check(acars::airportCount() == 1, "replaced table size");
    check(acars::findAirport("ZGGG") == nullptr, "old table replaced");
    check(acars::findAirport("ZBYN") != nullptr, "new table entry");

    std::remove(table);
    std::remove(second);

    if (failures != 0) {
        std::printf("acars_airports_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("acars_airports_test: ok\n");
    return 0;
}
