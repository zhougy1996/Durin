#include "Misc/Guid.h"

#include <random>

namespace Durin
{
	namespace
	{
		constexpr char HexDigits[] = "0123456789abcdef";

		auto HexValue(char Character) -> uint32
		{
			if (Character >= '0' && Character <= '9')
			{
				return Character - '0';
			}
			if (Character >= 'a' && Character <= 'f')
			{
				return Character - 'a' + 10;
			}
			if (Character >= 'A' && Character <= 'F')
			{
				return Character - 'A' + 10;
			}
			return 16;
		}

		auto ParseHex(std::string_view Text, size_t Offset, size_t Length, uint32& OutValue) -> bool
		{
			uint32 Value = 0;
			for (size_t Index = 0; Index < Length; ++Index)
			{
				const uint32 Digit = HexValue(Text[Offset + Index]);
				if (Digit >= 16)
				{
					return false;
				}
				Value = (Value << 4) | Digit;
			}
			OutValue = Value;
			return true;
		}

		auto MakeRandomGenerator() -> std::mt19937
		{
			std::random_device RandomDevice;
			std::array<uint32, std::mt19937::state_size> SeedData;
			std::generate(SeedData.begin(), SeedData.end(), [&RandomDevice] { return RandomDevice(); });
			std::seed_seq Seed(SeedData.begin(), SeedData.end());
			return std::mt19937(Seed);
		}
	}

	auto FGuid::ToString() const -> std::string
	{
		std::string Result(36, '0');
		size_t OutputIndex = 0;
		auto AppendWord = [&Result, &OutputIndex](uint32 Value, uint32 DigitCount) {
			for (uint32 Shift = DigitCount * 4; Shift != 0; Shift -= 4)
			{
				Result[OutputIndex++] = HexDigits[(Value >> (Shift - 4)) & 0xf];
			}
		};

		AppendWord(A, 8);
		Result[OutputIndex++] = '-';
		AppendWord(B >> 16, 4);
		Result[OutputIndex++] = '-';
		AppendWord(B, 4);
		Result[OutputIndex++] = '-';
		AppendWord(C >> 16, 4);
		Result[OutputIndex++] = '-';
		AppendWord(C, 4);
		AppendWord(D, 8);
		return Result;
	}

	auto FGuid::Parse(std::string_view Text, FGuid& OutGuid) -> bool
	{
		if (Text.size() != 36 || Text[8] != '-' || Text[13] != '-' || Text[18] != '-' || Text[23] != '-')
		{
			return false;
		}

		FGuid Parsed;
		uint32 BHigh = 0;
		uint32 BLow = 0;
		uint32 CHigh = 0;
		uint32 CLow = 0;
		if (!ParseHex(Text, 0, 8, Parsed.A)
			|| !ParseHex(Text, 9, 4, BHigh)
			|| !ParseHex(Text, 14, 4, BLow)
			|| !ParseHex(Text, 19, 4, CHigh)
			|| !ParseHex(Text, 24, 4, CLow)
			|| !ParseHex(Text, 28, 8, Parsed.D))
		{
			return false;
		}

		Parsed.B = (BHigh << 16) | BLow;
		Parsed.C = (CHigh << 16) | CLow;
		OutGuid = Parsed;
		return true;
	}

	auto FGuid::NewGuid() -> FGuid
	{
		thread_local std::mt19937 Generator = MakeRandomGenerator();
		std::uniform_int_distribution<uint32> Distribution;

		FGuid Result(Distribution(Generator), Distribution(Generator), Distribution(Generator), Distribution(Generator));
		Result.B = (Result.B & 0xffff0fffu) | 0x00004000u;
		Result.C = (Result.C & 0x3fffffffu) | 0x80000000u;
		return Result;
	}
}
