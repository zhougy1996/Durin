#include <gtest/gtest.h>

#include "Hash/XxHash.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "RHI.h"
#include "StaticMesh/StaticMeshDerivedData.h"

namespace
{
	using namespace Durin;

	auto MakeBounds(const FVector3& Minimum, const FVector3& Maximum) -> FBox
	{
		return FBox(Minimum, Maximum);
	}

	auto ParseGuid(std::string_view Text) -> FGuid
	{
		FGuid Result;
		EXPECT_TRUE(FGuid::Parse(Text, Result));
		return Result;
	}

	auto MakeSingleSectionFixture() -> FStaticMeshPayloadData
	{
		FStaticMeshPayloadData Payload;
		Payload.LocalBounds = MakeBounds(FVector3(0.0, 0.0, 0.0), FVector3(1.0, 1.0, 0.0));
		Payload.MaterialSlotIds = {ParseGuid("11111111-1111-1111-1111-111111111111")};

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
		Payload.MaterialSlotIds = {
			ParseGuid("22222222-2222-2222-2222-222222222222"),
			ParseGuid("33333333-3333-3333-3333-333333333333")};

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

	auto Encode(const FStaticMeshPayloadData& Payload) -> std::vector<uint8>
	{
		std::vector<uint8> Bytes;
		std::string Error;
		EXPECT_TRUE(EncodeStaticMeshPayload(Payload, EStaticMeshTargetPlatform::Win64, Bytes, Error)) << Error;
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

	auto MakeTestBuffer(
		const FRHIBufferCreateDesc& Desc) -> FBufferRHIRef
	{
		return FBufferRHIRef(new FRHIBuffer(Desc));
	}

	auto Rehash(std::vector<uint8>& Bytes) -> void
	{
		WriteU64(Bytes, 56, FXxHash64::HashBuffer(std::span<const uint8>(Bytes).subspan(64)).HashValue);
	}

	auto ExpectDecodeFailure(const std::vector<uint8>& Bytes) -> void
	{
		FStaticMeshPayloadData Sentinel = MakeMultiMaterialFixture();
		const FGuid SentinelSlot = Sentinel.MaterialSlotIds.front();
		std::string Error;
		EXPECT_FALSE(DecodeStaticMeshPayload(Bytes, EStaticMeshTargetPlatform::Win64, Sentinel, Error));
		EXPECT_FALSE(Error.empty());
		EXPECT_EQ(Sentinel.MaterialSlotIds.front(), SentinelSlot);
		EXPECT_EQ(Sentinel.MaterialSlotIds.size(), 2u);
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
		ASSERT_EQ(Actual.MaterialSlotIds, Expected.MaterialSlotIds);
		ASSERT_EQ(Actual.LODs.size(), Expected.LODs.size());
		EXPECT_EQ(Actual.LocalBounds.Min, Expected.LocalBounds.Min);
		EXPECT_EQ(Actual.LocalBounds.Max, Expected.LocalBounds.Max);
		for (size_t LODIndex = 0; LODIndex < Expected.LODs.size(); ++LODIndex)
		{
			const FStaticMeshPayloadLOD& ActualLOD = Actual.LODs[LODIndex];
			const FStaticMeshPayloadLOD& ExpectedLOD = Expected.LODs[LODIndex];
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

TEST(FStaticMeshPayloadCodecTests, CanonicalFixturesRoundTripDeterministically)
{
	const std::array Fixtures{MakeSingleSectionFixture(), MakeMultiMaterialFixture()};
	const std::array<std::string_view, 2> ExpectedPayloadHashes{
		"31c37f86ed5caa8a579bb2f7c2a1a7cb",
		"da830a88b13fea4311711b14cbf4c573"};
	const std::array<size_t, 2> ExpectedPayloadSizes{572, 856};
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
		ASSERT_TRUE(DecodeStaticMeshPayload(First, EStaticMeshTargetPlatform::Win64, Decoded, Error)) << Error;
		ExpectEquivalent(Decoded, Fixture);

		std::unique_ptr<FStaticMeshRenderData> RenderData;
		ASSERT_TRUE(MakeStaticMeshRenderData(Decoded, RenderData, Error)) << Error;
		FStaticMeshPayloadData ConvertedBack;
		ASSERT_TRUE(MakeStaticMeshPayloadData(*RenderData, ConvertedBack, Error)) << Error;
		ExpectEquivalent(ConvertedBack, Fixture);
	}
}

TEST(FStaticMeshPayloadCodecTests, SupportsMeshWithoutUVChannels)
{
	const FStaticMeshPayloadData Fixture = MakeNoUVFixture();
	const std::vector<uint8> Bytes = Encode(Fixture);

	FStaticMeshPayloadData Decoded;
	std::string Error;
	ASSERT_TRUE(DecodeStaticMeshPayload(Bytes, EStaticMeshTargetPlatform::Win64, Decoded, Error)) << Error;
	ExpectEquivalent(Decoded, Fixture);

	std::unique_ptr<FStaticMeshRenderData> RenderData;
	ASSERT_TRUE(MakeStaticMeshRenderData(Decoded, RenderData, Error)) << Error;
	ASSERT_EQ(RenderData->LODResources.size(), 1u);
	const FStaticMeshLODResources& LOD = RenderData->LODResources[0];
	EXPECT_EQ(LOD.NumTexCoords, 0u);
	for (const auto& Channel : LOD.TexCoords)
	{
		ASSERT_EQ(Channel.size(), LOD.Positions.size());
		for (const FVector2f& UV : Channel) ExpectVector(UV, FVector2f(0.0f));
	}
	ASSERT_EQ(LOD.Colors.size(), LOD.Positions.size());
	for (const FVector4f& Color : LOD.Colors)
		ExpectVector(Color, FVector4f(1.0f));

	FStaticMeshPayloadData ConvertedBack;
	ASSERT_TRUE(MakeStaticMeshPayloadData(*RenderData, ConvertedBack, Error)) << Error;
	ExpectEquivalent(ConvertedBack, Fixture);
}

TEST(FStaticMeshPayloadCodecTests,
	CurrentVertexInputAndSectionDrawContractIsPinned)
{
	const FVertexDeclarationElementList Elements =
		GetStaticMeshVertexDeclarationElements();
	constexpr uint16 PositionStride = sizeof(FVector3f);
	constexpr uint16 AttributeStride = sizeof(FStaticMeshPackedVertex);
	const std::array Expected{
		FVertexElement(
			0, 0, EVertexElementType::Float3, 0, PositionStride),
		FVertexElement(
			1, offsetof(FStaticMeshPackedVertex, Normal),
			EVertexElementType::Short4N, 1, AttributeStride),
		FVertexElement(
			1, offsetof(FStaticMeshPackedVertex, Tangent),
			EVertexElementType::Short4N, 2, AttributeStride),
		FVertexElement(
			1, offsetof(FStaticMeshPackedVertex, TexCoords),
			EVertexElementType::Float2, 3, AttributeStride),
		FVertexElement(
			1, offsetof(FStaticMeshPackedVertex, TexCoords)
				+ sizeof(FVector2f),
			EVertexElementType::Float2, 4, AttributeStride),
		FVertexElement(
			1, offsetof(FStaticMeshPackedVertex, TexCoords)
				+ sizeof(FVector2f) * 2,
			EVertexElementType::Float2, 5, AttributeStride),
		FVertexElement(
			1, offsetof(FStaticMeshPackedVertex, TexCoords)
				+ sizeof(FVector2f) * 3,
			EVertexElementType::Float2, 6, AttributeStride),
		FVertexElement(
			1, offsetof(FStaticMeshPackedVertex, Color),
			EVertexElementType::UByte4N, 7, AttributeStride)};
	for (size_t Index = 0; Index < Expected.size(); ++Index)
		EXPECT_EQ(Elements[Index], Expected[Index]);
	for (size_t Index = Expected.size(); Index < Elements.size(); ++Index)
		EXPECT_EQ(Elements[Index].Type, EVertexElementType::None);

	using FIndexElement =
		typename decltype(FStaticMeshLODResources::Indices)::value_type;
	EXPECT_EQ(sizeof(FIndexElement), 4u);

	const FStaticMeshPayloadData Fixture = MakeMultiMaterialFixture();
	ASSERT_EQ(Fixture.LODs[0].Sections.size(), 2u);
	EXPECT_EQ(Fixture.LODs[0].Sections[0].FirstIndex, 0u);
	EXPECT_EQ(Fixture.LODs[0].Sections[0].IndexCount, 3u);
	EXPECT_EQ(Fixture.LODs[0].Sections[1].FirstIndex, 3u);
	EXPECT_EQ(Fixture.LODs[0].Sections[1].IndexCount, 3u);

	std::string ShaderSource;
	const std::filesystem::path ShaderPath =
		std::filesystem::path(FPaths::EngineDir())
		/ "Shaders/Slang/StaticMesh.slang";
	ASSERT_TRUE(FFileHelper::LoadFileToString(
		ShaderSource, ShaderPath.generic_string()));
	size_t Previous = 0;
	for (const std::string_view Input : {
		"float3 pos : POSITION;",
		"float4 normal : NORMAL;",
		"float4 tangent : TANGENT;",
		"float2 uv0 : TEXCOORD0;",
		"float2 uv1 : TEXCOORD1;",
		"float2 uv2 : TEXCOORD2;",
		"float2 uv3 : TEXCOORD3;",
		"float4 color : COLOR;"})
	{
		const size_t Position = ShaderSource.find(Input, Previous);
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

	const FBufferRHIRef PositionBuffer = MakeTestBuffer(
		FRHIBufferCreateDesc::CreateVertex(
			"StaticMeshReadinessPosition",
			static_cast<uint32>(
				LOD.Positions.size() * sizeof(FVector3f))));
	const FBufferRHIRef AttributeBuffer = MakeTestBuffer(
		FRHIBufferCreateDesc::CreateVertex(
			"StaticMeshReadinessAttributes",
			static_cast<uint32>(
				LOD.Positions.size()
				* sizeof(FStaticMeshPackedVertex))));
	const FBufferRHIRef IndexBuffer = MakeTestBuffer(
		FRHIBufferCreateDesc::CreateIndex(
			"StaticMeshReadinessIndices",
			static_cast<uint32>(
				LOD.Indices.size() * sizeof(uint32)),
			sizeof(uint32)));

	LOD.PositionVertexBufferRHI = PositionBuffer;
	LOD.StaticMeshVertexBufferRHI = AttributeBuffer;
	EXPECT_FALSE(RenderData->IsReadyForRendering());

	LOD.IndexBufferRHI = IndexBuffer;
	EXPECT_TRUE(RenderData->IsReadyForRendering());

	const std::vector<FVector3f> SavedNormals =
		std::exchange(LOD.Normals, {});
	EXPECT_FALSE(RenderData->IsReadyForRendering());
	LOD.Normals = SavedNormals;
	EXPECT_TRUE(RenderData->IsReadyForRendering());

	RenderData->ReleaseResources();
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
	Mutate([](auto& Bytes) { WriteU32(Bytes, 4, 3); });
	Mutate([](auto& Bytes) { WriteU32(Bytes, 8, 2); });
	Mutate([](auto& Bytes) { WriteU32(Bytes, 12, 0); });
	Mutate([](auto& Bytes) { WriteU32(Bytes, 16, 2); });
	Mutate([](auto& Bytes) { WriteU32(Bytes, 28, 1); });
	Mutate([](auto& Bytes) { WriteU64(Bytes, 64 + 8, std::numeric_limits<uint64>::max() - 8); });
	Mutate([](auto& Bytes) { WriteU64(Bytes, 64 + 32 + 8, ReadU64(Bytes, 64 + 8)); });
	Mutate([](auto& Bytes) { WriteU64(Bytes, 64 + 8, ReadU64(Bytes, 64 + 8) + 1); });
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

TEST(FStaticMeshPayloadCodecTests, SkipsUnknownOptionalChunksAndRejectsUnknownRequiredChunks)
{
	const std::vector<uint8> Valid = Encode(MakeSingleSectionFixture());
	const std::vector<uint8> Optional = AddUnknownOptionalChunk(Valid, false);
	FStaticMeshPayloadData Decoded;
	std::string Error;
	ASSERT_TRUE(DecodeStaticMeshPayload(Optional, EStaticMeshTargetPlatform::Win64, Decoded, Error)) << Error;
	ExpectEquivalent(Decoded, MakeSingleSectionFixture());

	ExpectDecodeFailure(AddUnknownOptionalChunk(Valid, true));
}

TEST(FStaticMeshPayloadCodecTests, EncoderRejectsInvalidLogicalDataWithoutPublishingBytes)
{
	FStaticMeshPayloadData Invalid = MakeSingleSectionFixture();
	Invalid.LODs[0].Positions[0].x = std::numeric_limits<float>::quiet_NaN();
	std::vector<uint8> Bytes{1, 2, 3};
	std::string Error;
	EXPECT_FALSE(EncodeStaticMeshPayload(Invalid, EStaticMeshTargetPlatform::Win64, Bytes, Error));
	EXPECT_EQ(Bytes, (std::vector<uint8>{1, 2, 3}));
	EXPECT_FALSE(Error.empty());
	EXPECT_FALSE(EncodeStaticMeshPayload(
		MakeSingleSectionFixture(), static_cast<EStaticMeshTargetPlatform>(2), Bytes, Error));
}
