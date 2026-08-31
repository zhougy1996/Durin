#include "Image/ImageEncoder.h"

namespace Durin::Image
{
	namespace
	{
		constexpr size_t DeflateWindowSize = 32 * 1024;
		constexpr size_t DeflateMaximumMatch = 258;
		constexpr size_t DeflateHashSize = 1 << 15;

		struct FDeflateCode
		{
			uint32 Symbol = 0;
			uint32 ExtraValue = 0;
			uint32 ExtraBits = 0;
		};

		class FBitWriter
		{
		public:
			explicit FBitWriter(FByteArray& InBytes)
				: Bytes(InBytes)
			{
			}

			auto Write(uint32 Value, uint32 BitCount) -> void
			{
				PendingBits |= static_cast<uint64>(Value) << PendingBitCount;
				PendingBitCount += BitCount;
				while (PendingBitCount >= 8)
				{
					Bytes.push_back(static_cast<std::byte>(PendingBits));
					PendingBits >>= 8;
					PendingBitCount -= 8;
				}
			}

			auto Flush() -> void
			{
				if (PendingBitCount != 0) Bytes.push_back(static_cast<std::byte>(PendingBits));
				PendingBits = 0;
				PendingBitCount = 0;
			}

		private:
			FByteArray& Bytes;
			uint64 PendingBits = 0;
			uint32 PendingBitCount = 0;
		};

		auto AppendBigEndian(FByteArray& Bytes, uint32 Value) -> void
		{
			Bytes.push_back(static_cast<std::byte>(Value >> 24));
			Bytes.push_back(static_cast<std::byte>(Value >> 16));
			Bytes.push_back(static_cast<std::byte>(Value >> 8));
			Bytes.push_back(static_cast<std::byte>(Value));
		}

		auto ReverseBits(uint32 Value, uint32 BitCount) -> uint32
		{
			uint32 Result = 0;
			for (uint32 Bit = 0; Bit < BitCount; ++Bit)
			{
				Result = (Result << 1) | (Value & 1u);
				Value >>= 1;
			}
			return Result;
		}

		auto WriteFixedLiteral(FBitWriter& Writer, uint32 Symbol) -> void
		{
			if (Symbol <= 143)
				Writer.Write(ReverseBits(0x30u + Symbol, 8), 8);
			else if (Symbol <= 255)
				Writer.Write(ReverseBits(0x190u + Symbol - 144, 9), 9);
			else if (Symbol <= 279)
				Writer.Write(ReverseBits(Symbol - 256, 7), 7);
			else
				Writer.Write(ReverseBits(0xc0u + Symbol - 280, 8), 8);
		}

		auto EncodeLength(size_t Length) -> FDeflateCode
		{
			constexpr std::array<uint16, 29> Bases = {
				3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27,
				31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
			constexpr std::array<uint8, 29> ExtraBits = {
				0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
				2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
			for (size_t Index = 0; Index < Bases.size(); ++Index)
			{
				const size_t Maximum = Index + 1 < Bases.size() ? Bases[Index + 1] - 1 : Bases[Index];
				if (Length <= Maximum)
					return {static_cast<uint32>(257 + Index), static_cast<uint32>(Length - Bases[Index]), ExtraBits[Index]};
			}
			return {};
		}

		auto EncodeDistance(size_t Distance) -> FDeflateCode
		{
			constexpr std::array<uint16, 30> Bases = {
				1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
				193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
				6145, 8193, 12289, 16385, 24577};
			constexpr std::array<uint8, 30> ExtraBits = {
				0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
				6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
			for (size_t Index = 0; Index < Bases.size(); ++Index)
			{
				const size_t Maximum = Index + 1 < Bases.size() ? Bases[Index + 1] - 1 : DeflateWindowSize;
				if (Distance <= Maximum)
					return {static_cast<uint32>(Index), static_cast<uint32>(Distance - Bases[Index]), ExtraBits[Index]};
			}
			return {};
		}

		auto HashBytes(std::span<const std::byte> Bytes, size_t Offset) -> size_t
		{
			const uint32 Value = static_cast<uint32>(std::to_integer<uint8>(Bytes[Offset])) << 16
				| static_cast<uint32>(std::to_integer<uint8>(Bytes[Offset + 1])) << 8
				| std::to_integer<uint8>(Bytes[Offset + 2]);
			return (Value * 2'654'435'761u) >> 17;
		}

		auto AppendCompressedDeflate(std::span<const std::byte> Bytes, FByteArray& OutBytes) -> void
		{
			OutBytes.push_back(std::byte{0x78});
			OutBytes.push_back(std::byte{0x9c});
			FBitWriter Writer(OutBytes);
			Writer.Write(1, 1);
			Writer.Write(1, 2);

			std::vector<ptrdiff_t> LastOffsets(DeflateHashSize, -1);
			size_t Offset = 0;
			while (Offset < Bytes.size())
			{
				size_t MatchLength = 0;
				size_t MatchDistance = 0;
				if (Bytes.size() - Offset >= 3)
				{
					const size_t Hash = HashBytes(Bytes, Offset);
					const ptrdiff_t Candidate = LastOffsets[Hash];
					if (Candidate >= 0 && Offset - static_cast<size_t>(Candidate) <= DeflateWindowSize)
					{
						const size_t MaximumLength = std::min(DeflateMaximumMatch, Bytes.size() - Offset);
						while (MatchLength < MaximumLength
							&& Bytes[static_cast<size_t>(Candidate) + MatchLength] == Bytes[Offset + MatchLength])
							++MatchLength;
						if (MatchLength >= 3) MatchDistance = Offset - static_cast<size_t>(Candidate);
						else MatchLength = 0;
					}
				}

				if (MatchLength >= 3)
				{
					const FDeflateCode LengthCode = EncodeLength(MatchLength);
					WriteFixedLiteral(Writer, LengthCode.Symbol);
					Writer.Write(LengthCode.ExtraValue, LengthCode.ExtraBits);
					const FDeflateCode DistanceCode = EncodeDistance(MatchDistance);
					Writer.Write(ReverseBits(DistanceCode.Symbol, 5), 5);
					Writer.Write(DistanceCode.ExtraValue, DistanceCode.ExtraBits);
					for (size_t Index = 0; Index < MatchLength && Bytes.size() - (Offset + Index) >= 3; ++Index)
						LastOffsets[HashBytes(Bytes, Offset + Index)] = static_cast<ptrdiff_t>(Offset + Index);
					Offset += MatchLength;
				}
				else
				{
					WriteFixedLiteral(Writer, std::to_integer<uint8>(Bytes[Offset]));
					if (Bytes.size() - Offset >= 3)
						LastOffsets[HashBytes(Bytes, Offset)] = static_cast<ptrdiff_t>(Offset);
					++Offset;
				}
			}
			WriteFixedLiteral(Writer, 256);
			Writer.Flush();

			uint32 S1 = 1;
			uint32 S2 = 0;
			for (const std::byte Byte : Bytes)
			{
				S1 = (S1 + std::to_integer<uint8>(Byte)) % 65'521;
				S2 = (S2 + S1) % 65'521;
			}
			AppendBigEndian(OutBytes, (S2 << 16) | S1);
		}

		auto Crc32(std::span<const std::byte> Bytes) -> uint32
		{
			uint32 Crc = 0xffffffffu;
			for (const std::byte Byte : Bytes)
			{
				Crc ^= std::to_integer<uint8>(Byte);
				for (uint32 Bit = 0; Bit < 8; ++Bit)
					Crc = (Crc >> 1) ^ (0xedb88320u & (0u - (Crc & 1u)));
			}
			return ~Crc;
		}

		auto WritePngChunk(
			FByteArray& Bytes,
			std::string_view Type,
			std::span<const std::byte> Payload) -> void
		{
			AppendBigEndian(Bytes, static_cast<uint32>(Payload.size()));
			const size_t CrcStart = Bytes.size();
			const auto TypeBytes = std::as_bytes(std::span{Type});
			Bytes.insert(Bytes.end(), TypeBytes.begin(), TypeBytes.end());
			Bytes.insert(Bytes.end(), Payload.begin(), Payload.end());
			AppendBigEndian(Bytes, Crc32(std::span(Bytes).subspan(CrcStart)));
		}
	} // namespace

	auto EncodeRgba8Png(
		std::span<const std::byte> Pixels,
		uint32 Width,
		uint32 Height,
		FByteArray& OutEncodedBytes) -> bool
	{
		OutEncodedBytes.clear();
		if (Width == 0 || Height == 0 || Width > 0x7fffffffu || Height > 0x7fffffffu) return false;
		if (static_cast<uint64>(Width) * Height > std::numeric_limits<uint64>::max() / 4) return false;
		const uint64 ExpectedBytes = static_cast<uint64>(Width) * Height * 4;
		if (ExpectedBytes != Pixels.size()
			|| ExpectedBytes > std::numeric_limits<size_t>::max() - Height
			|| ExpectedBytes + Height > std::numeric_limits<uint32>::max())
			return false;

		FByteArray Scanlines;
		Scanlines.reserve(static_cast<size_t>(ExpectedBytes) + Height);
		const size_t RowBytes = static_cast<size_t>(Width) * 4;
		for (uint32 Y = 0; Y < Height; ++Y)
		{
			Scanlines.push_back(std::byte{1});
			const size_t RowOffset = static_cast<size_t>(Y) * RowBytes;
			for (size_t X = 0; X < RowBytes; ++X)
			{
				const uint8 Current = std::to_integer<uint8>(Pixels[RowOffset + X]);
				const uint8 Left = X >= 4 ? std::to_integer<uint8>(Pixels[RowOffset + X - 4]) : 0;
				Scanlines.push_back(static_cast<std::byte>(Current - Left));
			}
		}

		FByteArray Deflate;
		Deflate.reserve(Scanlines.size() / 2);
		AppendCompressedDeflate(Scanlines, Deflate);

		OutEncodedBytes = {std::byte{137}, std::byte{80}, std::byte{78}, std::byte{71},
			std::byte{13}, std::byte{10}, std::byte{26}, std::byte{10}};
		FByteArray Header;
		AppendBigEndian(Header, Width);
		AppendBigEndian(Header, Height);
		Header.insert(Header.end(), {
			std::byte{8}, std::byte{6}, std::byte{0}, std::byte{0}, std::byte{0}});
		WritePngChunk(OutEncodedBytes, "IHDR", Header);
		WritePngChunk(OutEncodedBytes, "IDAT", Deflate);
		WritePngChunk(OutEncodedBytes, "IEND", {});
		return true;
	}
} // namespace Durin::Image
