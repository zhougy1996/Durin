#include "Serialization/Archive.h"

#include <gtest/gtest.h>

namespace
{
	struct FArchiveFixtureValue
	{
		Durin::uint32 Count = 0;
		std::string Label;

		auto Serialize(Durin::FArchive& Ar) -> void
		{
			Ar << Count;
			Durin::SerializeBoundedString(Ar, Label, 32);
		}

		auto operator==(const FArchiveFixtureValue&) const -> bool = default;
	};
}

namespace ArchiveCustomizationTest
{
	struct FFreeValue { Durin::uint16 Value = 0; };
	inline auto Serialize(Durin::FArchive& Ar, FFreeValue& Value) -> void { Ar << Value.Value; }
}

TEST(FArchiveTests, WritesCanonicalLittleEndianPrimitivesAndRoundTrips)
{
	std::vector<Durin::uint8> Bytes;
	Durin::FCanonicalMemoryWriter Writer(Bytes, Durin::EArchivePurpose::DerivedDataPayload);
	Durin::uint16 U16 = 0x1234;
	Durin::int32 I32 = -2;
	float Float = 1.0f;
	bool Boolean = true;
	Writer << U16 << I32 << Float << Boolean;
	ASSERT_FALSE(Writer.HasError()) << Writer.GetError();
	EXPECT_EQ(Bytes, (std::vector<Durin::uint8>{
		0x34, 0x12, 0xfe, 0xff, 0xff, 0xff, 0x00, 0x00, 0x80, 0x3f, 0x01}));

	Durin::uint16 LoadedU16 = 0;
	Durin::int32 LoadedI32 = 0;
	float LoadedFloat = 0;
	bool LoadedBoolean = false;
	Durin::FCanonicalMemoryReader Reader(Bytes, Durin::EArchivePurpose::DerivedDataPayload);
	Reader << LoadedU16 << LoadedI32 << LoadedFloat << LoadedBoolean;
	EXPECT_TRUE(Durin::RequireArchiveEnd(Reader));
	EXPECT_EQ(LoadedU16, U16);
	EXPECT_EQ(LoadedI32, I32);
	EXPECT_EQ(LoadedFloat, Float);
	EXPECT_EQ(LoadedBoolean, Boolean);
}

TEST(FArchiveTests, SpanRegionsBoundsAndFailureAreTransactionalAndSticky)
{
	const std::array<Durin::uint8, 5> Bytes{1, 2, 3, 4, 5};
	Durin::FCanonicalMemoryReader Reader(Bytes, Durin::EArchivePurpose::CookedPayload);
	std::span<const Durin::uint8> Region;
	ASSERT_TRUE(Reader.ReadRegion(3, Region));
	EXPECT_TRUE(std::ranges::equal(Region, std::span<const Durin::uint8>(Bytes).first(3)));
	EXPECT_FALSE(Reader.ReadRegion(3, Region));
	ASSERT_NE(Reader.GetFailure(), nullptr);
	EXPECT_EQ(Reader.GetFailure()->Code, Durin::EArchiveFailureCode::TruncatedPayload);
	EXPECT_EQ(Reader.Tell(), 3);
	Durin::uint8 Unchanged = 42;
	Reader << Unchanged;
	EXPECT_EQ(Unchanged, 42);
}

TEST(FArchiveTests, BoundedValuesAlignmentAndTrailingBytesRejectHostileInput)
{
	std::vector<Durin::uint8> Bytes;
	Durin::FCanonicalMemoryWriter Writer(Bytes, Durin::EArchivePurpose::CookedPayload);
	std::string Label = "abc";
	Durin::SerializeBoundedString(Writer, Label, 3);
	Durin::SerializeAlignment(Writer, 8);
	ASSERT_FALSE(Writer.HasError()) << Writer.GetError();
	ASSERT_EQ(Bytes.size(), 16);

	Bytes.back() = 1;
	std::string Preserved = "old";
	Durin::FCanonicalMemoryReader Reader(Bytes, Durin::EArchivePurpose::CookedPayload);
	Durin::SerializeBoundedString(Reader, Preserved, 3);
	EXPECT_EQ(Preserved, "abc");
	Durin::SerializeAlignment(Reader, 8);
	ASSERT_NE(Reader.GetFailure(), nullptr);
	EXPECT_EQ(Reader.GetFailure()->Code, Durin::EArchiveFailureCode::NonZeroPadding);

	const std::array<Durin::uint8, 8> OversizedLength{4, 0, 0, 0, 0, 0, 0, 0};
	Durin::FCanonicalMemoryReader BoundedReader(OversizedLength);
	Preserved = "old";
	Durin::SerializeBoundedString(BoundedReader, Preserved, 3);
	EXPECT_EQ(Preserved, "old");
	EXPECT_EQ(BoundedReader.GetFailure()->Code, Durin::EArchiveFailureCode::LimitExceeded);
}

TEST(FArchiveTests, CountingAndHashingArchivesMatchCanonicalMemory)
{
	FArchiveFixtureValue Expected{17, "archive"};
	std::vector<Durin::uint8> Bytes;
	Durin::FCanonicalMemoryWriter Writer(Bytes, Durin::EArchivePurpose::DerivedDataKey);
	Writer << Expected;

	FArchiveFixtureValue Counted = Expected;
	Durin::FCountingArchive Counter(Durin::EArchivePurpose::DerivedDataKey);
	Counter << Counted;
	EXPECT_EQ(Counter.Tell(), Bytes.size());

	FArchiveFixtureValue Hashed = Expected;
	Durin::FHashingArchive Hash(Durin::EArchivePurpose::DerivedDataKey);
	Hash << Hashed;
	EXPECT_EQ(Hash.Tell(), Bytes.size());
	EXPECT_EQ(Hash.Finalize(), Durin::FXxHash128::HashBuffer(Bytes));
}

TEST(FArchiveTests, MemberAndFreeSerializeCustomizationsShareOneProtocol)
{
	FArchiveFixtureValue Member{9, "member"};
	ArchiveCustomizationTest::FFreeValue Free{0x1234};
	std::vector<Durin::uint8> Bytes;
	Durin::FCanonicalMemoryWriter Writer(Bytes, Durin::EArchivePurpose::DerivedDataPayload);
	Writer << Member << Free;
	ASSERT_FALSE(Writer.HasError()) << Writer.GetError();

	FArchiveFixtureValue LoadedMember;
	ArchiveCustomizationTest::FFreeValue LoadedFree;
	Durin::FCanonicalMemoryReader Reader(Bytes, Durin::EArchivePurpose::DerivedDataPayload);
	Reader << LoadedMember << LoadedFree;
	EXPECT_TRUE(Durin::RequireArchiveEnd(Reader));
	EXPECT_EQ(LoadedMember, Member);
	EXPECT_EQ(LoadedFree.Value, Free.Value);
}

TEST(FArchiveTests, ContextAndVersionsRemainOrthogonal)
{
	Durin::FArchiveState State;
	State.bFilterEditorOnly = true;
	State.BulkDataPolicy = Durin::EArchiveBulkDataPolicy::External;
	State.Target = {.Platform = "Win64", .Profile = "Shipping"};
	Durin::FArchiveVersionContext Versions{
		.Formats = {{Durin::FName("TXPL"), 3}},
		.CustomVersions = {{{1, 2, 3, 4}, 7}}};
	std::vector<Durin::uint8> Bytes;
	Durin::FCanonicalMemoryWriter Writer(
		Bytes, Durin::EArchivePurpose::CookedPayload, State, Versions);
	EXPECT_TRUE(Writer.IsSaving());
	EXPECT_TRUE(Writer.IsPersistent());
	EXPECT_TRUE(Writer.IsCooking());
	EXPECT_TRUE(Writer.IsFilterEditorOnly());
	EXPECT_EQ(Writer.GetBulkDataPolicy(), Durin::EArchiveBulkDataPolicy::External);
	EXPECT_EQ(Writer.GetTarget().Platform, "Win64");
	ASSERT_NE(Writer.GetVersionContext().FindFormat(Durin::FName("TXPL")), nullptr);
	EXPECT_EQ(Writer.GetVersionContext().FindFormat(Durin::FName("TXPL"))->Version, 3);
}
