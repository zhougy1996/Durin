#include "Hash/XxHash.h"

#include <gtest/gtest.h>

#include <array>

namespace
{
	TEST(FXxHashTests, BuilderStringUpdateMatchesHashBuffer)
	{
		Durin::FXxHash64Builder Builder;
		Builder.Update("abc");

		EXPECT_EQ(Builder.Finalize(), Durin::FXxHash64::HashBuffer("abc"));
	}

	TEST(FXxHashTests, ToStringRoundTripsFor64And128BitHashes)
	{
		const Durin::FXxHash64 Hash64{0x0123456789abcdefull};
		const std::string Hex64 = Hash64.ToString();
		EXPECT_EQ(Hex64, "0123456789abcdef");

		Durin::FXxHash64 Parsed64;
		ASSERT_TRUE(Durin::FXxHash64::TryFromString(Hex64, Parsed64));
		EXPECT_EQ(Parsed64, Hash64);
		EXPECT_EQ(Durin::FXxHash64::FromString(Hex64), Hash64);

		const Durin::FXxHash128 Hash128{0x0123456789abcdefull, 0xfedcba9876543210ull};
		const std::string Hex128 = Hash128.ToString();
		EXPECT_EQ(Hex128, "fedcba98765432100123456789abcdef");

		Durin::FXxHash128 Parsed128;
		ASSERT_TRUE(Durin::FXxHash128::TryFromString(Hex128, Parsed128));
		EXPECT_EQ(Parsed128, Hash128);
		EXPECT_EQ(Durin::FXxHash128::FromString(Hex128), Hash128);
	}

	TEST(FXxHashTests, TryFromStringRejectsInvalidInput)
	{
		Durin::FXxHash64 Hash64;
		EXPECT_FALSE(Durin::FXxHash64::TryFromString("", Hash64));
		EXPECT_FALSE(Durin::FXxHash64::TryFromString("xyz", Hash64));

		Durin::FXxHash128 Hash128;
		EXPECT_FALSE(Durin::FXxHash128::TryFromString("", Hash128));
		EXPECT_FALSE(Durin::FXxHash128::TryFromString("0123456789abcdef", Hash128));
		EXPECT_FALSE(Durin::FXxHash128::TryFromString("0123456789abcdef0123456789abcdeg", Hash128));
	}

	TEST(FXxHashTests, BytesToHexAndHexToBytesRoundTrip)
	{
		const std::array<Durin::uint8, 4> Bytes = {0x00, 0x12, 0xab, 0xff};
		EXPECT_EQ(Durin::BytesToHex(std::span<const Durin::uint8>(Bytes)), "0012abff");

		std::array<Durin::uint8, 4> ParsedBytes = {};
		ASSERT_TRUE(Durin::HexToBytes("0012ABff", ParsedBytes));
		EXPECT_EQ(ParsedBytes, Bytes);
	}

	TEST(FXxHashTests, HexToBytesRejectsInvalidInput)
	{
		std::array<Durin::uint8, 2> Bytes = {};
		EXPECT_FALSE(Durin::HexToBytes("0", Bytes));
		EXPECT_FALSE(Durin::HexToBytes("001122", Bytes));
		EXPECT_FALSE(Durin::HexToBytes("00g1", Bytes));
	}

	TEST(FXxHashTests, StructuredStringHashingCanPreserveFieldBoundariesAtCallSite)
	{
		Durin::FXxHash128Builder Left;
		const std::string_view LeftA = "ab";
		const std::string_view LeftB = "c";
		Left.UpdateValue(static_cast<Durin::uint64>(LeftA.size()));
		Left.Update(LeftA);
		Left.UpdateValue(static_cast<Durin::uint64>(LeftB.size()));
		Left.Update(LeftB);

		Durin::FXxHash128Builder Right;
		const std::string_view RightA = "a";
		const std::string_view RightB = "bc";
		Right.UpdateValue(static_cast<Durin::uint64>(RightA.size()));
		Right.Update(RightA);
		Right.UpdateValue(static_cast<Durin::uint64>(RightB.size()));
		Right.Update(RightB);

		EXPECT_NE(Left.Finalize(), Right.Finalize());
	}

	TEST(FXxHashTests, RawStringUpdatesComposeAsAByteStream)
	{
		Durin::FXxHash128Builder Left;
		Left.Update("ab");
		Left.Update("c");

		Durin::FXxHash128Builder Right;
		Right.Update("a");
		Right.Update("bc");

		EXPECT_EQ(Left.Finalize(), Right.Finalize());
	}
}
