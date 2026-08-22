#include <gtest/gtest.h>

#include "CoreGlobals.h"
#include "HAL/PlatformLTS.h"
#include "Hash/XxHash.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "RHI.h"
#include "RenderingThread.h"
#include "Serialization/Archive.h"
#include "Serialization/BoundedPayloadSerialization.h"
#include "StaticMesh/StaticMeshDerivedData.h"

namespace
{
	using namespace Durin;

	DECLARE_RENDER_COMMAND_TAG(
		FSetPartialStaticMeshReadinessResources,
		SetPartialStaticMeshReadinessResources);
	DECLARE_RENDER_COMMAND_TAG(
		FSetCompleteStaticMeshReadinessResources,
		SetCompleteStaticMeshReadinessResources);
	DECLARE_RENDER_COMMAND_TAG(
		FReleaseStaticMeshReadinessResources,
		ReleaseStaticMeshReadinessResources);

	auto MakeBounds(const FVector3& Minimum, const FVector3& Maximum) -> FBox
	{
		return FBox(Minimum, Maximum);
	}

	auto MakeSingleSectionFixture() -> FStaticMeshPayloadData
	{
		FStaticMeshPayloadData Payload;
		Payload.LocalBounds = MakeBounds(FVector3(0.0, 0.0, 0.0), FVector3(1.0, 1.0, 0.0));
		Payload.MaterialSlotCount = 1;

		FStaticMeshPayloadLOD& LOD = Payload.LODs.emplace_back();
		LOD.LocalBounds = Payload.LocalBounds;
		LOD.Positions = {
			FVector3f(0.0f, 0.0f, 0.0f),
			FVector3f(1.0f, 0.0f, 0.0f),
			FVector3f(0.0f, 1.0f, 0.0f)};
		LOD.Normals.assign(3, FVector3f(0.0f, 0.0f, 1.0f));
		LOD.Tangents.assign(3, FVector4f(1.0f, 0.0f, 0.0f, 1.0f));
		LOD.TexCoords[0] = {
			FVector2f(0.0f, 0.0f),
			FVector2f(1.0f, 0.0f),
			FVector2f(0.0f, 1.0f)};
		LOD.Indices = {0, 1, 2};
		LOD.Sections = {{
			.FirstIndex = 0,
			.IndexCount = 3,
			.MinVertexIndex = 0,
			.MaxVertexIndex = 2,
			.MaterialSlotIndex = 0,
			.LocalBounds = Payload.LocalBounds}};
		LOD.NumTexCoords = 1;
		return Payload;
	}

	auto MakeMultiMaterialFixture() -> FStaticMeshPayloadData
	{
		FStaticMeshPayloadData Payload;
		Payload.LocalBounds = MakeBounds(FVector3(-1.0, -1.0, 0.0), FVector3(1.0, 1.0, 0.0));
		Payload.MaterialSlotCount = 2;

		FStaticMeshPayloadLOD& LOD = Payload.LODs.emplace_back();
		LOD.LocalBounds = Payload.LocalBounds;
		LOD.Positions = {
			FVector3f(-1.0f, -1.0f, 0.0f),
			FVector3f(0.0f, -1.0f, 0.0f),
			FVector3f(-1.0f, 1.0f, 0.0f),
			FVector3f(1.0f, 1.0f, 0.0f)};
		LOD.Normals.assign(4, FVector3f(0.0f, 0.0f, 1.0f));
		LOD.Tangents.assign(4, FVector4f(1.0f, 0.0f, 0.0f, -1.0f));
		LOD.TexCoords[0] = {
			FVector2f(0.0f, 0.0f), FVector2f(0.5f, 0.0f),
			FVector2f(0.0f, 1.0f), FVector2f(1.0f, 1.0f)};
		LOD.TexCoords[1] = {
			FVector2f(0.1f, 0.2f), FVector2f(0.3f, 0.4f),
			FVector2f(0.5f, 0.6f), FVector2f(0.7f, 0.8f)};
		LOD.TexCoords[2] = {
			FVector2f(1.0f, 1.0f), FVector2f(1.0f, 0.0f),
			FVector2f(0.0f, 1.0f), FVector2f(0.0f, 0.0f)};
		LOD.TexCoords[3] = {
			FVector2f(0.25f, 0.25f), FVector2f(0.5f, 0.25f),
			FVector2f(0.25f, 0.5f), FVector2f(0.5f, 0.5f)};
		LOD.Colors = {
			FVector4f(1.0f, 0.0f, 0.0f, 1.0f),
			FVector4f(0.0f, 1.0f, 0.0f, 1.0f),
			FVector4f(0.0f, 0.0f, 1.0f, 1.0f),
			FVector4f(1.0f, 1.0f, 1.0f, 0.5f)};
		LOD.Indices = {0, 1, 2, 2, 1, 3};
		LOD.Sections = {
			{
				.FirstIndex = 0,
				.IndexCount = 3,
				.MinVertexIndex = 0,
				.MaxVertexIndex = 2,
				.MaterialSlotIndex = 0,
				.LocalBounds = MakeBounds(FVector3(-1.0, -1.0, 0.0), FVector3(0.0, 1.0, 0.0))
			},
			{
				.FirstIndex = 3,
				.IndexCount = 3,
				.MinVertexIndex = 1,
				.MaxVertexIndex = 3,
				.MaterialSlotIndex = 1,
				.LocalBounds = MakeBounds(FVector3(-1.0, -1.0, 0.0), FVector3(1.0, 1.0, 0.0))
			}};
		LOD.NumTexCoords = 4;
		LOD.bHasVertexColors = true;
		return Payload;
	}

	auto MakeNoUVFixture() -> FStaticMeshPayloadData
	{
		FStaticMeshPayloadData Payload = MakeSingleSectionFixture();
		Payload.LODs[0].TexCoords[0].clear();
		Payload.LODs[0].NumTexCoords = 0;
		return Payload;
	}

	auto MakeMultiLODFixture(uint32 LODCount) -> FStaticMeshPayloadData
	{
		check(LODCount == 2 || LODCount == 3);
		FStaticMeshPayloadData Payload = MakeMultiMaterialFixture();
		FStaticMeshPayloadLOD Middle = MakeSingleSectionFixture().LODs.front();
		FStaticMeshPayloadLOD Lowest = Middle;
		for (FVector3f& Position : Lowest.Positions) Position *= 0.5f;
		Lowest.LocalBounds = MakeBounds(
			FVector3(0.0, 0.0, 0.0), FVector3(0.5, 0.5, 0.0));
		Lowest.Sections.front().LocalBounds = Lowest.LocalBounds;

		Payload.LODs.front().ScreenSize = 0.5f;
		if (LODCount == 3)
		{
			Middle.ScreenSize = 0.25f;
			Payload.LODs.push_back(std::move(Middle));
		}
		Lowest.ScreenSize = 0.0f;
		Payload.LODs.push_back(std::move(Lowest));
		return Payload;
	}

	auto EncodePayload(
		const FStaticMeshPayloadData& Payload,
		EStaticMeshTargetPlatform Platform,
		std::vector<uint8>& OutBytes,
		std::string& OutError) -> bool
	{
		std::vector<uint8> Candidate;
		FCanonicalMemoryWriter Ar(Candidate, EArchivePurpose::DerivedDataPayload);
		const_cast<FStaticMeshPayloadData&>(Payload).Serialize(Ar, Platform);
		OutError = Ar.HasError() ? Ar.GetFailure()->Message : std::string{};
		if (Ar.HasError()) return false;
		OutBytes = std::move(Candidate);
		return true;
	}

	auto DecodePayload(
		std::span<const uint8> Bytes,
		EStaticMeshTargetPlatform Platform,
		FStaticMeshPayloadData& OutPayload) -> FPayloadDecodeResult
	{
		FStaticMeshPayloadData Candidate;
		FCanonicalMemoryReader Ar(Bytes, EArchivePurpose::DerivedDataPayload);
		Candidate.Serialize(Ar, Platform);
		if (Ar.HasError())
			return {Ar.GetFailure()->Code == EArchiveFailureCode::UnsupportedVersion
				? EPayloadDecodeError::Incompatible : EPayloadDecodeError::Corrupt,
				Ar.GetFailure()->Message};
		OutPayload = std::move(Candidate);
		return {};
	}

	auto Encode(const FStaticMeshPayloadData& Payload) -> std::vector<uint8>
	{
		std::vector<uint8> Bytes;
		std::string Error;
		EXPECT_TRUE(EncodePayload(Payload, EStaticMeshTargetPlatform::Win64, Bytes, Error)) << Error;
		return Bytes;
	}

	auto ReadU32(const std::vector<uint8>& Bytes, size_t Offset) -> uint32
	{
		uint32 Result = 0;
		for (uint32 Byte = 0; Byte < 4; ++Byte) Result |= static_cast<uint32>(Bytes[Offset + Byte]) << (Byte * 8);
		return Result;
	}

	auto ReadU64(const std::vector<uint8>& Bytes, size_t Offset) -> uint64
	{
		uint64 Result = 0;
		for (uint32 Byte = 0; Byte < 8; ++Byte) Result |= static_cast<uint64>(Bytes[Offset + Byte]) << (Byte * 8);
		return Result;
	}

	auto WriteU32(std::vector<uint8>& Bytes, size_t Offset, uint32 Value) -> void
	{
		for (uint32 Byte = 0; Byte < 4; ++Byte) Bytes[Offset + Byte] = static_cast<uint8>(Value >> (Byte * 8));
	}

	auto WriteU64(std::vector<uint8>& Bytes, size_t Offset, uint64 Value) -> void
	{
		for (uint32 Byte = 0; Byte < 8; ++Byte) Bytes[Offset + Byte] = static_cast<uint8>(Value >> (Byte * 8));
	}

	auto WriteFloat(std::vector<uint8>& Bytes, size_t Offset, float Value) -> void
	{
		WriteU32(Bytes, Offset, std::bit_cast<uint32>(Value));
	}

	auto MakeTestBuffer(
		const FRHIBufferCreateDesc& Desc) -> FBufferRHIRef
	{
		return FBufferRHIRef(new FRHIBuffer(Desc));
	}

	auto Rehash(std::vector<uint8>& Bytes) -> void
	{
		WriteU64(Bytes, 56, FXxHash64::HashBuffer(std::span<const uint8>(Bytes).subspan(64)).HashValue);
	}

	auto ExpectDecodeFailure(
		const std::vector<uint8>& Bytes,
		EPayloadDecodeError ExpectedCode = EPayloadDecodeError::None) -> void
	{
		FStaticMeshPayloadData Sentinel = MakeMultiMaterialFixture();
		const uint32 SentinelSlotCount = Sentinel.MaterialSlotCount;
		const FPayloadDecodeResult Result =
			DecodePayload(Bytes, EStaticMeshTargetPlatform::Win64, Sentinel);
		EXPECT_FALSE(Result);
		if (ExpectedCode != EPayloadDecodeError::None)
			EXPECT_EQ(Result.Code, ExpectedCode);
		EXPECT_FALSE(Result.Message.empty());
		EXPECT_EQ(Sentinel.MaterialSlotCount, SentinelSlotCount);
	}

	auto ExpectVector(const FVector2f& Actual, const FVector2f& Expected) -> void
	{
		EXPECT_EQ(Actual.x, Expected.x);
		EXPECT_EQ(Actual.y, Expected.y);
	}

	auto ExpectVector(const FVector3f& Actual, const FVector3f& Expected) -> void
	{
		EXPECT_EQ(Actual.x, Expected.x);
		EXPECT_EQ(Actual.y, Expected.y);
		EXPECT_EQ(Actual.z, Expected.z);
	}

	auto ExpectVector(const FVector4f& Actual, const FVector4f& Expected) -> void
	{
		EXPECT_EQ(Actual.x, Expected.x);
		EXPECT_EQ(Actual.y, Expected.y);
		EXPECT_EQ(Actual.z, Expected.z);
		EXPECT_EQ(Actual.w, Expected.w);
	}

	auto ExpectEquivalent(const FStaticMeshPayloadData& Actual, const FStaticMeshPayloadData& Expected) -> void
	{
		ASSERT_EQ(Actual.MaterialSlotCount, Expected.MaterialSlotCount);
		ASSERT_EQ(Actual.LODs.size(), Expected.LODs.size());
		EXPECT_EQ(Actual.LocalBounds.Min, Expected.LocalBounds.Min);
		EXPECT_EQ(Actual.LocalBounds.Max, Expected.LocalBounds.Max);
		for (size_t LODIndex = 0; LODIndex < Expected.LODs.size(); ++LODIndex)
		{
			const FStaticMeshPayloadLOD& ActualLOD = Actual.LODs[LODIndex];
			const FStaticMeshPayloadLOD& ExpectedLOD = Expected.LODs[LODIndex];
			EXPECT_EQ(ActualLOD.ScreenSize, ExpectedLOD.ScreenSize);
			ASSERT_EQ(ActualLOD.Positions.size(), ExpectedLOD.Positions.size());
			for (size_t Index = 0; Index < ExpectedLOD.Positions.size(); ++Index)
			{
				ExpectVector(ActualLOD.Positions[Index], ExpectedLOD.Positions[Index]);
				ExpectVector(ActualLOD.Normals[Index], ExpectedLOD.Normals[Index]);
				ExpectVector(ActualLOD.Tangents[Index], ExpectedLOD.Tangents[Index]);
			}
			for (uint32 Channel = 0; Channel < ExpectedLOD.NumTexCoords; ++Channel)
			{
				ASSERT_EQ(ActualLOD.TexCoords[Channel].size(), ExpectedLOD.TexCoords[Channel].size());
				for (size_t Index = 0; Index < ExpectedLOD.TexCoords[Channel].size(); ++Index)
					ExpectVector(ActualLOD.TexCoords[Channel][Index], ExpectedLOD.TexCoords[Channel][Index]);
			}
			ASSERT_EQ(ActualLOD.Colors.size(), ExpectedLOD.Colors.size());
			for (size_t Index = 0; Index < ExpectedLOD.Colors.size(); ++Index)
				ExpectVector(ActualLOD.Colors[Index], ExpectedLOD.Colors[Index]);
			EXPECT_EQ(ActualLOD.Indices, ExpectedLOD.Indices);
			ASSERT_EQ(ActualLOD.Sections.size(), ExpectedLOD.Sections.size());
			for (size_t Index = 0; Index < ExpectedLOD.Sections.size(); ++Index)
			{
				EXPECT_EQ(ActualLOD.Sections[Index].FirstIndex, ExpectedLOD.Sections[Index].FirstIndex);
				EXPECT_EQ(ActualLOD.Sections[Index].IndexCount, ExpectedLOD.Sections[Index].IndexCount);
				EXPECT_EQ(ActualLOD.Sections[Index].MinVertexIndex, ExpectedLOD.Sections[Index].MinVertexIndex);
				EXPECT_EQ(ActualLOD.Sections[Index].MaxVertexIndex, ExpectedLOD.Sections[Index].MaxVertexIndex);
				EXPECT_EQ(ActualLOD.Sections[Index].MaterialSlotIndex, ExpectedLOD.Sections[Index].MaterialSlotIndex);
			}
			EXPECT_EQ(ActualLOD.NumTexCoords, ExpectedLOD.NumTexCoords);
			EXPECT_EQ(ActualLOD.bHasVertexColors, ExpectedLOD.bHasVertexColors);
		}
	}

	auto AddUnknownOptionalChunk(std::vector<uint8> Bytes, bool bRequired) -> std::vector<uint8>
	{
		constexpr size_t NewEntryOffset = 64 + 6 * 32;
		Bytes.insert(Bytes.begin() + static_cast<ptrdiff_t>(NewEntryOffset), 32, 0);
		for (uint32 ChunkIndex = 0; ChunkIndex < 6; ++ChunkIndex)
		{
			const size_t EntryOffset = 64 + ChunkIndex * 32;
			WriteU64(Bytes, EntryOffset + 8, ReadU64(Bytes, EntryOffset + 8) + 32);
		}
		WriteU32(Bytes, NewEntryOffset, 0x7fffffffu);
		WriteU32(Bytes, NewEntryOffset + 4, bRequired ? 1u : 0u);
		const size_t AlignedEnd = (Bytes.size() + StaticMeshPayloadAlignment - 1)
			& ~(static_cast<size_t>(StaticMeshPayloadAlignment) - 1);
		Bytes.resize(AlignedEnd, 0);
		WriteU64(Bytes, NewEntryOffset + 8, AlignedEnd);
		WriteU64(Bytes, NewEntryOffset + 16, 0);
		WriteU64(Bytes, NewEntryOffset + 24, 0);
		WriteU32(Bytes, 24, 7);
		WriteU64(Bytes, 48, Bytes.size());
		Rehash(Bytes);
		return Bytes;
	}
}

TEST(FBoundedPayloadSerializationTests, SaveBuildsAndChecksTheEncodedLimitBeforeWriting)
{
	std::vector<Durin::uint8> Bytes;
	Durin::FCanonicalMemoryWriter Writer(Bytes, Durin::EArchivePurpose::DerivedDataPayload);
	bool bParsed = false;
	bool bCommitted = false;
	Durin::SerializeBoundedArchivePayload<int>(
		Writer,
		{2, "Fixture payload"},
		[](std::vector<Durin::uint8>& Encoded, std::string&) {
			Encoded = {1, 2};
			return true;
		},
		[&](std::span<const Durin::uint8>, int&) {
			bParsed = true;
			return Durin::FPayloadDecodeResult{};
		},
		[&](int&&) { bCommitted = true; });
	EXPECT_FALSE(Writer.HasError()) << Writer.GetError();
	EXPECT_EQ(Bytes, (std::vector<Durin::uint8>{1, 2}));
	EXPECT_FALSE(bParsed);
	EXPECT_FALSE(bCommitted);

	std::vector<Durin::uint8> OversizedBytes;
	Durin::FCanonicalMemoryWriter OversizedWriter(
		OversizedBytes, Durin::EArchivePurpose::DerivedDataPayload);
	Durin::SerializeBoundedArchivePayload<int>(
		OversizedWriter,
		{1, "Fixture payload"},
		[](std::vector<Durin::uint8>& Encoded, std::string&) {
			Encoded = {1, 2};
			return true;
		},
		[](std::span<const Durin::uint8>, int&) {
			return Durin::FPayloadDecodeResult{};
		},
		[](int&&) {});
	ASSERT_TRUE(OversizedWriter.HasError());
	EXPECT_EQ(OversizedWriter.GetFailure()->Code, Durin::EArchiveFailureCode::LimitExceeded);
	EXPECT_TRUE(OversizedBytes.empty());

	std::vector<Durin::uint8> FailedBytes;
	Durin::FCanonicalMemoryWriter FailedWriter(
		FailedBytes, Durin::EArchivePurpose::DerivedDataPayload);
	Durin::SerializeBoundedArchivePayload<int>(
		FailedWriter,
		{2, "Fixture payload"},
		[](std::vector<Durin::uint8>&, std::string& Error) {
			Error = "fixture build failed";
			return false;
		},
		[](std::span<const Durin::uint8>, int&) {
			return Durin::FPayloadDecodeResult{};
		},
		[](int&&) {});
	ASSERT_TRUE(FailedWriter.HasError());
	EXPECT_EQ(FailedWriter.GetFailure()->Code, Durin::EArchiveFailureCode::InvalidData);
	EXPECT_EQ(FailedWriter.GetFailure()->Message, "fixture build failed");
	EXPECT_TRUE(FailedBytes.empty());

	bool bBuiltAfterFailure = false;
	Durin::SerializeBoundedArchivePayload<int>(
		FailedWriter,
		{2, "Fixture payload"},
		[&](std::vector<Durin::uint8>&, std::string&) {
			bBuiltAfterFailure = true;
			return true;
		},
		[](std::span<const Durin::uint8>, int&) {
			return Durin::FPayloadDecodeResult{};
		},
		[](int&&) {});
	EXPECT_FALSE(bBuiltAfterFailure);
}

TEST(FBoundedPayloadSerializationTests, LoadRejectsMissingBoundsAndExcessiveInputBeforeParsing)
{
	Durin::FArchive Unbounded({
		.Direction = Durin::EArchiveDirection::Load,
		.Purpose = Durin::EArchivePurpose::DerivedDataPayload});
	bool bParsed = false;
	bool bCommitted = false;
	Durin::SerializeBoundedArchivePayload<int>(
		Unbounded,
		{4, "Fixture payload"},
		[](std::vector<Durin::uint8>&, std::string&) { return true; },
		[&](std::span<const Durin::uint8>, int&) {
			bParsed = true;
			return Durin::FPayloadDecodeResult{};
		},
		[&](int&&) { bCommitted = true; });
	ASSERT_TRUE(Unbounded.HasError());
	EXPECT_EQ(Unbounded.GetFailure()->Code,
		Durin::EArchiveFailureCode::UnsupportedCapability);
	EXPECT_FALSE(bParsed);
	EXPECT_FALSE(bCommitted);

	const std::array<Durin::uint8, 5> Encoded{};
	Durin::FCanonicalMemoryReader Oversized(Encoded,
		Durin::EArchivePurpose::DerivedDataPayload);
	Durin::SerializeBoundedArchivePayload<int>(
		Oversized,
		{4, "Fixture payload"},
		[](std::vector<Durin::uint8>&, std::string&) { return true; },
		[&](std::span<const Durin::uint8>, int&) {
			bParsed = true;
			return Durin::FPayloadDecodeResult{};
		},
		[&](int&&) { bCommitted = true; });
	ASSERT_TRUE(Oversized.HasError());
	EXPECT_EQ(Oversized.GetFailure()->Code, Durin::EArchiveFailureCode::LimitExceeded);
	EXPECT_FALSE(bParsed);
	EXPECT_FALSE(bCommitted);
}

TEST(FBoundedPayloadSerializationTests, LoadMapsDecodeFailuresAndCommitsOnlySuccess)
{
	const std::array<Durin::uint8, 1> Encoded{42};
	int Published = 7;
	auto Build = [](std::vector<Durin::uint8>&, std::string&) { return true; };

	Durin::FCanonicalMemoryReader IncompatibleReader(
		Encoded, Durin::EArchivePurpose::DerivedDataPayload);
	Durin::SerializeBoundedArchivePayload<int>(
		IncompatibleReader,
		{Encoded.size(), "Fixture payload"},
		Build,
		[](std::span<const Durin::uint8>, int&) {
			return Durin::FPayloadDecodeResult{
				Durin::EPayloadDecodeError::Incompatible, "unsupported fixture"};
		},
		[&](int&& Candidate) { Published = Candidate; });
	ASSERT_TRUE(IncompatibleReader.HasError());
	EXPECT_EQ(IncompatibleReader.GetFailure()->Code,
		Durin::EArchiveFailureCode::UnsupportedVersion);
	EXPECT_EQ(Published, 7);

	Durin::FCanonicalMemoryReader CorruptReader(
		Encoded, Durin::EArchivePurpose::DerivedDataPayload);
	Durin::SerializeBoundedArchivePayload<int>(
		CorruptReader,
		{Encoded.size(), "Fixture payload"},
		Build,
		[](std::span<const Durin::uint8>, int&) {
			return Durin::FPayloadDecodeResult{
				Durin::EPayloadDecodeError::Corrupt, "corrupt fixture"};
		},
		[&](int&& Candidate) { Published = Candidate; });
	ASSERT_TRUE(CorruptReader.HasError());
	EXPECT_EQ(CorruptReader.GetFailure()->Code, Durin::EArchiveFailureCode::InvalidData);
	EXPECT_EQ(Published, 7);

	Durin::FCanonicalMemoryReader SuccessReader(
		Encoded, Durin::EArchivePurpose::DerivedDataPayload);
	Durin::SerializeBoundedArchivePayload<int>(
		SuccessReader,
		{Encoded.size(), "Fixture payload"},
		Build,
		[](std::span<const Durin::uint8> Bytes, int& Candidate) {
			Candidate = Bytes.front();
			return Durin::FPayloadDecodeResult{};
		},
		[&](int&& Candidate) { Published = Candidate; });
	EXPECT_FALSE(SuccessReader.HasError()) << SuccessReader.GetError();
	EXPECT_EQ(Published, 42);
}

TEST(FStaticMeshPayloadCodecTests, CanonicalFixturesRoundTripDeterministically)
{
	const std::array Fixtures{MakeSingleSectionFixture(), MakeMultiMaterialFixture()};
	const std::array<std::string_view, 2> ExpectedPayloadHashes{
		"f8a1b99877a1dd9fd070e498ba1ca9b2",
		"fc478ee22fb777e44d793448c41be804"};
	const std::array<size_t, 2> ExpectedPayloadSizes{556, 824};
	for (size_t FixtureIndex = 0; FixtureIndex < Fixtures.size(); ++FixtureIndex)
	{
		const FStaticMeshPayloadData& Fixture = Fixtures[FixtureIndex];
		const std::vector<uint8> First = Encode(Fixture);
		const std::vector<uint8> Second = Encode(Fixture);
		EXPECT_EQ(First, Second);
		EXPECT_EQ(FXxHash128::HashBuffer(First).ToString(), ExpectedPayloadHashes[FixtureIndex]);
		EXPECT_EQ(First.size(), ExpectedPayloadSizes[FixtureIndex]);
		EXPECT_EQ(ReadU32(First, 0), StaticMeshPayloadMagic);
		EXPECT_EQ(ReadU64(First, 48), First.size());

		FStaticMeshPayloadData Decoded;
		std::string Error;
		const FPayloadDecodeResult DecodeResult =
			DecodePayload(First, EStaticMeshTargetPlatform::Win64, Decoded);
		ASSERT_TRUE(DecodeResult) << DecodeResult.Message;
		ExpectEquivalent(Decoded, Fixture);

		std::unique_ptr<FStaticMeshRenderData> RenderData;
		ASSERT_TRUE(MakeStaticMeshRenderData(Decoded, RenderData, Error)) << Error;
		FStaticMeshPayloadData ConvertedBack;
		ASSERT_TRUE(MakeStaticMeshPayloadData(*RenderData, ConvertedBack, Error)) << Error;
		ExpectEquivalent(ConvertedBack, Fixture);
	}
}

TEST(FStaticMeshPayloadCodecTests,
	MultiLODPoliciesAndDistinctGeometryRoundTripDeterministically)
{
	for (uint32 LODCount : {2u, 3u})
	{
		const FStaticMeshPayloadData Fixture = MakeMultiLODFixture(LODCount);
		const std::vector<uint8> First = Encode(Fixture);
		const std::vector<uint8> Second = Encode(Fixture);
		EXPECT_EQ(First, Second);

		FStaticMeshPayloadData Decoded;
		const FPayloadDecodeResult Result = DecodePayload(
			First, EStaticMeshTargetPlatform::Win64, Decoded);
		ASSERT_TRUE(Result) << Result.Message;
		ExpectEquivalent(Decoded, Fixture);
		ASSERT_EQ(Decoded.LODs.size(), LODCount);
		EXPECT_GT(Decoded.LODs.front().Indices.size(),
			Decoded.LODs.back().Indices.size());
	}
}

TEST(FStaticMeshPayloadCodecTests,
	DefaultLODPolicyIsExactFiniteAndHasLowestDetailFallback)
{
	EXPECT_EQ(GenerateDefaultStaticMeshLODScreenSizes(0),
		(std::vector<float>{}));
	EXPECT_EQ(GenerateDefaultStaticMeshLODScreenSizes(1),
		(std::vector<float>{0.0f}));
	EXPECT_EQ(GenerateDefaultStaticMeshLODScreenSizes(4),
		(std::vector<float>{0.5f, 0.25f, 0.125f, 0.0f}));

	std::vector<FStaticMeshLODResources> LODs(3);
	const std::vector<float> Defaults =
		GenerateDefaultStaticMeshLODScreenSizes(3);
	for (size_t Index = 0; Index < LODs.size(); ++Index)
		LODs[Index].ScreenSize = Defaults[Index];
	std::string Error;
	EXPECT_TRUE(ValidateStaticMeshLODScreenSizes(LODs, Error)) << Error;
	LODs[1].ScreenSize = std::numeric_limits<float>::quiet_NaN();
	EXPECT_FALSE(ValidateStaticMeshLODScreenSizes(LODs, Error));
	EXPECT_FALSE(Error.empty());
	LODs = std::vector<FStaticMeshLODResources>(1);
	LODs.front().ScreenSize = -0.0f;
	EXPECT_FALSE(ValidateStaticMeshLODScreenSizes(LODs, Error));
	LODs.clear();
	EXPECT_FALSE(ValidateStaticMeshLODScreenSizes(LODs, Error));
}

TEST(FStaticMeshPayloadCodecTests, SupportsMeshWithoutUVChannels)
{
	const FStaticMeshPayloadData Fixture = MakeNoUVFixture();
	const std::vector<uint8> Bytes = Encode(Fixture);

	FStaticMeshPayloadData Decoded;
	std::string Error;
	const FPayloadDecodeResult DecodeResult =
		DecodePayload(Bytes, EStaticMeshTargetPlatform::Win64, Decoded);
	ASSERT_TRUE(DecodeResult) << DecodeResult.Message;
	ExpectEquivalent(Decoded, Fixture);

	std::unique_ptr<FStaticMeshRenderData> RenderData;
	ASSERT_TRUE(MakeStaticMeshRenderData(Decoded, RenderData, Error)) << Error;
	ASSERT_EQ(RenderData->LODResources.size(), 1u);
	const FStaticMeshLODResources& LOD = RenderData->LODResources[0];
	EXPECT_EQ(LOD.NumTexCoords, 0u);
	const auto& Positions =
		LOD.VertexBuffers.PositionVertexBuffer.GetPositions();
	const auto& TexCoords =
		LOD.VertexBuffers.StaticMeshVertexBuffer.TexCoordVertexBuffer
			.GetTexCoords();
	for (const auto& Channel : TexCoords)
	{
		ASSERT_EQ(Channel.size(), Positions.size());
		for (const FVector2f& UV : Channel) ExpectVector(UV, FVector2f(0.0f));
	}
	const auto& Colors =
		LOD.VertexBuffers.ColorVertexBuffer.GetColors();
	ASSERT_EQ(Colors.size(), Positions.size());
	for (const FVector4f& Color : Colors)
		ExpectVector(Color, FVector4f(1.0f));

	FStaticMeshPayloadData ConvertedBack;
	ASSERT_TRUE(MakeStaticMeshPayloadData(*RenderData, ConvertedBack, Error)) << Error;
	ExpectEquivalent(ConvertedBack, Fixture);
}

TEST(FStaticMeshPayloadCodecTests,
	CurrentVertexInputAndSectionDrawContractIsPinned)
{
	std::string Error;
	std::unique_ptr<FStaticMeshRenderData> RenderData;
	ASSERT_TRUE(MakeStaticMeshRenderData(
		MakeMultiMaterialFixture(), RenderData, Error)) << Error;
	ASSERT_NE(RenderData, nullptr);
	ASSERT_EQ(RenderData->LODResources.size(), 1u);
	RenderData->LODVertexFactories.resize(1);
	FLocalVertexFactory& VertexFactory =
		RenderData->LODVertexFactories[0].VertexFactory;
	ASSERT_TRUE(VertexFactory.SetData(
		RenderData->LODResources[0].VertexBuffers));
	const FVertexDeclarationElementList Elements =
		VertexFactory.GetDeclarationElements();
	constexpr uint16 PositionStride = sizeof(FVector3f);
	constexpr uint16 TangentStride =
		sizeof(FStaticMeshPackedTangentBasis);
	constexpr uint16 TexCoordStride =
		sizeof(FStaticMeshTexcoordVertex);
	constexpr uint16 ColorStride =
		sizeof(FStaticMeshColorVertex);
	const std::array Expected{
		FVertexElement(
			0, 0, EVertexElementType::Float3, 0, PositionStride),
		FVertexElement(
			1, offsetof(FStaticMeshPackedTangentBasis, Normal),
			EVertexElementType::Short4N, 1, TangentStride),
		FVertexElement(
			1, offsetof(FStaticMeshPackedTangentBasis, Tangent),
			EVertexElementType::Short4N, 2, TangentStride),
		FVertexElement(
			2, offsetof(FStaticMeshTexcoordVertex, TexCoords),
			EVertexElementType::Float2, 3, TexCoordStride),
		FVertexElement(
			2, offsetof(FStaticMeshTexcoordVertex, TexCoords)
				+ sizeof(FVector2f),
			EVertexElementType::Float2, 4, TexCoordStride),
		FVertexElement(
			2, offsetof(FStaticMeshTexcoordVertex, TexCoords)
				+ sizeof(FVector2f) * 2,
			EVertexElementType::Float2, 5, TexCoordStride),
		FVertexElement(
			2, offsetof(FStaticMeshTexcoordVertex, TexCoords)
				+ sizeof(FVector2f) * 3,
			EVertexElementType::Float2, 6, TexCoordStride),
		FVertexElement(
			3, offsetof(FStaticMeshColorVertex, Color),
			EVertexElementType::UByte4N, 7, ColorStride)};
	for (size_t Index = 0; Index < Expected.size(); ++Index)
		EXPECT_EQ(Elements[Index], Expected[Index]);
	for (size_t Index = Expected.size(); Index < Elements.size(); ++Index)
		EXPECT_EQ(Elements[Index].Type, EVertexElementType::None);
	EXPECT_EQ(VertexFactory.GetTypeName(), "FLocalVertexFactory");
	EXPECT_EQ(
		FLocalVertexFactory::GetShaderModuleName(),
		"VertexFactory.LocalVertexFactory");
	EXPECT_EQ(VertexFactory.GetData().NumVertices, 4u);
	EXPECT_EQ(
		VertexFactory.GetData().PositionComponent.VertexBuffer,
		&RenderData->LODResources[0]
			.VertexBuffers.PositionVertexBuffer);
	EXPECT_EQ(
		VertexFactory.GetData()
			.TangentBasisComponents[0].VertexBuffer,
		&RenderData->LODResources[0]
			.VertexBuffers.StaticMeshVertexBuffer
				.TangentsVertexBuffer);
	EXPECT_EQ(
		VertexFactory.GetData().TextureCoordinates[0].VertexBuffer,
		&RenderData->LODResources[0]
			.VertexBuffers.StaticMeshVertexBuffer
				.TexCoordVertexBuffer);
	EXPECT_EQ(
		VertexFactory.GetData().ColorComponent.VertexBuffer,
		&RenderData->LODResources[0]
			.VertexBuffers.ColorVertexBuffer);

	FRawStaticIndexBuffer IndexBuffer;
	EXPECT_EQ(IndexBuffer.GetStride(), 4u);

	const FStaticMeshPayloadData Fixture = MakeMultiMaterialFixture();
	ASSERT_EQ(Fixture.LODs[0].Sections.size(), 2u);
	EXPECT_EQ(Fixture.LODs[0].Sections[0].FirstIndex, 0u);
	EXPECT_EQ(Fixture.LODs[0].Sections[0].IndexCount, 3u);
	EXPECT_EQ(Fixture.LODs[0].Sections[1].FirstIndex, 3u);
	EXPECT_EQ(Fixture.LODs[0].Sections[1].IndexCount, 3u);

	std::string EntryPointSource;
	const std::filesystem::path EntryPointPath =
		std::filesystem::path(FPaths::EngineDir())
		/ "Shaders/Slang/StaticMeshBasePass.slang";
	ASSERT_TRUE(FFileHelper::LoadFileToString(
		EntryPointSource, EntryPointPath.generic_string()));
	EXPECT_NE(
		EntryPointSource.find(
			"import VertexFactory.LocalVertexFactory;"),
		std::string::npos);
	EXPECT_NE(
		EntryPointSource.find("import Lighting.PBRLighting;"),
		std::string::npos);
	EXPECT_EQ(
		EntryPointSource.find("struct FLocalVertexFactoryInput"),
		std::string::npos);
	EXPECT_EQ(
		EntryPointSource.find("float3 position : POSITION;"),
		std::string::npos);
	EXPECT_EQ(
		EntryPointSource.find("float4 packedNormal : NORMAL;"),
		std::string::npos);
	EXPECT_EQ(
		EntryPointSource.find("float4 packedTangent : TANGENT;"),
		std::string::npos);
	EXPECT_EQ(
		EntryPointSource.find("float4 packedColor : COLOR;"),
		std::string::npos);
	EXPECT_EQ(
		EntryPointSource.find("float3 EvaluatePBRDirectLighting("),
		std::string::npos);

	std::string VertexFactorySource;
	const std::filesystem::path VertexFactoryPath =
		EntryPointPath.parent_path()
		/ "VertexFactory/LocalVertexFactory.slang";
	ASSERT_TRUE(FFileHelper::LoadFileToString(
		VertexFactorySource, VertexFactoryPath.generic_string()));
	EXPECT_NE(
		VertexFactorySource.find(
			"module LocalVertexFactory;"),
		std::string::npos);
	size_t Previous = 0;
	for (const std::string_view Input : {
		"public float3 position : POSITION;",
		"public float4 packedNormal : NORMAL;",
		"public float4 packedTangent : TANGENT;",
		"public float2 texCoord0 : TEXCOORD0;",
		"public float2 texCoord1 : TEXCOORD1;",
		"public float2 texCoord2 : TEXCOORD2;",
		"public float2 texCoord3 : TEXCOORD3;",
		"public float4 packedColor : COLOR;"})
	{
		const size_t Position =
			VertexFactorySource.find(Input, Previous);
		ASSERT_NE(Position, std::string::npos) << Input;
		Previous = Position + Input.size();
	}
}

TEST(FStaticMeshPayloadCodecTests,
	ResourceReadinessRejectsEmptyMalformedAndPartialLODs)
{
	FStaticMeshRenderData Empty;
	EXPECT_FALSE(Empty.IsReadyForRendering());

	std::string Error;
	std::unique_ptr<FStaticMeshRenderData> RenderData;
	ASSERT_TRUE(MakeStaticMeshRenderData(
		MakeMultiMaterialFixture(), RenderData, Error)) << Error;
	ASSERT_NE(RenderData, nullptr);
	ASSERT_EQ(RenderData->LODResources.size(), 1u);
	FStaticMeshLODResources& LOD = RenderData->LODResources[0];
	const uint32 NumVertices = LOD.GetNumVertices();

	const FBufferRHIRef PositionBuffer = MakeTestBuffer(
		FRHIBufferCreateDesc::CreateVertex(
			"StaticMeshReadinessPosition",
			static_cast<uint32>(
				NumVertices * sizeof(FVector3f))));
	const FBufferRHIRef TangentsBuffer = MakeTestBuffer(
		FRHIBufferCreateDesc::CreateVertex(
			"StaticMeshReadinessTangents",
			NumVertices * sizeof(FStaticMeshPackedTangentBasis)));
	const FBufferRHIRef TexCoordBuffer = MakeTestBuffer(
		FRHIBufferCreateDesc::CreateVertex(
			"StaticMeshReadinessTexCoords",
			NumVertices * sizeof(FStaticMeshTexcoordVertex)));
	const FBufferRHIRef ColorBuffer = MakeTestBuffer(
		FRHIBufferCreateDesc::CreateVertex(
			"StaticMeshReadinessColors",
			NumVertices * sizeof(FStaticMeshColorVertex)));
	const FBufferRHIRef IndexBuffer = MakeTestBuffer(
		FRHIBufferCreateDesc::CreateIndex(
			"StaticMeshReadinessIndices",
			static_cast<uint32>(
				LOD.GetNumIndices() * sizeof(uint32)),
			sizeof(uint32)));

	EnqueueRenderCommand<FSetPartialStaticMeshReadinessResources>(
		[&LOD, PositionBuffer](
			FRHICommandListImmediate&) {
			LOD.VertexBuffers.PositionVertexBuffer.SetRHI(
				PositionBuffer);
		});
	FlushRenderingCommands();
	EXPECT_FALSE(RenderData->IsReadyForRendering());

	EnqueueRenderCommand<FSetCompleteStaticMeshReadinessResources>(
		[&LOD,
			TangentsBuffer,
			TexCoordBuffer,
			ColorBuffer,
			IndexBuffer](FRHICommandListImmediate&) {
			LOD.VertexBuffers.StaticMeshVertexBuffer
				.TangentsVertexBuffer.SetRHI(TangentsBuffer);
			LOD.VertexBuffers.StaticMeshVertexBuffer
				.TexCoordVertexBuffer.SetRHI(TexCoordBuffer);
			LOD.VertexBuffers.ColorVertexBuffer.SetRHI(
				ColorBuffer);
			LOD.IndexBuffer.SetRHI(IndexBuffer);
		});
	FlushRenderingCommands();
	EXPECT_FALSE(RenderData->IsReadyForRendering());
	RenderData->LODVertexFactories.resize(1);
	EXPECT_TRUE(RenderData->LODVertexFactories[0].VertexFactory.SetData(
		LOD.VertexBuffers));
	EXPECT_FALSE(RenderData->IsReadyForRendering());
	EXPECT_EQ(
		LOD.VertexBuffers.PositionVertexBuffer.GetFriendlyName(),
		"FPositionVertexBuffer");
	EXPECT_EQ(
		LOD.VertexBuffers.StaticMeshVertexBuffer
			.TangentsVertexBuffer.GetStride(),
		16u);
	EXPECT_EQ(
		LOD.VertexBuffers.StaticMeshVertexBuffer
			.TexCoordVertexBuffer.GetStride(),
		32u);
	EXPECT_EQ(
		LOD.VertexBuffers.ColorVertexBuffer.GetStride(),
		4u);
	EXPECT_EQ(LOD.IndexBuffer.GetStride(), 4u);

	auto& Normals =
		LOD.VertexBuffers.StaticMeshVertexBuffer.TangentsVertexBuffer
			.GetMutableNormals();
	const std::vector<FVector3f> SavedNormals =
		std::exchange(Normals, {});
	EXPECT_FALSE(RenderData->IsReadyForRendering());
	Normals = SavedNormals;
	EXPECT_FALSE(RenderData->IsReadyForRendering());

	EnqueueRenderCommand<FReleaseStaticMeshReadinessResources>(
		[RenderDataView = RenderData.get()](
			FRHICommandListImmediate&) {
			RenderDataView->ReleaseResources();
		});
	FlushRenderingCommands();
	EXPECT_FALSE(RenderData->IsReadyForRendering());
}

TEST(FStaticMeshPayloadCodecTests, RejectsEveryTruncationAndChecksumCorruptionTransactionally)
{
	const std::vector<uint8> Valid = Encode(MakeSingleSectionFixture());
	for (size_t Size = 0; Size < Valid.size(); ++Size)
		ExpectDecodeFailure(std::vector<uint8>(Valid.begin(), Valid.begin() + static_cast<ptrdiff_t>(Size)));

	std::vector<uint8> Corrupt = Valid;
	Corrupt.back() ^= 0x80;
	ExpectDecodeFailure(Corrupt);
}

TEST(FStaticMeshPayloadCodecTests, RejectsInvalidEnvelopeAndChunkRanges)
{
	const std::vector<uint8> Valid = Encode(MakeSingleSectionFixture());
	auto Mutate = [&](auto Callback)
	{
		std::vector<uint8> Bytes = Valid;
		Callback(Bytes);
		Rehash(Bytes);
		ExpectDecodeFailure(Bytes);
	};

	Mutate([](auto& Bytes) { WriteU32(Bytes, 0, 0); });
	Mutate([](auto& Bytes) { WriteU32(Bytes, 8, StaticMeshBuilderVersion + 1); });
	Mutate([](auto& Bytes) { WriteU32(Bytes, 12, 0); });
	Mutate([](auto& Bytes) { WriteU32(Bytes, 16, 2); });
	Mutate([](auto& Bytes) { WriteU32(Bytes, 28, 1); });
	Mutate([](auto& Bytes) { WriteU64(Bytes, 64 + 8, std::numeric_limits<uint64>::max() - 8); });
	Mutate([](auto& Bytes) { WriteU64(Bytes, 64 + 32 + 8, ReadU64(Bytes, 64 + 8)); });
	Mutate([](auto& Bytes) { WriteU64(Bytes, 64 + 8, ReadU64(Bytes, 64 + 8) + 1); });

	std::vector<uint8> PreviousSchema = Valid;
	WriteU32(PreviousSchema, 4, StaticMeshPayloadSchemaVersion - 1);
	Rehash(PreviousSchema);
	ExpectDecodeFailure(PreviousSchema, EPayloadDecodeError::Incompatible);
	std::vector<uint8> FutureSchema = Valid;
	WriteU32(FutureSchema, 4, StaticMeshPayloadSchemaVersion + 1);
	Rehash(FutureSchema);
	ExpectDecodeFailure(FutureSchema, EPayloadDecodeError::Incompatible);
}

TEST(FStaticMeshPayloadCodecTests, RejectsLimitsCompressionBombAndInvalidEnumValues)
{
	const std::vector<uint8> Valid = Encode(MakeSingleSectionFixture());
	const uint64 LODChunkOffset = ReadU64(Valid, 64 + 2 * 32 + 8);
	auto Mutate = [&](auto Callback)
	{
		std::vector<uint8> Bytes = Valid;
		Callback(Bytes);
		Rehash(Bytes);
		ExpectDecodeFailure(Bytes);
	};

	Mutate([&](auto& Bytes) { WriteU32(Bytes, static_cast<size_t>(LODChunkOffset + 4), MaximumStaticMeshVerticesPerLOD + 1); });
	Mutate([&](auto& Bytes) { WriteU32(Bytes, static_cast<size_t>(LODChunkOffset + 8), MaximumStaticMeshIndicesPerLOD + 1); });
	Mutate([&](auto& Bytes) { Bytes[static_cast<size_t>(LODChunkOffset + 16)] = MaxStaticMeshUVChannels + 1; });
	Mutate([&](auto& Bytes) { Bytes[static_cast<size_t>(LODChunkOffset + 17)] = 2; });
	Mutate([](auto& Bytes) { WriteU32(Bytes, 64 + 4, 1 | (2 << 8)); });
	Mutate([](auto& Bytes)
	{
		WriteU32(Bytes, 16, 1);
		WriteU32(Bytes, 64 + 4, 1 | (1 << 8));
		WriteU64(Bytes, 64 + 16, 1);
		WriteU64(Bytes, 64 + 24, 65);
	});
}

TEST(FStaticMeshPayloadCodecTests, RejectsInvalidGeometryAndNonFiniteValues)
{
	const std::vector<uint8> Valid = Encode(MakeSingleSectionFixture());
	const uint64 SectionChunkOffset = ReadU64(Valid, 64 + 3 * 32 + 8);
	const uint64 VertexChunkOffset = ReadU64(Valid, 64 + 4 * 32 + 8);
	const uint64 IndexChunkOffset = ReadU64(Valid, 64 + 5 * 32 + 8);
	auto Mutate = [&](auto Callback)
	{
		std::vector<uint8> Bytes = Valid;
		Callback(Bytes);
		Rehash(Bytes);
		ExpectDecodeFailure(Bytes);
	};

	Mutate([&](auto& Bytes) { WriteU32(Bytes, static_cast<size_t>(ReadU64(Bytes, 64 + 2 * 32 + 8) + 4), 0); });
	Mutate([&](auto& Bytes) { WriteU32(Bytes, static_cast<size_t>(IndexChunkOffset), 3); });
	Mutate([&](auto& Bytes) { WriteU32(Bytes, static_cast<size_t>(SectionChunkOffset + 8), 4); });
	Mutate([&](auto& Bytes) { WriteU32(Bytes, static_cast<size_t>(SectionChunkOffset + 20), 1); });
	Mutate([&](auto& Bytes) { WriteU32(Bytes, static_cast<size_t>(VertexChunkOffset), 0x7fc00000u); });
	Mutate([&](auto& Bytes) { WriteU32(Bytes, static_cast<size_t>(ReadU64(Bytes, 64 + 8)), 0x7f800000u); });
}

TEST(FStaticMeshPayloadCodecTests,
	RejectsMalformedLODPoliciesTransactionally)
{
	const std::vector<uint8> Valid = Encode(MakeMultiLODFixture(3));
	const uint64 LODChunkOffset = ReadU64(Valid, 64 + 2 * 32 + 8);
	auto Mutate = [&](auto Callback)
	{
		std::vector<uint8> Bytes = Valid;
		Callback(Bytes);
		Rehash(Bytes);
		ExpectDecodeFailure(Bytes);
	};

	Mutate([&](auto& Bytes) {
		WriteFloat(Bytes, static_cast<size_t>(LODChunkOffset + 20),
			std::numeric_limits<float>::quiet_NaN());
	});
	Mutate([&](auto& Bytes) {
		WriteFloat(Bytes, static_cast<size_t>(LODChunkOffset + 4 + 44 + 16), 0.75f);
	});
	Mutate([&](auto& Bytes) {
		WriteFloat(Bytes, static_cast<size_t>(LODChunkOffset + 4 + 2 * 44 + 16), 0.125f);
	});
	Mutate([&](auto& Bytes) {
		WriteFloat(Bytes, static_cast<size_t>(LODChunkOffset + 4 + 2 * 44 + 16), -0.0f);
	});

	FStaticMeshPayloadData Invalid = MakeMultiLODFixture(2);
	Invalid.LODs[0].ScreenSize = 1.25f;
	std::vector<uint8> Sentinel{1, 2, 3};
	std::string Error;
	EXPECT_FALSE(EncodePayload(
		Invalid, EStaticMeshTargetPlatform::Win64, Sentinel, Error));
	EXPECT_EQ(Sentinel, (std::vector<uint8>{1, 2, 3}));
}

TEST(FStaticMeshPayloadCodecTests, SkipsUnknownOptionalChunksAndRejectsUnknownRequiredChunks)
{
	const std::vector<uint8> Valid = Encode(MakeSingleSectionFixture());
	const std::vector<uint8> Optional = AddUnknownOptionalChunk(Valid, false);
	FStaticMeshPayloadData Decoded;
	const FPayloadDecodeResult DecodeResult =
		DecodePayload(Optional, EStaticMeshTargetPlatform::Win64, Decoded);
	ASSERT_TRUE(DecodeResult) << DecodeResult.Message;
	ExpectEquivalent(Decoded, MakeSingleSectionFixture());

	ExpectDecodeFailure(
		AddUnknownOptionalChunk(Valid, true), EPayloadDecodeError::Incompatible);
}

TEST(FStaticMeshPayloadCodecTests, EncoderRejectsInvalidLogicalDataWithoutPublishingBytes)
{
	FStaticMeshPayloadData Invalid = MakeSingleSectionFixture();
	Invalid.LODs[0].Positions[0].x = std::numeric_limits<float>::quiet_NaN();
	std::vector<uint8> Bytes{1, 2, 3};
	std::string Error;
	EXPECT_FALSE(EncodePayload(Invalid, EStaticMeshTargetPlatform::Win64, Bytes, Error));
	EXPECT_EQ(Bytes, (std::vector<uint8>{1, 2, 3}));
	EXPECT_FALSE(Error.empty());
	EXPECT_FALSE(EncodePayload(
		MakeSingleSectionFixture(), static_cast<EStaticMeshTargetPlatform>(2), Bytes, Error));
}
