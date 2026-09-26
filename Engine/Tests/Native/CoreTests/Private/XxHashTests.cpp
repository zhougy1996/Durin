#include "Hash/XxHash.h"
#include "Hash/CanonicalHash.h"
#include "Misc/StringConvert.h"

#include <gtest/gtest.h>

namespace
{
	template<typename T>
	concept CCanonicalHashField = requires(Durin::FXxHash128Builder& Builder, T Value) { Durin::UpdateCanonicalHash(Builder, Value); };
	static_assert(!CCanonicalHashField<float> && !CCanonicalHashField<const char*>);

	TEST(FXxHashTests, StringBuildersMatchBuffersAndPreserveChosenFieldBoundaries)
	{
		Durin::FXxHash64Builder Builder;
		Builder.Update("abc");

		EXPECT_EQ(Builder.Finalize(), Durin::FXxHash64::HashBuffer("abc"));
		Durin::FXxHash128Builder Left;
		const std::string_view LeftA = "ab";
		const std::string_view LeftB = "c";
		Left.UpdateValue(static_cast<uint64>(LeftA.size()));
		Left.Update(LeftA);
		Left.UpdateValue(static_cast<uint64>(LeftB.size()));
		Left.Update(LeftB);

		Durin::FXxHash128Builder Right;
		const std::string_view RightA = "a";
		const std::string_view RightB = "bc";
		Right.UpdateValue(static_cast<uint64>(RightA.size()));
		Right.Update(RightA);
		Right.UpdateValue(static_cast<uint64>(RightB.size()));
		Right.Update(RightB);

		EXPECT_NE(Left.Finalize(), Right.Finalize());

		Durin::FXxHash128Builder RawLeft;
		RawLeft.Update("ab");
		RawLeft.Update("c");

		Durin::FXxHash128Builder RawRight;
		RawRight.Update("a");
		RawRight.Update("bc");

		EXPECT_EQ(RawLeft.Finalize(), RawRight.Finalize());
	}

	TEST(FXxHashTests, HexRepresentationsValidateAndRoundTripHashesAndBytes)
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

		EXPECT_TRUE(Durin::StringUtils::IsHex(""));
		EXPECT_FALSE(Durin::StringUtils::IsHex("xyz"));
		EXPECT_FALSE(Durin::StringUtils::IsHex("0123456789abcdef", 32));
		EXPECT_FALSE(Durin::StringUtils::IsHex("0123456789abcdef0123456789abcdeg", 32));

		const std::array<std::byte, 4> Bytes = {
			std::byte{0x00}, std::byte{0x12}, std::byte{0xab}, std::byte{0xff}};
		EXPECT_EQ(Durin::StringUtils::BytesToHex(Bytes), "0012abff");
		std::array<std::byte, 4> ParsedBytes = {};
		ASSERT_TRUE(Durin::StringUtils::IsHex("0012ABff", 8));
		Durin::StringUtils::HexToBytes("0012ABff", ParsedBytes);
		EXPECT_EQ(ParsedBytes, Bytes);
	}
}

TEST(FXxHashTests, CanonicalFieldsUseExplicitWidthsAndPreserveStringBoundaries)
{
	using namespace Durin;
	FXxHash128Builder Builder;
	UpdateCanonicalHash(Builder, uint16(0x1234));
	UpdateCanonicalHash(Builder, int16(-2));
	UpdateCanonicalHash(Builder, true);
	UpdateCanonicalHashString(Builder, std::string_view("a\0b", 3));
	UpdateCanonicalHash(Builder, FXxHash128{1, 2});
	const uint8 Expected[] = {0x34, 0x12, 0xfe, 0xff, 1,
		3, 0, 0, 0, 0, 0, 0, 0, 'a', 0, 'b',
		1, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0};
	EXPECT_EQ(Builder.Finalize(), FXxHash128::HashBuffer(Expected, sizeof(Expected)));
	const auto Big = EncodeBinaryInteger(uint32(0x12345678), EBinaryByteOrder::BigEndian);
	EXPECT_EQ(Big, (std::array{std::byte{0x12}, std::byte{0x34}, std::byte{0x56}, std::byte{0x78}}));
	FXxHash64Builder Left, Right;
	UpdateCanonicalHashString(Left, "ab"); UpdateCanonicalHashString(Left, "c");
	UpdateCanonicalHashString(Right, "a"); UpdateCanonicalHashString(Right, "bc");
	EXPECT_NE(Left.Finalize(), Right.Finalize());
}
