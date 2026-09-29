#include "DerivedDataBuildOutput.h"
#include <gtest/gtest.h>

namespace
{
	using namespace Durin;
	using namespace Durin::DerivedData;
	auto Bytes(std::string_view Text) -> FSharedByteBuffer { return FSharedByteBuffer::Copy(std::as_bytes(std::span(Text))); }
	auto Key() -> FCacheKey { return FCacheKey::FromHash(FCacheBucket::FromString("OutputFixtures"), FXxHash128::HashBuffer("action")); }
	auto Output(bool Error = false) -> FBuildOutput
	{
		FBuildOutputBuilder B("Fixture.Output", 2); auto Meta = MakeBuildMetadata(Bytes("meta")).value(); B.AddMeta(FValueId::FromName("Metadata"), std::move(Meta));
		B.AddValue(FValueId::FromName("B"), Bytes("small")); B.AddValue(FValueId::FromName("A"), Bytes("large")); B.AddMessage(EBuildMessageSeverity::Warning, "warning"); if (Error) B.AddMessage(EBuildMessageSeverity::Error, "error"); return std::move(B).Build().value();
	}
}

TEST(FDerivedDataBuildOutputTests, ValueIdsAreFixedStableAndIndexed)
{
	static_assert(sizeof(FValueId) == 12); const auto A = FValueId::FromName("Stable.Name"); EXPECT_FALSE(A.IsNull()); EXPECT_EQ(A, FValueId::FromName("Stable.Name")); EXPECT_NE(A, FValueId::FromName("Other")); EXPECT_NE(A.MakeIndexed(1), A.MakeIndexed(2)); EXPECT_TRUE(A.MakeIndexed(FValueId::MaximumIndex + 1).IsNull());
}

TEST(FDerivedDataBuildOutputTests, BuilderCanonicalizesAndRetainsBuffers)
{
	auto Backing = Bytes("large"); FBuildOutputBuilder Builder("Fixture.Output", 2); Builder.AddValue(FValueId::FromName("A"), Backing); auto Built = std::move(Builder).Build().value(); ASSERT_EQ(Built.GetValues().size(), 1u); const auto* Value = Built.FindValue(FValueId::FromName("A")); ASSERT_NE(Value, nullptr); EXPECT_EQ(Value->GetRawSize(), 5u); EXPECT_EQ(Value->GetRawHash(), FXxHash128::HashBuffer(Backing.GetBytes())); EXPECT_TRUE(Value->GetData().SharesStorageWith(Backing));
	EXPECT_FALSE(Builder.AddValue(FValueId::FromName("Late"), Bytes("late")));
}

TEST(FDerivedDataBuildOutputTests, RejectsDuplicatesNullIdsAndLimits)
{
	FBuildOutputBuilder Duplicate("Fixture.Output", 1); const auto Id = FValueId::FromName("Data"); EXPECT_TRUE(Duplicate.AddValue(Id, Bytes("a"))); EXPECT_FALSE(Duplicate.AddValue(Id, Bytes("b")));
	FBuildOutputBuilder Null("Fixture.Output", 1); EXPECT_FALSE(Null.AddValue({}, Bytes("a")));
	FBuildOutputBuilder Limited("Fixture.Output", 1, {.MaximumTotalBytes = 1}); Limited.AddValue(Id, Bytes("too large")); EXPECT_FALSE(std::move(Limited).Build());
	FBuildOutputBuilder NoMessages("Fixture.Output", 1, {.MaximumMessages = 0}); EXPECT_FALSE(NoMessages.AddValue({}, Bytes("a"))); EXPECT_FALSE(std::move(NoMessages).Build());
}

TEST(FDerivedDataBuildOutputTests, ErrorOutputDiscardsValues)
{
	auto Built = Output(true); EXPECT_TRUE(Built.HasError()); EXPECT_TRUE(Built.GetValues().empty()); ASSERT_EQ(Built.GetMessages().back().Severity, EBuildMessageSeverity::Error);
}

TEST(FDerivedDataBuildOutputTests, TransientLogsAreReturnedButNotCacheable)
{
	FBuildOutputBuilder Builder("Fixture.Output", 1); Builder.AddValue(FValueId::FromName("Data"), Bytes("value"));
	EXPECT_TRUE(Builder.AddLog("Fixture", EBuildLogSeverity::Warning, "transient"));
	auto Built = std::move(Builder).Build(); ASSERT_TRUE(Built); ASSERT_TRUE(Built->HasLogs()); EXPECT_EQ(Built->GetLogs().size(), 1u);
	auto Record = FCacheRecord::FromOutput(Key(), *Built); ASSERT_FALSE(Record); EXPECT_EQ(Record.error().Code, ECacheError::InvalidRequest);
}

TEST(FDerivedDataBuildOutputTests, RawAndCompressedRecordsRoundTripWithRetainedViews)
{
	auto Built = Output(); auto Record = FCacheRecord::FromOutput(Key(), Built); ASSERT_TRUE(Record); auto Raw = Record->Encode(); ASSERT_TRUE(Raw); auto Decoded = FCacheRecord::Decode(Key(), *Raw); ASSERT_TRUE(Decoded); auto Loaded = Decoded->ToOutput(Key()); ASSERT_TRUE(Loaded); EXPECT_TRUE(Loaded->FindValue(FValueId::FromName("A"))->GetData().SharesStorageWith(*Raw));
	auto Compressed = FCacheRecord::CompressEncoded(*Raw); ASSERT_TRUE(Compressed); auto Inflated = FCacheRecord::Decode(Key(), *Compressed); ASSERT_TRUE(Inflated); EXPECT_TRUE(Inflated->ToOutput(Key()).has_value());
}

TEST(FDerivedDataBuildOutputTests, CorruptionAndWrongKeysAreRejected)
{
	auto Record = FCacheRecord::FromOutput(Key(), Output()).value(); auto Raw = Record.Encode().value(); FByteBuffer Corrupt(Raw.begin(), Raw.end()); Corrupt[Corrupt.size() / 2] ^= std::byte{1}; EXPECT_FALSE(FCacheRecord::Decode(Key(), FSharedByteBuffer::Take(std::move(Corrupt))));
	auto Other = FCacheKey::FromHash(FCacheBucket::FromString("OutputFixtures"), FXxHash128::HashBuffer("other")); EXPECT_FALSE(FCacheRecord::Decode(Other, Raw));
}
