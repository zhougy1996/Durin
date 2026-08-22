#include <gtest/gtest.h>

#include "Asset/BulkData.h"

namespace
{
	using namespace Durin;
	using namespace Durin::Asset;

	constexpr FGuid PayloadId{1, 2, 3, 4};
	constexpr FGuid FormatId{5, 6, 7, 8};

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
			.FormatId = FormatId,
			.FormatVersion = 3,
			.LogicalByteCount = Buffer.GetSize(),
			.StoredByteCount = Buffer.GetSize(),
			.ContentHash = FXxHash128::HashBuffer(Buffer.GetBytes())};
	}

	class FTestBulkDataProvider final : public IBulkDataProvider
	{
	public:
		FSharedByteBuffer Result;
		std::string Error;
		mutable uint32 LoadCount = 0;
		bool bSucceed = true;

		auto GetStorageDomain() const -> EBulkDataStorageDomain override
		{
			return EBulkDataStorageDomain::Cooked;
		}

		auto LoadSynchronous(
			const FBulkDataDescriptor&,
			FSharedByteBuffer& OutBuffer,
			std::string& OutError) const -> bool override
		{
			++LoadCount;
			OutBuffer = Result;
			OutError = Error;
			return bSucceed;
		}
	};
}

TEST(FBulkDataTests, DefaultValueIsEmptyAndResident)
{
	FBulkData Value;
	EXPECT_FALSE(Value.HasPayload());
	EXPECT_TRUE(Value.IsResident());
	EXPECT_EQ(Value.GetStorageDomain(), EBulkDataStorageDomain::None);
	EXPECT_TRUE(Value.GetResidentBytes().empty());
	std::string Error = "stale";
	EXPECT_TRUE(Value.LoadSynchronous(Error));
	EXPECT_TRUE(Error.empty());
}

TEST(FBulkDataTests, CreatesVerifiedResidentValueTransactionally)
{
	const FSharedByteBuffer Bytes = MakeBytes({1, 2, 3, 4});
	const FBulkDataDescriptor Descriptor = MakeDescriptor(Bytes);
	FBulkData Value;
	std::string Error;
	ASSERT_TRUE(FBulkData::TryCreateResident(
		Descriptor, Bytes, EBulkDataStorageDomain::Authored, Value, &Error)) << Error;
	EXPECT_EQ(Value.GetDescriptor(), Descriptor);
	EXPECT_EQ(Value.GetStorageDomain(), EBulkDataStorageDomain::Authored);
	EXPECT_EQ(Value.GetResidentBytes().data(), Bytes.GetBytes().data());

	FBulkDataDescriptor Corrupt = Descriptor;
	++Corrupt.LogicalByteCount;
	EXPECT_FALSE(FBulkData::TryCreateResident(
		Corrupt, Bytes, EBulkDataStorageDomain::Authored, Value, &Error));
	EXPECT_EQ(Value.GetDescriptor(), Descriptor);
	EXPECT_EQ(Value.GetResidentBytes().data(), Bytes.GetBytes().data());
}

TEST(FBulkDataTests, RejectsInvalidIdentityDomainAndMissingProvider)
{
	const FSharedByteBuffer Bytes = MakeBytes({9});
	FBulkDataDescriptor Descriptor = MakeDescriptor(Bytes);
	FBulkData Value;
	std::string Error;
	Descriptor.FormatId = {};
	EXPECT_FALSE(FBulkData::TryCreateResident(
		Descriptor, Bytes, EBulkDataStorageDomain::Authored, Value, &Error));
	Descriptor = MakeDescriptor(Bytes);
	EXPECT_FALSE(FBulkData::TryCreateResident(
		Descriptor, Bytes, EBulkDataStorageDomain::None, Value, &Error));
	EXPECT_FALSE(FBulkData::TryCreateUnloaded(Descriptor, {}, Value, &Error));
	EXPECT_FALSE(Value.HasPayload());
	EXPECT_TRUE(Value.IsResident());
}

TEST(FBulkDataTests, LoadsOnceAndPublishesOnlyVerifiedImmutableBytes)
{
	const FSharedByteBuffer Bytes = MakeBytes({4, 5, 6});
	const FBulkDataDescriptor Descriptor = MakeDescriptor(Bytes);
	auto Provider = std::make_shared<FTestBulkDataProvider>();
	Provider->Result = Bytes;
	FBulkData Value;
	std::string Error;
	ASSERT_TRUE(FBulkData::TryCreateUnloaded(Descriptor, Provider, Value, &Error)) << Error;
	EXPECT_EQ(Value.GetResidency(), EBulkDataResidency::Unloaded);
	EXPECT_TRUE(Value.GetResidentBytes().empty());
	ASSERT_TRUE(Value.LoadSynchronous(Error)) << Error;
	EXPECT_EQ(Provider->LoadCount, 1u);
	EXPECT_EQ(Value.GetResidency(), EBulkDataResidency::Resident);
	EXPECT_EQ(Value.GetResidentBytes().data(), Bytes.GetBytes().data());
	EXPECT_TRUE(Value.LoadSynchronous(Error));
	EXPECT_EQ(Provider->LoadCount, 1u);
}

TEST(FBulkDataTests, FailedLoadIsStableAndNeverPublishesCandidateBytes)
{
	const FSharedByteBuffer Expected = MakeBytes({1, 2, 3});
	auto Provider = std::make_shared<FTestBulkDataProvider>();
	Provider->Result = MakeBytes({1, 2, 4});
	FBulkData Value;
	std::string Error;
	ASSERT_TRUE(FBulkData::TryCreateUnloaded(
		MakeDescriptor(Expected), Provider, Value, &Error)) << Error;
	EXPECT_FALSE(Value.LoadSynchronous(Error));
	EXPECT_EQ(Value.GetResidency(), EBulkDataResidency::Failed);
	EXPECT_TRUE(Value.GetResidentBytes().empty());
	EXPECT_FALSE(Error.empty());
	const std::string FirstError = Error;
	Provider->Result = Expected;
	EXPECT_FALSE(Value.LoadSynchronous(Error));
	EXPECT_EQ(Error, FirstError);
	EXPECT_EQ(Provider->LoadCount, 1u);
}

TEST(FBulkDataTests, ProviderFailureAndCopiesHaveIndependentResidency)
{
	const FSharedByteBuffer Bytes = MakeBytes({7, 8});
	const FBulkDataDescriptor Descriptor = MakeDescriptor(Bytes);
	auto Provider = std::make_shared<FTestBulkDataProvider>();
	Provider->Result = Bytes;
	FBulkData First;
	std::string Error;
	ASSERT_TRUE(FBulkData::TryCreateUnloaded(Descriptor, Provider, First, &Error));
	FBulkData Second = First;
	ASSERT_TRUE(First.LoadSynchronous(Error));
	EXPECT_EQ(Second.GetResidency(), EBulkDataResidency::Unloaded);
	ASSERT_TRUE(Second.LoadSynchronous(Error));
	EXPECT_EQ(Provider->LoadCount, 2u);
	EXPECT_EQ(First.GetResidentBytes().data(), Second.GetResidentBytes().data());

	Provider->bSucceed = false;
	Provider->Error = "provider unavailable";
	FBulkData Failed;
	ASSERT_TRUE(FBulkData::TryCreateUnloaded(Descriptor, Provider, Failed, &Error));
	EXPECT_FALSE(Failed.LoadSynchronous(Error));
	EXPECT_EQ(Error, "provider unavailable");
}

TEST(FBulkDataTests, LogicalDescriptorIncludesFormatSizesAndHash)
{
	const FSharedByteBuffer Bytes = MakeBytes({1, 3, 5});
	const FBulkDataDescriptor Descriptor = MakeDescriptor(Bytes);
	FBulkDataDescriptor Other = Descriptor;
	EXPECT_EQ(Descriptor, Other);
	++Other.FormatVersion;
	EXPECT_NE(Descriptor, Other);
	Other = Descriptor;
	++Other.StoredByteCount;
	EXPECT_NE(Descriptor, Other);
	Other = Descriptor;
	Other.ContentHash.HashLow ^= 1;
	EXPECT_NE(Descriptor, Other);
}
