#ifndef APRS_SYMBOLS_H
#define APRS_SYMBOLS_H

namespace aprs {

enum class SymbolKind {
    Unknown,
    Car,
    Truck,
    Van,
    Bus,
    Jeep,
    Motorcycle,
    Bicycle,
    Walker,
    Aircraft,
    Helicopter,
    Glider,
    Balloon,
    Ship,
    Yacht,
    Canoe,
    Weather,
    Digipeater,
    IGate,
    House,
    Hospital,
    Police,
    Fire,
    Aid,
    Satellite,
    Antenna,
    Node,
    Server,
    Bbs,
    Rover,
    Emergency
};

/**
 * Map one APRS symbol to a small semantic type. The overlay tables (0-9,
 * A-Z) use the primary symbol shapes, so only the alternate table '\' gets
 * special handling for the few symbols that differ.
 */
inline SymbolKind symbolKind(char table, char symbol) {
    if (table == '\\') {
        switch (symbol) {
            case '_': return SymbolKind::Weather;
            case '#': return SymbolKind::Digipeater;
            case '&': return SymbolKind::IGate;
            case 'O': return SymbolKind::Balloon;
            case '>': return SymbolKind::Car;
            case '^': return SymbolKind::Aircraft;
            case 's': return SymbolKind::Ship;
            case 'Y': return SymbolKind::Yacht;
            case '-': return SymbolKind::House;
            default: break;
        }
    }

    switch (symbol) {
        case '>': return SymbolKind::Car;
        case 'v': return SymbolKind::Van;
        case 'k':
        case 'u':
        case 't': return SymbolKind::Truck;
        case 'U': return SymbolKind::Bus;
        case 'j': return SymbolKind::Jeep;
        case '<': return SymbolKind::Motorcycle;
        case 'b': return SymbolKind::Bicycle;
        case '[': return SymbolKind::Walker;
        case '^':
        case '\'': return SymbolKind::Aircraft;
        case 'X': return SymbolKind::Helicopter;
        case 'g': return SymbolKind::Glider;
        case 'O': return SymbolKind::Balloon;
        case 's': return SymbolKind::Ship;
        case 'Y': return SymbolKind::Yacht;
        case 'C': return SymbolKind::Canoe;
        case '_':
        case 'W': return SymbolKind::Weather;
        case '#': return SymbolKind::Digipeater;
        case '&': return SymbolKind::IGate;
        case '-': return SymbolKind::House;
        case 'h': return SymbolKind::Hospital;
        case 'P': return SymbolKind::Police;
        case 'f': return SymbolKind::Fire;
        case '+':
        case 'a':
        case 'A': return SymbolKind::Aid;
        case 'S':
        case '`': return SymbolKind::Satellite;
        case 'r': return SymbolKind::Antenna;
        case 'n': return SymbolKind::Node;
        case 'I':
        case '?': return SymbolKind::Server;
        case 'B': return SymbolKind::Bbs;
        case 'p':
        case 'R': return SymbolKind::Rover;
        case 'o': return SymbolKind::Emergency;
        default: return SymbolKind::Unknown;
    }
}

inline const char* symbolLabel(char table, char symbol) {
    switch (symbolKind(table, symbol)) {
        case SymbolKind::Car: return "CAR";
        case SymbolKind::Truck: return "TRK";
        case SymbolKind::Van: return "VAN";
        case SymbolKind::Bus: return "BUS";
        case SymbolKind::Jeep: return "JEP";
        case SymbolKind::Motorcycle: return "MOT";
        case SymbolKind::Bicycle: return "BIK";
        case SymbolKind::Walker: return "WLK";
        case SymbolKind::Aircraft: return "AIR";
        case SymbolKind::Helicopter: return "HEL";
        case SymbolKind::Glider: return "GLI";
        case SymbolKind::Balloon: return "BAL";
        case SymbolKind::Ship: return "SHP";
        case SymbolKind::Yacht: return "YAC";
        case SymbolKind::Canoe: return "CAN";
        case SymbolKind::Weather: return "WX";
        case SymbolKind::Digipeater: return "DIG";
        case SymbolKind::IGate: return "IGT";
        case SymbolKind::House: return "HOM";
        case SymbolKind::Hospital: return "HOS";
        case SymbolKind::Police: return "POL";
        case SymbolKind::Fire: return "FIR";
        case SymbolKind::Aid: return "AID";
        case SymbolKind::Satellite: return "SAT";
        case SymbolKind::Antenna: return "ANT";
        case SymbolKind::Node: return "NOD";
        case SymbolKind::Server: return "SRV";
        case SymbolKind::Bbs: return "BBS";
        case SymbolKind::Rover: return "ROV";
        case SymbolKind::Emergency: return "EMG";
        default: return "---";
    }
}

inline const char* symbolName(char table, char symbol) {
    switch (symbolKind(table, symbol)) {
        case SymbolKind::Car: return "Car";
        case SymbolKind::Truck: return "Truck";
        case SymbolKind::Van: return "Van";
        case SymbolKind::Bus: return "Bus";
        case SymbolKind::Jeep: return "Jeep";
        case SymbolKind::Motorcycle: return "Motorcycle";
        case SymbolKind::Bicycle: return "Bicycle";
        case SymbolKind::Walker: return "Walker";
        case SymbolKind::Aircraft: return "Aircraft";
        case SymbolKind::Helicopter: return "Helicopter";
        case SymbolKind::Glider: return "Glider";
        case SymbolKind::Balloon: return "Balloon";
        case SymbolKind::Ship: return "Ship";
        case SymbolKind::Yacht: return "Yacht";
        case SymbolKind::Canoe: return "Canoe";
        case SymbolKind::Weather: return "Weather station";
        case SymbolKind::Digipeater: return "Digipeater";
        case SymbolKind::IGate: return "iGate / gateway";
        case SymbolKind::House: return "House / QTH";
        case SymbolKind::Hospital: return "Hospital";
        case SymbolKind::Police: return "Police";
        case SymbolKind::Fire: return "Fire truck";
        case SymbolKind::Aid: return "Aid station";
        case SymbolKind::Satellite: return "Satellite";
        case SymbolKind::Antenna: return "Antenna";
        case SymbolKind::Node: return "Node";
        case SymbolKind::Server: return "Server";
        case SymbolKind::Bbs: return "BBS";
        case SymbolKind::Rover: return "Rover";
        case SymbolKind::Emergency: return "Emergency";
        default: return "Unknown";
    }
}

}  // namespace aprs

#endif  // APRS_SYMBOLS_H
