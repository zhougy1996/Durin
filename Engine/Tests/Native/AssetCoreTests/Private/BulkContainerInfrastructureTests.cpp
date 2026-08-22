#include "BulkContainerInfrastructure.h"

#include <gtest/gtest.h>

namespace
{
	using namespace Durin;
	using namespace Durin::Asset::BulkContainer;
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
	std::vector<uint8> Published{9, 9};
	EXPECT_FALSE(Writer.TryTake(Published));
	EXPECT_EQ(Published, (std::vector<uint8>{9, 9}));

	const std::array<uint8, 7> Bytes{0x34, 0x12, 0xef, 0xcd, 0xab, 0x89, 0};
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
	std::vector<uint8> Published{9};
	ASSERT_TRUE(Writer.TryTake(Published));
	EXPECT_EQ(Published, (std::vector<uint8>{1, 2, 3, 4, 0, 0, 0, 0}));
}

TEST(FBulkContainerCodecTests, TruncatedGuidDoesNotAdvanceOrMutateDestination)
{
	const std::array<uint8, 15> Bytes{};
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
	const FLayoutPolicy Policy{4, 32, 128, true, false};
	const std::array Items{FLayoutItem{3, 16}, FLayoutItem{5, 64}};
	std::vector<FPayloadRange> Ranges;
	uint64 FileSize = 0;
	ASSERT_TRUE(TryBuildLayout(32, Items, Policy, Ranges, FileSize));
	ASSERT_EQ(Ranges.size(), 2u);
	EXPECT_EQ(Ranges[0].Offset, 32u);
	EXPECT_EQ(Ranges[1].Offset, 64u);
	EXPECT_EQ(FileSize, 69u);
	std::vector<uint8> Bytes(static_cast<size_t>(FileSize), 0);
	Bytes[32] = 1;
	Bytes[64] = 2;
	EXPECT_TRUE(ValidateLayout(Bytes, 24, 32, Ranges, Policy));
	Bytes[40] = 1;
	FFailure Failure;
	EXPECT_FALSE(ValidateLayout(Bytes, 24, 32, Ranges, Policy, &Failure));
	EXPECT_EQ(Failure.Category, EFailure::NonzeroPadding);
}

TEST(FBulkContainerLayoutTests, RejectsOverflowOverlapTrailingBytesAndUnsafeProjection)
{
	const FLayoutPolicy Policy{2, 16, 64, true, false};
	std::vector<FPayloadRange> Ranges;
	uint64 FileSize = 99;
	const std::array Overflow{FLayoutItem{16, 16}, FLayoutItem{1, 64}};
	EXPECT_FALSE(TryBuildLayout(48, Overflow, Policy, Ranges, FileSize));
	EXPECT_TRUE(Ranges.empty());
	EXPECT_EQ(FileSize, 99u);

	std::vector<uint8> Bytes(48, 0);
	const std::array Overlap{FPayloadRange{32, 8, 16}, FPayloadRange{32, 8, 16}};
	EXPECT_FALSE(ValidateLayout(Bytes, 32, 32, Overlap, Policy));
	const std::array One{FPayloadRange{32, 8, 16}};
	EXPECT_FALSE(ValidateLayout(Bytes, 32, 32, One, Policy));
	std::span<const uint8> Projected = Bytes;
	EXPECT_FALSE(TryProjectRange(Bytes, std::numeric_limits<uint64>::max(), 2, Projected));
	EXPECT_EQ(Projected.size(), Bytes.size());
}
