#include "DerivedDataBuildOutput.h"
#include "Serialization/BinaryFormat.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "NativeTestSupport.h"
#include <gtest/gtest.h>

namespace
{
	using namespace Durin;
	using namespace Durin::DerivedData;

	auto Bytes(std::string_view Text) -> FSharedByteBuffer
	{
		return FSharedByteBuffer::Copy(std::as_bytes(std::span(Text)));
	}
	auto Key(std::string_view Text = "action") -> FCacheKey
	{
		return FCacheKey::FromHash(FCacheBucket::FromString("OutputFixtures"), FXxHash128::HashBuffer(Text));
	}
	auto Data() -> FBuildOutputData
	{
		return {.Schema = "Fixture.Output", .SchemaVersion = 1, .Metadata = Bytes("meta"),
			.Values = {{"Mip/1", Bytes("small")}, {"Mip/0", Bytes("large")}},
			.Messages = {{EBuildMessageSeverity::Warning, "deterministic warning"}}};
	}
}

TEST(FDerivedDataBuildOutputTests, CanonicalValuesAndMessagesPreserveBacking)
{
	auto Input = Data();
	const auto Backing = Input.Values[0].Data;
	auto Output = FBuildOutput::TryCreate(std::move(Input));
	ASSERT_TRUE(Output);
	EXPECT_EQ(Output->GetSchema(), "Fixture.Output");
	EXPECT_EQ(Output->GetSchemaVersion(), 1u);
	ASSERT_EQ(Output->GetValues().size(), 2u);
	EXPECT_EQ(Output->GetValues()[0].Id, "Mip/0");
	ASSERT_NE(Output->FindValue("Mip/1"), nullptr);
	EXPECT_TRUE(Output->FindValue("Mip/1")->Data.SharesStorageWith(Backing));
	EXPECT_EQ(Output->FindValue("Missing"), nullptr);
	ASSERT_EQ(Output->GetMessages().size(), 1u);
	EXPECT_EQ(Output->GetMessages()[0].Text, "deterministic warning");
}

TEST(FDerivedDataBuildOutputTests, RejectsInvalidSchemasIdsDuplicatesAndMessages)
{
	auto Input = Data(); Input.SchemaVersion = 0;
	EXPECT_FALSE(FBuildOutput::TryCreate(std::move(Input)));
	for (const auto Id : {"", "bad id", "bad\\id"})
	{
		Input = Data(); Input.Values[0].Id = Id;
		EXPECT_FALSE(FBuildOutput::TryCreate(std::move(Input)));
	}
	Input = Data(); Input.Schema.assign(97, 's');
	EXPECT_FALSE(FBuildOutput::TryCreate(std::move(Input)));
	Input = Data(); Input.Values.push_back(Input.Values.front());
	EXPECT_FALSE(FBuildOutput::TryCreate(std::move(Input)));
	Input = Data(); Input.Messages[0].Severity = static_cast<EBuildMessageSeverity>(255);
	EXPECT_FALSE(FBuildOutput::TryCreate(std::move(Input)));
	Input = Data(); Input.Messages[0].Text.assign(4097, 'x');
	EXPECT_FALSE(FBuildOutput::TryCreate(std::move(Input)));
	Input = Data(); Input.Messages[0].Text = std::string("a\0b", 3);
	EXPECT_FALSE(FBuildOutput::TryCreate(std::move(Input)));
}

TEST(FDerivedDataBuildOutputTests, EnforcesIndependentBoundsAndCountsSharedViewsLogically)
{
	EXPECT_FALSE(FBuildOutput::TryCreate(Data(), {.MaximumTotalBytes = 1}));
	EXPECT_FALSE(FBuildOutput::TryCreate(Data(), {.MaximumMetadataBytes = 3}));
	EXPECT_FALSE(FBuildOutput::TryCreate(Data(), {.MaximumValues = 1}));
	EXPECT_FALSE(FBuildOutput::TryCreate(Data(), {.MaximumMessages = 0}));
	auto Input = Data();
	Input.Metadata = {}; Input.Messages.clear();
	Input.Values[1].Data = Input.Values[0].Data;
	EXPECT_FALSE(FBuildOutput::TryCreate(Input, {.MaximumTotalBytes = 9}));
	EXPECT_TRUE(FBuildOutput::TryCreate(Input, {.MaximumTotalBytes = 10}));
	Input.Values.resize(4097);
	EXPECT_FALSE(FBuildOutput::TryCreate(std::move(Input), {.MaximumValues = 5000}));
}

TEST(FDerivedDataBuildOutputTests, RecordAndConsumerOutliveAllOriginalOwners)
{
	FCacheRecord Record;
	FSharedByteBuffer Consumer;
	const std::byte* Address = nullptr;
	{
		auto Backing = Bytes("prefixpayloadsuffix");
		auto View = Backing.MakeView(6, 7);
		Address = View.data();
		auto Output = FBuildOutput::TryCreate({.Schema = "Fixture.View", .SchemaVersion = 1,
			.Values = {{"Payload", View}}});
		ASSERT_TRUE(Output);
		auto Stored = FCacheRecord::FromOutput(Key(), *Output);
		ASSERT_TRUE(Stored);
		Record = std::move(*Stored);
		EXPECT_EQ(Record.GetValues()[0].Data.data(), Address);
		EXPECT_EQ(Record.GetValueHashes()[0], FXxHash128::HashBuffer(View.GetBytes()));
	}
	auto Loaded = Record.ToOutput(Key());
	ASSERT_TRUE(Loaded);
	EXPECT_EQ(Loaded->FindValue("Payload")->Data.data(), Address);
	Consumer = Loaded->FindValue("Payload")->Data;
	Loaded = FBuildOutput{}; Record = {};
	EXPECT_EQ(Consumer.data(), Address);
	EXPECT_EQ(FXxHash128::HashBuffer(Consumer.GetBytes()), FXxHash128::HashBuffer("payload"));
}

TEST(FDerivedDataBuildOutputTests, OptionalRecordRejectionPreservesUsableOutput)
{
	auto Output = FBuildOutput::TryCreate(Data());
	ASSERT_TRUE(Output);
	const auto* Address = Output->FindValue("Mip/0")->Data.data();
	auto Rejected = FCacheRecord::FromOutput(Key(), *Output, {.MaximumTotalBytes = 1});
	ASSERT_FALSE(Rejected);
	EXPECT_EQ(Rejected.error().Code, ECacheError::ValueTooLarge);
	EXPECT_EQ(Output->FindValue("Mip/0")->Data.data(), Address);
	EXPECT_EQ(Output->GetMessages()[0].Text, "deterministic warning");
	EXPECT_FALSE(FCacheRecord::FromOutput({}, *Output));
	EXPECT_FALSE(FCacheRecord::FromOutput(Key(), {}));
}

TEST(FDerivedDataBuildOutputTests, RecordChecksRequestedKeyAndReadBudget)
{
	auto Output = FBuildOutput::TryCreate(Data());
	ASSERT_TRUE(Output);
	auto Record = FCacheRecord::FromOutput(Key(), *Output);
	ASSERT_TRUE(Record);
	EXPECT_EQ(Record->GetKey(), Key());
	EXPECT_EQ(Record->GetSchema(), Output->GetSchema());
	EXPECT_EQ(Record->GetSchemaVersion(), Output->GetSchemaVersion());
	EXPECT_TRUE(Record->GetMetadata().SharesStorageWith(Output->GetMetadata()));
	EXPECT_EQ(Record->GetMetadataHash(), FXxHash128::HashBuffer(Output->GetMetadata().GetBytes()));
	EXPECT_EQ(Record->GetMessages()[0].Text, Output->GetMessages()[0].Text);
	auto Wrong = Record->ToOutput(Key("other"));
	ASSERT_FALSE(Wrong);
	EXPECT_EQ(Wrong.error().Code, ECacheError::Corrupt);
	EXPECT_FALSE(Record->ToOutput(Key(), {.MaximumTotalBytes = 1}));
	EXPECT_TRUE(Record->ToOutput(Key()));
	EXPECT_FALSE(FCacheRecord{}.ToOutput(Key()));
}

TEST(FDerivedDataBuildOutputTests, RecordDetectsViolatedImmutableByteContract)
{
	// Deliberately violate Share's caller contract to verify both hash boundaries.
	for (bool bMetadata : {false, true})
	{
		auto Mutable = std::make_shared<FByteBuffer>(4, std::byte{1});
		auto Input = Data();
		if (bMetadata) Input.Metadata = FSharedByteBuffer::Share(Mutable);
		else Input.Values[0].Data = FSharedByteBuffer::Share(Mutable);
		auto Output = FBuildOutput::TryCreate(std::move(Input));
		ASSERT_TRUE(Output);
		auto Record = FCacheRecord::FromOutput(Key(), *Output);
		ASSERT_TRUE(Record);
		(*Mutable)[0] = std::byte{2};
		auto Loaded = Record->ToOutput(Key());
		ASSERT_FALSE(Loaded);
		EXPECT_EQ(Loaded.error().Code, ECacheError::Corrupt);
	}
}

TEST(FDerivedDataBuildOutputTests, EncodedRecordRetainsBackendAllocationAfterDecode)
{
	auto Output = FBuildOutput::TryCreate(Data());
	ASSERT_TRUE(Output);
	auto Record = FCacheRecord::FromOutput(Key(), *Output);
	ASSERT_TRUE(Record);
	auto Encoded = Record->Encode();
	ASSERT_TRUE(Encoded);
	auto Again = Record->Encode();
	ASSERT_TRUE(Again);
	EXPECT_TRUE(std::ranges::equal(Encoded->GetBytes(), Again->GetBytes()));
	const auto OriginalHash = FXxHash128::HashBuffer(Output->FindValue("Mip/0")->Data.GetBytes());
	auto Owner = std::make_shared<const FByteBuffer>(Encoded->begin(), Encoded->end());
	std::weak_ptr<const FByteBuffer> Lifetime = Owner;
	auto BackendBytes = FSharedByteBuffer::Share(Owner);
	auto Loaded = FCacheRecord::Decode(Key(), BackendBytes);
	ASSERT_TRUE(Loaded) << Loaded.error().Diagnostic;
	EXPECT_TRUE(Loaded->GetMetadata().SharesStorageWith(BackendBytes));
	EXPECT_TRUE(Loaded->GetValues()[0].Data.SharesStorageWith(BackendBytes));
	EXPECT_EQ(Loaded->GetMessages()[0].Text, "deterministic warning");
	auto Consumer = Loaded->ToOutput(Key());
	ASSERT_TRUE(Consumer);
	Owner.reset(); BackendBytes = {}; Loaded = FCacheRecord{}; Record = FCacheRecord{};
	Encoded = FSharedByteBuffer{}; Output = FBuildOutput{};
	EXPECT_FALSE(Lifetime.expired());
	EXPECT_EQ(FXxHash128::HashBuffer(Consumer->FindValue("Mip/0")->Data.GetBytes()), OriginalHash);
	Consumer = FBuildOutput{};
	EXPECT_TRUE(Lifetime.expired());
}

TEST(FDerivedDataBuildOutputTests, EnvelopeRejectsEveryTruncationAndSingleByteCorruption)
{
	auto Output = FBuildOutput::TryCreate(Data());
	ASSERT_TRUE(Output);
	auto Record = FCacheRecord::FromOutput(Key(), *Output);
	ASSERT_TRUE(Record);
	auto Encoded = Record->Encode();
	ASSERT_TRUE(Encoded);
	for (size_t Index = 0; Index < Encoded->size(); ++Index)
	{
		EXPECT_FALSE(FCacheRecord::Decode(Key(), Encoded->MakeView(0, Index))) << Index;
		FByteBuffer Corrupt(Encoded->begin(), Encoded->end());
		Corrupt[Index] ^= std::byte{1};
		EXPECT_FALSE(FCacheRecord::Decode(Key(), FSharedByteBuffer::Take(std::move(Corrupt)))) << Index;
	}
	EXPECT_FALSE(FCacheRecord::Decode(Key("different"), *Encoded));
	EXPECT_FALSE(FCacheRecord::Decode(FCacheKey::FromHash(FCacheBucket::FromString("OtherBucket"), Key().GetHash()), *Encoded));
	EXPECT_FALSE(FCacheRecord::Decode({}, *Encoded));
}

TEST(FDerivedDataBuildOutputTests, EncodedAndLogicalBudgetsAreIndependent)
{
	auto Output = FBuildOutput::TryCreate(Data());
	ASSERT_TRUE(Output);
	auto Record = FCacheRecord::FromOutput(Key(), *Output);
	ASSERT_TRUE(Record);
	const auto* Address = Output->FindValue("Mip/0")->Data.data();
	auto Encoded = Record->Encode();
	ASSERT_TRUE(Encoded);
	EXPECT_FALSE(Record->Encode(1));
	EXPECT_FALSE(Record->Encode(Encoded->size() - 1)); // Integrity trailer is budgeted.
	EXPECT_TRUE(Record->Encode(Encoded->size()));
	EXPECT_FALSE(FCacheRecord::Decode(Key(), *Encoded, {}, Encoded->size() - 1));
	EXPECT_TRUE(FCacheRecord::Decode(Key(), *Encoded, {}, Encoded->size()));
	EXPECT_FALSE(FCacheRecord::Decode(Key(), *Encoded, {.MaximumTotalBytes = 1}));
	EXPECT_FALSE(FCacheRecord::Decode(Key(), *Encoded, {.MaximumMetadataBytes = 3}));
	EXPECT_FALSE(FCacheRecord::Decode(Key(), *Encoded, {.MaximumValues = 1}));
	EXPECT_FALSE(FCacheRecord::Decode(Key(), *Encoded, {.MaximumMessages = 0}));
	EXPECT_EQ(Output->FindValue("Mip/0")->Data.data(), Address);
	EXPECT_FALSE(FCacheRecord{}.Encode());
}

namespace
{
	// Independent wire fixture with recomputed envelope integrity: invalid tables
	// must fail semantic/bounds checks even when the transport checksum is valid.
	auto WireRecord(uint32 Fault) -> FSharedByteBuffer
	{
		FBinaryWriter Writer;
		Writer.WriteHeader({0x52424444, Fault == 1 ? 2u : 1u, Fault == 2 ? 1u : 0u});
		Writer.WriteString(Key().GetBucket().ToString());
		Writer.WriteHash128(Key().GetHash());
		Writer.WriteString("Fixture.Output");
		Writer.WriteU32(Fault == 3 ? 0u : 1u);
		Writer.WriteU32(Fault == 4 ? 4097u : 2u);
		Writer.WriteU32(1);
		Writer.WriteU64(Fault == 5 ? 4ull * 1024 * 1024 + 1 : 4);
		Writer.WriteHash128(Fault == 6 ? FXxHash128{} : FXxHash128::HashBuffer("meta"));
		Writer.WriteString("Mip/0");
		Writer.WriteU64(Fault == 7 ? 3 : Fault == 8 ? 5 : 4);
		Writer.WriteU64(Fault == 9 ? std::numeric_limits<uint64>::max() : 5);
		Writer.WriteHash128(Fault == 10 ? FXxHash128{} : FXxHash128::HashBuffer("large"));
		Writer.WriteString(Fault == 11 ? "Mip/0" : Fault == 12 ? "AAA" : Fault == 13 ? "bad id" : "Mip/1");
		Writer.WriteU64(Fault == 14 ? 4 : 9);
		Writer.WriteU64(5);
		Writer.WriteHash128(FXxHash128::HashBuffer("small"));
		Writer.WriteU8(Fault == 15 ? 255 : static_cast<uint8>(EBuildMessageSeverity::Warning));
		Writer.WriteString(Fault == 16 ? std::string_view("a\0b", 3) : "warning");
		Writer.WriteBytes(std::as_bytes(std::span(std::string_view("metalargesmall"))));
		if (Fault == 17) Writer.WriteU8(0);
		const auto Hash = FXxHash128::HashBuffer(Writer.GetBytes());
		Writer.WriteHash128(Hash);
		return FSharedByteBuffer::Take(Writer.TakeBytes());
	}
}

TEST(FDerivedDataBuildOutputTests, ChecksAuthenticatedTablesOffsetsSchemasAndBlockIntegrity)
{
	auto Valid = FCacheRecord::Decode(Key(), WireRecord(0));
	ASSERT_TRUE(Valid) << Valid.error().Diagnostic;
	for (uint32 Fault = 1; Fault <= 17; ++Fault)
		EXPECT_FALSE(FCacheRecord::Decode(Key(), WireRecord(Fault))) << Fault;
}

TEST(FDerivedDataBuildOutputTests, RealBackendRoundTripAndFailedWritePreservePublishedBlocks)
{
	struct FDirectoryScope
	{
		std::string Previous = FPaths::DerivedDataCacheDir();
		std::filesystem::path Root = Testing::CreateTestFixtureDirectory("BuildRecordPersistence");
		FDirectoryScope() { FPaths::SetDerivedDataCacheDirForTests(Root.generic_string()); }
		~FDirectoryScope()
		{
			FPaths::SetDerivedDataCacheDirForTests(Previous);
			Testing::RemoveTestWorkDirectory(Root);
		}
	} Directory;
	auto Output = FBuildOutput::TryCreate(Data());
	ASSERT_TRUE(Output);
	const auto* OriginalAddress = Output->FindValue("Mip/0")->Data.data();
	auto Record = FCacheRecord::FromOutput(Key(), *Output);
	ASSERT_TRUE(Record);
	auto Encoded = Record->Encode();
	ASSERT_TRUE(Encoded);
	ASSERT_TRUE(GetCache().Put({Key(), *Encoded, 4096}));
	auto Backend = GetCache().Get({Key(), 4096});
	ASSERT_TRUE(Backend); ASSERT_TRUE(*Backend);
	auto Loaded = FCacheRecord::Decode(Key(), **Backend);
	ASSERT_TRUE(Loaded);
	EXPECT_TRUE(Loaded->GetValues()[0].Data.SharesStorageWith(**Backend));
	auto Consumer = Loaded->ToOutput(Key());
	ASSERT_TRUE(Consumer);
	Backend = std::optional<FSharedByteBuffer>{}; Loaded = FCacheRecord{};
	EXPECT_TRUE(std::ranges::equal(Consumer->FindValue("Mip/0")->Data.GetBytes(), Output->FindValue("Mip/0")->Data.GetBytes()));

	const auto Blocker = Directory.Root / "blocked";
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(Bytes("file").GetBytes(), Blocker));
	FPaths::SetDerivedDataCacheDirForTests((Blocker / "cache").generic_string());
	auto FailedWrite = GetCache().Put({Key(), *Encoded, 4096});
	ASSERT_FALSE(FailedWrite);
	EXPECT_EQ(FailedWrite.error().Code, ECacheError::StorageFailure);
	EXPECT_EQ(Output->FindValue("Mip/0")->Data.data(), OriginalAddress);
	EXPECT_EQ(Record->GetValues()[0].Data.data(), OriginalAddress);
	EXPECT_TRUE(Record->ToOutput(Key()));
}

TEST(FDerivedDataBuildOutputTests, OptionalCompressionRetainsOneInflatedAllocation)
{
	auto Input = Data();
	Input.Values[0].Data = FSharedByteBuffer::Take(FByteBuffer(16384, std::byte{7}));
	auto Output = FBuildOutput::TryCreate(std::move(Input));
	ASSERT_TRUE(Output);
	auto Record = FCacheRecord::FromOutput(Key(), *Output);
	ASSERT_TRUE(Record);
	auto Raw = Record->Encode();
	ASSERT_TRUE(Raw);
	auto Compressed = FCacheRecord::CompressEncoded(*Raw);
	ASSERT_TRUE(Compressed);
	EXPECT_LT(Compressed->size(), Raw->size());
	EXPECT_FALSE(FCacheRecord::CompressEncoded(*Compressed)); // No nested compression.
	EXPECT_FALSE(FCacheRecord::CompressEncoded(*Raw, 40));
	EXPECT_FALSE(FCacheRecord::CompressEncoded({}));
	auto Loaded = FCacheRecord::Decode(Key(), *Compressed);
	ASSERT_TRUE(Loaded) << Loaded.error().Diagnostic;
	EXPECT_FALSE(Loaded->GetMetadata().SharesStorageWith(*Compressed));
	EXPECT_TRUE(Loaded->GetMetadata().SharesStorageWith(Loaded->GetValues()[0].Data));
	EXPECT_TRUE(Loaded->GetValues()[0].Data.SharesStorageWith(Loaded->GetValues()[1].Data));
	auto Consumer = Loaded->ToOutput(Key());
	ASSERT_TRUE(Consumer);
	EXPECT_TRUE(Consumer->FindValue("Mip/1")->Data.SharesStorageWith(Loaded->GetValues()[1].Data));
	EXPECT_TRUE(std::ranges::equal(Consumer->FindValue("Mip/1")->Data.GetBytes(), Output->FindValue("Mip/1")->Data.GetBytes()));
	EXPECT_EQ(Record->GetValues()[1].Data.data(), Output->FindValue("Mip/1")->Data.data());
	Loaded = FCacheRecord{}; Raw = FSharedByteBuffer{}; Compressed = FSharedByteBuffer{};
	EXPECT_EQ(Consumer->FindValue("Mip/1")->Data.size(), 16384u);
}

TEST(FDerivedDataBuildOutputTests, CompressedFramesRejectFalseSizesTrailingBytesAndCorruption)
{
	auto Output = FBuildOutput::TryCreate(Data());
	ASSERT_TRUE(Output);
	auto Record = FCacheRecord::FromOutput(Key(), *Output);
	ASSERT_TRUE(Record);
	auto Raw = Record->Encode();
	ASSERT_TRUE(Raw);
	auto Compressed = FCacheRecord::CompressEncoded(*Raw);
	ASSERT_TRUE(Compressed);
	EXPECT_FALSE(FCacheRecord::Decode(Key(), *Compressed, {}, Compressed->size() - 1));
	EXPECT_FALSE(FCacheRecord::Decode(Key(), *Compressed, {.MaximumTotalBytes = 1}));
	for (uint32 Fault = 0; Fault < 4; ++Fault)
	{
		FByteBuffer Body(Compressed->begin(), Compressed->end() - 16);
		if (Fault == 0) Body[16] ^= std::byte{1}; // Declared decoded size disagrees with frame.
		if (Fault == 1) std::fill(Body.begin() + 16, Body.begin() + 24, std::byte{255});
		if (Fault == 2) Body.push_back(std::byte{0}); // Trailing bytes after one Zstd frame.
		if (Fault == 3) Body[24] ^= std::byte{255}; // Bad frame magic, valid envelope hash.
		FBinaryWriter Writer;
		Writer.WriteBytes(Body);
		Writer.WriteHash128(FXxHash128::HashBuffer(Body));
		EXPECT_FALSE(FCacheRecord::Decode(Key(), FSharedByteBuffer::Take(Writer.TakeBytes()))) << Fault;
	}
}

TEST(FDerivedDataBuildOutputTests, ByteAlignedRecordSubviewsAndEmptyOutputsRemainValid)
{
	static_assert(alignof(std::byte) == 1);
	for (bool bEmpty : {false, true})
	{
		auto Output = FBuildOutput::TryCreate(bEmpty
			? FBuildOutputData{.Schema = "Empty.Output", .SchemaVersion = 1} : Data());
		ASSERT_TRUE(Output);
		auto Record = FCacheRecord::FromOutput(Key(), *Output);
		ASSERT_TRUE(Record);
		auto Raw = Record->Encode();
		ASSERT_TRUE(Raw);
		FByteBuffer Prefixed{std::byte{0}};
		Prefixed.insert(Prefixed.end(), Raw->begin(), Raw->end());
		const auto Backing = FSharedByteBuffer::Take(std::move(Prefixed));
		auto Loaded = FCacheRecord::Decode(Key(), Backing.MakeView(1, Raw->size()));
		ASSERT_TRUE(Loaded) << Loaded.error().Diagnostic;
		EXPECT_TRUE(Loaded->GetMetadata().SharesStorageWith(Backing));
		EXPECT_EQ(Loaded->GetValues().size(), bEmpty ? 0u : 2u);
		EXPECT_EQ(Loaded->GetSchema(), Output->GetSchema());
	}
}

TEST(FDerivedDataBuildOutputTests, NativeProvenanceSurvivesMemoryRecordsButNeverSerialization)
{
	std::vector<uint32> Values{1, 7, 11, 19};
	const auto* Original = Values.data();
	auto Storage = FSharedByteBuffer::TakeNative(std::move(Values));
	auto Output = FBuildOutput::TryCreate({.Schema = "NativeFixture.Output", .SchemaVersion = 1,
		.Values = {{"Indices", Storage.MakeView(sizeof(uint32), sizeof(uint32) * 2)}}});
	ASSERT_TRUE(Output);
	auto Record = FCacheRecord::FromOutput(Key(), *Output);
	ASSERT_TRUE(Record);
	Storage = {}; Output = {};
	auto MemoryOutput = Record->ToOutput(Key());
	ASSERT_TRUE(MemoryOutput);
	auto Native = MemoryOutput->FindValue("Indices")->Data.GetNativeView<uint32>();
	ASSERT_TRUE(Native);
	EXPECT_EQ(Native->data(), Original + 1);
	EXPECT_EQ((*Native)[0], 7u);
	EXPECT_EQ((*Native)[1], 11u);
	auto Encoded = Record->Encode();
	ASSERT_TRUE(Encoded);
	for (bool Compressed : {false, true})
	{
		auto Bytes = Compressed ? FCacheRecord::CompressEncoded(*Encoded).value() : *Encoded;
		auto Decoded = FCacheRecord::Decode(Key(), Bytes);
		ASSERT_TRUE(Decoded);
		auto Loaded = Decoded->ToOutput(Key());
		ASSERT_TRUE(Loaded);
		const auto& Block = Loaded->FindValue("Indices")->Data;
		EXPECT_FALSE(Block.GetNativeView<uint32>());
		EXPECT_TRUE(std::ranges::equal(Block.GetBytes(), MemoryOutput->FindValue("Indices")->Data.GetBytes()));
	}
}
