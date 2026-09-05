#include "Asset/BulkContainerInfrastructure.h"
#include "Asset/ChunkedPayload.h"
#include "Hash/XxHash.h"

#include <gtest/gtest.h>

namespace
{
	using namespace Durin;
	using namespace Durin::BulkContainer;
}

namespace
{
	auto MakeChunkedPayloadFormat() -> Durin::FChunkedPayloadFormat
	{
		return {
			.HeaderSizeWordIndex = 5,
			.ChunkCountWordIndex = 6,
			.GlobalFlagsWordIndex = 4,
			.GlobalCompressedFlag = 1,
			.RequiredChunkCount = 2,
			.MaximumChunkCount = 4,
			.RequiredChunkFlag = Durin::ChunkedPayloadRequiredFlag,
			.KnownChunkFlags = Durin::ChunkedPayloadRequiredFlag | 0x0000ff00,
			.CompressionMask = 0x0000ff00,
			.CompressionShift = 8,
			.MaximumCompressionMethod = 1,
			.MaximumCompressionRatio = 64,
			.MaximumBytes = 1024,
			.Alignment = 16,
			.AllowTrailingZeroPadding = true};
	}

	auto WriteU32(Durin::FByteBuffer& Bytes, size_t Offset, uint32 Value) -> void
	{
		for (size_t Index = 0; Index < 4; ++Index)
			Bytes[Offset + Index] = static_cast<std::byte>(Value >> (Index * 8));
	}

	auto WriteU64(Durin::FByteBuffer& Bytes, size_t Offset, uint64 Value) -> void
	{
		for (size_t Index = 0; Index < 8; ++Index)
			Bytes[Offset + Index] = static_cast<std::byte>(Value >> (Index * 8));
	}

	auto RefreshChunkedPayloadHash(Durin::FByteBuffer& Bytes) -> void
	{
		WriteU64(Bytes, 56, FXxHash64::HashBuffer(std::span(Bytes).subspan(64)).HashValue);
	}
}

TEST(FBulkContainerArithmeticTests, RejectsOverflowAndInvalidAlignmentWithoutChangingOutputs)
{
	uint64 Value = 77;
	EXPECT_TRUE(TryAdd(2, 3, 5, Value));
	EXPECT_EQ(Value, 5u);
	Value = 77;
	EXPECT_FALSE(TryAdd(std::numeric_limits<uint64>::max(), 1,
		std::numeric_limits<uint64>::max(), Value));
	EXPECT_EQ(Value, 77u);
	EXPECT_FALSE(TryMultiply(std::numeric_limits<uint64>::max(), 2,
		std::numeric_limits<uint64>::max(), Value));
	EXPECT_EQ(Value, 77u);
	EXPECT_FALSE(TryAlignUp(7, 0, 100, Value));
	EXPECT_FALSE(TryAlignUp(7, 3, 100, Value));
	EXPECT_TRUE(TryAlignUp(17, 16, 32, Value));
	EXPECT_EQ(Value, 32u);
	Value = 77;
	EXPECT_FALSE(TryAlignUp(17, 16, 31, Value));
	EXPECT_EQ(Value, 77u);
}

TEST(FBulkContainerCodecTests, IsLittleEndianBoundedAndLatchesFirstFailure)
{
	FBoundedWriter Writer(7);
	ASSERT_TRUE(Writer.Write(uint16{0x1234}));
	ASSERT_TRUE(Writer.Write(uint32{0x89abcdef}));
	ASSERT_TRUE(Writer.PadTo(7));
	EXPECT_FALSE(Writer.Write(uint8{1}));
	EXPECT_EQ(Writer.GetFailure().Category, EFailure::LimitExceeded);
	EXPECT_EQ(Writer.GetFailure().Offset, 7u);
	Durin::FByteBuffer Published{std::byte{9}, std::byte{9}};
	EXPECT_FALSE(Writer.TryTake(Published));
	EXPECT_EQ(Published, (Durin::FByteBuffer{std::byte{9}, std::byte{9}}));

	const std::array<std::byte, 7> Bytes{std::byte{0x34}, std::byte{0x12},
		std::byte{0xef}, std::byte{0xcd}, std::byte{0xab}, std::byte{0x89}, std::byte{0}};
	FBoundedReader Reader(Bytes, Bytes.size());
	uint16 First = 0;
	uint32 Second = 0;
	ASSERT_TRUE(Reader.Read(First));
	ASSERT_TRUE(Reader.Read(Second));
	EXPECT_EQ(First, 0x1234u);
	EXPECT_EQ(Second, 0x89abcdefu);
	uint16 Unchanged = 0xbeef;
	EXPECT_FALSE(Reader.Read(Unchanged));
	EXPECT_EQ(Unchanged, 0xbeefu);
	EXPECT_EQ(Reader.Tell(), 6u);
	EXPECT_EQ(Reader.GetFailure().Category, EFailure::Truncated);
	EXPECT_FALSE(Reader.Read(First));
	EXPECT_EQ(Reader.GetFailure().Offset, 6u);
}

TEST(FBulkContainerCodecTests, PublishesOnlyDetachedSuccessfulOutput)
{
	FBoundedWriter Writer(16);
	ASSERT_TRUE(Writer.Write(uint32{0x04030201}));
	ASSERT_TRUE(Writer.PadTo(8));
	Durin::FByteBuffer Published{std::byte{9}};
	ASSERT_TRUE(Writer.TryTake(Published));
	EXPECT_EQ(Published, (Durin::FByteBuffer{std::byte{1}, std::byte{2},
		std::byte{3}, std::byte{4}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}}));
}

TEST(FBulkContainerCodecTests, TruncatedGuidDoesNotAdvanceOrMutateDestination)
{
	const std::array<std::byte, 15> Bytes{};
	FBoundedReader Reader(Bytes, Bytes.size());
	FGuid Guid{1, 2, 3, 4};
	EXPECT_FALSE(Reader.ReadGuid(Guid));
	EXPECT_EQ(Guid, FGuid(1, 2, 3, 4));
	EXPECT_EQ(Reader.Tell(), 0u);
}

TEST(FBulkContainerDirectoryTests, SortsDetachedProjectionAndRejectsDuplicateKeys)
{
	struct FValue { uint32 Key; uint32 Payload; };
	const std::array Values{FValue{2, 20}, FValue{1, 10}};
	std::vector<const FValue*> Sorted;
	ASSERT_TRUE(TryMakeSortedProjection<FValue>(Values, &FValue::Key, Sorted));
	ASSERT_EQ(Sorted.size(), 2u);
	EXPECT_EQ(Sorted[0]->Payload, 10u);
	EXPECT_EQ(Values[0].Key, 2u);
	const std::array Duplicate{FValue{1, 10}, FValue{1, 20}};
	EXPECT_FALSE(TryMakeSortedProjection<FValue>(Duplicate, &FValue::Key, Sorted));
	EXPECT_EQ(Sorted[0]->Payload, 10u);
}

TEST(FBulkContainerLayoutTests, BuildsMixedAlignmentAndValidatesCanonicalPadding)
{
	const FLayoutPolicy Policy{
		.MaximumCount = 4,
		.MaximumPayloadBytes = 32,
		.MaximumContainerBytes = 128,
		.RequireCanonicalOffsets = true,
		.AllowTrailingZeroPadding = false};
	const std::array Items{FLayoutItem{3, 16}, FLayoutItem{5, 64}};
	std::vector<FPayloadRange> Ranges;
	uint64 FileSize = 0;
	ASSERT_TRUE(TryBuildLayout(32, Items, Policy, Ranges, FileSize));
	ASSERT_EQ(Ranges.size(), 2u);
	EXPECT_EQ(Ranges[0].Offset, 32u);
	EXPECT_EQ(Ranges[1].Offset, 64u);
	EXPECT_EQ(FileSize, 69u);
	Durin::FByteBuffer Bytes(static_cast<size_t>(FileSize), std::byte{0});
	Bytes[32] = std::byte{1};
	Bytes[64] = std::byte{2};
	EXPECT_TRUE(ValidateLayout(Bytes, 24, 32, Ranges, Policy));
	Bytes[40] = std::byte{1};
	FFailure Failure;
	EXPECT_FALSE(ValidateLayout(Bytes, 24, 32, Ranges, Policy, &Failure));
	EXPECT_EQ(Failure.Category, EFailure::NonzeroPadding);
}

TEST(FBulkContainerLayoutTests, RejectsOverflowOverlapTrailingBytesAndUnsafeProjection)
{
	const FLayoutPolicy Policy{
		.MaximumCount = 2,
		.MaximumPayloadBytes = 16,
		.MaximumContainerBytes = 64,
		.RequireCanonicalOffsets = true,
		.AllowTrailingZeroPadding = false};
	std::vector<FPayloadRange> Ranges;
	uint64 FileSize = 99;
	const std::array Overflow{FLayoutItem{16, 16}, FLayoutItem{1, 64}};
	EXPECT_FALSE(TryBuildLayout(48, Overflow, Policy, Ranges, FileSize));
	EXPECT_TRUE(Ranges.empty());
	EXPECT_EQ(FileSize, 99u);

	Durin::FByteBuffer Bytes(48, std::byte{0});
	const std::array Overlap{FPayloadRange{32, 8, 16}, FPayloadRange{32, 8, 16}};
	EXPECT_FALSE(ValidateLayout(Bytes, 32, 32, Overlap, Policy));
	const std::array One{FPayloadRange{32, 8, 16}};
	EXPECT_FALSE(ValidateLayout(Bytes, 32, 32, One, Policy));
	Durin::FByteView Projected = Bytes;
	EXPECT_FALSE(TryProjectRange(Bytes, std::numeric_limits<uint64>::max(), 2, Projected));
	EXPECT_EQ(Projected.size(), Bytes.size());
}

TEST(FBulkContainerLayoutTests, ReportsLimitAndTrailingPaddingFailuresPrecisely)
{
	const FLayoutPolicy Policy{
		.MaximumCount = 1,
		.MaximumPayloadBytes = 4,
		.MaximumContainerBytes = 32,
		.RequireCanonicalOffsets = true,
		.AllowTrailingZeroPadding = true};
	std::vector<FPayloadRange> Ranges;
	uint64 FileSize = 0;
	FFailure Failure;
	const std::array Oversized{FLayoutItem{5, 1}};
	EXPECT_FALSE(TryBuildLayout(0, Oversized, Policy, Ranges, FileSize, &Failure));
	EXPECT_EQ(Failure.Category, EFailure::LimitExceeded);

	Durin::FByteBuffer Bytes(5, std::byte{0});
	Bytes.back() = std::byte{1};
	const std::array Payload{FPayloadRange{0, 4, 1}};
	EXPECT_FALSE(ValidateLayout(Bytes, 0, 0, Payload, Policy, &Failure));
	EXPECT_EQ(Failure.Category, EFailure::TrailingNonzeroPadding);
}

TEST(FChunkedPayloadCodecTests, RoundTripsRequiredAndUnknownOptionalChunksDeterministically)
{
	using namespace Durin;
	const std::array<std::byte, 3> First{std::byte{1}, std::byte{2}, std::byte{3}};
	const std::array<std::byte, 3> Second{std::byte{4}, std::byte{5}, std::byte{6}};
	const std::array<std::byte, 2> Optional{std::byte{7}, std::byte{8}};
	const std::array Chunks{
		FChunkedPayloadInput{1, ChunkedPayloadRequiredFlag, First, First.size()},
		FChunkedPayloadInput{2, ChunkedPayloadRequiredFlag, Second, Second.size()},
		FChunkedPayloadInput{99, 0, Optional, Optional.size()}};
	Durin::FByteBuffer Bytes;
	ASSERT_TRUE(EncodeChunkedPayload(
		{0x12345678, 4, 3, 1, 0, 0, 0, 0}, Chunks,
		MakeChunkedPayloadFormat(), Bytes));

	FDecodedChunkedPayload Decoded;
	ASSERT_TRUE(DecodeChunkedPayload(Bytes, MakeChunkedPayloadFormat(), Decoded));
	ASSERT_EQ(Decoded.RequiredChunks.size(), 2u);
	EXPECT_TRUE(std::ranges::equal(Decoded.RequiredChunks[0], First));
	EXPECT_TRUE(std::ranges::equal(Decoded.RequiredChunks[1], Second));
	ASSERT_EQ(Decoded.Chunks.size(), 3u);
	EXPECT_EQ(Decoded.Chunks[2].Type, 99u);
	EXPECT_TRUE(std::ranges::equal(Decoded.Chunks[2].Bytes, Optional));
}

TEST(FChunkedPayloadCodecTests, RejectsPaddingDuplicateRequiredAndCompressionCompatibility)
{
	using namespace Durin;
	const std::array<std::byte, 3> First{std::byte{1}, std::byte{2}, std::byte{3}};
	const std::array<std::byte, 3> Second{std::byte{4}, std::byte{5}, std::byte{6}};
	const std::array Chunks{
		FChunkedPayloadInput{1, ChunkedPayloadRequiredFlag, First, First.size()},
		FChunkedPayloadInput{2, ChunkedPayloadRequiredFlag, Second, Second.size()}};
	Durin::FByteBuffer Bytes;
	ASSERT_TRUE(EncodeChunkedPayload(
		{0x12345678, 4, 3, 1, 0, 0, 0, 0}, Chunks,
		MakeChunkedPayloadFormat(), Bytes));

	FDecodedChunkedPayload Sentinel;
	Sentinel.HeaderWords[0] = 77;
	Durin::FByteBuffer NonzeroPadding = Bytes;
	NonzeroPadding[131] = std::byte{1};
	RefreshChunkedPayloadHash(NonzeroPadding);
	const FChunkedPayloadResult PaddingResult = DecodeChunkedPayload(
		NonzeroPadding, MakeChunkedPayloadFormat(), Sentinel);
	EXPECT_EQ(PaddingResult.Failure, EChunkedPayloadFailure::NonzeroPadding);
	EXPECT_EQ(Sentinel.HeaderWords[0], 77u);

	Durin::FByteBuffer Duplicate = Bytes;
	WriteU32(Duplicate, 64 + ChunkedPayloadEntrySize, 1);
	RefreshChunkedPayloadHash(Duplicate);
	const FChunkedPayloadResult DuplicateResult = DecodeChunkedPayload(
		Duplicate, MakeChunkedPayloadFormat(), Sentinel);
	EXPECT_EQ(DuplicateResult.Failure, EChunkedPayloadFailure::MissingRequiredChunk);

	Durin::FByteBuffer Compressed = Bytes;
	WriteU32(Compressed, 16, 1);
	WriteU32(Compressed, 68, ChunkedPayloadRequiredFlag | (1u << 8));
	WriteU64(Compressed, 40, 195);
	WriteU64(Compressed, 88, 192);
	RefreshChunkedPayloadHash(Compressed);
	const FChunkedPayloadResult CompressionResult = DecodeChunkedPayload(
		Compressed, MakeChunkedPayloadFormat(), Sentinel);
	EXPECT_EQ(CompressionResult.Failure, EChunkedPayloadFailure::CompressionUnavailable);
	EXPECT_EQ(CompressionResult.Kind, EChunkedPayloadFailureKind::Incompatible);
}

TEST(FChunkedPayloadCodecTests, DoesNotPublishFailedEncodeOrDecodeCandidates)
{
	using namespace Durin;
	const std::array<std::byte, 900> Oversized{};
	const std::array Chunks{
		FChunkedPayloadInput{1, ChunkedPayloadRequiredFlag, Oversized, Oversized.size()},
		FChunkedPayloadInput{2, ChunkedPayloadRequiredFlag, Oversized, Oversized.size()}};
	Durin::FByteBuffer Published{std::byte{9}, std::byte{9}};
	EXPECT_FALSE(EncodeChunkedPayload(
		{0x12345678, 4, 3, 1, 0, 0, 0, 0}, Chunks,
		MakeChunkedPayloadFormat(), Published));
	EXPECT_EQ(Published, (Durin::FByteBuffer{std::byte{9}, std::byte{9}}));

	FDecodedChunkedPayload Decoded;
	Decoded.HeaderWords[0] = 88;
	const std::array<std::byte, 3> Truncated{};
	EXPECT_EQ(DecodeChunkedPayload(Truncated, MakeChunkedPayloadFormat(), Decoded).Failure,
		EChunkedPayloadFailure::TruncatedHeader);
	EXPECT_EQ(Decoded.HeaderWords[0], 88u);
}
