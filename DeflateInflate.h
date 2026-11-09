#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace fzinflate {

class BitReader {
public:
	BitReader(const std::vector<std::uint8_t>& data, size_t offset, size_t length)
		: data_(data), bitPosition_(offset * 8), end_(offset + length) {
		if (offset > data.size() || length > data.size() - offset) {
			throw std::runtime_error("Invalid compressed data range.");
		}
	}

	std::uint32_t readBits(unsigned count) {
		if (count > 24) throw std::runtime_error("Invalid deflate bit count.");
		std::uint32_t value = 0;
		for (unsigned bit = 0; bit < count; ++bit) {
			const size_t byteIndex = bitPosition_ / 8;
			if (byteIndex >= end_) throw std::runtime_error("Truncated deflate stream.");
			value |= static_cast<std::uint32_t>((data_[byteIndex] >> (bitPosition_ % 8)) & 1U) << bit;
			++bitPosition_;
		}
		return value;
	}

	void alignToByte() { bitPosition_ = (bitPosition_ + 7) & ~static_cast<size_t>(7); }
	size_t bytePosition() const { return bitPosition_ / 8; }

private:
	const std::vector<std::uint8_t>& data_;
	size_t bitPosition_;
	size_t end_;
};

class HuffmanTree {
public:
	explicit HuffmanTree(const std::vector<std::uint8_t>& lengths) {
		std::array<unsigned, 16> counts{};
		for (const auto length : lengths) {
			if (length > 15) throw std::runtime_error("Invalid deflate Huffman code length.");
			if (length != 0) ++counts[length];
		}

		std::array<unsigned, 16> nextCode{};
		unsigned code = 0;
		for (unsigned bits = 1; bits <= 15; ++bits) {
			code = (code + counts[bits - 1]) << 1;
			nextCode[bits] = code;
		}

		for (size_t symbol = 0; symbol < lengths.size(); ++symbol) {
			const auto length = lengths[symbol];
			if (length != 0) codes_[length].emplace(nextCode[length]++, static_cast<unsigned>(symbol));
		}
	}

	unsigned decode(BitReader& reader) const {
		unsigned code = 0;
		for (unsigned length = 1; length <= 15; ++length) {
			code = (code << 1) | reader.readBits(1);
			const auto found = codes_[length].find(code);
			if (found != codes_[length].end()) return found->second;
		}
		throw std::runtime_error("Invalid deflate Huffman code.");
	}

private:
	std::array<std::unordered_map<unsigned, unsigned>, 16> codes_;
};

inline void appendByte(std::vector<std::uint8_t>& output, std::uint8_t value) {
	constexpr size_t maxOutputSize = 256U * 1024U * 1024U;
	if (output.size() >= maxOutputSize) throw std::runtime_error("Inflated FZ section exceeds the 256 MiB safety limit.");
	output.push_back(value);
}

inline HuffmanTree fixedLiteralTree() {
	std::vector<std::uint8_t> lengths(288);
	std::fill(lengths.begin(), lengths.begin() + 144, 8);
	std::fill(lengths.begin() + 144, lengths.begin() + 256, 9);
	std::fill(lengths.begin() + 256, lengths.begin() + 280, 7);
	std::fill(lengths.begin() + 280, lengths.end(), 8);
	return HuffmanTree(lengths);
}

inline HuffmanTree fixedDistanceTree() {
	return HuffmanTree(std::vector<std::uint8_t>(32, 5));
}

inline void inflateBlock(BitReader& reader, const HuffmanTree& literalTree, const HuffmanTree& distanceTree,
						 std::vector<std::uint8_t>& output) {
	static constexpr std::array<unsigned, 29> lengthBase = {
		3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27,
		31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
	static constexpr std::array<unsigned, 29> lengthExtra = {
		0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
		2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
	static constexpr std::array<unsigned, 30> distanceBase = {
		1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
		193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
	static constexpr std::array<unsigned, 30> distanceExtra = {
		0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
		6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

	for (;;) {
		const unsigned symbol = literalTree.decode(reader);
		if (symbol < 256) {
			appendByte(output, static_cast<std::uint8_t>(symbol));
		} else if (symbol == 256) {
			return;
		} else {
			if (symbol < 257 || symbol > 285) throw std::runtime_error("Invalid deflate length symbol.");
			const unsigned lengthIndex = symbol - 257;
			const unsigned length = lengthBase[lengthIndex] + reader.readBits(lengthExtra[lengthIndex]);
			const unsigned distanceSymbol = distanceTree.decode(reader);
			if (distanceSymbol >= distanceBase.size()) throw std::runtime_error("Invalid deflate distance symbol.");
			const size_t distance = distanceBase[distanceSymbol] + reader.readBits(distanceExtra[distanceSymbol]);
			if (distance == 0 || distance > output.size()) throw std::runtime_error("Invalid deflate back-reference.");
			for (unsigned i = 0; i < length; ++i) appendByte(output, output[output.size() - distance]);
		}
	}
}

inline HuffmanTree dynamicTree(BitReader& reader, HuffmanTree& distanceTree) {
	const unsigned literalCount = reader.readBits(5) + 257;
	const unsigned distanceCount = reader.readBits(5) + 1;
	const unsigned codeLengthCount = reader.readBits(4) + 4;
	static constexpr std::array<unsigned, 19> order = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

	std::vector<std::uint8_t> codeLengthLengths(19, 0);
	for (unsigned i = 0; i < codeLengthCount; ++i) codeLengthLengths[order[i]] = static_cast<std::uint8_t>(reader.readBits(3));
	const HuffmanTree codeLengthTree(codeLengthLengths);

	std::vector<std::uint8_t> lengths;
	lengths.reserve(literalCount + distanceCount);
	while (lengths.size() < literalCount + distanceCount) {
		const unsigned symbol = codeLengthTree.decode(reader);
		if (symbol <= 15) {
			lengths.push_back(static_cast<std::uint8_t>(symbol));
		} else if (symbol == 16) {
			if (lengths.empty()) throw std::runtime_error("Invalid deflate code-length repeat.");
			const size_t repeat = reader.readBits(2) + 3;
			if (repeat > literalCount + distanceCount - lengths.size()) throw std::runtime_error("Deflate code-length repeat exceeds its table.");
			lengths.insert(lengths.end(), repeat, lengths.back());
		} else if (symbol == 17 || symbol == 18) {
			const size_t repeat = symbol == 17 ? reader.readBits(3) + 3 : reader.readBits(7) + 11;
			if (repeat > literalCount + distanceCount - lengths.size()) throw std::runtime_error("Deflate zero repeat exceeds its table.");
			lengths.insert(lengths.end(), repeat, 0);
		} else {
			throw std::runtime_error("Invalid deflate code-length symbol.");
		}
	}

	if (lengths[256] == 0) throw std::runtime_error("Deflate block has no end-of-block code.");
	distanceTree = HuffmanTree(std::vector<std::uint8_t>(lengths.begin() + literalCount, lengths.end()));
	return HuffmanTree(std::vector<std::uint8_t>(lengths.begin(), lengths.begin() + literalCount));
}

inline std::vector<std::uint8_t> inflateZlib(const std::vector<std::uint8_t>& input, size_t offset, size_t length) {
	if (offset > input.size() || length > input.size() - offset || length < 6) throw std::runtime_error("Invalid zlib section.");
	const unsigned cmf = input[offset];
	const unsigned flg = input[offset + 1];
	if ((cmf & 0x0f) != 8 || (cmf >> 4) > 7 || ((cmf << 8) | flg) % 31 != 0 || (flg & 0x20) != 0) {
		throw std::runtime_error("Invalid or unsupported zlib header.");
	}

	BitReader reader(input, offset + 2, length - 2);
	std::vector<std::uint8_t> output;
	bool finalBlock = false;
	while (!finalBlock) {
		finalBlock = reader.readBits(1) != 0;
		const unsigned blockType = reader.readBits(2);
		if (blockType == 0) {
			reader.alignToByte();
			const unsigned blockLength = reader.readBits(16);
			const unsigned complement = reader.readBits(16);
			if ((blockLength ^ 0xffffU) != complement) throw std::runtime_error("Invalid uncompressed deflate block length.");
			for (unsigned i = 0; i < blockLength; ++i) appendByte(output, static_cast<std::uint8_t>(reader.readBits(8)));
		} else if (blockType == 1) {
			const auto literals = fixedLiteralTree();
			const auto distances = fixedDistanceTree();
			inflateBlock(reader, literals, distances, output);
		} else if (blockType == 2) {
			HuffmanTree distances(std::vector<std::uint8_t>{});
			const auto literals = dynamicTree(reader, distances);
			inflateBlock(reader, literals, distances, output);
		} else {
			throw std::runtime_error("Reserved deflate block type.");
		}
	}

	reader.alignToByte();
	const size_t checksumOffset = reader.bytePosition();
	const size_t sectionEnd = offset + length;
	if (checksumOffset > sectionEnd || sectionEnd - checksumOffset < 4) throw std::runtime_error("Truncated zlib checksum.");
	std::uint32_t expected = (static_cast<std::uint32_t>(input[checksumOffset]) << 24) |
		(static_cast<std::uint32_t>(input[checksumOffset + 1]) << 16) |
		(static_cast<std::uint32_t>(input[checksumOffset + 2]) << 8) |
		static_cast<std::uint32_t>(input[checksumOffset + 3]);
	std::uint32_t a = 1;
	std::uint32_t b = 0;
	for (const auto byte : output) {
		a = (a + byte) % 65521;
		b = (b + a) % 65521;
	}
	const std::uint32_t actual = (b << 16) | a;
	if (actual != expected) throw std::runtime_error("Zlib checksum mismatch.");
	return output;
}

} // namespace fzinflate
