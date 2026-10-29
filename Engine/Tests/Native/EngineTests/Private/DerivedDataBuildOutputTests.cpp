#include "CacheRecordStorage.h"
#include "BuildCacheRecordPrivate.h"
#include "CacheRecordFixtureCodec.h"
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

TEST(FDerivedDataBuildOutputTests, ObservationsStayBoundedAndNeverEnterCacheRecords)
{
	auto Make = [](uint64 Time) {
		FCbWriter Observation; Observation.AddUInt64(Time, "DurationNanoseconds");
		FBuildOutputBuilder Builder("Fixture.Output", 2); Builder.AddValue(FValueId::FromName("Data"), Bytes("same"));
		Builder.AddObservation(FValueId::FromName("Timing"), Observation.SaveObject().value()); return std::move(Builder).Build().value();
	};
	const auto First = Make(1), Second = Make(2); ASSERT_EQ(First.GetObservations().size(), 1u);
	EXPECT_FALSE(First.CheckLimits({.MaximumObservationBytes = 1})); EXPECT_FALSE(First.CheckLimits({.MaximumObservations = 0}));
	auto A = Durin::DerivedData::Private::MakeBuildCacheRecord(Key(), First).value(), B = Durin::DerivedData::Private::MakeBuildCacheRecord(Key(), Second).value();
	EXPECT_TRUE(std::ranges::equal(Durin::DerivedData::Private::EncodeCacheManifest(A)->GetBytes(), Durin::DerivedData::Private::EncodeCacheManifest(B)->GetBytes()));
	EXPECT_TRUE(std::ranges::equal(FCacheRecordFixtureCodec::Encode(A)->GetBytes(), FCacheRecordFixtureCodec::Encode(B)->GetBytes()));
	EXPECT_TRUE(Durin::DerivedData::Private::ReadBuildOutput(Key(), A)->GetObservations().empty());
	const auto Encoded = Durin::DerivedData::Private::EncodeCacheManifest(A).value();
	auto Loaded = Durin::DerivedData::Private::DecodeCacheManifest(Key(), Encoded);
	ASSERT_TRUE(Loaded);
	EXPECT_FALSE(Durin::DerivedData::Private::ReadBuildOutput(Key(), *Loaded)); // Values are attachments until resolved.
	FBuildOutputBuilder Limited("Fixture.Output", 2, {.MaximumObservations = 0}); FCbWriter Empty;
	EXPECT_FALSE(Limited.AddObservation(FValueId::FromName("Timing"), Empty.SaveObject().value()));
}

TEST(FDerivedDataBuildOutputTests, StructuredManifestPreservesLargeTablesAndRejectsUnknownIdentities)
{
	FBuildOutputBuilder Builder("Fixture.Output", 2);
	for (uint32 I = 0; I < 600; ++I) ASSERT_TRUE(Builder.AddValue(FValueId::FromName("Data").MakeIndexed(I), Bytes("same")));
	FCbWriter Metadata; Metadata.AddString("owned", "Name"); ASSERT_TRUE(Builder.AddMeta(FValueId::FromName("Meta"), Metadata.SaveObject().value()));
	const auto Record = Durin::DerivedData::Private::MakeBuildCacheRecord(Key(), std::move(Builder).Build().value()).value();
	const auto Encoded = Durin::DerivedData::Private::EncodeCacheManifest(Record).value();
	auto Loaded = Durin::DerivedData::Private::DecodeCacheManifest(Key(), Encoded);
	ASSERT_TRUE(Loaded);
	const auto MetadataLimit = Record.GetMeta().GetBytes().GetSize();
	const auto BuildMetadataLimit = Durin::DerivedData::Private::ReadBuildCacheRecord(Record)->GetMetadata()[0].Object.GetBytes().GetSize();
	EXPECT_TRUE(Durin::DerivedData::Private::DecodeCacheManifest(Key(), Encoded, {.MaximumMetadataBytes = MetadataLimit}));
	EXPECT_FALSE(Durin::DerivedData::Private::DecodeCacheManifest(Key(), Encoded, {.MaximumMetadataBytes = MetadataLimit - 1}));
	const auto Resolved = Loaded->WithValues(Record.GetValues()).value();
	EXPECT_TRUE(std::ranges::equal(FCacheRecordFixtureCodec::Encode(Record)->GetBytes(), FCacheRecordFixtureCodec::Encode(Resolved)->GetBytes()));
	EXPECT_TRUE(FCacheRecordFixtureCodec::Decode(Key(), FCacheRecordFixtureCodec::Encode(Resolved).value(), {.MaximumMetadataBytes = BuildMetadataLimit}));
	ASSERT_EQ(Loaded->GetValues().size(), 600u);
	EXPECT_TRUE(Loaded->GetMeta().GetBytes().SharesStorageWith(Encoded));
	EXPECT_TRUE(std::ranges::equal(Encoded.GetBytes(), Durin::DerivedData::Private::EncodeCacheManifest(*Loaded)->GetBytes()));
	EXPECT_TRUE(Durin::DerivedData::Private::EncodeCacheManifest(Record, Encoded.GetSize()));
	EXPECT_FALSE(Durin::DerivedData::Private::EncodeCacheManifest(Record, Encoded.GetSize() - 1));
	EXPECT_FALSE(Durin::DerivedData::Private::EncodeCacheManifest(Record, UINT64_MAX, 1));
	FCbLimits Limits{.MaximumEncodedBytes = 8 * 1024 * 1024, .MaximumDepth = 40, .MaximumFields = 700000};
	const auto Root = FCbObject::TryLoad(Encoded.MakeView(16, Encoded.GetSize() - 32), Limits).value().GetView();
	ASSERT_EQ(Root.Find("Values").AsArray().Num(), 600u);
	for (uint32 Case = 0; Case < 5; ++Case)
	{
		FCbWriter Values(Limits), Changed(Limits);
		for (const auto& Field : Root.Find("Values").AsArray().GetFields())
		{
			const auto Value = Field.AsObject(); const auto Original = Value.Find("Attachment").AsObject(); FCbWriter Attachment, Descriptor;
			Attachment.AddUInt64(Case == 0 ? 2 : 1, "Version"); Attachment.AddString(Case == 1 ? "sha256" : "xxh3-128", "Algorithm");
			Attachment.AddString(Case == 2 ? "../escape" : "values/v1", "Namespace"); Attachment.AddBinary(*Original.Find("Hash").AsBinary(), "Hash");
			Descriptor.AddObjectId(*Value.Find("Id").AsObjectId(), "Id"); Descriptor.AddUInt64(*Value.Find("RawSize").AsUInt64(), "RawSize");
			Descriptor.AddObject(Attachment.SaveObject().value(), "Attachment"); Values.AddObject(Descriptor.SaveObject().value());
		}
		Changed.AddArray(Values.SaveArray().value(), "Values");
		Changed.AddObject(FCbObject::TryLoad(Root.Find("Meta").GetBytes()).value(), "Meta");
		Changed.AddString(std::string(*Root.Find("Bucket").AsString()), "Bucket"); Changed.AddBinary(*Root.Find("Key").AsBinary(), "Key");
		if (Case == 3) Changed.AddNull("Unknown");
		FBinaryWriter Wire;
		Wire.WriteHeader({0x4d424444, Case == 4 ? 2u : 3u, 0});
		Wire.WriteBytes(Changed.SaveObject()->GetBytes().GetBytes());
		Wire.WriteHash128(FXxHash128::HashBuffer(Wire.GetBytes()));
		EXPECT_FALSE(Durin::DerivedData::Private::DecodeCacheManifest(Key(), FSharedByteBuffer::Take(Wire.TakeBytes()))) << Case;
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
	auto Record = Durin::DerivedData::Private::MakeBuildCacheRecord(Key(), *Built);
	ASSERT_TRUE(Record);
	const auto Reference = Record->RemoveValueData();
	ASSERT_TRUE(Reference.IsValid());
	EXPECT_EQ(Reference.GetKey(), Key());
	EXPECT_EQ(Durin::DerivedData::Private::ReadBuildCacheRecord(Reference)->GetSchema(), Built->GetSchema());
	EXPECT_EQ(Durin::DerivedData::Private::ReadBuildCacheRecord(Reference)->GetSchemaVersion(), Built->GetSchemaVersion());
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
	const auto Incomplete = Durin::DerivedData::Private::ReadBuildOutput(Key(), Reference);
	ASSERT_FALSE(Incomplete);
	EXPECT_EQ(Incomplete.error().Code, ECacheError::InvalidRequest);
	const auto Encoded = FCacheRecordFixtureCodec::Encode(Reference); ASSERT_FALSE(Encoded);
	EXPECT_EQ(Encoded.error().Code, ECacheError::InvalidRequest);
	EXPECT_TRUE(Durin::DerivedData::Private::ReadBuildOutput(Key(), *Record));
	EXPECT_TRUE(FCacheRecordFixtureCodec::Encode(*Record));
	EXPECT_FALSE(FCacheRecord{}.RemoveValueData().IsValid());
}

TEST(FDerivedDataBuildOutputTests, IndependentRecordsRetainPayloadsMetadataAndMessages)
{
	auto Built = Output();
	const auto Backing = Built.FindValue(FValueId::FromName("A"))->GetData();
	auto Record = Durin::DerivedData::Private::MakeBuildCacheRecord(Key(), Built);
	ASSERT_TRUE(Record);
	Built = {};
	const auto Reference = Record->RemoveValueData();
	const auto View = Durin::DerivedData::Private::ReadBuildCacheRecord(Reference).value();
	ASSERT_EQ(View.GetMetadata().size(), 1u);
	EXPECT_TRUE(View.GetMetadata()[0].Object.GetBytes().SharesStorageWith(Durin::DerivedData::Private::ReadBuildCacheRecord(*Record)->GetMetadata()[0].Object.GetBytes()));
	ASSERT_EQ(View.GetMessages().size(), 1u);
	EXPECT_EQ(View.GetMessages()[0].Text, "warning");
	auto Loaded = Durin::DerivedData::Private::ReadBuildOutput(Key(), *Record);
	ASSERT_TRUE(Loaded);
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
	auto Record = Durin::DerivedData::Private::MakeBuildCacheRecord(Key(), *Built);
	ASSERT_FALSE(Record);
	EXPECT_EQ(Record.error().Code, ECacheError::InvalidRequest);
}

TEST(FDerivedDataBuildOutputTests, RawAndCompressedRecordsRoundTripWithRetainedViews)
{
	auto Built = Output();
	auto Record = Durin::DerivedData::Private::MakeBuildCacheRecord(Key(), Built);
	ASSERT_TRUE(Record);
	auto Raw = FCacheRecordFixtureCodec::Encode(*Record);
	ASSERT_TRUE(Raw);
	auto Decoded = FCacheRecordFixtureCodec::Decode(Key(), *Raw);
	ASSERT_TRUE(Decoded);
	auto Loaded = Durin::DerivedData::Private::ReadBuildOutput(Key(), *Decoded);
	ASSERT_TRUE(Loaded);
	EXPECT_TRUE(Loaded->FindValue(FValueId::FromName("A"))->GetData().SharesStorageWith(*Raw));
	auto Compressed = FCacheRecordFixtureCodec::CompressEncoded(*Raw);
	ASSERT_TRUE(Compressed);
	auto Inflated = FCacheRecordFixtureCodec::Decode(Key(), *Compressed);
	ASSERT_TRUE(Inflated);
	EXPECT_TRUE(Durin::DerivedData::Private::ReadBuildOutput(Key(), *Inflated).has_value());
}

TEST(FDerivedDataBuildOutputTests, CorruptionAndWrongKeysAreRejected)
{
	auto Record = Durin::DerivedData::Private::MakeBuildCacheRecord(Key(), Output()).value();
	auto Raw = FCacheRecordFixtureCodec::Encode(Record).value();
	FByteBuffer Corrupt(Raw.begin(), Raw.end());
	Corrupt[Corrupt.size() / 2] ^= std::byte{1};
	EXPECT_FALSE(FCacheRecordFixtureCodec::Decode(Key(), FSharedByteBuffer::Take(std::move(Corrupt))));
	auto Other = FCacheKey::FromHash(FCacheBucket::FromString("OutputFixtures"), FXxHash128::HashBuffer("other")); EXPECT_FALSE(FCacheRecordFixtureCodec::Decode(Other, Raw));
}

TEST(FDerivedDataBuildOutputTests, PayloadHashesAreCheckedEvenWithAValidEnvelopeHash)
{
	auto Record = Durin::DerivedData::Private::MakeBuildCacheRecord(Key(), Output()).value();
	auto Raw = FCacheRecordFixtureCodec::Encode(Record).value();
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
		auto Decoded = FCacheRecordFixtureCodec::Decode(Key(), FSharedByteBuffer::Take(Writer.TakeBytes()));
		ASSERT_FALSE(Decoded);
		EXPECT_EQ(Decoded.error().Code, ECacheError::Corrupt);
	}
}

TEST(FDerivedDataBuildOutputTests, ReusedOutputStillEnforcesStricterLimitsAndKeys)
{
	auto Built = Output();
	auto Record = Durin::DerivedData::Private::MakeBuildCacheRecord(Key(), Built);
	ASSERT_TRUE(Record);
	auto Raw = FCacheRecordFixtureCodec::Encode(*Record); ASSERT_TRUE(Raw);
	auto Loaded = FCacheRecordFixtureCodec::Decode(Key(), *Raw); ASSERT_TRUE(Loaded);
	for (const auto& Candidate : {*Record, *Loaded})
	{
		auto Warm = Durin::DerivedData::Private::ReadBuildOutput(Key(), Candidate);
		ASSERT_TRUE(Warm);
		EXPECT_TRUE(Warm->CheckLimits());
		EXPECT_FALSE(Warm->CheckLimits({.MaximumTotalBytes = 1}));
		EXPECT_FALSE(Durin::DerivedData::Private::ReadBuildOutput(Key(), Candidate, {.MaximumValues = 1}));
		EXPECT_FALSE(Durin::DerivedData::Private::ReadBuildOutput(Key(), Candidate, {.MaximumMetadataBytes = 1}));
		EXPECT_FALSE(Durin::DerivedData::Private::MakeBuildCacheRecord(Key(), *Warm, {.MaximumValues = 1}));
		auto Other = FCacheKey::FromHash(Key().GetBucket(), FXxHash128::HashBuffer("other"));
		EXPECT_FALSE(Durin::DerivedData::Private::ReadBuildOutput(Other, Candidate));
	}
}
