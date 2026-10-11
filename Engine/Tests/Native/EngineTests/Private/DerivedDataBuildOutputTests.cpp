#include "DerivedDataBuildOutput.h"
#include "Serialization/BinaryFormat.h"
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

TEST(FDerivedDataBuildOutputTests, ValueReferencesDistinguishEmptyDataFromAbsentData)
{
	EXPECT_FALSE(FValue{}.HasData());
	for (const auto& Data : {Bytes("value"), FSharedByteBuffer{}})
	{
		const FValue Value(Data);
		ASSERT_TRUE(Value.HasData());
		const auto Reference = Value.RemoveData();
		EXPECT_FALSE(Reference.HasData());
		EXPECT_TRUE(Reference.GetData().IsEmpty());
		EXPECT_EQ(Reference.GetRawHash(), Value.GetRawHash());
		EXPECT_EQ(Reference.GetRawSize(), Value.GetRawSize());
		EXPECT_TRUE(Value.HasData());
	}
}

TEST(FDerivedDataBuildOutputTests, DescriptorOnlyRecordsCannotBecomeCompleteOutputs)
{
	FBuildOutputBuilder Builder("Fixture.Output", 2);
	ASSERT_TRUE(Builder.AddValue(FValueId::FromName("Empty"), {}));
	ASSERT_TRUE(Builder.AddValue(FValueId::FromName("Data"), Bytes("value")));
	auto Built = std::move(Builder).Build(); ASSERT_TRUE(Built);
	auto Record = FCacheRecord::FromOutput(Key(), *Built); ASSERT_TRUE(Record);
	const auto Reference = Record->RemoveValueData();
	ASSERT_TRUE(Reference.IsValid());
	EXPECT_EQ(Reference.GetKey(), Key());
	EXPECT_EQ(Reference.GetSchema(), Built->GetSchema());
	EXPECT_EQ(Reference.GetSchemaVersion(), Built->GetSchemaVersion());
	ASSERT_EQ(Reference.GetValues().size(), Record->GetValues().size());
	for (size_t Index = 0; Index < Reference.GetValues().size(); ++Index)
	{
		const auto& Before = Record->GetValues()[Index];
		const auto& After = Reference.GetValues()[Index];
		EXPECT_EQ(After.Id, Before.Id);
		EXPECT_EQ(After.Value.GetRawHash(), Before.Value.GetRawHash());
		EXPECT_EQ(After.Value.GetRawSize(), Before.Value.GetRawSize());
		EXPECT_FALSE(After.Value.HasData());
		EXPECT_TRUE(Before.Value.HasData());
	}
	const auto Incomplete = Reference.ToOutput(Key()); ASSERT_FALSE(Incomplete);
	EXPECT_EQ(Incomplete.error().Code, ECacheError::InvalidRequest);
	const auto Encoded = Reference.Encode(); ASSERT_FALSE(Encoded);
	EXPECT_EQ(Encoded.error().Code, ECacheError::InvalidRequest);
	EXPECT_TRUE(Record->ToOutput(Key()));
	EXPECT_TRUE(Record->Encode());
	EXPECT_FALSE(FCacheRecord{}.RemoveValueData().IsValid());
}

TEST(FDerivedDataBuildOutputTests, IndependentRecordsRetainPayloadsMetadataAndMessages)
{
	auto Built = Output();
	const auto Backing = Built.FindValue(FValueId::FromName("A"))->GetData();
	auto Record = FCacheRecord::FromOutput(Key(), Built); ASSERT_TRUE(Record);
	Built = {};
	const auto Reference = Record->RemoveValueData();
	ASSERT_EQ(Reference.GetMetadata().size(), 1u);
	EXPECT_TRUE(Reference.GetMetadata()[0].Object.GetBytes().SharesStorageWith(Record->GetMetadata()[0].Object.GetBytes()));
	ASSERT_EQ(Reference.GetMessages().size(), 1u);
	EXPECT_EQ(Reference.GetMessages()[0].Text, "warning");
	auto Loaded = Record->ToOutput(Key()); ASSERT_TRUE(Loaded);
	Record = FCacheRecord{};
	EXPECT_TRUE(Loaded->FindValue(FValueId::FromName("A"))->GetData().SharesStorageWith(Backing));
	EXPECT_EQ(GetBuildMetadataPayload(*Loaded).GetSize(), 4u);
	EXPECT_EQ(Loaded->GetMessages()[0].Text, "warning");
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

TEST(FDerivedDataBuildOutputTests, PayloadHashesAreCheckedEvenWithAValidEnvelopeHash)
{
	auto Record = FCacheRecord::FromOutput(Key(), Output()).value();
	auto Raw = Record.Encode().value();
	for (std::string_view Payload : {"large", "meta"})
	{
		FByteBuffer Body(Raw.begin(), Raw.end() - 16);
		const auto Target = std::as_bytes(std::span(Payload));
		const auto Found = std::ranges::search(Body, Target);
		ASSERT_FALSE(Found.empty());
		*Found.begin() ^= std::byte{1};
		FBinaryWriter Writer;
		Writer.WriteBytes(Body);
		Writer.WriteHash128(FXxHash128::HashBuffer(Body));
		auto Decoded = FCacheRecord::Decode(Key(), FSharedByteBuffer::Take(Writer.TakeBytes()));
		ASSERT_FALSE(Decoded);
		EXPECT_EQ(Decoded.error().Code, ECacheError::Corrupt);
	}
}

TEST(FDerivedDataBuildOutputTests, ReusedOutputStillEnforcesStricterLimitsAndKeys)
{
	auto Built = Output();
	auto Record = FCacheRecord::FromOutput(Key(), Built); ASSERT_TRUE(Record);
	auto Raw = Record->Encode(); ASSERT_TRUE(Raw);
	auto Loaded = FCacheRecord::Decode(Key(), *Raw); ASSERT_TRUE(Loaded);
	for (const auto& Candidate : {*Record, *Loaded})
	{
		auto Warm = Candidate.ToOutput(Key()); ASSERT_TRUE(Warm);
		EXPECT_TRUE(Warm->CheckLimits());
		EXPECT_FALSE(Warm->CheckLimits({.MaximumTotalBytes = 1}));
		EXPECT_FALSE(Candidate.ToOutput(Key(), {.MaximumValues = 1}));
		EXPECT_FALSE(Candidate.ToOutput(Key(), {.MaximumMetadataBytes = 1}));
		EXPECT_FALSE(FCacheRecord::FromOutput(Key(), *Warm, {.MaximumValues = 1}));
		auto Other = FCacheKey::FromHash(Key().GetBucket(), FXxHash128::HashBuffer("other"));
		EXPECT_FALSE(Candidate.ToOutput(Other));
	}
}
