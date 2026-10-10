#include "Physics/PhysicsDerivedData.h"
#include "Physics/PhysicsSharedOutput.h"
#include <gtest/gtest.h>

#include "CoreGlobals.h"
#include "DynamicRHI.h"
#include "RHICommandList.h"
#include "Asset/CookedMeshProducts.h"
#include "HAL/PlatformLTS.h"
#include "Hash/XxHash.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "RHI.h"
#include "RenderingThread.h"
#include "Serialization/Archive.h"
#include "StaticMesh/StaticMeshDerivedData.h"
#include "StaticMesh/StaticMeshBuild.h"
#include "StaticMesh/StaticMeshSharedOutput.h"

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
		EAssetPayloadTargetPlatform Platform,
		Durin::FByteBuffer& OutBytes,
		std::string& OutError) -> bool
	{
		Durin::FByteBuffer Candidate;
		FCanonicalMemoryWriter Ar(Candidate,
			EArchivePurpose::DerivedDataPayload,
			{.Target = {Platform == EAssetPayloadTargetPlatform::Win64 ? "Win64"
				: Platform == EAssetPayloadTargetPlatform::MacOS ? "MacOS" : "", "Game"}});
		const_cast<FStaticMeshPayloadData&>(Payload).Serialize(Ar);
		OutError = Ar.IsError() ? Ar.GetFailure()->Message : std::string{};
		if (Ar.IsError()) return false;
		OutBytes = std::move(Candidate);
		return true;
	}

	auto DecodePayload(
		Durin::FByteView Bytes,
		EAssetPayloadTargetPlatform Platform,
		FStaticMeshPayloadData& OutPayload) -> std::expected<void, FArchiveFailure>
	{
		FStaticMeshPayloadData Candidate;
		FCanonicalMemoryReader Ar(Bytes,
			EArchivePurpose::DerivedDataPayload,
			{.Target = {Platform == EAssetPayloadTargetPlatform::Win64 ? "Win64"
				: Platform == EAssetPayloadTargetPlatform::MacOS ? "MacOS" : "", "Game"}});
		Candidate.Serialize(Ar);
		if (Ar.IsError() || !RequireArchiveEnd(Ar))
			return std::unexpected(*Ar.GetFailure());
		OutPayload = std::move(Candidate);
		return {};
	}

	auto Encode(const FStaticMeshPayloadData& Payload) -> Durin::FByteBuffer
	{
		Durin::FByteBuffer Bytes;
		std::string Error;
		EXPECT_TRUE(EncodePayload(Payload, EAssetPayloadTargetPlatform::Win64, Bytes, Error)) << Error;
		return Bytes;
	}

	auto ReadU32(const Durin::FByteBuffer& Bytes, size_t Offset) -> uint32
	{
		uint32 Result = 0;
		for (uint32 Byte = 0; Byte < 4; ++Byte)
			Result |= std::to_integer<uint32>(Bytes[Offset + Byte]) << (Byte * 8);
		return Result;
	}

	auto ReadU64(const Durin::FByteBuffer& Bytes, size_t Offset) -> uint64
	{
		uint64 Result = 0;
		for (uint32 Byte = 0; Byte < 8; ++Byte)
			Result |= std::to_integer<uint64>(Bytes[Offset + Byte]) << (Byte * 8);
		return Result;
	}

	auto WriteU32(Durin::FByteBuffer& Bytes, size_t Offset, uint32 Value) -> void
	{
		for (uint32 Byte = 0; Byte < 4; ++Byte)
			Bytes[Offset + Byte] = static_cast<std::byte>(Value >> (Byte * 8));
	}

	auto WriteU64(Durin::FByteBuffer& Bytes, size_t Offset, uint64 Value) -> void
	{
		for (uint32 Byte = 0; Byte < 8; ++Byte)
			Bytes[Offset + Byte] = static_cast<std::byte>(Value >> (Byte * 8));
	}

	auto WriteFloat(Durin::FByteBuffer& Bytes, size_t Offset, float Value) -> void
	{
		WriteU32(Bytes, Offset, std::bit_cast<uint32>(Value));
	}

	auto MakeTestBuffer(
		const FRHIBufferCreateDesc& Desc) -> FBufferRHIRef
	{
		return FBufferRHIRef(new FRHIBuffer(Desc));
	}

	auto Rehash(Durin::FByteBuffer& Bytes) -> void
	{
		WriteU64(Bytes, 56, FXxHash64::HashBuffer(Durin::FByteView(Bytes).subspan(64)).HashValue);
	}

	auto ExpectDecodeFailure(
		const Durin::FByteBuffer& Bytes,
		std::optional<EArchiveFailureCode> ExpectedCode = std::nullopt) -> void
	{
		FStaticMeshPayloadData Sentinel = MakeMultiMaterialFixture();
		const uint32 SentinelSlotCount = Sentinel.MaterialSlotCount;
		const std::expected<void, FArchiveFailure> Result =
			DecodePayload(Bytes, EAssetPayloadTargetPlatform::Win64, Sentinel);
		ASSERT_FALSE(Result);
		if (ExpectedCode.has_value())
			EXPECT_EQ(Result.error().Code, *ExpectedCode);
		EXPECT_FALSE(Result.error().Message.empty());
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

	auto AddUnknownOptionalChunk(Durin::FByteBuffer Bytes, bool bRequired) -> Durin::FByteBuffer
	{
		constexpr size_t NewEntryOffset = 64 + 6 * 32;
		Bytes.insert(Bytes.begin() + static_cast<ptrdiff_t>(NewEntryOffset),
			32, std::byte{0});
		for (uint32 ChunkIndex = 0; ChunkIndex < 6; ++ChunkIndex)
		{
			const size_t EntryOffset = 64 + ChunkIndex * 32;
			WriteU64(Bytes, EntryOffset + 8, ReadU64(Bytes, EntryOffset + 8) + 32);
		}
		WriteU32(Bytes, NewEntryOffset, 0x7fffffffu);
		WriteU32(Bytes, NewEntryOffset + 4, bRequired ? 1u : 0u);
		const size_t AlignedEnd = (Bytes.size() + StaticMeshPayloadAlignment - 1)
			& ~(static_cast<size_t>(StaticMeshPayloadAlignment) - 1);
		Bytes.resize(AlignedEnd, std::byte{0});
		WriteU64(Bytes, NewEntryOffset + 8, AlignedEnd);
		WriteU64(Bytes, NewEntryOffset + 16, 0);
		WriteU64(Bytes, NewEntryOffset + 24, 0);
		WriteU32(Bytes, 24, 7);
		WriteU64(Bytes, 48, Bytes.size());
		Rehash(Bytes);
		return Bytes;
	}
}

TEST(FStaticMeshCookedProductTests, DetachedCodecMatchesBaselineAndClassifiesTruncation)
{
	const Durin::FStaticMeshPayloadData Payload = MakeMultiMaterialFixture();
	const Durin::FByteBuffer Bytes = Encode(Payload);
	const std::vector<Durin::FMeshMaterialSlotDefinition> Slots{
		{.Name = Durin::FName("Body"), .SourceMaterialIndex = 4},
		{.Name = Durin::FName("Trim"), .SourceMaterialIndex = 9}};
	std::unique_ptr<Durin::FStaticMeshRenderData> Baseline;
	ASSERT_TRUE(Durin::MakeStaticMeshRenderData(Payload, Baseline));

	Durin::FStaticMeshCookedProduct Product;
	Durin::FCookedMeshProductResult Result;
	ASSERT_TRUE(Result = Durin::DecodeStaticMeshCookedProduct(
		Bytes, {}, Slots,
		Durin::EBodySetupCollisionSourceMode::None,
		Durin::EBodySetupCollisionQueryPolicy::SimpleAndComplex,
		Product)) << Durin::FormatCookedMeshProductError(Result.Error);
	ASSERT_NE(Product.RenderData, nullptr);
	ASSERT_EQ(Product.RenderData->LODResources.size(), Baseline->LODResources.size());
	EXPECT_TRUE(std::ranges::equal(Product.RenderData->LODResources[0].IndexBuffer.GetIndices(),
		Baseline->LODResources[0].IndexBuffer.GetIndices()));
	EXPECT_TRUE(std::ranges::equal(Product.RenderData->LODResources[0].VertexBuffers.PositionVertexBuffer.GetPositions(),
		Baseline->LODResources[0].VertexBuffers.PositionVertexBuffer.GetPositions()));
	ASSERT_EQ(Product.RenderData->MaterialSlots.size(), Slots.size());
	EXPECT_EQ(Product.RenderData->MaterialSlots[0].Name, "Body");
	EXPECT_EQ(Product.RenderData->MaterialSlots[0].SourceMaterialIndex, 4u);
	EXPECT_EQ(Product.RenderData->MaterialSlots[1].Name, "Trim");
	EXPECT_EQ(Product.RenderData->MaterialSlots[1].SourceMaterialIndex, 9u);

	Durin::FStaticMeshCookedProduct Rejected;
	Durin::FByteBuffer Truncated(Bytes.begin(), Bytes.end() - 1);
	EXPECT_FALSE(Result = Durin::DecodeStaticMeshCookedProduct(
		Truncated, {}, Slots,
		Durin::EBodySetupCollisionSourceMode::None,
		Durin::EBodySetupCollisionQueryPolicy::SimpleAndComplex,
		Rejected));
	EXPECT_EQ(Result.Error.Code, Durin::ECookedMeshProductError::RenderArchive);

	Durin::FByteBuffer Incompatible = Bytes;
	WriteU32(Incompatible, 4, StaticMeshPayloadSchemaVersion + 1);
	Rehash(Incompatible);
	EXPECT_FALSE(Result = Durin::DecodeStaticMeshCookedProduct(
		Incompatible, {}, Slots,
		Durin::EBodySetupCollisionSourceMode::None,
		Durin::EBodySetupCollisionQueryPolicy::SimpleAndComplex,
		Rejected));
	EXPECT_EQ(Result.Error.Code, Durin::ECookedMeshProductError::RenderArchive);

	Durin::FByteBuffer Oversized = Bytes;
	const uint64 LODChunkOffset = ReadU64(Oversized, 64 + 2 * 32 + 8);
	WriteU32(Oversized, static_cast<size_t>(LODChunkOffset + 4),
		MaximumStaticMeshVerticesPerLOD + 1);
	Rehash(Oversized);
	EXPECT_FALSE(Result = Durin::DecodeStaticMeshCookedProduct(
		Oversized, {}, Slots,
		Durin::EBodySetupCollisionSourceMode::None,
		Durin::EBodySetupCollisionQueryPolicy::SimpleAndComplex,
		Rejected));
	EXPECT_EQ(Result.Error.Code, Durin::ECookedMeshProductError::RenderArchive);

	Durin::FByteBuffer Compressed = Bytes;
	WriteU32(Compressed, 16, 1);
	WriteU32(Compressed, 64 + 4, 1 | (1 << 8));
	WriteU64(Compressed, 64 + 16, 1);
	WriteU64(Compressed, 64 + 24, 65);
	Rehash(Compressed);
	EXPECT_FALSE(Result = Durin::DecodeStaticMeshCookedProduct(
		Compressed, {}, Slots,
		Durin::EBodySetupCollisionSourceMode::None,
		Durin::EBodySetupCollisionQueryPolicy::SimpleAndComplex,
		Rejected));
	EXPECT_EQ(Result.Error.Code, Durin::ECookedMeshProductError::RenderArchive);

}

TEST(FStaticMeshPayloadCodecTests, CanonicalFixturesRoundTripDeterministically)
{
	const std::array Fixtures{MakeSingleSectionFixture(), MakeMultiMaterialFixture()};
	const std::array<std::string_view, 2> ExpectedPayloadHashes{
		"2712890c9c2bb6db92b943e4848fc3a9",
		"afbf34c6361f5a3d7a2f2c6b9bf785c8"};
	const std::array<size_t, 2> ExpectedPayloadSizes{556, 824};
	for (size_t FixtureIndex = 0; FixtureIndex < Fixtures.size(); ++FixtureIndex)
	{
		const FStaticMeshPayloadData& Fixture = Fixtures[FixtureIndex];
		const Durin::FByteBuffer First = Encode(Fixture);
		const Durin::FByteBuffer Second = Encode(Fixture);
		EXPECT_EQ(First, Second);
		EXPECT_EQ(FXxHash128::HashBuffer(First).ToString(), ExpectedPayloadHashes[FixtureIndex]);
		EXPECT_EQ(First.size(), ExpectedPayloadSizes[FixtureIndex]);
		EXPECT_EQ(ReadU32(First, 0), 0u);
		EXPECT_EQ(ReadU64(First, 48), First.size());

		FStaticMeshPayloadData Decoded;
		const std::expected<void, FArchiveFailure> DecodeResult =
			DecodePayload(First, EAssetPayloadTargetPlatform::Win64, Decoded);
		ASSERT_TRUE(DecodeResult) << DecodeResult.error().Message;
		ExpectEquivalent(Decoded, Fixture);

		std::unique_ptr<FStaticMeshRenderData> RenderData;
		ASSERT_TRUE(MakeStaticMeshRenderData(Decoded, RenderData));
		FStaticMeshPayloadData ConvertedBack;
		ASSERT_TRUE(MakeStaticMeshPayloadData(*RenderData, ConvertedBack));
		ExpectEquivalent(ConvertedBack, Fixture);
	}
}

TEST(FStaticMeshPayloadCodecTests, MacOSPayloadRoundTripsAndRejectsWin64Reader)
{
	const FStaticMeshPayloadData Fixture = MakeSingleSectionFixture();
	Durin::FByteBuffer Bytes;
	std::string Error;
	ASSERT_TRUE(EncodePayload(Fixture, EAssetPayloadTargetPlatform::MacOS,
		Bytes, Error)) << Error;
	FStaticMeshPayloadData Decoded;
	const auto MacOS = DecodePayload(Bytes, EAssetPayloadTargetPlatform::MacOS,
		Decoded);
	ASSERT_TRUE(MacOS) << MacOS.error().Message;
	ExpectEquivalent(Decoded, Fixture);
	const auto WrongPlatform = DecodePayload(Bytes,
		EAssetPayloadTargetPlatform::Win64, Decoded);
	EXPECT_FALSE(WrongPlatform);
}

TEST(FStaticMeshPayloadCodecTests,
	MultiLODPoliciesAndDistinctGeometryRoundTripDeterministically)
{
	for (uint32 LODCount : {2u, 3u})
	{
		const FStaticMeshPayloadData Fixture = MakeMultiLODFixture(LODCount);
		const Durin::FByteBuffer First = Encode(Fixture);
		const Durin::FByteBuffer Second = Encode(Fixture);
		EXPECT_EQ(First, Second);

		FStaticMeshPayloadData Decoded;
		const std::expected<void, FArchiveFailure> Result = DecodePayload(
			First, EAssetPayloadTargetPlatform::Win64, Decoded);
		ASSERT_TRUE(Result) << Result.error().Message;
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
	EXPECT_TRUE(ValidateStaticMeshLODScreenSizes(LODs));
	LODs[1].ScreenSize = std::numeric_limits<float>::quiet_NaN();
	const auto NonFinite = ValidateStaticMeshLODScreenSizes(LODs);
	EXPECT_FALSE(NonFinite);
	EXPECT_EQ(NonFinite.error().Code, EStaticMeshLODPolicyError::InvalidScreenSize);
	EXPECT_EQ(NonFinite.error().LODIndex, 1u);
	EXPECT_EQ(NonFinite.error().LODCount, 3u);
	LODs = std::vector<FStaticMeshLODResources>(1);
	EXPECT_TRUE(std::isnan(NonFinite.error().ScreenSize));
	LODs.front().ScreenSize = -0.0f;
	const auto SignedZero = ValidateStaticMeshLODScreenSizes(LODs);
	EXPECT_FALSE(SignedZero);
	EXPECT_EQ(SignedZero.error().Code, EStaticMeshLODPolicyError::InvalidScreenSize);
	LODs.clear();
	EXPECT_TRUE(std::signbit(SignedZero.error().ScreenSize));
	EXPECT_EQ(ValidateStaticMeshLODScreenSizes(LODs).error().Code, EStaticMeshLODPolicyError::Empty);
	LODs.resize(2);
	LODs[0].ScreenSize = 0.5f;
	LODs[1].ScreenSize = 0.5f;
	const auto Order = ValidateStaticMeshLODScreenSizes(LODs);
	EXPECT_FALSE(Order);
	LODs[1].ScreenSize = 0.25f;
	const auto Final = ValidateStaticMeshLODScreenSizes(LODs);
	EXPECT_FALSE(Final);
	LODs.clear();
	EXPECT_EQ(Order.error().Code, EStaticMeshLODPolicyError::NotDescending);
	EXPECT_EQ(Order.error().LODIndex, 1u);
	EXPECT_EQ(Order.error().ScreenSize, 0.5f);
	EXPECT_EQ(Order.error().PreviousScreenSize, 0.5f);
	EXPECT_EQ(Final.error().Code, EStaticMeshLODPolicyError::MissingFinalZero);
	EXPECT_EQ(Final.error().LODIndex, 1u);
	EXPECT_EQ(Final.error().ScreenSize, 0.25f);
}

TEST(FStaticMeshPayloadCodecTests, SupportsMeshWithoutUVChannels)
{
	const FStaticMeshPayloadData Fixture = MakeNoUVFixture();
	const Durin::FByteBuffer Bytes = Encode(Fixture);

	FStaticMeshPayloadData Decoded;
	const std::expected<void, FArchiveFailure> DecodeResult =
		DecodePayload(Bytes, EAssetPayloadTargetPlatform::Win64, Decoded);
	ASSERT_TRUE(DecodeResult) << DecodeResult.error().Message;
	ExpectEquivalent(Decoded, Fixture);

	std::unique_ptr<FStaticMeshRenderData> RenderData;
	ASSERT_TRUE(MakeStaticMeshRenderData(Decoded, RenderData));
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
	ASSERT_TRUE(MakeStaticMeshPayloadData(*RenderData, ConvertedBack));
	ExpectEquivalent(ConvertedBack, Fixture);
}

TEST(FStaticMeshPayloadCodecTests,
	CurrentVertexInputAndSectionDrawContractIsPinned)
{
	std::unique_ptr<FStaticMeshRenderData> RenderData;
	ASSERT_TRUE(MakeStaticMeshRenderData(
		MakeMultiMaterialFixture(), RenderData));
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
	auto EntryPointSourceRead = FFileHelper::LoadFileToString(EntryPointPath.generic_string());
	ASSERT_TRUE(EntryPointSourceRead) << EntryPointSourceRead.error().ToString();
	EntryPointSource = std::move(*EntryPointSourceRead);
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
	auto VertexFactorySourceRead = FFileHelper::LoadFileToString(VertexFactoryPath.generic_string());
	ASSERT_TRUE(VertexFactorySourceRead) << VertexFactorySourceRead.error().ToString();
	VertexFactorySource = std::move(*VertexFactorySourceRead);
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

	std::unique_ptr<FStaticMeshRenderData> RenderData;
	ASSERT_TRUE(MakeStaticMeshRenderData(
		MakeMultiMaterialFixture(), RenderData));
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
	const Durin::FByteBuffer Valid = Encode(MakeSingleSectionFixture());
	for (size_t Size = 0; Size < Valid.size(); ++Size)
		ExpectDecodeFailure(Durin::FByteBuffer(Valid.begin(), Valid.begin() + static_cast<ptrdiff_t>(Size)));

	Durin::FByteBuffer Corrupt = Valid;
	Corrupt.back() ^= std::byte{0x80};
	ExpectDecodeFailure(Corrupt);
}

TEST(FStaticMeshPayloadCodecTests, RejectsInvalidEnvelopeAndChunkRanges)
{
	const Durin::FByteBuffer Valid = Encode(MakeSingleSectionFixture());
	auto Mutate = [&](auto Callback)
	{
		Durin::FByteBuffer Bytes = Valid;
		Callback(Bytes);
		Rehash(Bytes);
		ExpectDecodeFailure(Bytes);
	};

	Mutate([](auto& Bytes) { WriteU32(Bytes, 0, 1); });
	Mutate([](auto& Bytes) { WriteU32(Bytes, 12, 0); });
	Mutate([](auto& Bytes) { WriteU32(Bytes, 16, 2); });
	Mutate([](auto& Bytes) { WriteU32(Bytes, 28, 1); });
	Mutate([](auto& Bytes) { WriteU64(Bytes, 64 + 8, std::numeric_limits<uint64>::max() - 8); });
	Mutate([](auto& Bytes) { WriteU64(Bytes, 64 + 32 + 8, ReadU64(Bytes, 64 + 8)); });
	Mutate([](auto& Bytes) { WriteU64(Bytes, 64 + 8, ReadU64(Bytes, 64 + 8) + 1); });

	Durin::FByteBuffer PreviousSchema = Valid;
	WriteU32(PreviousSchema, 4, StaticMeshPayloadSchemaVersion - 1);
	Rehash(PreviousSchema);
	ExpectDecodeFailure(PreviousSchema, EArchiveFailureCode::UnsupportedVersion);
	Durin::FByteBuffer FutureSchema = Valid;
	WriteU32(FutureSchema, 4, StaticMeshPayloadSchemaVersion + 1);
	Rehash(FutureSchema);
	ExpectDecodeFailure(FutureSchema, EArchiveFailureCode::UnsupportedVersion);
}

TEST(FStaticMeshPayloadCodecTests, RejectsLimitsCompressionBombAndInvalidEnumValues)
{
	const Durin::FByteBuffer Valid = Encode(MakeSingleSectionFixture());
	const uint64 LODChunkOffset = ReadU64(Valid, 64 + 2 * 32 + 8);
	auto Mutate = [&](auto Callback)
	{
		Durin::FByteBuffer Bytes = Valid;
		Callback(Bytes);
		Rehash(Bytes);
		ExpectDecodeFailure(Bytes);
	};

	Mutate([&](auto& Bytes) { WriteU32(Bytes, static_cast<size_t>(LODChunkOffset + 4), MaximumStaticMeshVerticesPerLOD + 1); });
	Mutate([&](auto& Bytes) { WriteU32(Bytes, static_cast<size_t>(LODChunkOffset + 8), MaximumStaticMeshIndicesPerLOD + 1); });
	Mutate([&](auto& Bytes) { Bytes[static_cast<size_t>(LODChunkOffset + 16)] =
		static_cast<std::byte>(MaxStaticMeshUVChannels + 1); });
	Mutate([&](auto& Bytes) { Bytes[static_cast<size_t>(LODChunkOffset + 17)] =
		std::byte{2}; });
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
	const Durin::FByteBuffer Valid = Encode(MakeSingleSectionFixture());
	const uint64 SectionChunkOffset = ReadU64(Valid, 64 + 3 * 32 + 8);
	const uint64 VertexChunkOffset = ReadU64(Valid, 64 + 4 * 32 + 8);
	const uint64 IndexChunkOffset = ReadU64(Valid, 64 + 5 * 32 + 8);
	auto Mutate = [&](auto Callback)
	{
		Durin::FByteBuffer Bytes = Valid;
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
	const Durin::FByteBuffer Valid = Encode(MakeMultiLODFixture(3));
	const uint64 LODChunkOffset = ReadU64(Valid, 64 + 2 * 32 + 8);
	auto Mutate = [&](auto Callback)
	{
		Durin::FByteBuffer Bytes = Valid;
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
	Durin::FByteBuffer Sentinel{std::byte{1}, std::byte{2}, std::byte{3}};
	std::string Error;
	EXPECT_FALSE(EncodePayload(
		Invalid, EAssetPayloadTargetPlatform::Win64, Sentinel, Error));
	EXPECT_EQ(Sentinel, (Durin::FByteBuffer{
		std::byte{1}, std::byte{2}, std::byte{3}}));
}

TEST(FStaticMeshPayloadCodecTests, SkipsUnknownOptionalChunksAndRejectsUnknownRequiredChunks)
{
	const Durin::FByteBuffer Valid = Encode(MakeSingleSectionFixture());
	const Durin::FByteBuffer Optional = AddUnknownOptionalChunk(Valid, false);
	FStaticMeshPayloadData Decoded;
	const std::expected<void, FArchiveFailure> DecodeResult =
		DecodePayload(Optional, EAssetPayloadTargetPlatform::Win64, Decoded);
	ASSERT_TRUE(DecodeResult) << DecodeResult.error().Message;
	ExpectEquivalent(Decoded, MakeSingleSectionFixture());

	ExpectDecodeFailure(
		AddUnknownOptionalChunk(Valid, true), EArchiveFailureCode::UnsupportedVersion);
}

TEST(FStaticMeshPayloadCodecTests, EncoderRejectsInvalidLogicalDataWithoutPublishingBytes)
{
	FStaticMeshPayloadData Invalid = MakeSingleSectionFixture();
	Invalid.LODs[0].Positions[0].x = std::numeric_limits<float>::quiet_NaN();
	Durin::FByteBuffer Bytes{std::byte{1}, std::byte{2}, std::byte{3}};
	std::string Error;
	EXPECT_FALSE(EncodePayload(Invalid, EAssetPayloadTargetPlatform::Win64, Bytes, Error));
	EXPECT_EQ(Bytes, (Durin::FByteBuffer{
		std::byte{1}, std::byte{2}, std::byte{3}}));
	EXPECT_FALSE(Error.empty());
	EXPECT_FALSE(EncodePayload(
		MakeSingleSectionFixture(), static_cast<EAssetPayloadTargetPlatform>(3), Bytes, Error));
}

TEST(FStaticMeshPayloadCodecTests, ArchiveReplacesOptionalStreamsAndPreservesSaveSources)
{
	FStaticMeshPayloadData Source = MakeSingleSectionFixture();
	const auto Before = Source;
	const FByteBuffer Bytes = Encode(Source);
	FCountingArchive Counter(EArchivePurpose::DerivedDataPayload, {.Target = {"Win64", "Game"}});
	Source.Serialize(Counter);
	ASSERT_FALSE(Counter.IsError()) << Counter.GetError();
	EXPECT_EQ(Counter.Tell(), Bytes.size());
	FHashingArchive Hasher(EArchivePurpose::DerivedDataPayload, {.Target = {"Win64", "Game"}});
	Source.Serialize(Hasher);
	EXPECT_FALSE(Hasher.IsError());
	EXPECT_EQ(Hasher.Finalize(), FXxHash128::HashBuffer(Bytes));
	ExpectEquivalent(Source, Before);

	FStaticMeshPayloadData Loaded = MakeMultiMaterialFixture();
	FCanonicalMemoryReader Reader(Bytes, EArchivePurpose::DerivedDataPayload, {.Target = {"Win64", "Game"}});
	Loaded.Serialize(Reader);
	ASSERT_FALSE(Reader.IsError()) << Reader.GetError();
	ASSERT_TRUE(RequireArchiveEnd(Reader));
	ExpectEquivalent(Loaded, Source);
	for (uint32 Channel = 1; Channel < MaxStaticMeshUVChannels; ++Channel)
		EXPECT_TRUE(Loaded.LODs.front().TexCoords[Channel].empty());
	EXPECT_TRUE(Loaded.LODs.front().Colors.empty());

	FByteBuffer Adjacent = Bytes;
	Adjacent.insert(Adjacent.end(), Bytes.begin(), Bytes.end());
	FCanonicalMemoryReader Parent(Adjacent, EArchivePurpose::DerivedDataPayload, {.Target = {"Win64", "Game"}});
	Loaded.Serialize(Parent);
	ASSERT_FALSE(Parent.IsError());
	EXPECT_EQ(Parent.GetRemainingPayloadBytes(), Bytes.size());
	EXPECT_FALSE(RequireArchiveEnd(Parent));
	EXPECT_EQ(Parent.GetFailure()->Code, EArchiveFailureCode::TrailingData);

	FStaticMeshPayloadData Replacement;
	FCanonicalMemoryReader Cancelled(Bytes, EArchivePurpose::DerivedDataPayload, {.Target = {"Win64", "Game"}});
	uint32 Checks = 0;
	Replacement.Serialize(Cancelled,
		[&] { return ++Checks >= 2; });
	EXPECT_TRUE(Cancelled.IsError());
	EXPECT_NE(Cancelled.GetError().find("cancelled"), std::string_view::npos);

	FByteBuffer Oversized = Bytes;
	WriteU64(Oversized, 48, MaximumStaticMeshPayloadBytes + 1);
	FCanonicalMemoryReader Limits(Oversized, EArchivePurpose::DerivedDataPayload, {.Target = {"Win64", "Game"}});
	FStaticMeshPayloadData Discarded;
	Discarded.Serialize(Limits);
	ASSERT_TRUE(Limits.IsError());
	EXPECT_EQ(Limits.GetFailure()->Code, EArchiveFailureCode::LimitExceeded);
	EXPECT_EQ(Limits.Tell(), 64u);
	EXPECT_TRUE(Discarded.LODs.empty());
}

TEST(FStaticMeshPayloadCodecTests, ArchiveTargetIsRequiredBeforePayloadTransfer)
{
	using namespace Durin;
	auto Check = [](auto Payload) {
		for (const char* Platform : {"", "Other"})
		{
			FArchiveState Context{.Target = {Platform, "Game"}};
			FByteBuffer Bytes;
			FCanonicalMemoryWriter Writer(Bytes, EArchivePurpose::DerivedDataPayload, Context);
			FCountingArchive Counter(EArchivePurpose::DerivedDataPayload, Context);
			FHashingArchive Hasher(EArchivePurpose::DerivedDataPayload, Context);
			FCanonicalMemoryReader Reader(Bytes, EArchivePurpose::DerivedDataPayload, Context);
			for (FArchive* Ar : {static_cast<FArchive*>(&Writer), static_cast<FArchive*>(&Counter),
				static_cast<FArchive*>(&Hasher), static_cast<FArchive*>(&Reader)})
			{
				Payload.Serialize(*Ar);
				ASSERT_TRUE(Ar->IsError());
				EXPECT_EQ(Ar->GetFailure()->Code, EArchiveFailureCode::UnsupportedTarget);
				EXPECT_EQ(Ar->Tell(), 0u);
			}
			EXPECT_TRUE(Bytes.empty());
		}
	};
	Check(FStaticMeshPayloadData{});
	Check(FPhysicsCollisionPayloadData{});
}

TEST(FPhysicsCollisionPayloadTests, RejectionOwnsOrdinalContextAndPreservesGeometry)
{
	using namespace Durin;
	const std::array Vertices{FVector3(0, 0, 0), FVector3(1, 0, 0), FVector3(1, 1, 0), FVector3(0, 1, 0)};
	const std::array<uint32, 6> Indices{0, 1, 2, 0, 2, 3};
	const std::array<uint32, 2> Ordinals{3, 8};
	const auto Geometry = FCollisionGeometryRef::MakeTriangleMesh(Vertices, Indices, Ordinals);
	ASSERT_TRUE(Geometry);
	FPhysicsCollisionPayloadData Payload;
	ASSERT_TRUE(MakePhysicsCollisionPayloadData(Geometry,
		EBodySetupCollisionQueryPolicy::SimpleAndComplex, Payload));
	FCollisionGeometryRef Output = Geometry;
	uint32 Checks = 0;
	const auto Cancelled = MakePhysicsCollisionGeometry(Payload, Output, [&] { return ++Checks == 2; });
	EXPECT_EQ(Cancelled.error().Code, EPhysicsCollisionPayloadError::Cancelled);
	EXPECT_EQ(Output.GetIdentity(), Geometry.GetIdentity());
	Payload.SourceOrdinals[1] = 3;
	const auto Duplicate = MakePhysicsCollisionGeometry(Payload, Output);
	EXPECT_EQ(Duplicate.error().Code, EPhysicsCollisionPayloadError::DuplicateOrdinal);
	EXPECT_EQ(Duplicate.error().Index, 1u);
	EXPECT_EQ(Duplicate.error().Ordinal, 3u);
	EXPECT_EQ(Duplicate.error().VertexCount, 4u);
	EXPECT_EQ(Duplicate.error().IndexCount, 6u);
	Payload.SourceOrdinals[1] = 8;
	Payload.LeafTriangles = {99};
	const auto Unknown = MakePhysicsCollisionGeometry(Payload, Output);
	EXPECT_EQ(Unknown.error().Code, EPhysicsCollisionPayloadError::UnknownOrdinal);
	EXPECT_EQ(Unknown.error().Index, 0u);
	EXPECT_EQ(Unknown.error().Ordinal, 99u);
	Payload = {};
	EXPECT_EQ(Unknown.error().SourceMode, EBodySetupCollisionSourceMode::TriangleMeshFromLOD0);
	EXPECT_EQ(Unknown.error().OrdinalCount, 2u);
	EXPECT_EQ(Output.GetIdentity(), Geometry.GetIdentity());
}

TEST(FPhysicsCollisionPayloadTests, InvalidExtractionAndCancellationPreserveOutput)
{
	using namespace Durin;
	FPhysicsCollisionPayloadData Output;
	Output.Positions.push_back(FVector3f(7, 8, 9));
	const auto Invalid = MakePhysicsCollisionPayloadData({},
		EBodySetupCollisionQueryPolicy::SimpleAndComplex, Output);
	EXPECT_EQ(Invalid.error().Code, EPhysicsCollisionPayloadError::InvalidGeometry);
	EXPECT_EQ(Invalid.error().Operation, EPhysicsCollisionPayloadOperation::Extract);
	EXPECT_EQ(Output.Positions, (std::vector<FVector3f>{FVector3f(7, 8, 9)}));
	const auto Cancelled = MakePhysicsCollisionPayloadData({},
		EBodySetupCollisionQueryPolicy::SimpleAndComplex, Output, [] { return true; });
	EXPECT_EQ(Cancelled.error().Code, EPhysicsCollisionPayloadError::Cancelled);
	EXPECT_EQ(Output.Positions.size(), 1u);
	FCollisionGeometryRef Geometry;
	const auto Construct = MakePhysicsCollisionGeometry(Output, Geometry, [] { return true; });
	EXPECT_EQ(Construct.error().Code, EPhysicsCollisionPayloadError::Cancelled);
	EXPECT_EQ(Construct.error().Operation, EPhysicsCollisionPayloadOperation::Construct);
}

TEST(FStaticMeshPayloadConversionTests, RejectionOwnsStreamAndSectionContext)
{
	FStaticMeshPayloadData Payload = MakeSingleSectionFixture();
	std::unique_ptr<FStaticMeshRenderData> Output;
	ASSERT_TRUE(MakeStaticMeshRenderData(Payload, Output));
	const auto* Original = Output.get();
	Payload.LODs[0].TexCoords[0].pop_back();
	const auto Stream = MakeStaticMeshRenderData(Payload, Output);
	EXPECT_FALSE(Stream);
	EXPECT_EQ(Stream.error().Code, EStaticMeshPayloadError::UVStreamCount);
	EXPECT_EQ(Stream.error().LODIndex, 0u);
	EXPECT_EQ(Stream.error().Channel, 0u);
	EXPECT_EQ(Stream.error().Actual, 2u);
	EXPECT_EQ(Stream.error().Expected, 3u);
	EXPECT_EQ(Output.get(), Original);

	Payload = MakeSingleSectionFixture();
	Payload.LODs[0].Sections[0].MinVertexIndex = 1;
	const auto Section = MakeStaticMeshRenderData(Payload, Output);
	EXPECT_FALSE(Section);
	Payload = {};
	EXPECT_EQ(Section.error().Code, EStaticMeshPayloadError::SectionVertexMismatch);
	EXPECT_EQ(Section.error().LODIndex, 0u);
	EXPECT_EQ(Section.error().SectionIndex, 0u);
	ASSERT_TRUE(Section.error().Section);
	EXPECT_EQ(Section.error().Section->MinVertexIndex, 1u);
	EXPECT_EQ(Section.error().Actual, 0u);
	EXPECT_EQ(Section.error().Expected, 1u);
	EXPECT_EQ(Section.error().AdditionalActual, 2u);
	EXPECT_EQ(Section.error().AdditionalExpected, 2u);
	EXPECT_EQ(Output.get(), Original);
	EXPECT_FALSE(FormatStaticMeshPayloadError(Section.error()).empty());
}

TEST(FStaticMeshPayloadConversionTests, RejectionOwnsAttributeAndIndexValues)
{
	FStaticMeshPayloadData Payload = MakeSingleSectionFixture();
	std::unique_ptr<FStaticMeshRenderData> Output;
	Payload.LODs[0].Tangents[1].w = std::numeric_limits<float>::infinity();
	const auto Attribute = MakeStaticMeshRenderData(Payload, Output);
	EXPECT_FALSE(Attribute);
	Payload = MakeSingleSectionFixture();
	EXPECT_EQ(Attribute.error().Code, EStaticMeshPayloadError::NonFiniteAttribute);
	EXPECT_EQ(Attribute.error().Stream, EStaticMeshPayloadStream::Tangent);
	EXPECT_EQ(Attribute.error().ElementIndex, 1u);
	EXPECT_TRUE(std::isinf(Attribute.error().Value.w));
	Payload.LODs[0].Indices[2] = 99;
	const auto Index = MakeStaticMeshRenderData(Payload, Output);
	EXPECT_FALSE(Index);
	Payload = {};
	EXPECT_EQ(Index.error().Code, EStaticMeshPayloadError::IndexRange);
	EXPECT_EQ(Index.error().ElementIndex, 2u);
	EXPECT_EQ(Index.error().Actual, 99u);
	EXPECT_EQ(Index.error().Expected, 3u);
	EXPECT_EQ(Output, nullptr);
}

TEST(FStaticMeshPayloadConversionTests, InvalidExtractionAndCancellationPreserveOutputs)
{
	const FStaticMeshPayloadData Payload = MakeSingleSectionFixture();
	std::unique_ptr<FStaticMeshRenderData> RenderData;
	ASSERT_TRUE(MakeStaticMeshRenderData(Payload, RenderData));
	const auto* Original = RenderData.get();
	uint32 Checks = 0;
	const auto Construction = MakeStaticMeshRenderData(Payload, RenderData, [&] { return ++Checks == 3; });
	EXPECT_FALSE(Construction);
	EXPECT_EQ(Construction.error().Code, EStaticMeshPayloadError::Cancelled);
	EXPECT_EQ(RenderData.get(), Original);
	FStaticMeshPayloadData Output;
	Output.MaterialSlotCount = 123;
	Checks = 0;
	const auto Extraction = MakeStaticMeshPayloadData(*RenderData, Output, [&] { return ++Checks == 3; });
	EXPECT_FALSE(Extraction);
	EXPECT_EQ(Extraction.error().Code, EStaticMeshPayloadError::Cancelled);
	EXPECT_EQ(Output.MaterialSlotCount, 123u);
	EXPECT_TRUE(Output.LODs.empty());
	RenderData->LODResources[0].NumTexCoords = MaxStaticMeshUVChannels + 1;
	const auto Invalid = MakeStaticMeshPayloadData(*RenderData, Output);
	EXPECT_FALSE(Invalid);
	EXPECT_EQ(Invalid.error().Code, EStaticMeshPayloadError::UVChannelCount);
	EXPECT_EQ(Invalid.error().LODIndex, 0u);
	EXPECT_EQ(Invalid.error().Actual, MaxStaticMeshUVChannels + 1u);
	EXPECT_EQ(Invalid.error().Expected, MaxStaticMeshUVChannels);
	EXPECT_EQ(Output.MaterialSlotCount, 123u);
	EXPECT_TRUE(Output.LODs.empty());
}

TEST(FStaticMeshCookedProductTests, TypedArchiveAndMetadataFailuresPreserveProduct)
{
	FByteBuffer Bytes = Encode(MakeMultiMaterialFixture());
	std::vector<FMeshMaterialSlotDefinition> Slots{
		{.Name = FName("Body")}, {.Name = FName("Trim")}};
	FStaticMeshCookedProduct Product;
	ASSERT_TRUE(DecodeStaticMeshCookedProduct(Bytes, {}, Slots,
		EBodySetupCollisionSourceMode::None, EBodySetupCollisionQueryPolicy::SimpleAndComplex, Product));
	const auto* Original = Product.RenderData.get();
	Slots.pop_back();
	const auto Metadata = DecodeStaticMeshCookedProduct(Bytes, {}, Slots,
		EBodySetupCollisionSourceMode::None, EBodySetupCollisionQueryPolicy::SimpleAndComplex, Product);
	EXPECT_FALSE(Metadata);
	Slots.clear();
	EXPECT_EQ(Metadata.Error.Code, ECookedMeshProductError::MaterialSlotCount);
	EXPECT_EQ(Metadata.Error.Actual, 2u);
	EXPECT_EQ(Metadata.Error.Expected, 1u);
	EXPECT_EQ(Product.RenderData.get(), Original);

	WriteU32(Bytes, 4, StaticMeshPayloadSchemaVersion + 1);
	Rehash(Bytes);
	const auto Schema = DecodeStaticMeshCookedProduct(Bytes, {}, Slots,
		EBodySetupCollisionSourceMode::None, EBodySetupCollisionQueryPolicy::SimpleAndComplex, Product);
	EXPECT_FALSE(Schema);
	const auto ByteCount = Bytes.size();
	Bytes.clear();
	EXPECT_EQ(Schema.Error.Code, ECookedMeshProductError::RenderArchive);
	EXPECT_EQ(Schema.Error.ArchiveCode, EArchiveFailureCode::UnsupportedVersion);
	EXPECT_EQ(Schema.Error.ByteCount, ByteCount);
	EXPECT_LE(Schema.Error.ByteOffset, Schema.Error.ByteCount);
	EXPECT_EQ(Product.RenderData.get(), Original);
	const auto Missing = DecodeStaticMeshCookedProduct({}, {}, Slots,
		EBodySetupCollisionSourceMode::ConvexHullFromLOD0, EBodySetupCollisionQueryPolicy::SimpleAndComplex, Product);
	EXPECT_FALSE(Missing);
	EXPECT_EQ(Missing.Error.Code, ECookedMeshProductError::MissingCollision);
	EXPECT_EQ(Missing.Error.ExpectedMode, EBodySetupCollisionSourceMode::ConvexHullFromLOD0);
	EXPECT_EQ(Product.RenderData.get(), Original);
}

TEST(FStaticMeshCookedProductTests, CollisionMismatchOwnsModeAndPolicy)
{
	const std::array Vertices{FVector3(0, 0, 0), FVector3(1, 0, 0), FVector3(0, 1, 0), FVector3(0, 0, 1)};
	const std::array<uint32, 12> Indices{0, 2, 1, 0, 1, 3, 0, 3, 2, 1, 2, 3};
	const auto Geometry = FCollisionGeometryRef::MakeConvexHull(Vertices, Indices);
	ASSERT_TRUE(Geometry);
	FPhysicsCollisionPayloadData Payload;
	ASSERT_TRUE(MakePhysicsCollisionPayloadData(Geometry, EBodySetupCollisionQueryPolicy::SimpleAndComplex, Payload));
	FByteBuffer Bytes;
	FCanonicalMemoryWriter Ar(Bytes, EArchivePurpose::CookedPayload, {.Target = {"Win64", "Game"}});
	Payload.Serialize(Ar);
	ASSERT_FALSE(Ar.IsError()) << Ar.GetError();
	FStaticMeshCookedProduct Product;
	const auto Result = DecodeStaticMeshCookedProduct({}, Bytes, {},
		EBodySetupCollisionSourceMode::TriangleMeshFromLOD0, EBodySetupCollisionQueryPolicy::SimpleAndComplex, Product);
	ASSERT_FALSE(Result);
	Payload = {};
	Bytes.clear();
	EXPECT_EQ(Result.Error.Code, ECookedMeshProductError::CollisionMetadata);
	EXPECT_EQ(Result.Error.ActualMode, EBodySetupCollisionSourceMode::ConvexHullFromLOD0);
	EXPECT_EQ(Result.Error.ExpectedMode, EBodySetupCollisionSourceMode::TriangleMeshFromLOD0);
	EXPECT_EQ(Result.Error.ActualPolicy, EBodySetupCollisionQueryPolicy::SimpleAndComplex);
	EXPECT_EQ(Result.Error.ExpectedPolicy, EBodySetupCollisionQueryPolicy::SimpleAndComplex);
	EXPECT_EQ(Product.RenderData, nullptr);
	EXPECT_FALSE(FormatCookedMeshProductError(Result.Error).empty());
}

TEST(FStaticMeshPayloadCodecTests, SharedNativeStreamsRetainAllocationsAndDetachOnlyOnWrite)
{
	using namespace Durin;
	FStaticMeshLODResources LOD;
	auto& Buffers = LOD.VertexBuffers;
	auto& Frames = Buffers.StaticMeshVertexBuffer.TangentsVertexBuffer;
	auto& UVs = Buffers.StaticMeshVertexBuffer.TexCoordVertexBuffer;
	std::vector<FVector3f> PositionValues{{1, 2, 3}, {4, 5, 6}};
	PositionValues.reserve(32);
	const auto PositionCapacity = PositionValues.capacity();
	auto Positions = FSharedByteBuffer::TakeNative(std::move(PositionValues));
	auto Normals = FSharedByteBuffer::TakeNative(std::vector<FVector3f>(2, FVector3f(0, 0, 1)));
	auto Tangents = FSharedByteBuffer::TakeNative(std::vector<FVector4f>(2, FVector4f(1, 0, 0, 1)));
	auto TexCoords = FSharedByteBuffer::TakeNative(std::vector<FVector2f>(2, FVector2f(0.5f)));
	auto Colors = FSharedByteBuffer::TakeNative(std::vector<FVector4f>(2, FVector4f(0.75f)));
	auto Indices = FSharedByteBuffer::TakeNative(std::vector<uint32>{0, 1, 0});
	ASSERT_TRUE(Buffers.PositionVertexBuffer.SetSharedPositions(Positions));
	EXPECT_EQ(Buffers.PositionVertexBuffer.GetPositionCapacity(), PositionCapacity);
	ASSERT_TRUE(Frames.SetSharedNormals(Normals));
	ASSERT_TRUE(Frames.SetSharedTangents(Tangents));
	ASSERT_TRUE(UVs.SetSharedTexCoord(0, TexCoords));
	ASSERT_TRUE(Buffers.ColorVertexBuffer.SetSharedColors(Colors));
	ASSERT_TRUE(LOD.IndexBuffer.SetSharedIndices(Indices));
	Buffers.Finalize(1, true);
	EXPECT_EQ(Buffers.PositionVertexBuffer.GetPositions().data(), Positions.GetNativeView<FVector3f>()->data());
	EXPECT_EQ(Frames.GetNormals().data(), Normals.GetNativeView<FVector3f>()->data());
	EXPECT_EQ(Frames.GetTangents().data(), Tangents.GetNativeView<FVector4f>()->data());
	EXPECT_EQ(UVs.GetTexCoords()[0].data(), TexCoords.GetNativeView<FVector2f>()->data());
	EXPECT_EQ(Buffers.ColorVertexBuffer.GetColors().data(), Colors.GetNativeView<FVector4f>()->data());
	EXPECT_EQ(LOD.IndexBuffer.GetIndices().data(), Indices.GetNativeView<uint32>()->data());
	EXPECT_FALSE(Buffers.PositionVertexBuffer.SetSharedPositions(Tangents));
	EXPECT_FALSE(LOD.IndexBuffer.SetSharedIndices(FSharedByteBuffer::Copy(Indices.GetBytes())));
	EXPECT_EQ(LOD.IndexBuffer.GetIndices().data(), Indices.GetNativeView<uint32>()->data());
	EXPECT_FALSE(UVs.SetSharedTexCoord(MaxStaticMeshUVChannels, TexCoords));

	Buffers.PositionVertexBuffer.VertexPosition(0).x = 99;
	Frames.GetMutableNormals()[0].z = -1;
	Frames.GetMutableTangents()[0].w = -1;
	UVs.GetMutableTexCoord(0)[0].x = 0.25f;
	Buffers.ColorVertexBuffer.GetMutableColors()[0].x = 0;
	LOD.IndexBuffer.GetMutableIndices()[0] = 1;
	EXPECT_EQ(Positions.GetNativeView<FVector3f>()->front().x, 1);
	EXPECT_EQ(Normals.GetNativeView<FVector3f>()->front().z, 1);
	EXPECT_EQ(Tangents.GetNativeView<FVector4f>()->front().w, 1);
	EXPECT_EQ(TexCoords.GetNativeView<FVector2f>()->front().x, 0.5f);
	EXPECT_EQ(Colors.GetNativeView<FVector4f>()->front().x, 0.75f);
	EXPECT_EQ(Indices.GetNativeView<uint32>()->front(), 0u);
	EXPECT_NE(Buffers.PositionVertexBuffer.GetPositions().data(), Positions.GetNativeView<FVector3f>()->data());
	ASSERT_TRUE(Buffers.PositionVertexBuffer.SetSharedPositions(Positions));
	Positions = {};
	EXPECT_EQ(Buffers.PositionVertexBuffer.VertexPosition(1).z, 6);
}


namespace
{
	auto MakeRecipeRenderData(FStaticMeshPayloadData Payload) -> std::unique_ptr<FStaticMeshRenderData>
	{
		auto Product = std::make_unique<FStaticMeshRenderData>();
		Product->LocalBounds = Payload.LocalBounds;
		for (auto& Source : Payload.LODs)
		{
			auto& LOD = Product->LODResources.emplace_back();
			auto& Buffers = LOD.VertexBuffers;
			Buffers.PositionVertexBuffer.Init(std::move(Source.Positions));
			Buffers.StaticMeshVertexBuffer.TangentsVertexBuffer.Init(std::move(Source.Normals), std::move(Source.Tangents));
			Buffers.StaticMeshVertexBuffer.TexCoordVertexBuffer.Init(std::move(Source.TexCoords), Buffers.PositionVertexBuffer.GetNumVertices(), Source.NumTexCoords);
			Buffers.ColorVertexBuffer.Init(std::move(Source.Colors), Buffers.PositionVertexBuffer.GetNumVertices());
			LOD.IndexBuffer.Init(std::move(Source.Indices));
			LOD.LocalBounds = Source.LocalBounds; LOD.ScreenSize = Source.ScreenSize;
			LOD.NumTexCoords = Source.NumTexCoords; LOD.bHasColorVertexData = Source.bHasVertexColors;
			for (const auto& Section : Source.Sections)
				LOD.Sections.push_back({.FirstIndex = Section.FirstIndex, .IndexCount = Section.IndexCount,
					.MinVertexIndex = Section.MinVertexIndex, .MaxVertexIndex = Section.MaxVertexIndex,
					.MaterialSlotIndex = Section.MaterialSlotIndex, .LocalBounds = Section.LocalBounds});
		}
		return Product;
	}

	struct FMutableBuildOutput
	{
		std::string Schema;
		uint32 SchemaVersion = 0;
		FSharedByteBuffer Metadata;
		std::vector<std::pair<DerivedData::FValueId, FSharedByteBuffer>> Values;
		auto Build() && -> std::expected<DerivedData::FBuildOutput, std::string>
		{
			DerivedData::FBuildOutputBuilder Builder(std::move(Schema), SchemaVersion);
			if (!Metadata.IsEmpty())
			{
				auto Object = DerivedData::MakeBuildMetadata(Metadata);
				if (!Object || !Builder.AddMeta(DerivedData::FValueId::FromName("Metadata"), std::move(*Object)))
					return std::unexpected("Invalid test metadata.");
			}
			for (auto& [Id, Data] : Values) if (!Builder.AddValue(Id, std::move(Data)))
				return std::unexpected("Invalid test value.");
			return std::move(Builder).Build();
		}
	};
	auto CopyOutputDescriptors(const DerivedData::FBuildOutput& Output) -> FMutableBuildOutput
	{
		FMutableBuildOutput Copy{.Schema = std::string(Output.GetSchema()), .SchemaVersion = Output.GetSchemaVersion(),
			.Metadata = DerivedData::GetBuildMetadataPayload(Output)};
		for (const auto& Value : Output.GetValues()) Copy.Values.emplace_back(Value.Id, Value.Value.GetData());
		return Copy;
	}
	auto StaticMeshStreamId(uint32 Stream) -> DerivedData::FValueId
	{ return DerivedData::FValueId::FromName("Durin.StaticMesh.Stream").MakeIndexed(Stream); }
	auto CollisionBlockId(uint32 Geometry, uint32 Array) -> DerivedData::FValueId
	{ return DerivedData::FValueId::FromName("Durin.Physics.CollisionBlock").MakeIndexed(Geometry * 7 + Array); }
}

TEST(FStaticMeshPayloadCodecTests, SharedOutputRetainsEveryColdStreamAndOutlivesRecipe)
{
	auto Product = MakeRecipeRenderData(MakeMultiMaterialFixture());
	const auto& Source = Product->LODResources.front();
	const auto* Positions = Source.VertexBuffers.PositionVertexBuffer.GetPositions().data(); const auto* Normals = Source.VertexBuffers.StaticMeshVertexBuffer.TangentsVertexBuffer.GetNormals().data();
	const auto* Tangents = Source.VertexBuffers.StaticMeshVertexBuffer.TangentsVertexBuffer.GetTangents().data(); const auto* Colors = Source.VertexBuffers.ColorVertexBuffer.GetColors().data();
	const auto* Indices = Source.IndexBuffer.GetIndices().data();
	std::array<const FVector2f*, MaxStaticMeshUVChannels> UVs;
	for (uint32 Channel = 0; Channel < MaxStaticMeshUVChannels; ++Channel) UVs[Channel] = Source.VertexBuffers.StaticMeshVertexBuffer.TexCoordVertexBuffer.GetTexCoords()[Channel].data();
	auto Output = StaticMeshPrivate::MakeSharedOutput(std::move(Product), 2);
	ASSERT_TRUE(Output) << Output.error();
	auto Render = StaticMeshPrivate::AssembleSharedOutput(*Output);
	ASSERT_TRUE(Render) << Render.error();
	Output = DerivedData::FBuildOutput{};
	const auto& LOD = (*Render)->LODResources.front(); const auto& Buffers = LOD.VertexBuffers;
	EXPECT_EQ(Buffers.PositionVertexBuffer.GetPositions().data(), Positions);
	EXPECT_EQ(Buffers.StaticMeshVertexBuffer.TangentsVertexBuffer.GetNormals().data(), Normals);
	EXPECT_EQ(Buffers.StaticMeshVertexBuffer.TangentsVertexBuffer.GetTangents().data(), Tangents);
	EXPECT_EQ(Buffers.ColorVertexBuffer.GetColors().data(), Colors);
	EXPECT_EQ(LOD.IndexBuffer.GetIndices().data(), Indices);
	for (uint32 Channel = 0; Channel < MaxStaticMeshUVChannels; ++Channel)
		EXPECT_EQ(Buffers.StaticMeshVertexBuffer.TexCoordVertexBuffer.GetTexCoords()[Channel].data(), UVs[Channel]);
	EXPECT_TRUE(ValidateStaticMeshRenderData(**Render));
	// Re-export a cache-assembled value without detaching or copying its shared streams.
	auto Reexported = StaticMeshPrivate::MakeSharedOutput(std::move(*Render), 2);
	ASSERT_TRUE(Reexported) << Reexported.error();
	auto Reassembled = StaticMeshPrivate::AssembleSharedOutput(*Reexported);
	ASSERT_TRUE(Reassembled) << Reassembled.error();
	const auto& Again = (*Reassembled)->LODResources.front();
	EXPECT_EQ(Again.VertexBuffers.PositionVertexBuffer.GetPositions().data(), Positions);
	EXPECT_EQ(Again.IndexBuffer.GetIndices().data(), Indices);
}

TEST(FStaticMeshPayloadCodecTests, IndependentlyBuiltOutputIsValidatedDuringAssembly)
{
	using namespace DerivedData;
	auto Output = StaticMeshPrivate::MakeSharedOutput(MakeRecipeRenderData(MakeMultiMaterialFixture()), 2);
	ASSERT_TRUE(Output) << Output.error();
	auto Clone = std::move(CopyOutputDescriptors(*Output)).Build();
	ASSERT_TRUE(Clone) << Clone.error();
	EXPECT_FALSE(Clone->SharesStateWith(*Output));
	EXPECT_TRUE(StaticMeshPrivate::AssembleSharedOutput(*Clone));
}

TEST(FStaticMeshPayloadCodecTests, SharedOutputRestoresRawAndCompressedRecordsWithoutChangingCookBytes)
{
	using namespace DerivedData;
	for (bool Compressed : {false, true}) for (uint32 LODCount : {1u, 2u, 3u})
	{
		auto Payload = LODCount == 1 ? MakeNoUVFixture() : MakeMultiLODFixture(LODCount);
		FByteBuffer Expected; std::string Error;
		ASSERT_TRUE(EncodePayload(Payload, EAssetPayloadTargetPlatform::Win64, Expected, Error)) << Error;
		auto Output = StaticMeshPrivate::MakeSharedOutput(MakeRecipeRenderData(Payload), Payload.MaterialSlotCount);
		ASSERT_TRUE(Output) << Output.error();
		const auto Key = FCacheKey::FromHash(FCacheBucket::FromString("MeshOutputFixture"), FXxHash128::HashBuffer("mesh"));
		auto Record = FCacheRecord::FromOutput(Key, *Output); ASSERT_TRUE(Record);
		auto Encoded = Record->Encode(); ASSERT_TRUE(Encoded);
		if (Compressed) Encoded = FCacheRecord::CompressEncoded(*Encoded);
		ASSERT_TRUE(Encoded);
		auto Loaded = FCacheRecord::Decode(Key, *Encoded); ASSERT_TRUE(Loaded);
		auto Warm = Loaded->ToOutput(Key); ASSERT_TRUE(Warm);
		EXPECT_FALSE(Warm->FindValue(StaticMeshStreamId(0))->GetData().GetNativeView<FVector3f>());
		auto Render = StaticMeshPrivate::AssembleSharedOutput(*Warm); ASSERT_TRUE(Render) << Render.error();
		Warm = FBuildOutput{}; Loaded = FCacheRecord{}; Encoded = FSharedByteBuffer{};
		FStaticMeshPayloadData Restored; ASSERT_TRUE(MakeStaticMeshPayloadData(**Render, Restored));
		FByteBuffer Actual;
		ASSERT_TRUE(EncodePayload(Restored, EAssetPayloadTargetPlatform::Win64, Actual, Error)) << Error;
		EXPECT_EQ(Actual, Expected);
	}
}

TEST(FStaticMeshPayloadCodecTests, SharedOutputRejectsMalformedDescriptorsAndStreamsBeforeAssembly)
{
	using namespace DerivedData;
	auto Output = StaticMeshPrivate::MakeSharedOutput(MakeRecipeRenderData(MakeMultiMaterialFixture()), 2);
	ASSERT_TRUE(Output);
	for (uint32 Mutation = 0; Mutation < 12; ++Mutation)
	{
		SCOPED_TRACE(Mutation);
		auto Data = CopyOutputDescriptors(*Output);
		if (Mutation == 0) Data.SchemaVersion++;
		if (Mutation == 1) Data.Values.pop_back();
		if (Mutation == 2) Data.Values.push_back({FValueId::FromName("Extra"), FSharedByteBuffer{}});
		if (Mutation >= 3 && Mutation <= 6)
		{
			auto View = Data.Metadata.GetBytes(); FByteBuffer Bytes(View.begin(), View.end());
			if (Mutation == 3) Bytes.pop_back();
			if (Mutation == 4) Bytes[0] = std::byte{0xff};
			if (Mutation == 5) std::fill(Bytes.begin() + 116, Bytes.begin() + 120, std::byte{0xff});
			if (Mutation == 6) Bytes.push_back(std::byte{});
			Data.Metadata = FSharedByteBuffer::Take(std::move(Bytes));
		}
		for (auto& [Id, Value] : Data.Values)
		{
			if (Mutation == 7 && Id == StaticMeshStreamId(0)) Value = Value.MakeView(0, 1);
			if (Mutation == 8 && Id == StaticMeshStreamId(1))
			{
				auto Values = MakeMultiMaterialFixture().LODs[0].Normals;
				Values[0].x = std::numeric_limits<float>::quiet_NaN();
				Value = FSharedByteBuffer::TakeNative(std::move(Values));
			}
			if (Mutation == 9 && Id == StaticMeshStreamId(3))
				Value = FSharedByteBuffer::TakeNative(std::vector<uint32>{0, 1, 99, 2, 1, 3});
			if (Mutation >= 10 && Id == StaticMeshStreamId(9))
			{
				auto View = Value.GetBytes(); FByteBuffer Bytes(View.begin(), View.end());
				Bytes[Mutation == 10 ? 0 : 16] = std::byte{99};
				Value = FSharedByteBuffer::Take(std::move(Bytes));
			}
		}
		auto Invalid = std::move(Data).Build(); ASSERT_TRUE(Invalid);
		EXPECT_FALSE(StaticMeshPrivate::ValidateSharedOutput(*Invalid));
		EXPECT_FALSE(StaticMeshPrivate::AssembleSharedOutput(*Invalid));
	}
	EXPECT_FALSE(StaticMeshPrivate::ValidateSharedOutput(*Output, [] { return true; }));
	EXPECT_FALSE(StaticMeshPrivate::AssembleSharedOutput(*Output, [] { return true; }));
	auto InvalidBounds = MakeRecipeRenderData(MakeSingleSectionFixture());
	InvalidBounds->LODResources[0].Sections[0].LocalBounds.bIsValid = false;
	EXPECT_FALSE(StaticMeshPrivate::MakeSharedOutput(std::move(InvalidBounds), 1));
}


TEST(FStaticMeshPayloadCodecTests, SharedOutputRejectsNonTriangleSectionsAndHonorsMidAssemblyCancellation)
{
	auto Product = MakeRecipeRenderData(MakeSingleSectionFixture());
	Product->LODResources[0].IndexBuffer.GetMutableIndices() = {0, 1};
	Product->LODResources[0].Sections[0].IndexCount = 2;
	Product->LODResources[0].Sections[0].MaxVertexIndex = 1;
	EXPECT_FALSE(StaticMeshPrivate::MakeSharedOutput(std::move(Product), 1));
	Product = MakeRecipeRenderData(MakeMultiMaterialFixture());
	Product->LODResources[0].Sections[0].IndexCount = 2;
	Product->LODResources[0].Sections[0].MaxVertexIndex = 1;
	Product->LODResources[0].Sections[1].FirstIndex = 2;
	Product->LODResources[0].Sections[1].IndexCount = 4;
	EXPECT_FALSE(StaticMeshPrivate::MakeSharedOutput(std::move(Product), 2));
	auto Output = StaticMeshPrivate::MakeSharedOutput(MakeRecipeRenderData(MakeSingleSectionFixture()), 1);
	ASSERT_TRUE(Output);
	// Make every stream byte-backed, exercising typed restoration after validation.
	auto Data = CopyOutputDescriptors(*Output);
	for (auto& [Id, Value] : Data.Values) Value = FSharedByteBuffer::Copy(Value.GetBytes());
	Output = std::move(Data).Build(); ASSERT_TRUE(Output);
	uint32 ValidationChecks = 0;
	ASSERT_TRUE(StaticMeshPrivate::ValidateSharedOutput(*Output, [&] { ++ValidationChecks; return false; }));
	uint32 Checks = 0;
	EXPECT_FALSE(StaticMeshPrivate::AssembleSharedOutput(*Output, [&] { return ++Checks > ValidationChecks + 1; }));
	EXPECT_GT(Checks, ValidationChecks + 1);
	EXPECT_TRUE(StaticMeshPrivate::AssembleSharedOutput(*Output));
}

TEST(FStaticMeshPayloadCodecTests, RenderViewValidationMatchesArchiveValidationWithoutMutatingStreams)
{
	for (uint32 Mutation = 0; Mutation < 5; ++Mutation)
	{
		SCOPED_TRACE(Mutation);
		std::unique_ptr<FStaticMeshRenderData> Render;
		ASSERT_TRUE(MakeStaticMeshRenderData(MakeSingleSectionFixture(), Render));
		auto& LOD = Render->LODResources.front();
		if (Mutation == 1) LOD.IndexBuffer.GetMutableIndices()[0] = 100;
		if (Mutation == 2) LOD.VertexBuffers.PositionVertexBuffer.VertexPosition(0).x = std::numeric_limits<float>::quiet_NaN();
		if (Mutation == 3) LOD.Sections[0].MaterialSlotIndex = 100;
		if (Mutation == 4) LOD.NumTexCoords = 5;
		const auto* Address = LOD.VertexBuffers.PositionVertexBuffer.GetPositions().data();
		auto ViewResult = ValidateStaticMeshRenderData(*Render);
		FStaticMeshPayloadData Payload;
		auto ArchiveResult = MakeStaticMeshPayloadData(*Render, Payload);
		ASSERT_EQ(bool(ViewResult), bool(ArchiveResult));
		if (!ViewResult) EXPECT_EQ(ViewResult.error().Code, ArchiveResult.error().Code);
		EXPECT_EQ(Address, LOD.VertexBuffers.PositionVertexBuffer.GetPositions().data());
		auto Cancelled = ValidateStaticMeshRenderData(*Render, [] { return true; });
		ASSERT_FALSE(Cancelled);
		EXPECT_EQ(Cancelled.error().Code, EStaticMeshPayloadError::Cancelled);
	}
}

TEST(FStaticMeshPayloadCodecTests, CollisionSharedOutputRetainsColdArraysAndPreservesCookBytes)
{
	using namespace DerivedData;
	const std::array<FVector3, 4> Points{FVector3{0.0}, FVector3{1, 0, 0}, FVector3{0, 1, 0}, FVector3{0, 0, 1}};
	const std::array<uint32, 12> Indices{0, 2, 1, 0, 1, 3, 0, 3, 2, 1, 2, 3};
	for (bool Hull : {false, true}) for (bool Compressed : {false, true})
	{
		const auto Mode = Hull ? EBodySetupCollisionSourceMode::ConvexHullFromLOD0 : EBodySetupCollisionSourceMode::TriangleMeshFromLOD0;
		const auto Policy = EBodySetupCollisionQueryPolicy::SimpleAndComplex;
		auto Cooked = Hull ? FCollisionCookedData::BuildConvexHull(Points) : FCollisionCookedData::BuildTriangleMesh(Points, Indices);
		ASSERT_TRUE(Cooked);
		const auto Block = Cooked.GetVertices();
		auto Output = PhysicsPrivate::MakeSharedOutput(std::move(Cooked), Mode, Policy); ASSERT_TRUE(Output) << Output.error();
		auto Cold = PhysicsPrivate::AssembleSharedOutput(*Output, Mode, Policy); ASSERT_TRUE(Cold) << Cold.error();
		const auto& Geometry = Hull ? Cold->Simple : Cold->Complex;
		EXPECT_EQ(Geometry.GetVertex(0), Block.GetNativeView<FVector3>()->data());
		FPhysicsCollisionPayloadData ExpectedPayload;
		ASSERT_TRUE(MakePhysicsCollisionPayloadData(Geometry, Policy, ExpectedPayload));
		FByteBuffer ExpectedBytes;
		FCanonicalMemoryWriter ExpectedWriter(ExpectedBytes, EArchivePurpose::CookedPackage, {.Target = {"Win64", "Game"}});
		ExpectedPayload.Serialize(ExpectedWriter); ASSERT_FALSE(ExpectedWriter.IsError());
		const auto Key = FCacheKey::FromHash(FCacheBucket::FromString("CollisionOutputFixture"), FXxHash128::HashBuffer("collision"));
		auto Record = FCacheRecord::FromOutput(Key, *Output); ASSERT_TRUE(Record);
		auto Encoded = Record->Encode(); ASSERT_TRUE(Encoded);
		if (Compressed) Encoded = FCacheRecord::CompressEncoded(*Encoded);
		ASSERT_TRUE(Encoded);
		auto Loaded = FCacheRecord::Decode(Key, *Encoded); ASSERT_TRUE(Loaded);
		auto Warm = Loaded->ToOutput(Key); ASSERT_TRUE(Warm);
		EXPECT_FALSE(Warm->FindValue(CollisionBlockId(Hull ? 0 : 1, 0))->GetData().GetNativeView<FVector3>());
		auto Assembled = PhysicsPrivate::AssembleSharedOutput(*Warm, Mode, Policy); ASSERT_TRUE(Assembled) << Assembled.error();
		const auto& Restored = Hull ? Assembled->Simple : Assembled->Complex;
		EXPECT_NE(Restored.GetIdentity(), Geometry.GetIdentity());
		Output = FBuildOutput{}; Warm = FBuildOutput{}; Loaded = FCacheRecord{}; Encoded = FSharedByteBuffer{}; Cold = FPhysicsCookResult{};
		FPhysicsCollisionPayloadData ActualPayload; ASSERT_TRUE(MakePhysicsCollisionPayloadData(Restored, Policy, ActualPayload));
		FByteBuffer ActualBytes;
		FCanonicalMemoryWriter ActualWriter(ActualBytes, EArchivePurpose::CookedPackage, {.Target = {"Win64", "Game"}});
		ActualPayload.Serialize(ActualWriter); ASSERT_FALSE(ActualWriter.IsError());
		EXPECT_EQ(ActualBytes, ExpectedBytes);
	}
}

TEST(FStaticMeshPayloadCodecTests, CollisionSharedOutputRejectsInvalidMetadataAndExtraBlocks)
{
	using namespace DerivedData;
	const std::array<FVector3, 3> Points{FVector3{0.0}, FVector3{1, 0, 0}, FVector3{0, 1, 0}};
	const std::array<uint32, 3> Indices{0, 1, 2};
	const auto Mode = EBodySetupCollisionSourceMode::TriangleMeshFromLOD0;
	const auto Policy = EBodySetupCollisionQueryPolicy::SimpleAndComplex;
	auto Output = PhysicsPrivate::MakeSharedOutput(FCollisionCookedData::BuildTriangleMesh(Points, Indices), Mode, Policy);
	ASSERT_TRUE(Output);
	for (uint32 Fault = 0; Fault < 13; ++Fault)
	{
		SCOPED_TRACE(Fault);
		auto Data = CopyOutputDescriptors(*Output);
		FByteBuffer Metadata(Data.Metadata.begin(), Data.Metadata.end());
		if (Fault == 0) ++Data.SchemaVersion;
		if (Fault == 1) WriteU32(Metadata, 0, 999);
		if (Fault == 2) WriteU32(Metadata, 4, 999);
		if (Fault == 3) WriteU32(Metadata, 8, uint32(EBodySetupCollisionSourceMode::ConvexHullFromLOD0));
		if (Fault == 4) WriteU32(Metadata, 12, uint32(EBodySetupCollisionQueryPolicy::SimpleOnly));
		if (Fault == 5) WriteU32(Metadata, 16, 1);
		if (Fault == 6) WriteU32(Metadata, 24, uint32(ECollisionGeometryKind::Primitive));
		if (Fault == 7) Metadata.pop_back();
		if (Fault == 8) Metadata.push_back(std::byte{});
		if (Fault == 9) Data.Values.pop_back();
		if (Fault == 10) Data.Values.push_back({CollisionBlockId(0, 0), Output->FindValue(CollisionBlockId(1, 0))->GetData()});
		if (Fault == 11) Data.Values.push_back({CollisionBlockId(1, 4), FSharedByteBuffer::TakeNative(std::vector<FCollisionHullPlane>(1))});
		if (Fault == 12) for (auto& [Id, Value] : Data.Values) if (Id == CollisionBlockId(1, 3))
			Value = FSharedByteBuffer::TakeNative(std::vector<uint32>{999});
		Data.Metadata = FSharedByteBuffer::Take(std::move(Metadata));
		auto Invalid = std::move(Data).Build(); ASSERT_TRUE(Invalid);
		EXPECT_FALSE(PhysicsPrivate::ValidateSharedOutput(*Invalid, Mode, Policy));
		EXPECT_FALSE(PhysicsPrivate::AssembleSharedOutput(*Invalid, Mode, Policy));
	}
	EXPECT_FALSE(PhysicsPrivate::MakeSharedOutput({}, Mode, Policy));
	EXPECT_FALSE(PhysicsPrivate::ValidateSharedOutput(*Output, Mode, Policy, [] { return true; }));
	uint32 Checks = 0;
	EXPECT_FALSE(PhysicsPrivate::AssembleSharedOutput(*Output, Mode, Policy, [&] { return ++Checks == 3; }));
}

TEST(FStaticMeshPayloadCodecTests, PositionBufferMovesRvaluesAndPreservesBorrowedInputs)
{
	std::vector<FVector3f> Positions{{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}};
	const auto* Allocation = Positions.data();
	FPositionVertexBuffer Borrowed;
	Borrowed.Init(Positions);
	EXPECT_EQ(Positions.data(), Allocation);
	EXPECT_NE(Borrowed.GetPositions().data(), Allocation);
	FPositionVertexBuffer Owned;
	Owned.Init(std::move(Positions));
	EXPECT_EQ(Owned.GetPositions().data(), Allocation);
	const auto Frozen = Owned.FreezePositions();
	EXPECT_EQ(Frozen.GetNativeView<FVector3f>()->data(), Allocation);
	EXPECT_FALSE(Owned.IsInitialized());
}

TEST(FStaticMeshPayloadCodecTests, PositionBufferSupportsUEAccessAndDetachesSharedWrites)
{
	FPositionVertexBuffer Buffer;
	Buffer.Init(uint32(2), false);
	EXPECT_EQ(Buffer.GetNumVertices(), 2u);
	EXPECT_EQ(Buffer.GetStride(), sizeof(FVector3f));
	EXPECT_FALSE(Buffer.GetAllowCPUAccess());
	Buffer.VertexPosition(0) = FVector3f(1, 2, 3);
	Buffer.VertexPosition(1) = FVector3f(4, 5, 6);
	const auto Frozen = Buffer.FreezePositions();
	const auto& ReadOnly = Buffer;
	EXPECT_EQ(ReadOnly.GetVertexData(), Frozen.GetNativeView<FVector3f>()->data());
	EXPECT_EQ(ReadOnly.VertexPosition(0).x, 1);
	Buffer.VertexPosition(0).x = 9;
	EXPECT_EQ(ReadOnly.VertexPosition(0).x, 9);
	EXPECT_EQ((*Frozen.GetNativeView<FVector3f>())[0].x, 1);
	EXPECT_NE(ReadOnly.GetVertexData(), Frozen.GetNativeView<FVector3f>()->data());
	static_cast<FVector3f*>(Buffer.GetVertexData())[1].z = 8;
	EXPECT_EQ(ReadOnly.VertexPosition(1).z, 8);
	EXPECT_FALSE(Buffer.IsInitialized());
}

namespace
{
	DECLARE_RENDER_COMMAND_TAG(FPositionBufferLifetimeCommand, PositionBufferLifetimeCommand);

	auto RunPositionBufferCommand(std::function<void(FRHICommandListImmediate&)> Operation) -> void
	{
		EnqueueRenderCommand<FPositionBufferLifetimeCommand>(std::move(Operation));
		FlushRenderingCommands();
	}

	class FPositionBufferTestDeclaration final : public FRHIVertexDeclaration
	{
	public:
		explicit FPositionBufferTestDeclaration(const FVertexDeclarationElementList& InElements) : Elements(InElements) {}
		auto GetElements() const -> const FVertexDeclarationElementList& override { return Elements; }
	private:
		FVertexDeclarationElementList Elements;
	};

	// CPU backend snapshots creation bytes so lifetime policy can be tested without a GPU.
	class FPositionBufferTestRHI final : public FDynamicRHI
	{
	public:
		FPositionBufferTestRHI() : Previous(GDynamicRHI) { GDynamicRHI = this; }
		~FPositionBufferTestRHI() override { RHIFlushDeferredResources(); GDynamicRHI = Previous; }
		bool bFailCreation = false;
		uint32 Creates = 0;
		std::vector<std::byte> Uploaded;
		auto Init(const FRHIInitializationContext&) -> void override {}
		auto Shutdown() -> void override {}
		auto RHIBeginFrame(const FRHIBeginFrameArgs&) -> void override {}
		auto RHIEndFrame() -> void override {}
		auto RHICreateViewport(const FRHIViewportCreateInfo&) -> FViewportRHIRef override { return {}; }
		auto RHIResizeViewport(FRHIViewport*, uint32, uint32, bool) -> void override {}
		auto RHICreateGraphicsPipelineState(FName, const FGraphicsPipelineStateInitializer&) -> FGraphicsPipelineStateRHIRef override { return {}; }
		auto RHIGetDefaultContext() -> IRHICommandContext* override { return nullptr; }
		auto RHIGetViewportBackBuffer(FRHIViewport*) -> FTextureRHIRef override { return {}; }
		auto RHICreateVertexDeclaration(const FVertexDeclarationElementList& Elements) -> FVertexDeclarationRHIRef override
		{
			return MakeRefCount<FPositionBufferTestDeclaration>(Elements);
		}
		auto RHIIsTextureSupported(const FRHITextureCreateDesc&) const -> bool override { return false; }
		auto RHITryCreateTexture(FRHICommandListBase&, const FRHITextureCreateDesc&) -> std::expected<FTextureRHIRef, FRHICreationError> override
		{
			return std::unexpected(FRHICreationError{ERHIResourceCreationFailure::Unknown, ERHICreationFailureSource::BackendReturnedNull});
		}
		auto RHICreateSampler(const FRHISamplerDesc&) -> FSamplerRHIRef override { return {}; }
		auto RHICreateShader(const FRHIShaderCreateDesc&) -> FShaderRHIRef override { return {}; }
		auto RHITryCreateBuffer(FRHICommandListImmediate&, const FRHIBufferCreateDesc& Desc) -> std::expected<FBufferRHIRef, FRHICreationError> override
		{
			++Creates;
			if (bFailCreation)
				return std::unexpected(FRHICreationError{ERHIResourceCreationFailure::OutOfMemory, ERHICreationFailureSource::NativeBackend});
			const auto* Bytes = static_cast<const std::byte*>(Desc.InitialData.Data);
			Uploaded.assign(Bytes, Bytes + Desc.InitialData.Size);
			return MakeRefCount<FRHIBuffer>(Desc);
		}
	private:
		FDynamicRHI* Previous;
	};
}

TEST(FStaticMeshPayloadCodecTests, PositionBufferDiscardsCPUStorageOnlyAfterSuccessfulUpload)
{
	FPositionBufferTestRHI RHI;
	FPositionVertexBuffer Buffer;
	std::vector<FVector3f> Positions{{1, 2, 3}, {4, 5, 6}};
	const auto Expected = std::as_bytes(std::span(Positions));
	const std::vector<std::byte> ExpectedBytes(Expected.begin(), Expected.end());
	Buffer.Init(std::move(Positions), false);
	RHI.bFailCreation = true;
	RunPositionBufferCommand([&](auto& Cmd) { Buffer.InitRHI(Cmd); });
	EXPECT_FALSE(Buffer.IsReady());
	EXPECT_EQ(Buffer.GetPositions().size(), 2u);
	EXPECT_GT(Buffer.GetPositionCapacity(), 0u);
	RHI.bFailCreation = false;
	RunPositionBufferCommand([&](auto& Cmd) { Buffer.InitRHI(Cmd); });
	EXPECT_EQ(RHI.Uploaded, ExpectedBytes);
	EXPECT_TRUE(Buffer.IsReady());
	EXPECT_EQ(Buffer.GetNumVertices(), 2u);
	EXPECT_EQ(Buffer.GetStride(), sizeof(FVector3f));
	EXPECT_TRUE(Buffer.GetPositions().empty());
	EXPECT_EQ(Buffer.GetPositionCapacity(), 0u);
	EXPECT_EQ(std::as_const(Buffer).GetVertexData(), nullptr);
	RunPositionBufferCommand([&](auto& Cmd) { Buffer.InitRHI(Cmd); });
	EXPECT_EQ(RHI.Creates, 2u);
	RunPositionBufferCommand([&](auto&) { Buffer.ReleaseRHI(); });
	Buffer.Init(std::vector<FVector3f>{{7, 8, 9}}, true);
	EXPECT_EQ(Buffer.GetNumVertices(), 1u);
	RunPositionBufferCommand([&](auto& Cmd) { Buffer.InitRHI(Cmd); });
	EXPECT_EQ(Buffer.GetPositions().size(), 1u);
	EXPECT_EQ(std::as_const(Buffer).VertexPosition(0).x, 7);
}

TEST(FStaticMeshPayloadCodecTests, PositionBufferReleasesSharedOwnerAndPreservesCPUAccessData)
{
	FPositionBufferTestRHI RHI;
	const auto Shared = FSharedByteBuffer::TakeNative(std::vector<FVector3f>{{1, 2, 3}});
	FPositionVertexBuffer Discarded;
	ASSERT_TRUE(Discarded.SetSharedPositions(Shared, false));
	EXPECT_EQ(Discarded.GetNumVertices(), 1u);
	EXPECT_FALSE(Discarded.SetSharedPositions(FSharedByteBuffer::Copy(Shared.GetBytes()), true));
	EXPECT_FALSE(Discarded.GetAllowCPUAccess());
	EXPECT_EQ(Discarded.GetNumVertices(), 1u);
	RunPositionBufferCommand([&](auto& Cmd) { Discarded.InitRHI(Cmd); });
	EXPECT_TRUE(Discarded.GetPositions().empty());
	EXPECT_EQ(Discarded.GetPositionCapacity(), 0u);
	EXPECT_EQ(Discarded.GetNumVertices(), 1u);
	EXPECT_EQ((*Shared.GetNativeView<FVector3f>())[0].x, 1);
	FPositionVertexBuffer Retained;
	ASSERT_TRUE(Retained.SetSharedPositions(Shared));
	RunPositionBufferCommand([&](auto& Cmd) { Retained.InitRHI(Cmd); });
	EXPECT_EQ(Retained.GetPositions().data(), Shared.GetNativeView<FVector3f>()->data());
	EXPECT_TRUE(Retained.GetAllowCPUAccess());
	RunPositionBufferCommand([&](auto&) { Retained.ReleaseRHI(); });
	RunPositionBufferCommand([&](auto& Cmd) { Retained.InitRHI(Cmd); });
	EXPECT_TRUE(Retained.IsReady());
	EXPECT_EQ(Retained.GetNumVertices(), 1u);
}

TEST(FStaticMeshPayloadCodecTests, FinalizationRepairsBoundsAcrossLODsAndRetainsScreenSizeValidation)
{
	using namespace Durin;
	auto Payload = MakeMultiLODFixture(2);
	for (auto& Position : Payload.LODs.back().Positions) Position += FVector3f(-5, 7, 2);
	auto Render = MakeRecipeRenderData(std::move(Payload));
	Render->MaterialSlots = {{.Name = "First"}, {.Name = "Second"}};
	const auto Finalized = FinalizeStaticMeshRenderData(*Render);
	ASSERT_TRUE(Finalized) << Finalized.error().ToString();
	EXPECT_EQ(Render->LODResources.back().LocalBounds.Min, FVector3(-5, 7, 2));
	EXPECT_EQ(Render->LODResources.back().LocalBounds.Max, FVector3(-4.5, 7.5, 2));
	EXPECT_EQ(Render->LODResources.back().Sections.front().LocalBounds.Min, FVector3(-5, 7, 2));
	EXPECT_EQ(Render->LocalBounds.Min, FVector3(-5, -1, 0));
	EXPECT_EQ(Render->LocalBounds.Max, FVector3(1, 7.5, 2));
	Render->LODResources.back().ScreenSize = Render->LODResources.front().ScreenSize;
	EXPECT_FALSE(FinalizeStaticMeshRenderData(*Render));
}

TEST(FStaticMeshPayloadCodecTests, RenderDataInitializationRemainsValidAfterPositionDiscard)
{
	FPositionBufferTestRHI RHI;
	std::unique_ptr<FStaticMeshRenderData> Data;
	ASSERT_TRUE(MakeStaticMeshRenderData(MakeSingleSectionFixture(), Data));
	auto& Position = Data->LODResources[0].VertexBuffers.PositionVertexBuffer;
	Position.Init(std::vector<FVector3f>(Position.GetPositions().begin(), Position.GetPositions().end()), false);
	bool FirstInit = false, RepeatedInit = false, Ready = false;
	RunPositionBufferCommand([&](auto& Cmd) {
		FirstInit = Data->InitResources(Cmd);
		RepeatedInit = Data->InitResources(Cmd);
		Ready = Data->IsReadyForRendering();
		Data->ReleaseResources();
	});
	EXPECT_TRUE(FirstInit);
	EXPECT_TRUE(RepeatedInit);
	EXPECT_TRUE(Ready);
	EXPECT_EQ(Position.GetNumVertices(), 3u);
	EXPECT_TRUE(Position.GetPositions().empty());
}

TEST(FStaticMeshPayloadCodecTests, LegacyProducerVersionDoesNotControlFormatCompatibility)
{
	const auto Fixture = MakeSingleSectionFixture();
	const auto Valid = Encode(Fixture);
	EXPECT_EQ(ReadU32(Valid, 8), 0u);
	for (const uint32 Producer : {0u, 4u, 5u, std::numeric_limits<uint32>::max()})
	{
		auto Bytes = Valid;
		WriteU32(Bytes, 8, Producer);
		Rehash(Bytes);
		FStaticMeshPayloadData Decoded;
		const auto Result = DecodePayload(Bytes, EAssetPayloadTargetPlatform::Win64, Decoded);
		ASSERT_TRUE(Result) << Result.error().Message;
		ExpectEquivalent(Decoded, Fixture);
	}
}
