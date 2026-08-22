#include <gtest/gtest.h>

#include "Animation/AnimationClip.h"
#include "Actors/SkeletalMeshActor.h"
#include "AssetTools.h"
#include "Components/SkeletalMeshComponent.h"
#include "DObject/Package.h"
#include "Engine/Level.h"
#include "EngineTestSupport.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleTestSupport.h"
#include "NativeTestSupport.h"
#include "Serialization/Archive.h"
#include "Skeletal/SkeletalBuildOperations.h"
#include "SkeletalMesh/SkeletalAssetPostLoad.h"
#include "SkeletalMesh/SkeletalDerivedData.h"
#include "SkeletalMesh/SkeletalMesh.h"
#include "SkeletalMesh/SkeletalMeshResources.h"

namespace
{
	class FTestSkeletalDerivedDataFeature final : public Durin::ISkeletalDerivedDataFeature
	{
	public:
		auto LoadSkeletalMeshPayload(
			std::string_view Key,
			const Durin::FSkeletalPayloadSerializationContext& Context,
			Durin::FSkeletalMeshPayloadData& OutPayload,
			std::string& OutMessage) -> bool override
		{
			return Durin::Asset::Build::LoadSkeletalMeshDerivedData(Key, Context, OutPayload, OutMessage);
		}

		auto LoadAnimationClipPayload(
			std::string_view Key,
			const Durin::FSkeletalPayloadSerializationContext& Context,
			Durin::FAnimationClipPayloadData& OutPayload,
			std::string& OutMessage) -> bool override
		{
			return Durin::Asset::Build::LoadAnimationClipDerivedData(Key, Context, OutPayload, OutMessage);
		}
	};

	auto SerializeMeshPayload(
		const Durin::FSkeletalMeshPayloadData& Payload,
		const Durin::DSkeleton& Skeleton,
		Durin::uint32 MaterialSlotCount,
		std::vector<Durin::uint8>& OutBytes,
		std::string& OutError) -> bool
	{
		OutBytes.clear();
		Durin::FCanonicalMemoryWriter Ar(
			OutBytes, Durin::EArchivePurpose::DerivedDataPayload);
		const_cast<Durin::FSkeletalMeshPayloadData&>(Payload).Serialize(Ar, {
			.SkeletonBoneCount = Skeleton.GetBoneCount(),
			.MaterialSlotCount = MaterialSlotCount,
			.TargetPlatform = Durin::ESkeletalPayloadTargetPlatform::Win64,
			.TargetProfile = Durin::ESkeletalPayloadTargetProfile::Game});
		OutError = Ar.HasError() ? Ar.GetFailure()->Message : std::string{};
		return !Ar.HasError();
	}

	auto SerializeClipPayload(
		const Durin::FAnimationClipPayloadData& Payload,
		const Durin::DSkeleton& Skeleton,
		std::vector<Durin::uint8>& OutBytes,
		std::string& OutError) -> bool
	{
		OutBytes.clear();
		Durin::FCanonicalMemoryWriter Ar(
			OutBytes, Durin::EArchivePurpose::DerivedDataPayload);
		const_cast<Durin::FAnimationClipPayloadData&>(Payload).Serialize(Ar, {
			.SkeletonBoneCount = Skeleton.GetBoneCount(),
			.TargetPlatform = Durin::ESkeletalPayloadTargetPlatform::Win64,
			.TargetProfile = Durin::ESkeletalPayloadTargetProfile::Game});
		OutError = Ar.HasError() ? Ar.GetFailure()->Message : std::string{};
		return !Ar.HasError();
	}

	template<typename T>
	auto DeserializePayload(
		std::span<const Durin::uint8> Bytes,
		const Durin::FSkeletalPayloadSerializationContext& Context,
		T& OutPayload) -> bool
	{
		Durin::FCanonicalMemoryReader Ar(
			Bytes, Durin::EArchivePurpose::DerivedDataPayload);
		OutPayload.Serialize(Ar, Context);
		return !Ar.HasError();
	}

	auto MakeTransform(
		Durin::FVector4 Row0 = Durin::FVector4(1.0, 0.0, 0.0, 0.0),
		Durin::FVector4 Row1 = Durin::FVector4(0.0, 1.0, 0.0, 0.0),
		Durin::FVector4 Row2 = Durin::FVector4(0.0, 0.0, 1.0, 0.0),
		Durin::FVector4 Row3 = Durin::FVector4(0.0, 0.0, 0.0, 1.0)) -> Durin::FSkeletonTransform
	{
		return {.Row0 = Row0, .Row1 = Row1, .Row2 = Row2, .Row3 = Row3};
	}

	auto MakeContractBones() -> std::vector<Durin::FSkeletonBone>
	{
		return {
			{.Name = Durin::FName("$DurinRoot"), .ParentIndex = -1,
				.ReferenceTransform = MakeTransform()},
			{.Name = Durin::FName("Hip"), .ParentIndex = 0,
				.ReferenceTransform = MakeTransform(
					{0.7071068, 0.7071068, 0.0, 0.0},
					{-0.7071068, 0.7071068, 0.0, 0.0},
					{0.0, 0.0, 1.0, 1.0})},
			{.Name = Durin::FName("Knee"), .ParentIndex = 1,
				.ReferenceTransform = MakeTransform(
					{1.0, 0.0, 0.0, 0.0},
					{0.0, 0.0, -1.0, 0.0},
					{0.0, 1.0, 0.0, 1.0})},
			{.Name = Durin::FName("Shoulder"), .ParentIndex = 0,
				.ReferenceTransform = MakeTransform(
					{0.7071068, 0.7071068, 0.0, -0.7071068},
					{-0.7071068, 0.7071068, 0.0, 0.7071068},
					{0.0, 0.0, 2.0, 0.0})},
			{.Name = Durin::FName("Hand"), .ParentIndex = 3,
				.ReferenceTransform = MakeTransform(
					{1.0, 0.0, 0.0, 0.0},
					{0.0, 1.0, 0.0, 1.0})},
		};
	}

	auto MakeAlternateBones() -> std::vector<Durin::FSkeletonBone>
	{
		auto Bones = MakeContractBones();
		Bones[4].ReferenceTransform.Row1.w = 2.0;
		return Bones;
	}

	auto InitializeSkeleton(
		Durin::DSkeleton& Skeleton,
		std::vector<Durin::FSkeletonBone> Bones = MakeContractBones()) -> void
	{
		std::string Error;
		ASSERT_TRUE(Skeleton.InitializeCanonicalBones(std::move(Bones), Error)) << Error;
	}

	auto MakeMeshPayload(Durin::uint16 BoneIndex = 1) -> std::shared_ptr<const Durin::FSkeletalMeshPayloadData>
	{
		auto Payload = std::make_shared<Durin::FSkeletalMeshPayloadData>();
		Payload->Positions = {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}};
		Payload->Normals.assign(3, Durin::FVector3f(0.0f, 0.0f, 1.0f));
		Payload->Tangents.assign(3, Durin::FVector4f(1.0f, 0.0f, 0.0f, 1.0f));
		Payload->UVChannels[0] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}};
		Payload->Indices = {0, 1, 2};
		Durin::FSkeletalMeshVertexInfluences Influence;
		Influence.BoneIndices[0] = BoneIndex;
		Influence.Weights[0] = 1.0f;
		Influence.Count = 1;
		Payload->Influences.assign(3, Influence);
		Payload->LocalBounds = Durin::FBox(Durin::FVector3(0.0), Durin::FVector3(1.0, 1.0, 0.0));
		Payload->Sections.push_back({
			.Name = Durin::FName("Body"),
			.FirstIndex = 0,
			.IndexCount = 3,
			.MinVertexIndex = 0,
			.MaxVertexIndex = 2,
			.MaterialSlotIndex = 0,
			.LocalBounds = Payload->LocalBounds});
		Payload->PaletteBoneIndices = {BoneIndex};
		Payload->InverseBindMatrices = {Durin::FMatrix4f(1.0f)};
		return Payload;
	}

	auto MakeCookedDescriptor(Durin::uint32 Seed) -> Durin::Asset::FCookedPayloadDescriptor
	{
		return {
			.PayloadId = Durin::FGuid(Seed, Seed + 1, Seed + 2, Seed + 3),
			.LocationKind = Seed + 4,
			.Offset = Seed + 5,
			.StoredSize = Seed + 6,
			.UncompressedSize = Seed + 7,
			.Alignment = Seed + 8,
			.PayloadHashLow = Seed + 9,
			.PayloadHashHigh = Seed + 10,
			.PayloadSchemaVersion = Seed + 11,
			.TargetPlatform = Seed + 12,
			.TargetProfile = Seed + 13,
			.CompressionMethod = Seed + 14};
	}

	auto InitializeMesh(
		Durin::DSkeletalMesh& Mesh,
		Durin::DSkeleton& Skeleton,
		std::string_view SlotName = "Body",
		std::string DerivedDataKey = {}) -> void
	{
		Durin::FSkeletalMeshPublicationCandidate Data{
			.Skeleton = &Skeleton,
			.SkeletonCompatibilityIdentity = Skeleton.GetCompatibilityIdentity(),
			.MeshNodeBindTransform = MakeTransform(),
			.MaterialSlots = {{
				.Name = Durin::FName(SlotName),
				.SourceName = std::string(SlotName),
				.SourceMaterialIndex = 0}},
			.Payload = MakeMeshPayload(),
			.CookedPayload = MakeCookedDescriptor(100),
			.DerivedDataKey = DerivedDataKey};
		std::string Error;
		if (!DerivedDataKey.empty())
		{
			Durin::Asset::Build::FSkeletalMeshBuildProduct Product;
			Durin::Asset::Build::FSkeletalMeshBuildKeyInput KeyInput;
			auto& Fields = static_cast<Durin::Asset::Build::FSkeletalBuildKeyFields&>(KeyInput);
			Fields.ProviderIdentity = "Durin.Tests";
			Fields.ProviderVersion = 1;
			Fields.SourceClosureHash = Durin::FXxHash128::HashBuffer(DerivedDataKey);
			Fields.SettingsHash = Durin::FXxHash128::HashBuffer("settings");
			Fields.ProviderStateHash = Durin::FXxHash128::HashBuffer("state");
			Fields.StableOutputIdentity = DerivedDataKey;
			Fields.SkeletonCompatibilityIdentity = Skeleton.GetCompatibilityIdentity();
			Fields.TargetPlatform = Durin::ESkeletalPayloadTargetPlatform::Win64;
			Fields.TargetProfile = Durin::ESkeletalPayloadTargetProfile::Game;
			const bool bBuilt = Durin::Asset::Build::BuildSkeletalMeshProduct({
				.SkeletonBoneCount = Skeleton.GetBoneCount(),
				.SkeletonCompatibilityIdentity = Skeleton.GetCompatibilityIdentity(),
				.MeshNodeBindTransform = Data.MeshNodeBindTransform,
				.MaterialSlotCount = static_cast<Durin::uint32>(Data.MaterialSlots.size()),
				.Payload = Data.Payload,
				.KeyInput = std::move(KeyInput)}, Product, Error);
			if (bBuilt)
			{
				Data.Payload = std::move(Product.Payload);
				Data.DerivedDataKey = std::move(Product.DerivedDataKey);
				Data.DiagnosticMessage = std::move(Product.Diagnostic);
			}
			else Data.DiagnosticMessage = std::format("SkeletalMesh DDC write failed: {}", Error);
		}
		ASSERT_TRUE(Mesh.PublishBuiltProduct(std::move(Data), Error)) << Error;
	}

	auto MakeClipPayload(float EndValue = 1.0f) -> std::shared_ptr<const Durin::FAnimationClipPayloadData>
	{
		auto Payload = std::make_shared<Durin::FAnimationClipPayloadData>();
		Payload->DurationSeconds = 1.0f;
		Payload->Tracks = {
			{
				.BoneIndex = 1,
				.Path = Durin::EAnimationTrackPath::Translation,
				.Interpolation = Durin::EAnimationInterpolation::Linear,
				.Times = {0.0f, 1.0f},
				.VectorValues = {{0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, EndValue}}},
			{
				.BoneIndex = 2,
				.Path = Durin::EAnimationTrackPath::Rotation,
				.Interpolation = Durin::EAnimationInterpolation::Step,
				.Times = {0.0f, 1.0f},
				.RotationValues = {{0.0f, 0.0f, 0.0f, 1.0f},
					{0.70710677f, 0.0f, 0.0f, 0.70710677f}}},
		};
		return Payload;
	}

	auto InitializeClip(
		Durin::DAnimationClip& Clip,
		Durin::DSkeleton& Skeleton,
		std::string_view Name = "Walk",
		float EndValue = 1.0f,
		std::string DerivedDataKey = {}) -> void
	{
		Durin::FAnimationClipPublicationCandidate Data{
			.Skeleton = &Skeleton,
			.SkeletonCompatibilityIdentity = Skeleton.GetCompatibilityIdentity(),
			.ClipName = Durin::FName(Name),
			.Payload = MakeClipPayload(EndValue),
			.CookedPayload = MakeCookedDescriptor(200),
			.DerivedDataKey = DerivedDataKey};
		std::string Error;
		if (!DerivedDataKey.empty())
		{
			Durin::Asset::Build::FAnimationClipBuildProduct Product;
			Durin::Asset::Build::FAnimationClipBuildKeyInput KeyInput;
			auto& Fields = static_cast<Durin::Asset::Build::FSkeletalBuildKeyFields&>(KeyInput);
			Fields.ProviderIdentity = "Durin.Tests";
			Fields.ProviderVersion = 1;
			Fields.SourceClosureHash = Durin::FXxHash128::HashBuffer(DerivedDataKey);
			Fields.SettingsHash = Durin::FXxHash128::HashBuffer("settings");
			Fields.ProviderStateHash = Durin::FXxHash128::HashBuffer("state");
			Fields.StableOutputIdentity = DerivedDataKey;
			Fields.SkeletonCompatibilityIdentity = Skeleton.GetCompatibilityIdentity();
			Fields.TargetPlatform = Durin::ESkeletalPayloadTargetPlatform::Win64;
			Fields.TargetProfile = Durin::ESkeletalPayloadTargetProfile::Game;
			const bool bBuilt = Durin::Asset::Build::BuildAnimationClipProduct({
				.SkeletonBoneCount = Skeleton.GetBoneCount(),
				.SkeletonCompatibilityIdentity = Skeleton.GetCompatibilityIdentity(),
				.ClipName = Data.ClipName,
				.Payload = Data.Payload,
				.KeyInput = std::move(KeyInput)}, Product, Error);
			if (bBuilt)
			{
				Data.Payload = std::move(Product.Payload);
				Data.DerivedDataKey = std::move(Product.DerivedDataKey);
				Data.DiagnosticMessage = std::move(Product.Diagnostic);
			}
			else Data.DiagnosticMessage = std::format("AnimationClip DDC write failed: {}", Error);
		}
		ASSERT_TRUE(Clip.PublishBuiltProduct(std::move(Data), Error)) << Error;
	}

	auto InitializeAssetMount() -> std::filesystem::path
	{
		InitializeDObjectSystem();
		static Durin::FModuleTestOwner Context("SkeletalAssetTests");
		static FTestSkeletalDerivedDataFeature Feature;
		static auto Registration = Context.RegisterFeature<Durin::ISkeletalDerivedDataFeature>(Feature);
		check(Registration.IsValid());
		const std::filesystem::path Root =
			Durin::Testing::GetTestWorkDirectory() / "SkeletalAssets";
		Durin::Testing::RemoveTestWorkDirectory(Root);
		Durin::PathUtilities::RegisterMountPointForTests(
			"/SkeletalAssetTests/", Root.generic_string() + "/");
		return Root;
	}

	auto ReadWireU64(const std::vector<Durin::uint8>& Bytes, size_t Offset) -> Durin::uint64
	{
		Durin::uint64 Value = 0;
		for (Durin::uint32 Byte = 0; Byte < 8; ++Byte)
			Value |= static_cast<Durin::uint64>(Bytes[Offset + Byte]) << (Byte * 8);
		return Value;
	}

	auto WriteWireU32(
		std::vector<Durin::uint8>& Bytes,
		size_t Offset,
		Durin::uint32 Value) -> void
	{
		for (Durin::uint32 Byte = 0; Byte < 4; ++Byte)
			Bytes[Offset + Byte] = static_cast<Durin::uint8>(Value >> (Byte * 8));
	}

	auto WriteWireU64(
		std::vector<Durin::uint8>& Bytes,
		size_t Offset,
		Durin::uint64 Value) -> void
	{
		for (Durin::uint32 Byte = 0; Byte < 8; ++Byte)
			Bytes[Offset + Byte] = static_cast<Durin::uint8>(Value >> (Byte * 8));
	}

	auto RefreshSkeletalPayloadHash(std::vector<Durin::uint8>& Bytes) -> void
	{
		const Durin::uint64 Hash = Durin::FXxHash64::HashBuffer(
			std::span<const Durin::uint8>(Bytes).subspan(
				Durin::SkeletalPayloadHeaderSize)).HashValue;
		WriteWireU64(Bytes, 56, Hash);
	}

	auto ContainsText(
		std::span<const Durin::uint8> Bytes,
		std::string_view Text) -> bool
	{
		return std::search(
			Bytes.begin(), Bytes.end(),
			Text.begin(), Text.end(),
			[](Durin::uint8 Byte, char Character) {
				return Byte == static_cast<Durin::uint8>(Character);
			}) != Bytes.end();
	}

	auto RestartAssetManager(const std::filesystem::path& CookRoot = {}) -> void
	{
		Durin::Asset::ShutdownAssetManager();
		Durin::CollectGarbage();
		if (CookRoot.empty())
		{
			ASSERT_TRUE(Durin::Asset::InitializeAssetManager());
			return;
		}
		auto Configuration = Durin::Asset::FAssetRuntimeConfiguration::Authored();
		ASSERT_TRUE(Durin::Asset::FAssetRuntimeConfiguration::Cooked(
			CookRoot, Configuration));
		ASSERT_TRUE(Durin::Asset::InitializeAssetManager(std::move(Configuration)));
	}
}

TEST(FSkeletalRenderDataTests, ConvertsPayloadAndBuildsDedicatedVertexFactoryContract)
{
	InitializeDObjectSystem();
	auto* Skeleton = Durin::NewObject<Durin::DSkeleton>(nullptr, "RenderDataSkeleton");
	InitializeSkeleton(*Skeleton);
	auto* Mesh = Durin::NewObject<Durin::DSkeletalMesh>(nullptr, "RenderDataMesh");
	InitializeMesh(*Mesh, *Skeleton);
	const Durin::FSkeletalMeshRenderData* RenderData = Mesh->GetRenderData();
	ASSERT_NE(RenderData, nullptr);
	EXPECT_EQ(RenderData->LODIndex, 0u);
	EXPECT_EQ(RenderData->VertexBuffers.Geometry.PositionVertexBuffer.GetPositions(),
		Mesh->GetPayloadData()->Positions);
	EXPECT_EQ(RenderData->VertexBuffers.InfluenceVertexBuffer.GetInfluences(),
		Mesh->GetPayloadData()->Influences);
	EXPECT_EQ(RenderData->IndexBuffer.GetIndices(), Mesh->GetPayloadData()->Indices);
	ASSERT_EQ(RenderData->Sections.size(), 1u);
	EXPECT_EQ(RenderData->Sections[0].MaterialSlotIndex, 0u);
	ASSERT_EQ(RenderData->InfluenceBounds.size(), 1u);
	EXPECT_TRUE(RenderData->InfluenceBounds[0].bIsValid);
	EXPECT_EQ(RenderData->InfluenceBounds[0].Min, Durin::FVector3(0.0));
	EXPECT_EQ(RenderData->InfluenceBounds[0].Max, Durin::FVector3(1.0, 1.0, 0.0));

	Durin::FSkeletalMeshVertexFactory VertexFactory;
	ASSERT_TRUE(VertexFactory.SetData(RenderData->VertexBuffers));
	const Durin::FVertexDeclarationElementList Elements =
		VertexFactory.GetDeclarationElements();
	EXPECT_EQ(Elements[8].AttributeIndex, 8u);
	EXPECT_EQ(Elements[8].Type, Durin::EVertexElementType::UShort4);
	EXPECT_EQ(Elements[9].AttributeIndex, 9u);
	EXPECT_EQ(Elements[9].Type, Durin::EVertexElementType::Float4);
	EXPECT_EQ(Elements[8].StreamIndex, Elements[9].StreamIndex);
	EXPECT_EQ(VertexFactory.GetTypeName(), "FSkeletalMeshVertexFactory");
}

TEST(FSkeletalRenderDataTests, FailedReplacementKeepsCompleteRenderData)
{
	InitializeDObjectSystem();
	auto* Skeleton = Durin::NewObject<Durin::DSkeleton>(nullptr, "RenderFailureSkeleton");
	InitializeSkeleton(*Skeleton);
	auto* Mesh = Durin::NewObject<Durin::DSkeletalMesh>(nullptr, "RenderFailureMesh");
	InitializeMesh(*Mesh, *Skeleton);
	const Durin::FSkeletalMeshRenderData* Previous = Mesh->GetRenderData();
	ASSERT_NE(Previous, nullptr);
	auto InvalidPayload = std::make_shared<Durin::FSkeletalMeshPayloadData>(*MakeMeshPayload());
	InvalidPayload->Influences[0].Weights[0] = 0.5f;
	std::string Error;
	EXPECT_FALSE(Mesh->PublishBuiltProduct({
		.Skeleton = Skeleton,
		.SkeletonCompatibilityIdentity = Skeleton->GetCompatibilityIdentity(),
		.MeshNodeBindTransform = MakeTransform(),
		.MaterialSlots = {{.Name = Durin::FName("Body"), .SourceMaterialIndex = 0}},
		.Payload = std::move(InvalidPayload)}, Error));
	EXPECT_EQ(Mesh->GetRenderData(), Previous);
	EXPECT_EQ(Mesh->GetRenderData()->IndexBuffer.GetIndices(),
		std::vector<Durin::uint32>({0, 1, 2}));
}

TEST(FSkeletalAssetTests, SkeletonCompatibilityMatchesFixtureEncoding)
{
	InitializeDObjectSystem();
	auto* Skeleton = Durin::NewObject<Durin::DSkeleton>(nullptr, "ContractSkeleton");
	InitializeSkeleton(*Skeleton);
	EXPECT_EQ(
		Skeleton->GetCompatibilityIdentity(),
		"be0f679ef83133e5acfab7f12b688f54");
	std::string Error;
	EXPECT_TRUE(Skeleton->Validate(Error)) << Error;
	auto SignedZeroBones = MakeContractBones();
	for (Durin::FSkeletonBone& Bone : SignedZeroBones)
		Bone.ReferenceTransform.CanonicalizeFloat32();
	SignedZeroBones[0].ReferenceTransform.Row0.y = -0.0;
	std::string SignedZeroIdentity;
	ASSERT_TRUE(Durin::DSkeleton::ComputeCompatibilityIdentity(
		SignedZeroBones, SignedZeroIdentity, Error)) << Error;
	EXPECT_EQ(SignedZeroIdentity, Skeleton->GetCompatibilityIdentity());

	auto Invalid = MakeContractBones();
	Invalid[1].ParentIndex = 2;
	const std::vector<Durin::FSkeletonBone> Before(
		Skeleton->GetBones().begin(), Skeleton->GetBones().end());
	EXPECT_FALSE(Skeleton->InitializeCanonicalBones(std::move(Invalid), Error));
	EXPECT_FALSE(Error.empty());
	EXPECT_TRUE(std::ranges::equal(Skeleton->GetBones(), Before));
}

TEST(FSkeletalAssetTests, PayloadValidationRejectsMalformedGraphWithoutMutation)
{
	InitializeDObjectSystem();
	auto* Skeleton = Durin::NewObject<Durin::DSkeleton>(nullptr, "PayloadSkeleton");
	InitializeSkeleton(*Skeleton);
	auto* Mesh = Durin::NewObject<Durin::DSkeletalMesh>(nullptr, "PayloadMesh");
	InitializeMesh(*Mesh, *Skeleton);
	const Durin::FSkeletalMeshSummary MeshSummary = Mesh->GetSummary();

	auto InvalidPayload = std::make_shared<Durin::FSkeletalMeshPayloadData>(*MakeMeshPayload());
	InvalidPayload->Influences[0].Weights[0] = 0.5f;
	Durin::FSkeletalMeshPublicationCandidate InvalidMesh{
		.Skeleton = Skeleton,
		.SkeletonCompatibilityIdentity = Skeleton->GetCompatibilityIdentity(),
		.MeshNodeBindTransform = MakeTransform(),
		.MaterialSlots = {{.Name = Durin::FName("Body"), .SourceMaterialIndex = 0}},
		.Payload = InvalidPayload};
	std::string Error;
	EXPECT_FALSE(Mesh->PublishBuiltProduct(std::move(InvalidMesh), Error));
	EXPECT_EQ(Mesh->GetSummary(), MeshSummary);

	auto* Clip = Durin::NewObject<Durin::DAnimationClip>(nullptr, "PayloadClip");
	InitializeClip(*Clip, *Skeleton);
	const Durin::FAnimationClipSummary ClipSummary = Clip->GetSummary();
	auto InvalidClipPayload = std::make_shared<Durin::FAnimationClipPayloadData>(*MakeClipPayload());
	InvalidClipPayload->Tracks[0].Times[1] = 0.0f;
	Durin::FAnimationClipPublicationCandidate InvalidClip{
		.Skeleton = Skeleton,
		.SkeletonCompatibilityIdentity = Skeleton->GetCompatibilityIdentity(),
		.ClipName = Durin::FName("Invalid"),
		.Payload = InvalidClipPayload};
	EXPECT_FALSE(Clip->PublishBuiltProduct(std::move(InvalidClip), Error));
	EXPECT_EQ(Clip->GetSummary(), ClipSummary);
}

TEST(FSkeletalAssetTests, PayloadValidationRequiresBoundsToContainGeometry)
{
	InitializeDObjectSystem();
	auto* Skeleton = Durin::NewObject<Durin::DSkeleton>(nullptr, "BoundsSkeleton");
	InitializeSkeleton(*Skeleton);
	std::string Error;

	auto ExpectRejected = [&](const Durin::FSkeletalMeshPayloadData& Payload) {
		EXPECT_FALSE(Durin::ValidateSkeletalMeshPayload(Payload, *Skeleton, 1, Error));
		EXPECT_NE(Error.find("bounds"), std::string::npos) << Error;
	};

	auto TooSmallPayloadBounds = *MakeMeshPayload();
	TooSmallPayloadBounds.LocalBounds.Max.x = 0.5;
	ExpectRejected(TooSmallPayloadBounds);

	auto OffsetPayloadBounds = *MakeMeshPayload();
	OffsetPayloadBounds.LocalBounds.Min.y = 0.5;
	ExpectRejected(OffsetPayloadBounds);

	auto TooSmallSectionBounds = *MakeMeshPayload();
	TooSmallSectionBounds.Sections[0].LocalBounds.Max.y = 0.5;
	ExpectRejected(TooSmallSectionBounds);

	auto OffsetSectionBounds = *MakeMeshPayload();
	OffsetSectionBounds.Sections[0].LocalBounds.Min.x = 0.5;
	ExpectRejected(OffsetSectionBounds);

	auto ConservativeBounds = *MakeMeshPayload();
	ConservativeBounds.LocalBounds = Durin::FBox(Durin::FVector3(-1.0), Durin::FVector3(2.0));
	ConservativeBounds.Sections[0].LocalBounds = ConservativeBounds.LocalBounds;
	EXPECT_TRUE(Durin::ValidateSkeletalMeshPayload(ConservativeBounds, *Skeleton, 1, Error)) << Error;
}

TEST(FSkeletalAssetTests, ImportedStateExchangeCommitsReversesAndRejectsInvalidCandidates)
{
	InitializeDObjectSystem();
	auto* FirstSkeleton = Durin::NewObject<Durin::DSkeleton>(nullptr, "FirstSkeleton");
	auto* SecondSkeleton = Durin::NewObject<Durin::DSkeleton>(nullptr, "SecondSkeleton");
	InitializeSkeleton(*FirstSkeleton);
	InitializeSkeleton(*SecondSkeleton, MakeAlternateBones());
	const std::string FirstIdentity = FirstSkeleton->GetCompatibilityIdentity();
	const std::string SecondIdentity = SecondSkeleton->GetCompatibilityIdentity();
	std::string Error;
	auto SkeletonExchange = FirstSkeleton->PrepareImportedStateExchange(*SecondSkeleton, Error);
	ASSERT_NE(SkeletonExchange, nullptr) << Error;
	SkeletonExchange->Commit();
	EXPECT_EQ(FirstSkeleton->GetCompatibilityIdentity(), SecondIdentity);
	SkeletonExchange->Reverse();
	EXPECT_EQ(FirstSkeleton->GetCompatibilityIdentity(), FirstIdentity);
	SkeletonExchange->Finalize();

	auto* FirstMesh = Durin::NewObject<Durin::DSkeletalMesh>(nullptr, "FirstMesh");
	auto* SecondMesh = Durin::NewObject<Durin::DSkeletalMesh>(nullptr, "SecondMesh");
	InitializeMesh(*FirstMesh, *FirstSkeleton, "Body");
	InitializeMesh(*SecondMesh, *SecondSkeleton, "Armor");
	auto MeshExchange = FirstMesh->PrepareImportedStateExchange(*SecondMesh, Error);
	ASSERT_NE(MeshExchange, nullptr) << Error;
	MeshExchange->Commit();
	EXPECT_EQ(FirstMesh->GetSkeleton(), SecondSkeleton);
	EXPECT_EQ(FirstMesh->GetMaterialSlots()[0].Name, Durin::FName("Armor"));
	MeshExchange->Reverse();
	EXPECT_EQ(FirstMesh->GetSkeleton(), FirstSkeleton);
	EXPECT_EQ(FirstMesh->GetMaterialSlots()[0].Name, Durin::FName("Body"));
	MeshExchange->Finalize();

	auto* FirstClip = Durin::NewObject<Durin::DAnimationClip>(nullptr, "FirstClip");
	auto* SecondClip = Durin::NewObject<Durin::DAnimationClip>(nullptr, "SecondClip");
	InitializeClip(*FirstClip, *FirstSkeleton, "Walk", 1.0f);
	InitializeClip(*SecondClip, *SecondSkeleton, "Run", 2.0f);
	auto ClipExchange = FirstClip->PrepareImportedStateExchange(*SecondClip, Error);
	ASSERT_NE(ClipExchange, nullptr) << Error;
	ClipExchange->Commit();
	EXPECT_EQ(FirstClip->GetClipName(), Durin::FName("Run"));
	ClipExchange->Reverse();
	EXPECT_EQ(FirstClip->GetClipName(), Durin::FName("Walk"));
	ClipExchange->Finalize();

	auto* InvalidCandidate = Durin::NewObject<Durin::DAnimationClip>(nullptr, "InvalidCandidate");
	EXPECT_EQ(FirstClip->PrepareImportedStateExchange(*InvalidCandidate, Error), nullptr);
	EXPECT_EQ(FirstClip->GetClipName(), Durin::FName("Walk"));
}

TEST(FSkeletalAssetTests, ProspectiveSkeletonValidatesAtomicDependentReplacement)
{
	InitializeDObjectSystem();
	auto* TargetSkeleton = Durin::NewObject<Durin::DSkeleton>(nullptr, "TargetSkeleton");
	auto* ProspectiveSkeleton = Durin::NewObject<Durin::DSkeleton>(nullptr, "ProspectiveSkeleton");
	InitializeSkeleton(*TargetSkeleton);
	InitializeSkeleton(*ProspectiveSkeleton, MakeAlternateBones());
	auto* TargetMesh = Durin::NewObject<Durin::DSkeletalMesh>(nullptr, "TargetMesh");
	auto* CandidateMesh = Durin::NewObject<Durin::DSkeletalMesh>(nullptr, "CandidateMesh");
	InitializeMesh(*TargetMesh, *TargetSkeleton);
	std::string Error;
	ASSERT_TRUE(CandidateMesh->PublishBuiltProduct({
		.Skeleton = TargetSkeleton,
		.ValidationSkeleton = ProspectiveSkeleton,
		.SkeletonCompatibilityIdentity = ProspectiveSkeleton->GetCompatibilityIdentity(),
		.MeshNodeBindTransform = MakeTransform(),
		.MaterialSlots = {{.Name = Durin::FName("Armor"), .SourceMaterialIndex = 0}},
		.Payload = MakeMeshPayload()}, Error)) << Error;
	EXPECT_FALSE(CandidateMesh->Validate(Error));
	Error.clear();
	EXPECT_TRUE(CandidateMesh->ValidateAgainstSkeleton(*ProspectiveSkeleton, Error)) << Error;

	auto* TargetClip = Durin::NewObject<Durin::DAnimationClip>(nullptr, "TargetClip");
	auto* CandidateClip = Durin::NewObject<Durin::DAnimationClip>(nullptr, "CandidateClip");
	InitializeClip(*TargetClip, *TargetSkeleton);
	ASSERT_TRUE(CandidateClip->PublishBuiltProduct({
		.Skeleton = TargetSkeleton,
		.ValidationSkeleton = ProspectiveSkeleton,
		.SkeletonCompatibilityIdentity = ProspectiveSkeleton->GetCompatibilityIdentity(),
		.ClipName = Durin::FName("Run"),
		.Payload = MakeClipPayload(2.0f)}, Error)) << Error;
	EXPECT_FALSE(CandidateClip->Validate(Error));
	Error.clear();
	EXPECT_TRUE(CandidateClip->ValidateAgainstSkeleton(*ProspectiveSkeleton, Error)) << Error;

	auto SkeletonExchange = TargetSkeleton->PrepareImportedStateExchange(
		*ProspectiveSkeleton, Error);
	auto MeshExchange = TargetMesh->PrepareImportedStateExchange(
		*CandidateMesh, *ProspectiveSkeleton, Error);
	auto ClipExchange = TargetClip->PrepareImportedStateExchange(
		*CandidateClip, *ProspectiveSkeleton, Error);
	ASSERT_NE(SkeletonExchange, nullptr) << Error;
	ASSERT_NE(MeshExchange, nullptr) << Error;
	ASSERT_NE(ClipExchange, nullptr) << Error;

	SkeletonExchange->Commit();
	MeshExchange->Commit();
	ClipExchange->Commit();
	EXPECT_EQ(TargetMesh->GetSkeleton(), TargetSkeleton);
	EXPECT_EQ(TargetClip->GetSkeleton(), TargetSkeleton);
	Error.clear();
	EXPECT_TRUE(TargetMesh->Validate(Error)) << Error;
	EXPECT_TRUE(TargetClip->Validate(Error)) << Error;

	ClipExchange->Reverse();
	MeshExchange->Reverse();
	SkeletonExchange->Reverse();
	Error.clear();
	EXPECT_TRUE(TargetMesh->Validate(Error)) << Error;
	EXPECT_TRUE(TargetClip->Validate(Error)) << Error;
	ClipExchange->Finalize();
	MeshExchange->Finalize();
	SkeletonExchange->Finalize();
}

TEST(FSkeletalAssetTests, PayloadCodecsAreDeterministicAndRoundTripExactValues)
{
	InitializeDObjectSystem();
	auto* Skeleton = Durin::NewObject<Durin::DSkeleton>(nullptr, "CodecSkeleton");
	InitializeSkeleton(*Skeleton);
	const std::shared_ptr<const Durin::FSkeletalMeshPayloadData> MeshPayload =
		MakeMeshPayload();
	const std::shared_ptr<const Durin::FAnimationClipPayloadData> ClipPayload =
		MakeClipPayload();
	std::vector<Durin::uint8> MeshBytes;
	std::vector<Durin::uint8> RepeatedMeshBytes;
	std::vector<Durin::uint8> ClipBytes;
	std::vector<Durin::uint8> RepeatedClipBytes;
	std::string Error;
	ASSERT_TRUE(SerializeMeshPayload(*MeshPayload, *Skeleton, 1, MeshBytes, Error)) << Error;
	ASSERT_TRUE(SerializeMeshPayload(*MeshPayload, *Skeleton, 1, RepeatedMeshBytes, Error)) << Error;
	ASSERT_TRUE(SerializeClipPayload(*ClipPayload, *Skeleton, ClipBytes, Error)) << Error;
	ASSERT_TRUE(SerializeClipPayload(*ClipPayload, *Skeleton, RepeatedClipBytes, Error)) << Error;
	EXPECT_EQ(MeshBytes, RepeatedMeshBytes);
	EXPECT_EQ(ClipBytes, RepeatedClipBytes);
	EXPECT_EQ(Durin::FXxHash128::HashBuffer(MeshBytes).ToString(),
		"fc7f61d6067225ce84e3c50ccce55c51");
	EXPECT_EQ(Durin::FXxHash128::HashBuffer(ClipBytes).ToString(),
		"7da58a36cd32f38a3dcb1daa910994f7");
	ASSERT_GE(MeshBytes.size(), Durin::SkeletalPayloadHeaderSize);
	ASSERT_GE(ClipBytes.size(), Durin::SkeletalPayloadHeaderSize);
	EXPECT_EQ(MeshBytes[0], 'D'); EXPECT_EQ(MeshBytes[1], 'S');
	EXPECT_EQ(MeshBytes[2], 'K'); EXPECT_EQ(MeshBytes[3], 'M');
	EXPECT_EQ(ClipBytes[0], 'D'); EXPECT_EQ(ClipBytes[1], 'A');
	EXPECT_EQ(ClipBytes[2], 'N'); EXPECT_EQ(ClipBytes[3], 'M');

	Durin::FSkeletalMeshPayloadData DecodedMesh;
	Durin::FAnimationClipPayloadData DecodedClip;
	ASSERT_TRUE(DeserializePayload(MeshBytes, {
		.SkeletonBoneCount = Skeleton->GetBoneCount(),
		.MaterialSlotCount = 1,
		.TargetPlatform = Durin::ESkeletalPayloadTargetPlatform::Win64,
		.TargetProfile = Durin::ESkeletalPayloadTargetProfile::Game}, DecodedMesh));
	ASSERT_TRUE(DeserializePayload(ClipBytes, {
		.SkeletonBoneCount = Skeleton->GetBoneCount(),
		.TargetPlatform = Durin::ESkeletalPayloadTargetPlatform::Win64,
		.TargetProfile = Durin::ESkeletalPayloadTargetProfile::Game}, DecodedClip));
	EXPECT_EQ(DecodedMesh, *MeshPayload);
	EXPECT_EQ(DecodedClip, *ClipPayload);
	auto DifferentProducer = MeshBytes;
	WriteWireU32(DifferentProducer, 8, 99);
	Durin::FSkeletalMeshPayloadData ProducerCompatible;
	EXPECT_TRUE(DeserializePayload(DifferentProducer, {
		.SkeletonBoneCount = Skeleton->GetBoneCount(),
		.MaterialSlotCount = 1,
		.TargetPlatform = Durin::ESkeletalPayloadTargetPlatform::Win64,
		.TargetProfile = Durin::ESkeletalPayloadTargetProfile::Game}, ProducerCompatible));
	EXPECT_EQ(ProducerCompatible, *MeshPayload);

	Durin::FXxHash128 MeshFingerprint;
	Durin::FXxHash128 ClipFingerprint;
	MeshFingerprint = Durin::FXxHash128::HashBuffer(MeshBytes);
	ClipFingerprint = Durin::FXxHash128::HashBuffer(ClipBytes);
	Durin::Asset::Build::FSkeletalMeshBuildKeyInput MeshKeyInput;
	auto& KeyInput = static_cast<Durin::Asset::Build::FSkeletalBuildKeyFields&>(MeshKeyInput);
	KeyInput.ProviderIdentity = "Durin.Scene";
	KeyInput.ProviderVersion = 3;
	KeyInput.SourceClosureHash = Durin::FXxHash128::HashBuffer("sources");
	KeyInput.SettingsHash = Durin::FXxHash128::HashBuffer("settings");
	KeyInput.ProviderStateHash = Durin::FXxHash128::HashBuffer("state");
	KeyInput.PayloadInputFingerprint = MeshFingerprint;
	KeyInput.StableOutputIdentity = "skeletal-mesh:node/1/mesh/0";
	KeyInput.SkeletonCompatibilityIdentity = Skeleton->GetCompatibilityIdentity();
	KeyInput.TargetPlatform = Durin::ESkeletalPayloadTargetPlatform::Win64;
	KeyInput.TargetProfile = Durin::ESkeletalPayloadTargetProfile::Game;
	const std::string MeshKey = Durin::Asset::Build::BuildSkeletalMeshDerivedDataKey(
		MeshKeyInput, Error);
	EXPECT_EQ(MeshKey, "d4a4365a271f98c048d49b3491170eb3");
	EXPECT_EQ(MeshKey.size(), 32u);
	EXPECT_EQ(Durin::Asset::Build::BuildSkeletalMeshDerivedDataKey(MeshKeyInput, Error), MeshKey);
	Durin::Asset::Build::FAnimationClipBuildKeyInput ClipKeyInput;
	static_cast<Durin::Asset::Build::FSkeletalBuildKeyFields&>(ClipKeyInput) = KeyInput;
	ClipKeyInput.PayloadInputFingerprint = ClipFingerprint;
	ClipKeyInput.StableOutputIdentity = "animation-clip:animation/0/skin/0";
	const std::string ClipKey = Durin::Asset::Build::BuildAnimationClipDerivedDataKey(
		ClipKeyInput, Error);
	EXPECT_EQ(ClipKey, "a8eb4f43273627152dd71bea93f6d2e9");
	EXPECT_EQ(ClipKey.size(), 32u);
	EXPECT_NE(ClipKey, MeshKey);
}

TEST(FSkeletalAssetTests, PayloadDecodersRejectMalformedContainersTransactionally)
{
	InitializeDObjectSystem();
	auto* Skeleton = Durin::NewObject<Durin::DSkeleton>(nullptr, "MalformedCodecSkeleton");
	InitializeSkeleton(*Skeleton);
	std::vector<Durin::uint8> MeshBytes;
	std::vector<Durin::uint8> ClipBytes;
	std::string Error;
	ASSERT_TRUE(SerializeMeshPayload(*MakeMeshPayload(), *Skeleton, 1, MeshBytes, Error)) << Error;
	ASSERT_TRUE(SerializeClipPayload(*MakeClipPayload(), *Skeleton, ClipBytes, Error)) << Error;
	const Durin::FSkeletalMeshPayloadData MeshSentinel = *MakeMeshPayload();
	const Durin::FAnimationClipPayloadData ClipSentinel = *MakeClipPayload();
	auto ExpectMeshFailure = [&](std::vector<Durin::uint8> Corrupt) {
		Durin::FSkeletalMeshPayloadData Output = MeshSentinel;
		EXPECT_FALSE(DeserializePayload(Corrupt, {
			.SkeletonBoneCount = Skeleton->GetBoneCount(),
			.MaterialSlotCount = 1,
			.TargetPlatform = Durin::ESkeletalPayloadTargetPlatform::Win64,
			.TargetProfile = Durin::ESkeletalPayloadTargetProfile::Game}, Output));
		EXPECT_EQ(Output, MeshSentinel);
	};
	auto ExpectClipFailure = [&](std::vector<Durin::uint8> Corrupt) {
		Durin::FAnimationClipPayloadData Output = ClipSentinel;
		EXPECT_FALSE(DeserializePayload(Corrupt, {
			.SkeletonBoneCount = Skeleton->GetBoneCount(),
			.TargetPlatform = Durin::ESkeletalPayloadTargetPlatform::Win64,
			.TargetProfile = Durin::ESkeletalPayloadTargetProfile::Game}, Output));
		EXPECT_EQ(Output, ClipSentinel);
	};

	auto WrongMagic = MeshBytes;
	WrongMagic[0] ^= 0xff;
	ExpectMeshFailure(std::move(WrongMagic));
	auto WrongProfile = MeshBytes;
	WriteWireU32(WrongProfile, 16, 99);
	RefreshSkeletalPayloadHash(WrongProfile);
	ExpectMeshFailure(std::move(WrongProfile));
	auto WrongSchema = MeshBytes;
	WriteWireU32(WrongSchema, 4, 99);
	ExpectMeshFailure(std::move(WrongSchema));
	auto Truncated = MeshBytes;
	Truncated.pop_back();
	ExpectMeshFailure(std::move(Truncated));
	auto DuplicateChunk = MeshBytes;
	WriteWireU32(DuplicateChunk,
		Durin::SkeletalPayloadHeaderSize + Durin::SkeletalPayloadChunkEntrySize, 1);
	RefreshSkeletalPayloadHash(DuplicateChunk);
	ExpectMeshFailure(std::move(DuplicateChunk));
	auto Overlap = MeshBytes;
	const Durin::uint64 FirstOffset = ReadWireU64(
		Overlap, Durin::SkeletalPayloadHeaderSize + 8);
	WriteWireU64(Overlap,
		Durin::SkeletalPayloadHeaderSize + Durin::SkeletalPayloadChunkEntrySize + 8,
		FirstOffset);
	RefreshSkeletalPayloadHash(Overlap);
	ExpectMeshFailure(std::move(Overlap));
	auto NonFinitePosition = MeshBytes;
	const size_t PositionEntry = Durin::SkeletalPayloadHeaderSize
		+ 2 * Durin::SkeletalPayloadChunkEntrySize;
	const size_t PositionOffset = static_cast<size_t>(ReadWireU64(
		NonFinitePosition, PositionEntry + 8));
	WriteWireU32(NonFinitePosition, PositionOffset + 4, 0x7fc00000);
	RefreshSkeletalPayloadHash(NonFinitePosition);
	ExpectMeshFailure(std::move(NonFinitePosition));

	auto ClipWrongProfile = ClipBytes;
	WriteWireU32(ClipWrongProfile, 16, 0);
	RefreshSkeletalPayloadHash(ClipWrongProfile);
	ExpectClipFailure(std::move(ClipWrongProfile));
	auto DuplicateTrackTarget = ClipBytes;
	const size_t TrackEntry = Durin::SkeletalPayloadHeaderSize
		+ Durin::SkeletalPayloadChunkEntrySize;
	const size_t TrackOffset = static_cast<size_t>(ReadWireU64(
		DuplicateTrackTarget, TrackEntry + 8));
	const size_t FirstRecord = TrackOffset + 4;
	const size_t SecondRecord = FirstRecord + 20;
	DuplicateTrackTarget[SecondRecord] = DuplicateTrackTarget[FirstRecord];
	DuplicateTrackTarget[SecondRecord + 1] = DuplicateTrackTarget[FirstRecord + 1];
	DuplicateTrackTarget[SecondRecord + 2] = DuplicateTrackTarget[FirstRecord + 2];
	RefreshSkeletalPayloadHash(DuplicateTrackTarget);
	ExpectClipFailure(std::move(DuplicateTrackTarget));
}

TEST(FSkeletalAssetTests, AuthoredReloadConsumesValidatedDerivedDataObjects)
{
	const std::filesystem::path Root = InitializeAssetMount();
	const std::filesystem::path CacheRoot = Root / "DerivedDataCache";
	Durin::FPaths::SetDerivedDataCacheDirForTests(CacheRoot.generic_string());
	Durin::FAssetPath SkeletonPath;
	Durin::FAssetPath MeshPath;
	Durin::FAssetPath ClipPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/SkeletalAssetTests/DdcSkeleton", SkeletonPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/SkeletalAssetTests/DdcMesh", MeshPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/SkeletalAssetTests/DdcClip", ClipPath));
	std::string MeshKey = Durin::FXxHash128::HashBuffer("mesh-ddc-key").ToString();
	std::string ClipKey = Durin::FXxHash128::HashBuffer("clip-ddc-key").ToString();

	Durin::DSkeleton* Skeleton = nullptr;
	Durin::DSkeletalMesh* Mesh = nullptr;
	Durin::DAnimationClip* Clip = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(SkeletonPath, Skeleton));
	InitializeSkeleton(*Skeleton);
	ASSERT_TRUE(Durin::Asset::CreateAsset(MeshPath, Mesh));
	InitializeMesh(*Mesh, *Skeleton, "Body", MeshKey);
	MeshKey = Mesh->GetDerivedDataKey();
	ASSERT_TRUE(Durin::Asset::CreateAsset(ClipPath, Clip));
	InitializeClip(*Clip, *Skeleton, "Walk", 1.0f, ClipKey);
	ClipKey = Clip->GetDerivedDataKey();
	const Durin::FSkeletalMeshPayloadData ExpectedMesh = *Mesh->GetPayloadData();
	const Durin::FAnimationClipPayloadData ExpectedClip = *Clip->GetPayloadData();
	EXPECT_NE(Mesh->GetPayloadStorageDiagnostic().find("Stored"), std::string::npos);
	EXPECT_NE(Clip->GetPayloadStorageDiagnostic().find("Stored"), std::string::npos);
	ASSERT_TRUE(Durin::Asset::SavePackage(Skeleton->GetPackage()));
	ASSERT_TRUE(Durin::Asset::SavePackage(Mesh->GetPackage()));
	ASSERT_TRUE(Durin::Asset::SavePackage(Clip->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(ClipPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(MeshPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(SkeletonPath));

	const Durin::Asset::FAssetResult MeshLoad = Durin::Asset::LoadAsset(MeshPath, Mesh);
	ASSERT_TRUE(MeshLoad) << MeshLoad.Message;
	ASSERT_NE(Mesh, nullptr);
	ASSERT_NE(Mesh->GetPayloadData(), nullptr);
	EXPECT_EQ(*Mesh->GetPayloadData(), ExpectedMesh);
	EXPECT_TRUE(Mesh->WasLoadedFromDerivedDataCache());
	EXPECT_EQ(Mesh->GetDerivedDataKey(), MeshKey);
	ASSERT_TRUE(Durin::Asset::UnloadPackage(MeshPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(SkeletonPath));
	ASSERT_TRUE(Durin::Asset::LoadAsset(ClipPath, Clip));
	ASSERT_NE(Clip, nullptr);
	ASSERT_NE(Clip->GetPayloadData(), nullptr);
	EXPECT_EQ(*Clip->GetPayloadData(), ExpectedClip);
	EXPECT_TRUE(Clip->WasLoadedFromDerivedDataCache());
	EXPECT_EQ(Clip->GetDerivedDataKey(), ClipKey);
	ASSERT_TRUE(Durin::Asset::UnloadPackage(ClipPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(SkeletonPath));
	const std::filesystem::path ClipObject = CacheRoot / "AnimationClip/Objects"
		/ ClipKey.substr(0, 2) / (ClipKey + ".bin");
	ASSERT_TRUE(std::filesystem::remove(ClipObject));
	{
		const Durin::FScopedSkeletalDerivedDataRepairLoad RepairLoad;
		ASSERT_TRUE(Durin::Asset::LoadAsset(ClipPath, Clip));
		ASSERT_NE(Clip, nullptr);
		EXPECT_EQ(Clip->GetPayloadData(), nullptr);
		ASSERT_EQ(RepairLoad.GetMissingAssets().size(), 1);
		EXPECT_EQ(RepairLoad.GetMissingAssets().front(), Clip);
	}
	ASSERT_TRUE(Durin::Asset::UnloadPackage(ClipPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(SkeletonPath));

	const std::filesystem::path MeshObject = CacheRoot / "SkeletalMesh/Objects"
		/ MeshKey.substr(0, 2) / (MeshKey + ".bin");
	std::vector<Durin::uint8> CorruptBytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(CorruptBytes, MeshObject));
	ASSERT_FALSE(CorruptBytes.empty());
	CorruptBytes.back() ^= 0x80;
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(
		std::as_bytes(std::span(CorruptBytes)), MeshObject));
	Mesh = nullptr;
	const Durin::Asset::FAssetResult CorruptLoad = Durin::Asset::LoadAsset(MeshPath, Mesh);
	EXPECT_FALSE(CorruptLoad);
	EXPECT_EQ(Mesh, nullptr);
	EXPECT_NE(CorruptLoad.Message.find("checksum"), std::string::npos) << CorruptLoad.Message;
	Durin::FPaths::SetDerivedDataCacheDirForTests({});
}

TEST(FSkeletalAssetTests, AuthoredLoadFailsClosedWithoutRequiredDerivedData)
{
	const std::filesystem::path Root = InitializeAssetMount();
	const std::filesystem::path CacheRoot = Root / "MigrationDerivedDataCache";
	Durin::FPaths::SetDerivedDataCacheDirForTests(CacheRoot.generic_string());
	Durin::FAssetPath SkeletonPath;
	Durin::FAssetPath MeshPath;
	Durin::FAssetPath ClipPath;
	Durin::FAssetPath LevelPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/SkeletalAssetTests/MigrationSkeleton", SkeletonPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/SkeletalAssetTests/MigrationMesh", MeshPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/SkeletalAssetTests/MigrationClip", ClipPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/SkeletalAssetTests/MigrationLevel", LevelPath));
	std::string MeshKey = Durin::FXxHash128::HashBuffer("migration-mesh-key").ToString();
	std::string ClipKey = Durin::FXxHash128::HashBuffer("migration-clip-key").ToString();

	Durin::DSkeleton* Skeleton = nullptr;
	Durin::DSkeletalMesh* Mesh = nullptr;
	Durin::DAnimationClip* Clip = nullptr;
	Durin::DLevel* Level = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(SkeletonPath, Skeleton));
	InitializeSkeleton(*Skeleton);
	ASSERT_TRUE(Durin::Asset::CreateAsset(MeshPath, Mesh));
	InitializeMesh(*Mesh, *Skeleton, "Body", MeshKey);
	MeshKey = Mesh->GetDerivedDataKey();
	ASSERT_TRUE(Durin::Asset::CreateAsset(ClipPath, Clip));
	InitializeClip(*Clip, *Skeleton, "Walk", 1.0f, ClipKey);
	ClipKey = Clip->GetDerivedDataKey();
	ASSERT_TRUE(Durin::Asset::CreateAsset(LevelPath, Level));
	auto* Actor = Level->SpawnActor<Durin::ASkeletalMeshActor>("AnimatedActor");
	ASSERT_NE(Actor, nullptr);
	std::string Error;
	ASSERT_TRUE(Actor->GetSkeletalMeshComponent()->SetSkeletalMesh(Mesh, Error)) << Error;
	ASSERT_TRUE(Actor->GetSkeletalMeshComponent()->SetAnimationClip(Clip, Error)) << Error;
	ASSERT_TRUE(Durin::Asset::SavePackage(Skeleton->GetPackage()));
	ASSERT_TRUE(Durin::Asset::SavePackage(Mesh->GetPackage()));
	ASSERT_TRUE(Durin::Asset::SavePackage(Clip->GetPackage()));
	ASSERT_TRUE(Durin::Asset::SavePackage(Level->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(LevelPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(ClipPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(MeshPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(SkeletonPath));
	ASSERT_TRUE(std::filesystem::remove(CacheRoot / "SkeletalMesh/Objects"
		/ MeshKey.substr(0, 2) / (MeshKey + ".bin")));
	ASSERT_TRUE(std::filesystem::remove(CacheRoot / "AnimationClip/Objects"
		/ ClipKey.substr(0, 2) / (ClipKey + ".bin")));

	Durin::DLevel* ReloadedLevel = nullptr;
	const Durin::Asset::FAssetResult Load =
		Durin::Asset::LoadAsset(LevelPath, ReloadedLevel);
	EXPECT_FALSE(Load);
	EXPECT_EQ(ReloadedLevel, nullptr);
	EXPECT_EQ(Durin::Asset::FindResidentPackage(LevelPath), nullptr);
	EXPECT_EQ(Durin::Asset::FindResidentPackage(ClipPath), nullptr);
	EXPECT_EQ(Durin::Asset::FindResidentPackage(MeshPath), nullptr);
	EXPECT_EQ(Durin::Asset::FindResidentPackage(SkeletonPath), nullptr);
	Durin::FPaths::SetDerivedDataCacheDirForTests({});
}

TEST(FSkeletalAssetTests, DerivedDataWriteFailureKeepsCompleteMemoryCandidate)
{
	InitializeDObjectSystem();
	const std::filesystem::path Root =
		Durin::Testing::GetTestWorkDirectory() / "SkeletalAssetsDdcWriteFailure";
	Durin::Testing::RemoveTestWorkDirectory(Root);
	std::filesystem::create_directories(Root);
	const std::filesystem::path BlockedRoot = Root / "Blocked";
	const std::array<Durin::uint8, 1> BlockedBytes{0xff};
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(
		std::as_bytes(std::span(BlockedBytes)), BlockedRoot));
	Durin::FPaths::SetDerivedDataCacheDirForTests(BlockedRoot.generic_string());
	auto* Skeleton = Durin::NewObject<Durin::DSkeleton>(nullptr, "WriteFailureSkeleton");
	InitializeSkeleton(*Skeleton);
	auto* Mesh = Durin::NewObject<Durin::DSkeletalMesh>(nullptr, "WriteFailureMesh");
	InitializeMesh(*Mesh, *Skeleton, "Body",
		Durin::FXxHash128::HashBuffer("write-failure").ToString());
	ASSERT_NE(Mesh->GetPayloadData(), nullptr);
	EXPECT_NE(Mesh->GetPayloadStorageDiagnostic().find("failed"), std::string::npos);
	std::string Error;
	EXPECT_TRUE(Mesh->Validate(Error)) << Error;
	Durin::FPaths::SetDerivedDataCacheDirForTests({});
}

TEST(FSkeletalAssetTests, AuthoredPackagesRoundTripHardReferencesAndSummaries)
{
	InitializeAssetMount();
	Durin::FAssetPath SkeletonPath;
	Durin::FAssetPath MeshPath;
	Durin::FAssetPath ClipPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/SkeletalAssetTests/RoundTripSkeleton", SkeletonPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/SkeletalAssetTests/RoundTripMesh", MeshPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/SkeletalAssetTests/RoundTripClip", ClipPath));

	Durin::DSkeleton* Skeleton = nullptr;
	Durin::DSkeletalMesh* Mesh = nullptr;
	Durin::DAnimationClip* Clip = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(SkeletonPath, Skeleton));
	InitializeSkeleton(*Skeleton);
	ASSERT_TRUE(Durin::Asset::CreateAsset(MeshPath, Mesh));
	InitializeMesh(*Mesh, *Skeleton);
	ASSERT_TRUE(Durin::Asset::CreateAsset(ClipPath, Clip));
	InitializeClip(*Clip, *Skeleton);
	const std::string Identity = Skeleton->GetCompatibilityIdentity();
	const Durin::FSkeletalMeshSummary MeshSummary = Mesh->GetSummary();
	const Durin::FAnimationClipSummary ClipSummary = Clip->GetSummary();
	const Durin::Asset::FCookedPayloadDescriptor MeshCooked = Mesh->GetCookedPayloadDescriptor();
	const Durin::Asset::FCookedPayloadDescriptor ClipCooked = Clip->GetCookedPayloadDescriptor();
	ASSERT_TRUE(Durin::Asset::SavePackage(Skeleton->GetPackage()));
	ASSERT_TRUE(Durin::Asset::SavePackage(Mesh->GetPackage()));
	ASSERT_TRUE(Durin::Asset::SavePackage(Clip->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(ClipPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(MeshPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(SkeletonPath));

	ASSERT_TRUE(Durin::Asset::LoadAsset(MeshPath, Mesh));
	ASSERT_NE(Mesh->GetSkeleton(), nullptr);
	EXPECT_EQ(Mesh->GetSkeleton()->GetCompatibilityIdentity(), Identity);
	EXPECT_EQ(Mesh->GetSkeletonCompatibilityIdentity(), Identity);
	EXPECT_EQ(Mesh->GetSummary(), MeshSummary);
	EXPECT_EQ(Mesh->GetCookedPayloadDescriptor(), MeshCooked);
	ASSERT_EQ(Mesh->GetMaterialSlots().size(), 1u);
	EXPECT_EQ(Mesh->GetMaterialSlots()[0].SourceName, "Body");
	EXPECT_EQ(Mesh->GetPayloadData(), nullptr);
	ASSERT_TRUE(Durin::Asset::UnloadPackage(MeshPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(SkeletonPath));

	ASSERT_TRUE(Durin::Asset::LoadAsset(ClipPath, Clip));
	ASSERT_NE(Clip->GetSkeleton(), nullptr);
	EXPECT_EQ(Clip->GetSkeletonCompatibilityIdentity(), Identity);
	EXPECT_EQ(Clip->GetSummary(), ClipSummary);
	EXPECT_EQ(Clip->GetCookedPayloadDescriptor(), ClipCooked);
	ASSERT_EQ(Clip->GetSkeleton()->GetBones().size(), 5u);
	EXPECT_EQ(Clip->GetSkeleton()->GetBones()[4].Name, Durin::FName("Hand"));
	EXPECT_EQ(Clip->GetPayloadData(), nullptr);
	ASSERT_TRUE(Durin::Asset::UnloadPackage(ClipPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(SkeletonPath));
	ASSERT_TRUE(Durin::Asset::DeleteAssetForTesting(ClipPath));
	ASSERT_TRUE(Durin::Asset::DeleteAssetForTesting(MeshPath));
	ASSERT_TRUE(Durin::Asset::DeleteAssetForTesting(SkeletonPath));
}

TEST(FSkeletalAssetTests, DuplicationPreservesAuthoredStateAndExternalReferences)
{
	InitializeDObjectSystem();
	auto* Skeleton = Durin::NewObject<Durin::DSkeleton>(nullptr, "DuplicateSkeleton");
	InitializeSkeleton(*Skeleton);
	auto* Mesh = Durin::NewObject<Durin::DSkeletalMesh>(nullptr, "DuplicateMesh");
	InitializeMesh(*Mesh, *Skeleton);
	auto* Clip = Durin::NewObject<Durin::DAnimationClip>(nullptr, "DuplicateClip");
	InitializeClip(*Clip, *Skeleton);

	std::string Error;
	auto* MeshDuplicate = Durin::Cast<Durin::DSkeletalMesh>(
		Durin::DuplicateObjectGraph(Mesh, nullptr, "MeshDuplicate", &Error));
	ASSERT_NE(MeshDuplicate, nullptr) << Error;
	EXPECT_EQ(MeshDuplicate->GetSkeleton(), Skeleton);
	EXPECT_EQ(MeshDuplicate->GetSummary(), Mesh->GetSummary());
	EXPECT_EQ(MeshDuplicate->GetCookedPayloadDescriptor(), Mesh->GetCookedPayloadDescriptor());
	EXPECT_EQ(MeshDuplicate->GetPayloadData(), nullptr);
	auto* ClipDuplicate = Durin::Cast<Durin::DAnimationClip>(
		Durin::DuplicateObjectGraph(Clip, nullptr, "ClipDuplicate", &Error));
	ASSERT_NE(ClipDuplicate, nullptr) << Error;
	EXPECT_EQ(ClipDuplicate->GetSkeleton(), Skeleton);
	EXPECT_EQ(ClipDuplicate->GetSummary(), Clip->GetSummary());
	EXPECT_EQ(ClipDuplicate->GetCookedPayloadDescriptor(), Clip->GetCookedPayloadDescriptor());
	EXPECT_EQ(ClipDuplicate->GetPayloadData(), nullptr);
}

TEST(FSkeletalAssetTests, RuntimeTypesExposeOnlySourceIndependentAuthoredFields)
{
	InitializeDObjectSystem();
	EXPECT_NE(Durin::DSkeleton::StaticClass(), nullptr);
	EXPECT_NE(Durin::DSkeletalMesh::StaticClass(), nullptr);
	EXPECT_NE(Durin::DAnimationClip::StaticClass(), nullptr);
	for (Durin::DClass* Class : {
		Durin::DSkeleton::StaticClass(),
		Durin::DSkeletalMesh::StaticClass(),
		Durin::DAnimationClip::StaticClass()})
	{
		EXPECT_EQ(Class->FindPropertyByName("SourcePath", false), nullptr);
		EXPECT_EQ(Class->FindPropertyByName("ProviderState", false), nullptr);
		EXPECT_EQ(Class->FindPropertyByName("ImportRecord", false), nullptr);
		EXPECT_EQ(Class->FindPropertyByName("RetiredUnknownField", false), nullptr);
	}
}

TEST(FSkeletalAssetTests, CleanCookIsDeterministicAndRuntimeLoadsWithoutSourceOrDdc)
{
	InitializeDObjectSystem();
	const std::filesystem::path Root =
		Durin::Testing::GetTestWorkDirectory() / "SkeletalCookedConsumer";
	const std::filesystem::path ContentRoot = Root / "Content";
	const std::filesystem::path CacheRoot = Root / "DerivedDataCache";
	const std::filesystem::path FirstCookRoot = std::filesystem::absolute(Root / "CookFirst");
	const std::filesystem::path SecondCookRoot = std::filesystem::absolute(Root / "CookSecond");
	Durin::Testing::RemoveTestWorkDirectory(Root);
	std::filesystem::create_directories(ContentRoot);
	Durin::PathUtilities::RegisterMountPointForTests(
		"/Game/", ContentRoot.generic_string() + "/");
	Durin::FPaths::SetDerivedDataCacheDirForTests(CacheRoot.generic_string());
	Durin::FAssetPath SkeletonPath;
	Durin::FAssetPath MeshPath;
	Durin::FAssetPath ClipPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/Game/Skeleton", SkeletonPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/Game/Mesh", MeshPath));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/Game/Clip", ClipPath));
	Durin::DSkeleton* Skeleton = nullptr;
	Durin::DSkeletalMesh* Mesh = nullptr;
	Durin::DAnimationClip* Clip = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(SkeletonPath, Skeleton));
	InitializeSkeleton(*Skeleton);
	ASSERT_TRUE(Durin::Asset::CreateAsset(MeshPath, Mesh));
	InitializeMesh(*Mesh, *Skeleton, "Body",
		Durin::FXxHash128::HashBuffer("cook-mesh").ToString());
	ASSERT_TRUE(Durin::Asset::CreateAsset(ClipPath, Clip));
	InitializeClip(*Clip, *Skeleton, "Walk", 1.0f,
		Durin::FXxHash128::HashBuffer("cook-clip").ToString());
	const Durin::FSkeletalMeshPayloadData ExpectedMesh = *Mesh->GetPayloadData();
	const Durin::FAnimationClipPayloadData ExpectedClip = *Clip->GetPayloadData();
	ASSERT_TRUE(Durin::Asset::SavePackage(Skeleton->GetPackage()));
	ASSERT_TRUE(Durin::Asset::SavePackage(Mesh->GetPackage()));
	ASSERT_TRUE(Durin::Asset::SavePackage(Clip->GetPackage()));

	std::string Error;
	auto Cook = [&](const std::filesystem::path& CookRoot) {
		Durin::Asset::FCookContext Context(
			CookRoot, Durin::Asset::ECookTargetPlatform::Win64,
			Durin::Asset::ECookTargetProfile::Game);
		ASSERT_TRUE(Skeleton->AddToCook(Context, "/Game/Skeleton", Error)) << Error;
		ASSERT_TRUE(Mesh->AddToCook(Context, "/Game/Mesh", Error)) << Error;
		ASSERT_TRUE(Clip->AddToCook(Context, "/Game/Clip", Error)) << Error;
		ASSERT_TRUE(Context.Publish(&Error)) << Error;
	};
	Cook(FirstCookRoot);
	Cook(SecondCookRoot);

	for (const std::filesystem::path& Relative : {
		std::filesystem::path("Game/Skeleton.dasset"),
		std::filesystem::path("Game/Mesh.dasset"),
		std::filesystem::path("Game/Mesh.dbulk"),
		std::filesystem::path("Game/Clip.dasset"),
		std::filesystem::path("Game/Clip.dbulk"),
		std::filesystem::path("CookManifest.bin")})
	{
		std::vector<Durin::uint8> FirstBytes;
		std::vector<Durin::uint8> SecondBytes;
		ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
			FirstBytes, (FirstCookRoot / Relative)));
		ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
			SecondBytes, (SecondCookRoot / Relative)));
		EXPECT_EQ(FirstBytes, SecondBytes) << Relative.generic_string();
		if (Relative.extension() == ".dasset")
		{
			EXPECT_FALSE(ContainsText(FirstBytes, "DerivedDataKey"));
			EXPECT_FALSE(ContainsText(FirstBytes, "cook-mesh"));
			EXPECT_FALSE(ContainsText(FirstBytes, "cook-clip"));
		}
	}
	std::vector<Durin::uint8> MeshBulk;
	std::vector<Durin::uint8> ClipBulk;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		MeshBulk, (FirstCookRoot / "Game/Mesh.dbulk")));
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		ClipBulk, (FirstCookRoot / "Game/Clip.dbulk")));
	Durin::Asset::FCookedBulkContainer MeshContainer;
	Durin::Asset::FCookedBulkContainer ClipContainer;
	ASSERT_TRUE(Durin::Asset::DecodeCookedBulk(
		MeshBulk, Durin::Asset::ECookTargetPlatform::Win64,
		Durin::Asset::ECookTargetProfile::Game, MeshContainer, &Error)) << Error;
	ASSERT_TRUE(Durin::Asset::DecodeCookedBulk(
		ClipBulk, Durin::Asset::ECookTargetPlatform::Win64,
		Durin::Asset::ECookTargetProfile::Game, ClipContainer, &Error)) << Error;
	ASSERT_EQ(MeshContainer.Entries.size(), 1u);
	ASSERT_EQ(ClipContainer.Entries.size(), 1u);
	EXPECT_EQ(MeshContainer.Entries[0].PayloadId, Durin::SkeletalMeshPrimaryCookedPayloadId);
	EXPECT_EQ(ClipContainer.Entries[0].PayloadId, Durin::AnimationClipPrimaryCookedPayloadId);

	ASSERT_TRUE(Durin::Asset::UnloadPackage(ClipPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(MeshPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(SkeletonPath));
	Durin::Testing::RemoveTestWorkDirectory(ContentRoot);
	Durin::Testing::RemoveTestWorkDirectory(CacheRoot);
	RestartAssetManager(FirstCookRoot);
	Durin::PathUtilities::RegisterMountPointForTests(
		"/Game/", (FirstCookRoot / "Game").generic_string() + "/");
	ASSERT_TRUE(Durin::Asset::RefreshAssetCatalog(
		Durin::Asset::EAssetRegistryScanMode::FullValidation));
	Mesh = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(MeshPath, Mesh));
	ASSERT_NE(Mesh, nullptr);
	ASSERT_NE(Mesh->GetSkeleton(), nullptr);
	ASSERT_NE(Mesh->GetPayloadData(), nullptr);
	EXPECT_EQ(*Mesh->GetPayloadData(), ExpectedMesh);
	EXPECT_TRUE(Mesh->GetDerivedDataKey().empty());
	EXPECT_EQ(Mesh->GetCookedPayloadDescriptor().PayloadId,
		Durin::SkeletalMeshPrimaryCookedPayloadId);
	Clip = nullptr;
	ASSERT_TRUE(Durin::Asset::LoadAsset(ClipPath, Clip));
	ASSERT_NE(Clip, nullptr);
	EXPECT_EQ(Clip->GetSkeleton(), Mesh->GetSkeleton());
	ASSERT_NE(Clip->GetPayloadData(), nullptr);
	EXPECT_EQ(*Clip->GetPayloadData(), ExpectedClip);
	EXPECT_TRUE(Clip->GetDerivedDataKey().empty());
	EXPECT_EQ(Clip->GetCookedPayloadDescriptor().PayloadId,
		Durin::AnimationClipPrimaryCookedPayloadId);

	ASSERT_TRUE(Durin::Asset::UnloadPackage(ClipPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(MeshPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(SkeletonPath));
	MeshBulk.back() ^= 0x80;
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(
		std::as_bytes(std::span(MeshBulk)), FirstCookRoot / "Game/Mesh.dbulk"));
	RestartAssetManager(FirstCookRoot);
	Durin::PathUtilities::RegisterMountPointForTests(
		"/Game/", (FirstCookRoot / "Game").generic_string() + "/");
	ASSERT_TRUE(Durin::Asset::RefreshAssetCatalog(
		Durin::Asset::EAssetRegistryScanMode::FullValidation));
	Mesh = nullptr;
	const Durin::Asset::FAssetResult Corrupt = Durin::Asset::LoadAsset(MeshPath, Mesh);
	EXPECT_FALSE(Corrupt);
	EXPECT_EQ(Mesh, nullptr);
	EXPECT_NE(Corrupt.Message.find("checksum"), std::string::npos) << Corrupt.Message;

	RestartAssetManager();
	Durin::FPaths::SetDerivedDataCacheDirForTests({});
}
