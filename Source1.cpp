#include "DeflateInflate.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct Point {
    double x = 0.0;
    double y = 0.0;
};

struct Pin {
    std::string number;
    std::string name;
    std::string net;
    Point position;
};

struct Component {
    std::string reference;
    std::string value = "ASC import";
    bool bottom = false;
    bool hasPlacement = false;
    Point position;
    std::vector<Pin> pins;
};

struct Nail {
    std::string probe;
    std::string net;
    bool bottom = false;
    Point position;
};

struct Board {
    std::vector<Point> outline;
    std::vector<Component> components;
    std::vector<Nail> nails;
};

#include "FzImporter.h"

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string trim(const std::string& value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string normalizeReference(const std::string& reference) {
    if (!reference.empty() && !std::isdigit(static_cast<unsigned char>(reference.back()))) return reference + "0";
    return reference;
}

std::vector<std::string> tokens(const std::string& line) {
    std::istringstream stream(line);
    std::vector<std::string> result;
    std::string token;
    while (stream >> token) result.push_back(token);
    return result;
}

bool parseNumber(const std::string& token, double& value) {
    char* end = nullptr;
    value = std::strtod(token.c_str(), &end);
    return end != token.c_str() && *end == '\0' && std::isfinite(value);
}

std::vector<std::string> readLines(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot open " + path.string());

    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line)) lines.push_back(line);
    if (input.bad()) throw std::runtime_error("Failed while reading " + path.string());
    if (!lines.empty() && lines.front().size() >= 3 &&
        static_cast<unsigned char>(lines.front()[0]) == 0xEF &&
        static_cast<unsigned char>(lines.front()[1]) == 0xBB &&
        static_cast<unsigned char>(lines.front()[2]) == 0xBF) {
        lines.front().erase(0, 3);
    }
    return lines;
}

fs::path findFileInsensitive(const fs::path& directory, const std::string& expected) {
    const auto wanted = lower(expected);
    std::error_code error;
    for (fs::directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) {
        if (lower(it->path().filename().string()) == wanted) return it->path();
    }
    if (error) throw std::runtime_error("Cannot list ASC directory: " + error.message());
    return {};
}

double coordinateScale(const std::vector<std::string>& headerLines) {
    for (const auto& line : headerLines) {
        const auto text = lower(line);
        if (text.find("inch") != std::string::npos) return 25.4;
        if (text.find("millimeter") != std::string::npos || text.find(" mm") != std::string::npos) return 1.0;
        if (text.find("mil") != std::string::npos || text.find("thou") != std::string::npos) return 0.0254;
    }
    // OpenBoardView's ASC importer treats unlabelled tester coordinates as inches.
    return 25.4;
}

bool parseOutline(const fs::path& path, double scale, std::vector<Point>& outline) {
    const auto lines = readLines(path);
    const size_t firstDataLine = std::min<size_t>(8, lines.size());
    for (size_t i = firstDataLine; i < lines.size(); ++i) {
        const auto fields = tokens(lines[i]);
        double x = 0.0;
        double y = 0.0;
        if (fields.size() >= 2 && parseNumber(fields[0], x) && parseNumber(fields[1], y)) {
            outline.push_back({x * scale, -y * scale});
        }
    }
    if (outline.size() > 1 && std::abs(outline.front().x - outline.back().x) < 1e-8 &&
        std::abs(outline.front().y - outline.back().y) < 1e-8) {
        outline.pop_back();
    }
    return outline.size() >= 3;
}

bool parsePins(const fs::path& path, double scale, std::vector<Component>& components) {
    const auto lines = readLines(path);
    const size_t firstDataLine = std::min<size_t>(8, lines.size());
    size_t currentComponent = static_cast<size_t>(-1);
    std::unordered_map<std::string, size_t> componentByName;

    for (size_t lineNumber = firstDataLine; lineNumber < lines.size(); ++lineNumber) {
        const auto text = trim(lines[lineNumber]);
        if (text.empty()) continue;
        const auto fields = tokens(text);
        if (fields.empty()) continue;

        if (lower(fields[0]) == "part") {
            if (fields.size() < 2) throw std::runtime_error(path.string() + ":" + std::to_string(lineNumber + 1) + ": missing part reference");
            const auto reference = normalizeReference(fields[1]);
            const auto existing = componentByName.find(reference);
            if (existing == componentByName.end()) {
                currentComponent = components.size();
                componentByName.emplace(reference, currentComponent);
                components.push_back({});
                components.back().reference = reference;
            } else {
                currentComponent = existing->second;
            }
            for (const auto& field : fields) {
                if (field == "(B)" || lower(field) == "b") components[currentComponent].bottom = true;
                if (field == "(T)" || lower(field) == "t") components[currentComponent].bottom = false;
            }
            continue;
        }

        if (currentComponent == static_cast<size_t>(-1) || fields.size() < 7) continue;
        const size_t coordinateIndex = fields.size() - 5;
        double x = 0.0;
        double y = 0.0;
        if (!parseNumber(fields[coordinateIndex], x) || !parseNumber(fields[coordinateIndex + 1], y)) continue;

        Pin pin;
        pin.number = fields[0];
        for (size_t i = 1; i < coordinateIndex; ++i) {
            if (!pin.name.empty()) pin.name += " ";
            pin.name += fields[i];
        }
        pin.net = fields[coordinateIndex + 3];
        pin.position = {x * scale, -y * scale};
        components[currentComponent].pins.push_back(std::move(pin));
    }
    return !components.empty();
}

bool parseNails(const fs::path& path, double scale, std::vector<Nail>& nails) {
    const auto lines = readLines(path);
    const size_t firstDataLine = std::min<size_t>(7, lines.size());
    for (size_t lineNumber = firstDataLine; lineNumber < lines.size(); ++lineNumber) {
        const auto fields = tokens(lines[lineNumber]);
        if (fields.empty()) continue;

        size_t index = 0;
        std::string probe;
        if (lower(fields[0]) == "n") {
            index = 1;
        } else if (fields[0].size() > 1 && std::tolower(static_cast<unsigned char>(fields[0][0])) == 'n') {
            probe = fields[0].substr(1);
            index = 1;
        }
        if (fields.size() < index + 8) continue;
        if (probe.empty()) probe = fields[index];

        double x = 0.0;
        double y = 0.0;
        if (!parseNumber(fields[index + 1], x) || !parseNumber(fields[index + 2], y)) continue;

        Nail nail;
        nail.probe = probe;
        nail.position = {x * scale, -y * scale};
        const auto side = lower(fields[index + 5]);
        nail.bottom = side == "(b)" || side == "b";
        nail.net = fields[index + 7];
        nails.push_back(std::move(nail));
    }
    return true;
}

void parsePartPlacements(const fs::path& path, double scale, std::vector<Component>& components) {
    const auto lines = readLines(path);
    std::unordered_map<std::string, size_t> componentByName;
    for (size_t i = 0; i < components.size(); ++i) componentByName.emplace(components[i].reference, i);

    const size_t firstDataLine = std::min<size_t>(4, lines.size());
    for (size_t lineNumber = firstDataLine; lineNumber < lines.size(); ++lineNumber) {
        const auto fields = tokens(lines[lineNumber]);
        double x = 0.0;
        double y = 0.0;
        if (fields.size() < 3 || !parseNumber(fields[1], x) || !parseNumber(fields[2], y)) continue;
        const auto found = componentByName.find(normalizeReference(fields[0]));
        if (found == componentByName.end()) continue;

        auto& component = components[found->second];
        component.position = {x * scale, -y * scale};
        component.hasPlacement = true;
    }
}

Board importAsc(const fs::path& input) {
    fs::path directory = fs::is_directory(input) ? input : input.parent_path();
    if (directory.empty()) directory = fs::current_path();

    const auto formatPath = findFileInsensitive(directory, "format.asc");
    const auto pinsPath = findFileInsensitive(directory, "pins.asc");
    const auto nailsPath = findFileInsensitive(directory, "nails.asc");
    const auto partsPath = findFileInsensitive(directory, "parts.asc");
    if (formatPath.empty() || pinsPath.empty() || nailsPath.empty()) {
        throw std::runtime_error("An OpenBoardView ASC board needs FORMAT.ASC, PINS.ASC, and NAILS.ASC in the same directory.");
    }

    std::vector<std::string> header;
    const auto appendHeader = [&header](const fs::path& path) {
        const auto lines = readLines(path);
        for (size_t i = 0; i < std::min<size_t>(8, lines.size()); ++i) header.push_back(lines[i]);
    };
    appendHeader(formatPath);
    appendHeader(pinsPath);
    appendHeader(nailsPath);
    const double scale = coordinateScale(header);

    Board board;
    if (!parseOutline(formatPath, scale, board.outline)) {
        throw std::runtime_error("FORMAT.ASC contains no usable board outline points.");
    }
    if (!parsePins(pinsPath, scale, board.components)) {
        throw std::runtime_error("PINS.ASC contains no component pin records.");
    }
    parseNails(nailsPath, scale, board.nails);
    if (!partsPath.empty()) parsePartPlacements(partsPath, scale, board.components);
    return board;
}

std::string quote(const std::string& value) {
    std::string result = "\"";
    for (const char ch : value) {
        if (ch == '\\' || ch == '\"') result += '\\';
        if (ch == '\n' || ch == '\r') result += ' ';
        else result += ch;
    }
    result += '\"';
    return result;
}

std::string uuid(unsigned long long id) {
    std::ostringstream result;
    result << "00000000-0000-4000-8000-" << std::hex << std::setw(12) << std::setfill('0') << id;
    return result.str();
}

struct BoardTransform {
    double offsetX = 0.0;
    double offsetY = 0.0;
    Point apply(const Point& point) const { return {point.x + offsetX, point.y + offsetY}; }
};

BoardTransform makeTransform(const Board& board) {
    double minX = board.outline.front().x;
    double minY = board.outline.front().y;
    for (const auto& point : board.outline) {
        minX = std::min(minX, point.x);
        minY = std::min(minY, point.y);
    }
    for (const auto& component : board.components) {
        for (const auto& pin : component.pins) {
            minX = std::min(minX, pin.position.x);
            minY = std::min(minY, pin.position.y);
        }
    }
    for (const auto& nail : board.nails) {
        minX = std::min(minX, nail.position.x);
        minY = std::min(minY, nail.position.y);
    }
    return {10.0 - minX, 10.0 - minY};
}

void writeFootprintHeader(std::ostream& out, const std::string& reference, const std::string& value,
                          const std::string& layer, const Point& at, bool throughHole, unsigned long long& id) {
    out << "  (footprint \"BoardView:Imported\" (layer " << quote(layer) << ") (at " << at.x << " " << at.y << ")\n";
    out << "    (property \"Reference\" " << quote(reference) << " (at 0 -1.27 0) (layer " << quote(layer == "B.Cu" ? "B.SilkS" : "F.SilkS")
        << ") (uuid " << quote(uuid(id++)) << "))\n";
    out << "    (property \"Value\" " << quote(value) << " (at 0 1.27 0) (layer \"F.Fab\") (hide yes) (uuid " << quote(uuid(id++)) << "))\n";
    out << (throughHole ? "    (attr through_hole)\n" : "    (attr smd)\n");
}

int netId(const std::string& name, std::map<std::string, int>& nets) {
    if (name.empty() || name == "(NC)" || name == "UNCONNECTED" || name == "?") return 0;
    const auto found = nets.find(name);
    if (found != nets.end()) return found->second;
    const int next = static_cast<int>(nets.size()) + 1;
    nets.emplace(name, next);
    return next;
}

void writeBoard(const Board& board, const fs::path& outputPath) {
    std::ofstream out(outputPath, std::ios::binary);
    if (!out) throw std::runtime_error("Cannot create " + outputPath.string());
    out << std::fixed << std::setprecision(4);

    const auto transform = makeTransform(board);
    std::map<std::string, int> nets;
    for (const auto& component : board.components) {
        for (const auto& pin : component.pins) netId(pin.net, nets);
    }
    for (const auto& nail : board.nails) netId(nail.net, nets);

    out << "(kicad_pcb (version 20241229) (generator \"fz2kicad\")\n";
    out << "  (general (thickness 1.6))\n  (paper \"A4\")\n";
    out << "  (layers\n"
        << "    (0 \"F.Cu\" signal) (31 \"B.Cu\" signal)\n"
        << "    (34 \"B.Paste\" user \"b.paste\") (35 \"F.Paste\" user \"f.paste\")\n"
        << "    (36 \"B.SilkS\" user \"b.silkscreen\") (37 \"F.SilkS\" user \"f.silkscreen\")\n"
        << "    (38 \"B.Mask\" user \"b.mask\") (39 \"F.Mask\" user \"f.mask\")\n"
        << "    (44 \"Edge.Cuts\" user) (47 \"B.Fab\" user \"b.fab\") (48 \"F.Fab\" user \"f.fab\")\n"
        << "  )\n  (setup (pad_to_mask_clearance 0))\n  (net 0 \"\")\n";
    for (const auto& net : nets) out << "  (net " << net.second << " " << quote(net.first) << ")\n";

    unsigned long long id = 1;
    for (const auto& component : board.components) {
        if (component.pins.empty()) continue;
        Point center{};
        for (const auto& pin : component.pins) {
            center.x += pin.position.x;
            center.y += pin.position.y;
        }
        center.x /= component.pins.size();
        center.y /= component.pins.size();
        center = transform.apply(center);
        const std::string layer = component.bottom ? "B.Cu" : "F.Cu";
        const std::string copper = component.bottom ? "B.Cu" : "F.Cu";
        const std::string paste = component.bottom ? "B.Paste" : "F.Paste";
        const std::string mask = component.bottom ? "B.Mask" : "F.Mask";
        writeFootprintHeader(out, component.reference, component.value, layer, center, false, id);
        for (const auto& pin : component.pins) {
            const auto position = Point{pin.position.x - (center.x - transform.offsetX), pin.position.y - (center.y - transform.offsetY)};
            const int net = netId(pin.net, nets);
            out << "    (pad " << quote(pin.number.empty() ? "1" : pin.number) << " smd circle (at " << position.x << " " << position.y
                << ") (size 0.6 0.6) (layers " << quote(copper) << " " << quote(paste) << " " << quote(mask) << ")";
            if (!pin.name.empty()) out << " (pinfunction " << quote(pin.name) << ") (pintype \"passive\")";
            if (net != 0) out << " (net " << net << " " << quote(pin.net) << ")";
            out << " (uuid " << quote(uuid(id++)) << "))\n";
        }
        out << "  )\n";
    }

    size_t testPoint = 1;
    for (const auto& nail : board.nails) {
        const auto position = transform.apply(nail.position);
        const std::string layer = nail.bottom ? "B.Cu" : "F.Cu";
        const int net = netId(nail.net, nets);
        const auto reference = "TP" + std::to_string(testPoint++);
        writeFootprintHeader(out, reference, "Test point", layer, position, true, id);
        out << "    (pad " << quote(nail.probe.empty() ? "1" : nail.probe) << " thru_hole circle (at 0 0) (size 1 1) (drill 0.4) (layers \"*.Cu\" \"*.Mask\")";
        if (net != 0) out << " (net " << net << " " << quote(nail.net) << ")";
        out << " (uuid " << quote(uuid(id++)) << "))\n  )\n";
    }

    for (size_t i = 0; i < board.outline.size(); ++i) {
        const auto start = transform.apply(board.outline[i]);
        const auto end = transform.apply(board.outline[(i + 1) % board.outline.size()]);
        out << "  (gr_line (start " << start.x << " " << start.y << ") (end " << end.x << " " << end.y
            << ") (stroke (width 0.05) (type default)) (layer \"Edge.Cuts\") (uuid " << quote(uuid(id++)) << "))\n";
    }
    out << ")\n";
    if (!out) throw std::runtime_error("Failed while writing " + outputPath.string());
}

struct SchematicPinGeometry {
    Point position;
    int orientation = 0;
    int labelOrientation = 0;
};

struct SchematicLibrarySymbol {
    std::string name;
    double bodyWidth = 0.0;
    double bodyHeight = 0.0;
    std::vector<SchematicPinGeometry> pins;
    std::vector<Pin> sourcePins;
};

SchematicLibrarySymbol makeSchematicSymbol(const Component& component, size_t index) {
    static constexpr double pinSpacing = 2.54;
    static constexpr double pinLength = 3.81;
    const size_t pinCount = component.pins.size();
    const bool fourSided = pinCount > 150;
    const size_t pinsPerSide = fourSided ? (pinCount + 3) / 4 : (pinCount + 1) / 2;

    SchematicLibrarySymbol symbol;
    symbol.name = "Imported_" + std::to_string(index + 1);
    if (fourSided) {
        symbol.bodyWidth = std::max(15.24, pinsPerSide * pinSpacing + 5.08);
        symbol.bodyHeight = symbol.bodyWidth;
    } else {
        symbol.bodyWidth = std::max(12.7, pinsPerSide * pinSpacing + 5.08);
        symbol.bodyHeight = 25.4;
    }
    symbol.pins.reserve(pinCount);
    symbol.sourcePins = component.pins;

    for (size_t i = 0; i < pinCount; ++i) {
        SchematicPinGeometry pin;
        if (!fourSided) {
            const bool top = i < pinsPerSide;
            const size_t slot = top ? i : i - pinsPerSide;
            const double x = (static_cast<double>(slot) - (static_cast<double>(pinsPerSide) - 1.0) / 2.0) * pinSpacing;
            pin.position = {x, top ? symbol.bodyHeight / 2.0 + pinLength : -symbol.bodyHeight / 2.0 - pinLength};
            pin.orientation = top ? 270 : 90;
            pin.labelOrientation = 90;
        } else {
            const size_t side = i / pinsPerSide;
            const size_t slot = i % pinsPerSide;
            const double offset = (static_cast<double>(slot) + 0.5) * pinSpacing;
            switch (side) {
            case 0:
                pin.position = {-symbol.bodyWidth / 2.0 - pinLength, symbol.bodyHeight / 2.0 - offset};
                pin.orientation = 0;
                pin.labelOrientation = 180;
                break;
            case 1:
                pin.position = {-symbol.bodyWidth / 2.0 + offset, -symbol.bodyHeight / 2.0 - pinLength};
                pin.orientation = 90;
                pin.labelOrientation = 90;
                break;
            case 2:
                pin.position = {symbol.bodyWidth / 2.0 + pinLength, symbol.bodyHeight / 2.0 - offset};
                pin.orientation = 180;
                pin.labelOrientation = 0;
                break;
            default:
                pin.position = {-symbol.bodyWidth / 2.0 + offset, symbol.bodyHeight / 2.0 + pinLength};
                pin.orientation = 270;
                pin.labelOrientation = 90;
                break;
            }
        }
        symbol.pins.push_back(pin);
    }
    return symbol;
}

bool isUnconnectedNet(const std::string& name) {
    const auto normalized = lower(trim(name));
    return normalized.empty() || normalized == "?" || normalized == "(nc)" || normalized == "unconnected";
}

void writeSchematic(const Board& board, const fs::path& outputPath) {
    std::vector<SchematicLibrarySymbol> symbols;
    std::vector<size_t> componentSymbols(board.components.size(), static_cast<size_t>(-1));
    size_t placedComponentCount = 0;

    for (size_t componentIndex = 0; componentIndex < board.components.size(); ++componentIndex) {
        const auto& component = board.components[componentIndex];
        if (component.pins.empty()) continue;
        ++placedComponentCount;
        const size_t symbolIndex = symbols.size();
        symbols.push_back(makeSchematicSymbol(component, symbolIndex));
        componentSymbols[componentIndex] = symbolIndex;
    }
    if (placedComponentCount == 0) throw std::runtime_error("Cannot create a schematic without component pins.");

    struct Placement {
        Point center;
        size_t symbolIndex = 0;
    };
    std::vector<Placement> placements(board.components.size());
    bool useBoardPlacements = true;
    double minBoardX = std::numeric_limits<double>::max();
    double minBoardY = std::numeric_limits<double>::max();
    for (size_t i = 0; i < board.components.size(); ++i) {
        if (componentSymbols[i] == static_cast<size_t>(-1)) continue;
        if (!board.components[i].hasPlacement) {
            useBoardPlacements = false;
            continue;
        }
        minBoardX = std::min(minBoardX, board.components[i].position.x);
        minBoardY = std::min(minBoardY, board.components[i].position.y);
    }
    const double pageWidthLimit = std::max(600.0, std::min(5000.0, std::sqrt(static_cast<double>(placedComponentCount)) * 55.0));
    const double margin = 25.0;
    const double gap = 12.0;
    double cursorX = margin;
    double cursorY = margin;
    double rowHeight = 0.0;

    for (size_t i = 0; i < board.components.size(); ++i) {
        const size_t symbolIndex = componentSymbols[i];
        if (symbolIndex == static_cast<size_t>(-1)) continue;
        const auto& component = board.components[i];
        const auto& symbol = symbols[symbolIndex];
        const double cellWidth = symbol.bodyWidth + 2.0 * (3.81 + 12.0);
        const double cellHeight = symbol.bodyHeight + 2.0 * (3.81 + 12.0);
        if (useBoardPlacements) {
            constexpr double boardPlacementScale = 10.0;
            placements[i] = {{margin + (component.position.x - minBoardX) * boardPlacementScale,
                              margin + (component.position.y - minBoardY) * boardPlacementScale}, symbolIndex};
            continue;
        }
        if (cursorX > margin && cursorX + cellWidth > pageWidthLimit) {
            cursorX = margin;
            cursorY += rowHeight + gap;
            rowHeight = 0.0;
        }
        placements[i] = {{cursorX + cellWidth / 2.0, cursorY + cellHeight / 2.0}, symbolIndex};
        cursorX += cellWidth + gap;
        rowHeight = std::max(rowHeight, cellHeight);
    }

    std::set<std::pair<long long, long long>> occupiedPinLocations;
    constexpr double schematicCoordinatePrecision = 10000.0;
    constexpr size_t maxPinOffsetSteps = 10000;
    for (size_t componentIndex = 0; componentIndex < board.components.size(); ++componentIndex) {
        const size_t symbolIndex = componentSymbols[componentIndex];
        if (symbolIndex == static_cast<size_t>(-1)) continue;
        auto& symbol = symbols[symbolIndex];
        const auto& center = placements[componentIndex].center;
        double pinOffset = 0.0;
        for (size_t attempt = 0;; ++attempt) {
            std::vector<std::pair<long long, long long>> locations;
            std::set<std::pair<long long, long long>> componentLocations;
            locations.reserve(symbol.pins.size());
            bool overlaps = false;
            for (const auto& pin : symbol.pins) {
                const auto location = std::make_pair(
                    std::llround((center.x + pin.position.x + pinOffset) * schematicCoordinatePrecision),
                    std::llround((center.y - pin.position.y) * schematicCoordinatePrecision));
                if (occupiedPinLocations.count(location) != 0 || !componentLocations.insert(location).second) {
                    overlaps = true;
                    break;
                }
                locations.push_back(location);
            }
            if (!overlaps) {
                for (const auto& location : locations) occupiedPinLocations.insert(location);
                for (auto& pin : symbol.pins) pin.position.x += pinOffset;
                break;
            }
            if (attempt >= maxPinOffsetSteps) {
                throw std::runtime_error("Cannot place schematic pins without overlapping electrical anchors.");
            }
            pinOffset = static_cast<double>(attempt + 1) / schematicCoordinatePrecision;
        }
    }

    std::ofstream out(outputPath, std::ios::binary);
    if (!out) throw std::runtime_error("Cannot create " + outputPath.string());
    out << std::fixed << std::setprecision(4);
    unsigned long long id = 1;
    const auto rootUuid = uuid(id++);
    const auto projectName = outputPath.stem().string();

    out << "(kicad_sch (version 20250114) (generator \"eeschema\") (generator_version \"9.0\")\n"
        << "  (uuid " << quote(rootUuid) << ")\n"
        << "  (paper \"A0\")\n"
        << "  (title_block (title " << quote(projectName + " boardview connectivity") << "))\n"
        << "  (lib_symbols\n";

    for (const auto& symbol : symbols) {
        out << "    (symbol " << quote("BoardView:" + symbol.name) << " (pin_names (offset 1.016)) (exclude_from_sim no) (in_bom yes) (on_board yes)\n"
            << "      (property \"Reference\" \"U\" (at 0 " << symbol.bodyHeight / 2.0 + 8.0 << " 0) (effects (font (size 1.27 1.27))))\n"
            << "      (property \"Value\" \"Imported\" (at 0 " << -symbol.bodyHeight / 2.0 - 8.0 << " 0) (effects (font (size 1.27 1.27))))\n"
            << "      (property \"Footprint\" \"\" (at 0 0 0) (effects (font (size 1.27 1.27)) (hide yes)))\n"
            << "      (property \"Datasheet\" \"~\" (at 0 0 0) (effects (font (size 1.27 1.27)) (hide yes)))\n"
            << "      (property \"Description\" \"Generated from boardview pin and net data\" (at 0 0 0) (effects (font (size 1.27 1.27)) (hide yes)))\n"
            << "      (symbol " << quote(symbol.name + "_1_1") << "\n"
            << "        (rectangle (start " << -symbol.bodyWidth / 2.0 << " " << symbol.bodyHeight / 2.0 << ") (end "
            << symbol.bodyWidth / 2.0 << " " << -symbol.bodyHeight / 2.0
            << ") (stroke (width 0.254) (type default)) (fill (type background)))\n";

        for (size_t i = 0; i < symbol.pins.size(); ++i) {
            const auto& pinData = symbol.pins[i];
            const auto& sourcePin = symbol.sourcePins[i];
            const std::string number = sourcePin.number.empty() ? "1" : sourcePin.number;
            const std::string name = sourcePin.name.empty() ? "~" : sourcePin.name;
            out << "        (pin passive line (at " << pinData.position.x << " " << pinData.position.y << " " << pinData.orientation
                << ") (length 3.81) (name " << quote(name) << " (effects (font (size 1.27 1.27)))) (number " << quote(number)
                << " (effects (font (size 1.27 1.27)))))\n";
        }
        out << "      )\n      (embedded_fonts no)\n    )\n";
    }
    out << "  )\n";

    for (size_t componentIndex = 0; componentIndex < board.components.size(); ++componentIndex) {
        const size_t symbolIndex = componentSymbols[componentIndex];
        if (symbolIndex == static_cast<size_t>(-1)) continue;
        const auto& component = board.components[componentIndex];
        const auto& symbol = symbols[symbolIndex];
        const auto& placement = placements[componentIndex];
        const auto instanceUuid = uuid(id++);
        const double referenceY = placement.center.y - symbol.bodyHeight / 2.0 - 8.0;
        const double valueY = placement.center.y + symbol.bodyHeight / 2.0 + 8.0;

        out << "  (symbol (lib_id " << quote("BoardView:" + symbol.name) << ") (at " << placement.center.x << " " << placement.center.y
            << " 0) (unit 1) (exclude_from_sim no) (in_bom yes) (on_board yes) (dnp no) (uuid " << quote(instanceUuid) << ")\n"
            << "    (property \"Reference\" " << quote(component.reference) << " (at " << placement.center.x << " " << referenceY
            << " 0) (effects (font (size 1.27 1.27)) (justify left bottom)))\n"
            << "    (property \"Value\" " << quote(component.value) << " (at " << placement.center.x << " " << valueY
            << " 0) (effects (font (size 1.27 1.27)) (justify left top)))\n";
        for (const auto& pin : component.pins) {
            const std::string number = pin.number.empty() ? "1" : pin.number;
            out << "    (pin " << quote(number) << " (uuid " << quote(uuid(id++)) << "))\n";
        }
        out << "    (instances (project " << quote(projectName) << " (path " << quote("/" + rootUuid + "/" + instanceUuid)
            << " (reference " << quote(component.reference) << ") (unit 1))))\n  )\n";

        for (size_t pinIndex = 0; pinIndex < component.pins.size(); ++pinIndex) {
            const auto& pin = component.pins[pinIndex];
            const auto& geometry = symbol.pins[pinIndex];
            const double x = placement.center.x + geometry.position.x;
            const double y = placement.center.y - geometry.position.y;
            if (isUnconnectedNet(pin.net)) {
                out << "  (no_connect (at " << x << " " << y << ") (uuid " << quote(uuid(id++)) << "))\n";
            } else {
                out << "  (global_label " << quote(trim(pin.net)) << " (shape bidirectional) (at " << x << " " << y << " "
                    << geometry.labelOrientation << ") (effects (font (size 1.27 1.27)) (justify left bottom)) (uuid "
                    << quote(uuid(id++)) << "))\n";
            }
        }
    }

    out << "  (sheet_instances (path \"/\" (page \"1\")))\n)\n";
    if (!out) throw std::runtime_error("Failed while writing " + outputPath.string());
}

void writeProject(const fs::path& outputPath) {
    std::ofstream out(outputPath, std::ios::binary);
    if (!out) throw std::runtime_error("Cannot create " + outputPath.string());
    out << "{}\n";
    if (!out) throw std::runtime_error("Failed while writing " + outputPath.string());
}

void printUsage() {
    std::cout << "Usage: fz2kicad <ASC file, FZ file, or directory> [output.kicad_pcb] [--fz-key \"0x..., ...\"]\n"
              << "Imports OpenBoardView ASC bundles or FZ files into KiCad PCB and schematic formats.\n"
              << "Encrypted FZ files use the built-in key when possible; other keys can be supplied with --fz-key.\n";
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        std::vector<std::string> positional;
        std::string fzKeyText;
        bool showHelp = false;
        for (int i = 1; i < argc; ++i) {
            const std::string argument = argv[i];
            if (argument == "--help" || argument == "-h") {
                showHelp = true;
            } else if (argument == "--fz-key") {
                if (i + 1 >= argc) throw std::runtime_error("--fz-key requires 44 comma- or whitespace-separated hexadecimal words.");
                fzKeyText = argv[++i];
            } else if (argument.rfind("--", 0) == 0) {
                throw std::runtime_error("Unknown option: " + argument);
            } else {
                positional.push_back(argument);
            }
        }
        if (showHelp) {
            printUsage();
            return 0;
        }
        if (positional.empty() || positional.size() > 2) {
            printUsage();
            return 2;
        }

        const fs::path input = fs::u8path(positional[0]);
        const bool isFz = !fs::is_directory(input) && lower(input.extension().string()) == ".fz";
        const auto board = isFz ? importFz(input, fzKeyText) : importAsc(input);
        fs::path output;
        if (positional.size() == 2) {
            output = fs::u8path(positional[1]);
        } else {
            const auto directory = fs::is_directory(input) ? input : input.parent_path();
            const auto name = fs::is_directory(input) ? input.filename().string() : input.stem().string();
            output = (directory.empty() ? fs::current_path() : directory) / (name + ".kicad_pcb");
        }
        writeBoard(board, output);
        auto schematicOutput = output;
        schematicOutput.replace_extension(".kicad_sch");
        writeSchematic(board, schematicOutput);
        auto projectOutput = output;
        projectOutput.replace_extension(".kicad_pro");
        writeProject(projectOutput);
        std::cout << "Wrote " << output.string() << " (" << board.components.size() << " components, "
                  << board.nails.size() << " test points, " << board.outline.size() << " outline vertices).\n"
                  << "Wrote " << schematicOutput.string() << " (boardview connectivity schematic).\n"
                  << "Wrote " << projectOutput.string() << " (KiCad project).\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Conversion failed: " << error.what() << "\n";
        return 1;
    }
}
