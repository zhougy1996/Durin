#include <gtest/gtest.h>

#include "Asset/BulkData.h"

namespace
{
	using namespace Durin;
	using namespace Durin::Asset;

	constexpr FGuid PayloadId{1, 2, 3, 4};

	auto MakeBytes(std::initializer_list<uint8> Values) -> FSharedByteBuffer
	{
		std::vector<std::byte> Bytes;
		Bytes.reserve(Values.size());
		for (const uint8 Value : Values) Bytes.push_back(static_cast<std::byte>(Value));
		return FSharedByteBuffer::Take(std::move(Bytes));
	}

	auto MakeDescriptor(const FSharedByteBuffer& Buffer) -> FBulkDataDescriptor
	{
		return {
			.PayloadId = PayloadId,
			.LogicalByteCount = Buffer.GetSize(),
			.ContentHash = FXxHash128::HashBuffer(Buffer.GetBytes())};
	}
}

TEST(FBulkDataTests, DefaultValueIsEmpty)
{
	FBulkData Value;
	EXPECT_FALSE(Value.HasPayload());
	EXPECT_TRUE(Value.GetBytes().empty());
}

TEST(FBulkDataTests, CreatesVerifiedResidentValueTransactionally)
{
	const FSharedByteBuffer Bytes = MakeBytes({1, 2, 3, 4});
	const FBulkDataDescriptor Descriptor = MakeDescriptor(Bytes);
	FBulkData Value;
	std::string Error;
	ASSERT_TRUE(FBulkData::TryCreate(Descriptor, Bytes, Value, &Error)) << Error;
	EXPECT_EQ(Value.GetDescriptor(), Descriptor);
	EXPECT_EQ(Value.GetBytes().data(), Bytes.GetBytes().data());

	FBulkDataDescriptor Corrupt = Descriptor;
	++Corrupt.LogicalByteCount;
	EXPECT_FALSE(FBulkData::TryCreate(Corrupt, Bytes, Value, &Error));
	EXPECT_EQ(Value.GetDescriptor(), Descriptor);
	EXPECT_EQ(Value.GetBytes().data(), Bytes.GetBytes().data());
}

TEST(FBulkDataTests, RejectsInvalidIdentityAndCorruptBytes)
{
	const FSharedByteBuffer Bytes = MakeBytes({9});
	FBulkDataDescriptor Descriptor = MakeDescriptor(Bytes);
	FBulkData Value;
	std::string Error;
	Descriptor.PayloadId = {};
	EXPECT_FALSE(FBulkData::TryCreate(Descriptor, Bytes, Value, &Error));
	Descriptor = MakeDescriptor(Bytes);
	Descriptor.ContentHash.HashLow ^= 1;
	EXPECT_FALSE(FBulkData::TryCreate(Descriptor, Bytes, Value, &Error));
	EXPECT_FALSE(Value.HasPayload());
}

TEST(FBulkDataTests, CopiesShareImmutableBytes)
{
	const FSharedByteBuffer Bytes = MakeBytes({7, 8});
	const FBulkDataDescriptor Descriptor = MakeDescriptor(Bytes);
	FBulkData First;
	std::string Error;
	ASSERT_TRUE(FBulkData::TryCreate(Descriptor, Bytes, First, &Error));
	FBulkData Second = First;
	EXPECT_EQ(First.GetBytes().data(), Second.GetBytes().data());
	EXPECT_EQ(First.GetBytes().data(), Bytes.GetBytes().data());
}

TEST(FBulkDataTests, DescriptorIncludesOnlyStorageNeutralIdentity)
{
	const FSharedByteBuffer Bytes = MakeBytes({1, 3, 5});
	const FBulkDataDescriptor Descriptor = MakeDescriptor(Bytes);
	FBulkDataDescriptor Other = Descriptor;
	EXPECT_EQ(Descriptor, Other);
	++Other.LogicalByteCount;
	EXPECT_NE(Descriptor, Other);
	Other = Descriptor;
	Other.PayloadId = FGuid{9, 8, 7, 6};
	EXPECT_NE(Descriptor, Other);
	Other = Descriptor;
	Other.ContentHash.HashLow ^= 1;
	EXPECT_NE(Descriptor, Other);
}
