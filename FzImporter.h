#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fz_import_detail {

inline std::string trimLine(const std::string& value) {
	const auto first = value.find_first_not_of(" \t\r\n");
	if (first == std::string::npos) return {};
	const auto last = value.find_last_not_of(" \t\r\n");
	return value.substr(first, last - first + 1);
}

inline bool parseCoordinate(const std::string& token, double& value) {
	char* end = nullptr;
	value = std::strtod(token.c_str(), &end);
	return end != token.c_str() && *end == '\0' && std::isfinite(value);
}

inline std::vector<std::string> splitFields(const std::string& line, char delimiter) {
	std::vector<std::string> fields;
	size_t start = 0;
	for (;;) {
		const auto end = line.find(delimiter, start);
		fields.push_back(line.substr(start, end == std::string::npos ? end : end - start));
		if (end == std::string::npos) return fields;
		start = end + 1;
	}
}

inline void validateKey(const std::array<std::uint32_t, 44>& key) {
	static constexpr std::array<std::uint8_t, 44> expectedParity = {
		0, 1, 1, 0, 1, 0, 1, 0, 0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 0, 1, 0, 0,
		0, 1, 1, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 1, 1, 0, 1};
	for (size_t i = 0; i < key.size(); ++i) {
		std::uint32_t folded = key[i];
		folded ^= folded >> 16;
		folded ^= folded >> 8;
		folded ^= folded >> 4;
		folded ^= folded >> 2;
		folded ^= folded >> 1;
		if (((~folded) & 1U) != expectedParity[i]) throw std::runtime_error("FZ key parity check failed.");
	}
}

inline std::array<std::uint32_t, 44> parseKey(const std::string& text) {
	std::string normalized = text;
	std::replace(normalized.begin(), normalized.end(), ',', ' ');
	std::istringstream stream(normalized);
	std::array<std::uint32_t, 44> key{};
	std::string token;
	size_t count = 0;
	while (stream >> token) {
		if (count == key.size()) throw std::runtime_error("FZ key must contain exactly 44 hexadecimal 32-bit words.");
		size_t consumed = 0;
		unsigned long long value = 0;
		try {
			value = std::stoull(token, &consumed, 16);
		} catch (const std::exception&) {
			throw std::runtime_error("FZ key contains an invalid hexadecimal word.");
		}
		if (consumed != token.size() || value > 0xffffffffULL) {
			throw std::runtime_error("FZ key contains an invalid 32-bit hexadecimal word.");
		}
		key[count++] = static_cast<std::uint32_t>(value);
	}
	if (count != key.size()) throw std::runtime_error("FZ key must contain exactly 44 hexadecimal 32-bit words.");
	validateKey(key);
	return key;
}

inline std::array<std::uint32_t, 44> builtInKey() {
	static constexpr std::array<std::uint32_t, 44> key = {
		0x25d8d248U, 0xe1502405U, 0x56b5d486U, 0x69213fe0U,
		0xa22490ecU, 0x01fdd9faU, 0x0681955fU, 0x0fac202dU,
		0xdac9eeb4U, 0xf6024abaU, 0xcd8b4cc6U, 0x9f307c8eU,
		0x4ab8fad7U, 0x232f967dU, 0x5e8666a3U, 0xde966d4bU,
		0xc64bfb1cU, 0xea7fb092U, 0x1a751a7eU, 0x37e8f0bcU,
		0x3359c8f3U, 0x969ac22bU, 0x610f5804U, 0xd99d10e6U,
		0xc58d54d6U, 0x1f9aea8bU, 0x8e388c1aU, 0xe4f7d2edU,
		0x3e5da1f6U, 0xedfe818aU, 0x7252b016U, 0xb503a170U,
		0xc4128fb6U, 0x2c93ceebU, 0x53539a6eU, 0xdacf7668U,
		0x3ab78e52U, 0x8ee9d815U, 0x7043f799U, 0xc6a05dcfU,
		0x727f1da2U, 0x0dfd983bU, 0x78c53872U, 0x00945692U};
	validateKey(key);
	return key;
}

inline std::uint32_t rotateLeft(std::uint32_t value, std::uint32_t amount) {
	const auto shift = amount & 31U;
	return shift == 0 ? value : (value << shift) | (value >> (32U - shift));
}

inline void decrypt(std::vector<std::uint8_t>& data, const std::array<std::uint32_t, 44>& key) {
	std::uint32_t a = 0;
	std::uint32_t b = 0;
	std::uint32_t c = 0;
	std::uint32_t d = 0;
	std::array<std::uint8_t, 16> history{};
	for (auto& byte : data) {
		b += key[0];
		d += key[1];
		for (std::uint32_t i = 1; i <= 20; ++i) {
			const auto t = rotateLeft(b * (2U * b + 1U), 5);
			const auto u = rotateLeft(d * (2U * d + 1U), 5);
			a = rotateLeft(a ^ t, u) + key[2 * i];
			c = rotateLeft(c ^ u, t) + key[2 * i + 1];
			const auto previousA = a;
			a = b;
			b = c;
			c = d;
			d = previousA;
		}
		a += key[42];
		c += key[43];
		const auto encryptedByte = byte;
		byte = static_cast<std::uint8_t>(encryptedByte ^ (a & 0xffU));
		std::move(history.begin() + 1, history.end(), history.begin());
		history.back() = encryptedByte;
		const auto word = [&history](size_t offset) {
			return static_cast<std::uint32_t>(history[offset]) |
				(static_cast<std::uint32_t>(history[offset + 1]) << 8) |
				(static_cast<std::uint32_t>(history[offset + 2]) << 16) |
				(static_cast<std::uint32_t>(history[offset + 3]) << 24);
		};
		a = word(0);
		b = word(4);
		c = word(8);
		d = word(12);
	}
}

inline bool hasZlibHeader(const std::vector<std::uint8_t>& data) {
	if (data.size() < 6) return false;
	const unsigned cmf = data[4];
	const unsigned flg = data[5];
	return (cmf & 0x0fU) == 8 && (cmf >> 4) <= 7 && ((cmf << 8) | flg) % 31 == 0 && (flg & 0x20U) == 0;
}

inline std::vector<std::uint8_t> readBytes(const std::filesystem::path& path) {
	std::ifstream input(path, std::ios::binary);
	if (!input) throw std::runtime_error("Cannot open " + path.string());
	std::vector<std::uint8_t> bytes;
	char byte = 0;
	while (input.get(byte)) bytes.push_back(static_cast<std::uint8_t>(static_cast<unsigned char>(byte)));
	if (input.bad()) throw std::runtime_error("Failed while reading " + path.string());
	return bytes;
}

} // namespace fz_import_detail

inline Board importFz(const std::filesystem::path& path, const std::string& keyText) {
	using namespace fz_import_detail;
	auto bytes = readBytes(path);
	if (bytes.size() < 8) throw std::runtime_error("FZ file is too small to contain its compressed sections.");
	if (!hasZlibHeader(bytes)) {
		const auto key = keyText.empty() ? builtInKey() : parseKey(keyText);
		decrypt(bytes, key);
		if (!hasZlibHeader(bytes)) {
			throw std::runtime_error(keyText.empty()
				? "The built-in FZ key did not decrypt this file; provide --fz-key."
				: "The supplied FZ key did not decrypt this file; verify --fz-key.");
		}
	}

	const size_t size = bytes.size();
	const std::uint32_t descriptionLength = static_cast<std::uint32_t>(bytes[size - 4]) |
		(static_cast<std::uint32_t>(bytes[size - 3]) << 8) |
		(static_cast<std::uint32_t>(bytes[size - 2]) << 16) |
		(static_cast<std::uint32_t>(bytes[size - 1]) << 24);
	if (descriptionLength > size || size > std::numeric_limits<size_t>::max() - 16) {
		throw std::runtime_error("FZ section lengths are invalid.");
	}
	const size_t contentLength = size - descriptionLength + 4;
	const size_t descriptionOffset = contentLength;
	if (contentLength < 6 || descriptionOffset > size + 8 || descriptionLength > size + 8 - descriptionOffset) {
		throw std::runtime_error("FZ description section is outside the file.");
	}
	bytes.resize(size + 16, 0);
	const auto contentBytes = fzinflate::inflateZlib(bytes, 4, contentLength);
	const auto descriptionBytes = fzinflate::inflateZlib(bytes, descriptionOffset, descriptionLength);
	if (descriptionBytes.empty()) throw std::runtime_error("FZ description section is empty.");
	std::string content(contentBytes.begin(), contentBytes.end());
	const std::string descriptions(descriptionBytes.begin(), descriptionBytes.end());
	std::replace(content.begin(), content.end(), ',', '.');

	Board board;
	std::unordered_map<std::string, size_t> componentByName;
	int currentBlock = 0;
	double unitMultiplier = 1.0;
	double minX = std::numeric_limits<double>::max();
	double minY = std::numeric_limits<double>::max();
	double maxX = std::numeric_limits<double>::lowest();
	double maxY = std::numeric_limits<double>::lowest();
	std::istringstream contentStream(content);
	std::string line;
	while (std::getline(contentStream, line)) {
		line = trimLine(line);
		if (line.empty()) continue;
		if (line.rfind("A!UNIT!", 0) == 0) {
			const auto fields = splitFields(line, '!');
			if (fields.size() > 2) {
				const auto unit = fields[2];
				if (unit == "millimeters" || unit == "mm") unitMultiplier = 25.4;
				else if (unit == "inches" || unit == "inch") unitMultiplier = 1000.0;
				else unitMultiplier = 1.0;
			}
			continue;
		}
		if (line.rfind("A!", 0) == 0) {
			const auto block = line.substr(2);
			if (block.rfind("REFDES", 0) == 0) currentBlock = 1;
			else if (block.rfind("NET_NAME", 0) == 0) currentBlock = 2;
			else if (block.rfind("TESTVIA", 0) == 0) currentBlock = 3;
			else currentBlock = 0;
			continue;
		}
		if (line.rfind("S!", 0) != 0) continue;
		const auto fields = splitFields(line.substr(2), '!');
		if (currentBlock == 1) {
			if (fields.size() < 4 || fields[0].empty()) continue;
			if (componentByName.find(fields[0]) != componentByName.end()) {
				throw std::runtime_error("FZ file contains a duplicate component reference: " + fields[0]);
			}
			const size_t index = board.components.size();
			componentByName.emplace(fields[0], index);
			board.components.push_back({});
			auto& component = board.components.back();
			component.reference = fields[0].empty() || !std::isdigit(static_cast<unsigned char>(fields[0].back()))
				? fields[0] + "0" : fields[0];
			component.value = "FZ import";
			component.bottom = fields[3] == "YES";
			if (fields.size() >= 7) {
				double x = 0.0;
				double y = 0.0;
				if (parseCoordinate(fields[5], x) && parseCoordinate(fields[6], y)) {
					component.position = {x * unitMultiplier * 0.0254, -y * unitMultiplier * 0.0254};
					component.hasPlacement = true;
				}
			}
		} else if (currentBlock == 2) {
			if (fields.size() < 8) continue;
			const auto component = componentByName.find(fields[1]);
			if (component == componentByName.end()) throw std::runtime_error("FZ pin refers to an unknown component: " + fields[1]);
			double x = 0.0;
			double y = 0.0;
			if (!parseCoordinate(fields[4], x) || !parseCoordinate(fields[5], y)) throw std::runtime_error("FZ pin has invalid coordinates.");
			Pin pin;
			pin.number = fields[2].empty() || fields[2] == "0" ? fields[3] : fields[2];
			pin.name = fields[3];
			pin.net = fields[0];
			pin.position = {x * unitMultiplier * 0.0254, -y * unitMultiplier * 0.0254};
			minX = std::min(minX, pin.position.x);
			minY = std::min(minY, pin.position.y);
			maxX = std::max(maxX, pin.position.x);
			maxY = std::max(maxY, pin.position.y);
			board.components[component->second].pins.push_back(std::move(pin));
		} else if (currentBlock == 3 && fields.size() >= 9 && fields[0] == "Y") {
			double x = 0.0;
			double y = 0.0;
			if (!parseCoordinate(fields[5], x) || !parseCoordinate(fields[6], y)) throw std::runtime_error("FZ test point has invalid coordinates.");
			Nail nail;
			nail.net = fields[1];
			nail.probe = fields[3];
			nail.bottom = fields[7] == "B";
			nail.position = {x * unitMultiplier * 0.0254, -y * unitMultiplier * 0.0254};
			board.nails.push_back(std::move(nail));
		}
	}

	std::istringstream descriptionStream(descriptions);
	std::string descriptionLine;
	size_t descriptionLineNumber = 0;
	while (std::getline(descriptionStream, descriptionLine)) {
		if (descriptionLineNumber++ < 2) continue;
		const auto fields = splitFields(trimLine(descriptionLine), '\t');
		if (fields.size() < 4 || fields[0].empty() || fields[0][0] == 's' || fields[1].empty()) continue;
		std::istringstream locations(fields[3]);
		std::string reference;
		while (locations >> reference) {
			const auto component = componentByName.find(reference);
			if (component != componentByName.end()) board.components[component->second].value = fields[1];
		}
	}

	if (board.components.empty() || minX == std::numeric_limits<double>::max()) {
		throw std::runtime_error("FZ data contains no component pin records.");
	}

	const double margin = 20.0 * unitMultiplier * 0.0254;
	board.outline = {{minX - margin, minY - margin}, {maxX + margin, minY - margin},
					 {maxX + margin, maxY + margin}, {minX - margin, maxY + margin}};
	return board;
}
