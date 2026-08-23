#include "Serialization/Archive.h"

#include <gtest/gtest.h>

namespace
{
	struct FArchiveFixtureValue
	{
		uint32 Count = 0;
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
	struct FFreeValue { uint16 Value = 0; };
	inline auto Serialize(Durin::FArchive& Ar, FFreeValue& Value) -> void { Ar << Value.Value; }
}

TEST(FArchiveTests, WritesCanonicalLittleEndianPrimitivesAndRoundTrips)
{
	std::vector<std::byte> Bytes;
	Durin::FCanonicalMemoryWriter Writer(Bytes, Durin::EArchivePurpose::DerivedDataPayload);
	uint16 U16 = 0x1234;
	int32 I32 = -2;
	float Float = 1.0f;
	bool Boolean = true;
	Writer << U16 << I32 << Float << Boolean;
	ASSERT_FALSE(Writer.HasError()) << Writer.GetError();
	EXPECT_EQ(Bytes, (std::vector<std::byte>{
		std::byte{0x34}, std::byte{0x12}, std::byte{0xfe}, std::byte{0xff},
		std::byte{0xff}, std::byte{0xff}, std::byte{0x00}, std::byte{0x00},
		std::byte{0x80}, std::byte{0x3f}, std::byte{0x01}}));

	uint16 LoadedU16 = 0;
	int32 LoadedI32 = 0;
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
	const std::array<std::byte, 5> Bytes{
		std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}, std::byte{5}};
	Durin::FCanonicalMemoryReader Reader(Bytes, Durin::EArchivePurpose::CookedPayload);
	std::span<const std::byte> Region;
	ASSERT_TRUE(Reader.ReadRegion(3, Region));
	EXPECT_TRUE(std::ranges::equal(Region, std::span(Bytes).first(3)));
	EXPECT_FALSE(Reader.ReadRegion(3, Region));
	ASSERT_NE(Reader.GetFailure(), nullptr);
	EXPECT_EQ(Reader.GetFailure()->Code, Durin::EArchiveFailureCode::TruncatedPayload);
	EXPECT_EQ(Reader.Tell(), 3);
	uint8 Unchanged = 42;
	Reader << Unchanged;
	EXPECT_EQ(Unchanged, 42);
}

TEST(FArchiveTests, BoundedValuesAlignmentAndTrailingBytesRejectHostileInput)
{
	std::vector<std::byte> Bytes;
	Durin::FCanonicalMemoryWriter Writer(Bytes, Durin::EArchivePurpose::CookedPayload);
	std::string Label = "abc";
	Durin::SerializeBoundedString(Writer, Label, 3);
	Durin::SerializeAlignment(Writer, 8);
	ASSERT_FALSE(Writer.HasError()) << Writer.GetError();
	ASSERT_EQ(Bytes.size(), 16);

	Bytes.back() = std::byte{1};
	std::string Preserved = "old";
	Durin::FCanonicalMemoryReader Reader(Bytes, Durin::EArchivePurpose::CookedPayload);
	Durin::SerializeBoundedString(Reader, Preserved, 3);
	EXPECT_EQ(Preserved, "abc");
	Durin::SerializeAlignment(Reader, 8);
	ASSERT_NE(Reader.GetFailure(), nullptr);
	EXPECT_EQ(Reader.GetFailure()->Code, Durin::EArchiveFailureCode::NonZeroPadding);

	const std::array<std::byte, 8> OversizedLength{
		std::byte{4}, std::byte{0}, std::byte{0}, std::byte{0},
		std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}};
	Durin::FCanonicalMemoryReader BoundedReader(OversizedLength);
	Preserved = "old";
	Durin::SerializeBoundedString(BoundedReader, Preserved, 3);
	EXPECT_EQ(Preserved, "old");
	EXPECT_EQ(BoundedReader.GetFailure()->Code, Durin::EArchiveFailureCode::LimitExceeded);
}

TEST(FArchiveTests, CountingAndHashingArchivesMatchCanonicalMemory)
{
	FArchiveFixtureValue Expected{17, "archive"};
	std::vector<std::byte> Bytes;
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
	std::vector<std::byte> Bytes;
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
	std::vector<std::byte> Bytes;
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

TEST(FArchiveTests, SharedByteBuffersShareImmutableStorageAndReplaceByCandidate)
{
	const std::array<std::byte, 3> Source{std::byte{1}, std::byte{2}, std::byte{3}};
	const Durin::FSharedByteBuffer First = Durin::FSharedByteBuffer::Copy(Source);
	const Durin::FSharedByteBuffer Shared = First;
	const Durin::FSharedByteBuffer Replacement = Durin::FSharedByteBuffer::Copy(
		std::span<const std::byte>(Source).first(2));

	EXPECT_TRUE(First.SharesStorageWith(Shared));
	EXPECT_FALSE(First.SharesStorageWith(Replacement));
	EXPECT_TRUE(std::ranges::equal(First.GetBytes(), Source));
}

TEST(FArchiveTests, BulkDataInlineRoundTripsAndRejectsCorruptionTransactionally)
{
	const std::array<std::byte, 4> Payload{
		std::byte{0x11}, std::byte{0x22}, std::byte{0x33}, std::byte{0x44}};
	Durin::FArchiveBulkDataTransfer Source{
		.PayloadId = {1, 2, 3, 4},
		.LogicalSize = Payload.size(),
		.StoredSize = Payload.size(),
		.ContentHash = Durin::FXxHash128::HashBuffer(Payload),
		.Buffer = Durin::FSharedByteBuffer::Copy(Payload)};

	std::vector<std::byte> Bytes;
	Durin::FCanonicalMemoryWriter Writer(Bytes, Durin::EArchivePurpose::BulkData);
	Writer.SerializeBulkData(Source);
	ASSERT_FALSE(Writer.HasError()) << Writer.GetError();
	ASSERT_GE(Bytes.size(), 37u);
	EXPECT_TRUE(std::ranges::all_of(
		std::span(Bytes).subspan(17, 20), [](std::byte Byte) { return Byte == std::byte{0}; }));

	Durin::FArchiveBulkDataTransfer Loaded;
	Durin::FCanonicalMemoryReader Reader(Bytes, Durin::EArchivePurpose::BulkData);
	Reader.SerializeBulkData(Loaded);
	ASSERT_TRUE(Durin::RequireArchiveEnd(Reader)) << Reader.GetError();
	EXPECT_EQ(Loaded.PayloadId, Source.PayloadId);
	EXPECT_EQ(Loaded.ContentHash, Source.ContentHash);
	EXPECT_TRUE(Loaded.ContainerHash.IsZero());
	EXPECT_TRUE(std::ranges::equal(Loaded.Buffer.GetBytes(), Payload));

	auto HistoricalSlots = Bytes;
	HistoricalSlots[17] = std::byte{1};
	HistoricalSlots[33] = std::byte{1};
	Durin::FArchiveBulkDataTransfer Historical;
	Durin::FCanonicalMemoryReader HistoricalReader(
		HistoricalSlots, Durin::EArchivePurpose::BulkData);
	HistoricalReader.SerializeBulkData(Historical);
	ASSERT_TRUE(Durin::RequireArchiveEnd(HistoricalReader)) << HistoricalReader.GetError();
	EXPECT_EQ(Historical.PayloadId, Source.PayloadId);
	EXPECT_TRUE(std::ranges::equal(Historical.Buffer.GetBytes(), Payload));

	Bytes.back() ^= std::byte{0xff};
	Durin::FArchiveBulkDataTransfer Preserved = Loaded;
	Durin::FCanonicalMemoryReader Corrupt(Bytes, Durin::EArchivePurpose::BulkData);
	Corrupt.SerializeBulkData(Preserved);
	ASSERT_TRUE(Corrupt.HasError());
	EXPECT_EQ(Preserved.ContentHash, Loaded.ContentHash);
	EXPECT_TRUE(Preserved.Buffer.SharesStorageWith(Loaded.Buffer));
}

TEST(FArchiveTests, BulkDataPoliciesSkipOrRejectBeforeMutation)
{
	Durin::FArchiveBulkDataTransfer Value{
		.PayloadId = {1, 1, 1, 1},
		.StoredSize = 0};

	Durin::FArchiveState SkipState;
	SkipState.BulkDataPolicy = Durin::EArchiveBulkDataPolicy::Skip;
	std::vector<std::byte> Bytes;
	Durin::FCanonicalMemoryWriter Skip(
		Bytes, Durin::EArchivePurpose::BulkData, SkipState);
	Skip.SerializeBulkData(Value);
	EXPECT_FALSE(Skip.HasError());
	EXPECT_TRUE(Bytes.empty());

	Durin::FArchiveState ExternalState;
	ExternalState.BulkDataPolicy = Durin::EArchiveBulkDataPolicy::External;
	Durin::FCanonicalMemoryWriter External(
		Bytes, Durin::EArchivePurpose::BulkData, ExternalState);
	External.SerializeBulkData(Value);
	ASSERT_TRUE(External.HasError());
	EXPECT_EQ(External.GetFailure()->Code,
		Durin::EArchiveFailureCode::UnsupportedCapability);
}
