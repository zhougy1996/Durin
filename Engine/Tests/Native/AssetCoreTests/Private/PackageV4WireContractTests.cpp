#include <gtest/gtest.h>

#include "PackageV4WireContract.h"

#include <array>
#include <bit>
#include <limits>

namespace
{
	using namespace Durin;
	using namespace Durin::Testing::DastV4;

	auto Bytes(std::initializer_list<uint8> Values) -> std::vector<std::byte>
	{
		std::vector<std::byte> Result;
		Result.reserve(Values.size());
		for (uint8 Value : Values) Result.push_back(static_cast<std::byte>(Value));
		return Result;
	}

	auto MakeSummary() -> FPublicSummary
	{
		return {
			.AssetClass = "A",
			.EntryKind = 0,
			.RedirectDestination = "",
			.Dependencies = {},
			.ObjectCount = 1,
		};
	}

	auto MakeRawEnvelope(std::span<const std::byte> Summary) -> std::vector<std::byte>
	{
		FWireWriter Writer;
		Writer.WriteU32(Magic);
		Writer.WriteU32(Version);
		Writer.WriteU32(uint32(Summary.size()));
		Writer.WriteU8(SectionCount);
		Writer.WriteBytes(Summary);
		const uint32 FirstSection = uint32(13 + Summary.size() + SectionCount * 9);
		for (uint8 Kind = 1; Kind <= SectionCount; ++Kind)
		{
			Writer.WriteU8(Kind);
			Writer.WriteU32(FirstSection);
			Writer.WriteU32(0);
		}
		return Writer.TakeBytes();
	}

	auto StoreU32(std::vector<std::byte>& Data, uint64 Offset, uint32 Value) -> void
	{
		ASSERT_LE(Offset + 4, Data.size());
		for (uint32 Index = 0; Index < 4; ++Index)
			Data[Offset + Index] = static_cast<std::byte>(Value >> (Index * 8));
	}

	auto ExpectHeaderFailure(std::span<const std::byte> Data, std::string_view Message = {}) -> void
	{
		FValidatedHeader Header;
		std::string Error;
		EXPECT_FALSE(DecodeHeader(Data, Header, Error));
		EXPECT_FALSE(Error.empty());
		if (!Message.empty())
			EXPECT_NE(Error.find(Message), std::string::npos)
				<< "expected category: " << Message << ", actual: " << Error;
	}
}

TEST(FPackageV4WireContractTests, WritesEveryPrimitiveAsExactGoldenBytes)
{
	FWireWriter Writer;
	Writer.WriteU8(0x7f);
	Writer.WriteU16(0x1234);
	Writer.WriteU32(0x89abcdef);
	Writer.WriteU64(0x0123456789abcdef);
	Writer.WriteF32(1.0f);
	Writer.WriteF64(-2.0);
	for (const uint64 Value : {0ull, 127ull, 128ull, 16383ull, 16384ull,
		std::numeric_limits<uint64>::max()})
		Writer.WriteVarUInt(Value);
	std::string Error;
	ASSERT_TRUE(Writer.WriteString("", Error)) << Error;
	ASSERT_TRUE(Writer.WriteString("A\xe2\x82\xac", Error)) << Error;

	const std::vector<std::byte> Golden = Bytes({
		0x7f, 0x34, 0x12, 0xef, 0xcd, 0xab, 0x89,
		0xef, 0xcd, 0xab, 0x89, 0x67, 0x45, 0x23, 0x01,
		0x00, 0x00, 0x80, 0x3f,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc0,
		0x00, 0x7f, 0x80, 0x01, 0xff, 0x7f, 0x80, 0x80, 0x01,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01,
		0x00, 0x04, 0x41, 0xe2, 0x82, 0xac,
	});
	EXPECT_EQ(Writer.Bytes(), Golden);

	FWireReader Reader(Golden);
	uint8 U8 = 0;
	uint16 U16 = 0;
	uint32 U32 = 0;
	uint64 U64 = 0;
	float F32 = 0;
	double F64 = 0;
	ASSERT_TRUE(Reader.ReadU8(U8, Error));
	ASSERT_TRUE(Reader.ReadU16(U16, Error));
	ASSERT_TRUE(Reader.ReadU32(U32, Error));
	ASSERT_TRUE(Reader.ReadU64(U64, Error));
	ASSERT_TRUE(Reader.ReadF32(F32, Error));
	ASSERT_TRUE(Reader.ReadF64(F64, Error));
	EXPECT_EQ(U8, 0x7f);
	EXPECT_EQ(U16, 0x1234);
	EXPECT_EQ(U32, 0x89abcdef);
	EXPECT_EQ(U64, 0x0123456789abcdef);
	EXPECT_EQ(std::bit_cast<uint32>(F32), 0x3f800000);
	EXPECT_EQ(std::bit_cast<uint64>(F64), 0xc000000000000000);
	for (const uint64 Expected : {0ull, 127ull, 128ull, 16383ull, 16384ull,
		std::numeric_limits<uint64>::max()})
	{
		uint64 Actual = 0;
		ASSERT_TRUE(Reader.ReadVarUInt(Actual, Error)) << Error;
		EXPECT_EQ(Actual, Expected);
	}
	std::string String;
	ASSERT_TRUE(Reader.ReadString(String, Error));
	EXPECT_TRUE(String.empty());
	ASSERT_TRUE(Reader.ReadString(String, Error));
	EXPECT_EQ(String, "A\xe2\x82\xac");
	EXPECT_EQ(Reader.Remaining(), 0);
	EXPECT_TRUE(Reader.RequireEnd(Error));
}

TEST(FPackageV4WireContractTests, RejectsNoncanonicalVarUIntUtf8AndUnconsumedBytes)
{
	for (const std::vector<std::byte>& Invalid : {
		Bytes({0x80, 0x00}),
		Bytes({0x81, 0x00}),
		Bytes({0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x02}),
		Bytes({0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x00}),
	})
	{
		FWireReader Reader(Invalid);
		uint64 Value = 0;
		std::string Error;
		EXPECT_FALSE(Reader.ReadVarUInt(Value, Error));
		EXPECT_FALSE(Error.empty());
	}

	for (const std::vector<std::byte>& Invalid : {
		Bytes({0x01, 0x00}),
		Bytes({0x02, 0xc0, 0x80}),
		Bytes({0x03, 0xed, 0xa0, 0x80}),
		Bytes({0x04, 0xf4, 0x90, 0x80, 0x80}),
		Bytes({0x02, 0xe2, 0x82}),
		Bytes({0x81, 0x80, 0x40}),
	})
	{
		FWireReader Reader(Invalid);
		std::string Value;
		std::string Error;
		EXPECT_FALSE(Reader.ReadString(Value, Error));
		EXPECT_FALSE(Error.empty());
	}

	const std::vector<std::byte> WithTrailingByte = Bytes({0x00, 0xff});
	FWireReader Reader(WithTrailingByte);
	uint8 Value = 0;
	std::string Error;
	ASSERT_TRUE(Reader.ReadU8(Value, Error));
	EXPECT_FALSE(Reader.RequireEnd(Error));
}

TEST(FPackageV4WireContractTests, ZigZagSignedBoundariesHaveExactGoldenBytes)
{
	const std::array<std::pair<int64, std::vector<std::byte>>, 5> Cases = {{
		{std::numeric_limits<int64>::min(), Bytes({
			0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01})},
		{-1, Bytes({0x01})},
		{0, Bytes({0x00})},
		{1, Bytes({0x02})},
		{std::numeric_limits<int64>::max(), Bytes({
			0xfe, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01})},
	}};
	for (const auto& [Value, Golden] : Cases)
	{
		FWireWriter Writer;
		Writer.WriteVarInt(Value);
		EXPECT_EQ(Writer.Bytes(), Golden);
		FWireReader Reader(Golden);
		int64 Decoded = 0;
		std::string Error;
		ASSERT_TRUE(Reader.ReadVarInt(Decoded, Error)) << Error;
		EXPECT_EQ(Decoded, Value);
		EXPECT_EQ(Reader.Remaining(), 0);
	}
}

TEST(FPackageV4WireContractTests, EncodesEmptyEnvelopeAsExactGoldenBytes)
{
	std::array<std::vector<std::byte>, SectionCount> Sections;
	std::vector<std::byte> Encoded;
	std::string Error;
	ASSERT_TRUE(EncodeEnvelope(MakeSummary(), Sections, Encoded, Error)) << Error;

	const std::vector<std::byte> Golden = Bytes({
		0x44, 0x41, 0x53, 0x54, 0x04, 0x00, 0x00, 0x00,
		0x06, 0x00, 0x00, 0x00, 0x05,
		0x01, 0x41, 0x00, 0x00, 0x00, 0x01,
		0x01, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x02, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x03, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x04, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x05, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	});
	EXPECT_EQ(Encoded, Golden);

	FValidatedHeader Header;
	ASSERT_TRUE(DecodeHeader(Encoded, Header, Error)) << Error;
	EXPECT_EQ(Header.Summary, MakeSummary());
	for (uint8 Index = 0; Index < SectionCount; ++Index)
	{
		EXPECT_EQ(Header.Sections[Index].Kind, ESectionKind(Index + 1));
		EXPECT_EQ(Header.Sections[Index].Offset, 64);
		EXPECT_EQ(Header.Sections[Index].Length, 0);
	}
}

TEST(FPackageV4WireContractTests, EncodesNonemptySectionsAndRoundTripsDeterministically)
{
	const FPublicSummary Summary = {
		.AssetClass = "Durin::Material",
		.EntryKind = 1,
		.RedirectDestination = "/Game/M_Target",
		.Dependencies = {"/Engine/A", "/Game/B"},
		.ObjectCount = 3,
	};
	const std::array<std::vector<std::byte>, SectionCount> Sections = {
		Bytes({0xaa}), Bytes({0xbb, 0xcc}), Bytes({}), Bytes({0xdd}), Bytes({0xee, 0xff, 0x11}),
	};
	std::vector<std::byte> First;
	std::vector<std::byte> Second;
	std::string Error;
	ASSERT_TRUE(EncodeEnvelope(Summary, Sections, First, Error)) << Error;
	ASSERT_TRUE(EncodeEnvelope(Summary, Sections, Second, Error)) << Error;
	EXPECT_EQ(First, Second);

	FValidatedHeader Header;
	ASSERT_TRUE(DecodeHeader(First, Header, Error)) << Error;
	EXPECT_EQ(Header.Summary, Summary);
	std::vector<std::byte> Reencoded;
	ASSERT_TRUE(EncodeEnvelope(Header.Summary, Sections, Reencoded, Error)) << Error;
	EXPECT_EQ(Reencoded, First);

	const uint32 FirstBody = Header.Sections[0].Offset;
	EXPECT_EQ(Header.Sections[0].Length, 1);
	EXPECT_EQ(Header.Sections[1].Offset, FirstBody + 1);
	EXPECT_EQ(Header.Sections[1].Length, 2);
	EXPECT_EQ(Header.Sections[2].Offset, FirstBody + 3);
	EXPECT_EQ(Header.Sections[2].Length, 0);
	EXPECT_EQ(Header.Sections[3].Offset, FirstBody + 3);
	EXPECT_EQ(Header.Sections[4].Offset, FirstBody + 4);
	EXPECT_EQ(uint64(Header.Sections[4].Offset) + Header.Sections[4].Length, First.size());
}

TEST(FPackageV4WireContractTests, EncodesNonemptySectionsAsExactGoldenDirectory)
{
	const std::array<std::vector<std::byte>, SectionCount> Sections = {
		Bytes({0xaa}), Bytes({0xbb, 0xcc}), Bytes({}), Bytes({0xdd}), Bytes({0xee, 0xff, 0x11}),
	};
	std::vector<std::byte> Encoded;
	std::string Error;
	ASSERT_TRUE(EncodeEnvelope(MakeSummary(), Sections, Encoded, Error)) << Error;
	const std::vector<std::byte> Golden = Bytes({
		0x44, 0x41, 0x53, 0x54, 0x04, 0x00, 0x00, 0x00,
		0x06, 0x00, 0x00, 0x00, 0x05,
		0x01, 0x41, 0x00, 0x00, 0x00, 0x01,
		0x01, 0x40, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
		0x02, 0x41, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
		0x03, 0x43, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x04, 0x43, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
		0x05, 0x44, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
		0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x11,
	});
	EXPECT_EQ(Encoded, Golden);
}

TEST(FPackageV4WireContractTests, HeaderOnlyValidationDoesNotInterpretSectionBodies)
{
	std::array<std::vector<std::byte>, SectionCount> Sections = {
		Bytes({0xff, 0xff}), Bytes({0x80}), Bytes({0x00, 0xff}), Bytes({0xfe}), Bytes({0x80, 0x00}),
	};
	std::vector<std::byte> Encoded;
	std::string Error;
	ASSERT_TRUE(EncodeEnvelope(MakeSummary(), Sections, Encoded, Error)) << Error;
	FValidatedHeader Header;
	EXPECT_TRUE(DecodeHeader(Encoded, Header, Error)) << Error;
}

TEST(FPackageV4WireContractTests, RejectsMalformedSummaryAndDirectoryMutations)
{
	std::array<std::vector<std::byte>, SectionCount> Sections;
	std::vector<std::byte> Valid;
	std::string Error;
	ASSERT_TRUE(EncodeEnvelope(MakeSummary(), Sections, Valid, Error)) << Error;
	const uint64 Directory = 19;

	std::vector<std::byte> Mutated = Valid;
	Mutated.pop_back();
	ExpectHeaderFailure(Mutated, "truncation");

	Mutated = Valid;
	Mutated[0] = std::byte{0};
	ExpectHeaderFailure(Mutated, "magic");

	Mutated = Valid;
	Mutated[4] = std::byte{3};
	ExpectHeaderFailure(Mutated, "version");

	Mutated = Valid;
	Mutated[12] = std::byte{4};
	ExpectHeaderFailure(Mutated, "five sections");

	Mutated = MakeRawEnvelope(Bytes({0x81, 0x00, 0x00, 0x00, 0x00, 0x01}));
	ExpectHeaderFailure(Mutated, "nonminimal");

	Mutated = MakeRawEnvelope(Bytes({
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x02,
		0x00, 0x00, 0x00, 0x01,
	}));
	ExpectHeaderFailure(Mutated, "overflow");

	Mutated = MakeRawEnvelope(Bytes({0x01, 0x80, 0x00, 0x00, 0x00, 0x01}));
	ExpectHeaderFailure(Mutated, "UTF-8");

	Mutated = MakeRawEnvelope(Bytes({0x01, 0x41, 0x00, 0x00, 0x00, 0x01, 0xff}));
	ExpectHeaderFailure(Mutated, "unconsumed");

	Mutated = Valid;
	Mutated[Directory] = std::byte{0};
	ExpectHeaderFailure(Mutated, "unknown");

	Mutated = Valid;
	Mutated[Directory] = std::byte{6};
	ExpectHeaderFailure(Mutated, "unknown required");

	Mutated = Valid;
	Mutated[Directory + 9] = std::byte{1};
	ExpectHeaderFailure(Mutated, "duplicate");

	Mutated = Valid;
	Mutated[Directory] = std::byte{2};
	ExpectHeaderFailure(Mutated, "out-of-order");

	Mutated = Valid;
	StoreU32(Mutated, Directory + 9 + 1, 63);
	ExpectHeaderFailure(Mutated, "overlapping");

	Mutated = Valid;
	StoreU32(Mutated, Directory + 9 + 1, 65);
	ExpectHeaderFailure(Mutated, "gapped");

	Mutated = Valid;
	StoreU32(Mutated, Directory + 1 + 4, 0xffffffff);
	ExpectHeaderFailure(Mutated, "overflow");

	Mutated = Valid;
	Mutated.push_back(std::byte{0xee});
	ExpectHeaderFailure(Mutated, "trailing");
}

TEST(FPackageV4WireContractTests, RejectsNoncanonicalSummaryModelsBeforePublication)
{
	std::array<std::vector<std::byte>, SectionCount> Sections;
	std::vector<std::byte> Encoded = Bytes({0xaa});
	std::string Error;

	FPublicSummary Summary = MakeSummary();
	Summary.Dependencies = {"B", "A"};
	EXPECT_FALSE(EncodeEnvelope(Summary, Sections, Encoded, Error));
	EXPECT_EQ(Encoded, Bytes({0xaa}));

	Summary = MakeSummary();
	Summary.EntryKind = 1;
	EXPECT_FALSE(EncodeEnvelope(Summary, Sections, Encoded, Error));

	Summary = MakeSummary();
	Summary.ObjectCount = MaximumObjects + 1;
	EXPECT_FALSE(EncodeEnvelope(Summary, Sections, Encoded, Error));
}
