#include "Hash/XxHash.h"
#include "Misc/StringConvert.h"

#include <gtest/gtest.h>

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
		ASSERT_TRUE(Durin::StringUtils::IsHex(Hex64, 16));
		EXPECT_EQ(Durin::FXxHash64::FromString(Hex64), Hash64);

		const Durin::FXxHash128 Hash128{0x0123456789abcdefull, 0xfedcba9876543210ull};
		const std::string Hex128 = Hash128.ToString();
		EXPECT_EQ(Hex128, "fedcba98765432100123456789abcdef");
		ASSERT_TRUE(Durin::StringUtils::IsHex(Hex128, 32));
		EXPECT_EQ(Durin::FXxHash128::FromString(Hex128), Hash128);
	}

	TEST(FXxHashTests, IsHexRejectsInvalidInput)
	{
		EXPECT_TRUE(Durin::StringUtils::IsHex(""));
		EXPECT_FALSE(Durin::StringUtils::IsHex("xyz"));
		EXPECT_FALSE(Durin::StringUtils::IsHex("0123456789abcdef", 32));
		EXPECT_FALSE(Durin::StringUtils::IsHex("0123456789abcdef0123456789abcdeg", 32));
	}

	TEST(FXxHashTests, BytesToHexAndHexToBytesRoundTrip)
	{
		const std::array<Durin::uint8, 4> Bytes = {0x00, 0x12, 0xab, 0xff};
		EXPECT_EQ(Durin::StringUtils::BytesToHex(std::span<const Durin::uint8>(Bytes)), "0012abff");

		std::array<Durin::uint8, 4> ParsedBytes = {};
		ASSERT_TRUE(Durin::StringUtils::IsHex("0012ABff", 8));
		Durin::StringUtils::HexToBytes("0012ABff", ParsedBytes);
		EXPECT_EQ(ParsedBytes, Bytes);
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
