#include "NativeAssetTestSupport.h"
#include <gtest/gtest.h>

#include "NativeDObjectTestSupport.h"

#include <chrono>
#include <thread>

#include "Animation/AnimationClip.h"
#include "Actors/SkeletalMeshActor.h"
#include "Asset/EditorBulkDataStorage.h"
#include "Asset/CookedMeshProducts.h"
#include "Asset/CookedMeshLoadManager.h"
#include "Asset/Mutation.h"
#include "Asset/PackageSerialization.h"
#include "Asset/AssetCook.h"
#include "Components/SkeletalMeshComponent.h"
#include "DObject/Class.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"
#include "Engine/Level.h"
#include "EngineTestSupport.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/MountPathTestSupport.h"
#include "Modules/ModuleTestSupport.h"
#include "NativeTestSupport.h"
#include "Serialization/Archive.h"
#include "SkeletalMesh/SkeletalBuild.h"
#include "Runtime/Engine/Private/SkeletalMesh/SkeletalDerivedDataKey.h"
#include "SkeletalMesh/SkeletalDerivedData.h"
#include "SkeletalMesh/SkeletalMesh.h"
#include "SkeletalMesh/SkeletalMeshResources.h"

namespace
{
	class FCountingSkeletalBuildProvider final : public Durin::ISkeletalBuildProvider
	{
	public:
		uint32 MeshBuilds = 0;
		uint32 ClipBuilds = 0;
		auto GetDescriptor() const -> Durin::FSkeletalBuildProviderDescriptor override
		{
			return {"CanonicalSkeletalMesh", Durin::SkeletalMeshImportedDataSchemaVersion,
				"CanonicalAnimationClip", Durin::AnimationClipImportedDataSchemaVersion};
		}
		auto BuildSkeletalMesh(const Durin::FSkeletalMeshRecipeRequest& Request,
			Durin::FSkeletalMeshRecipeProduct& OutProduct, std::string& OutError) -> bool override
		{
			++MeshBuilds;
			OutProduct.Payload = Request.Payload;
			OutError.clear();
			return true;
		}
		auto BuildAnimationClip(const Durin::FAnimationClipRecipeRequest& Request,
			Durin::FAnimationClipRecipeProduct& OutProduct, std::string& OutError) -> bool override
		{
			++ClipBuilds;
			OutProduct.Payload = Request.Payload;
			OutError.clear();
			return true;
		}
	};

	struct FTestSkeletalProvider
	{
		Durin::FModuleTestOwner Owner{"SkeletalAssetTests"};
		FCountingSkeletalBuildProvider Provider;
		Durin::FModularFeatureRegistration Registration =
			Owner.RegisterFeature<Durin::ISkeletalBuildProvider>(Provider);
	};

	auto GetTestProvider() -> FTestSkeletalProvider&
	{
		static FTestSkeletalProvider State;
		return State;
	}

	template<typename T>
	auto GetAssetKey(const T& Asset) -> std::string
	{
		using namespace Durin;
		using TKey = std::conditional_t<std::is_same_v<T, DSkeletalMesh>,
			FSkeletalMeshBuildKeyInput, FAnimationClipBuildKeyInput>;
		TKey Key;
		const auto Descriptor = GetTestProvider().Provider.GetDescriptor();
		static_cast<FSkeletalBuildKeyFields&>(Key) = {
			.ProviderIdentity = std::is_same_v<T, DSkeletalMesh>
				? Descriptor.SkeletalMeshProducerIdentity : Descriptor.AnimationClipProducerIdentity,
			.ProviderVersion = std::is_same_v<T, DSkeletalMesh>
				? Descriptor.SkeletalMeshProducerVersion : Descriptor.AnimationClipProducerVersion,
			.ImportedDataIdentity = Asset.GetImportedData().GetIdentity(),
			.PayloadInputFingerprint = Asset.GetImportedData().GetIdentity(),
			.SkeletonCompatibilityIdentity = Asset.GetSkeleton()->GetCompatibilityIdentity(),
			.TargetPlatform = ESkeletalPayloadTargetPlatform::Win64,
			.TargetProfile = ESkeletalPayloadTargetProfile::Game};
		std::string Error;
		if constexpr (std::is_same_v<T, DSkeletalMesh>)
			return BuildSkeletalMeshDerivedDataKey(Key, Error);
		else return BuildAnimationClipDerivedDataKey(Key, Error);
	}

	auto SerializeMeshPayload(
		const Durin::FSkeletalMeshPayloadData& Payload,
		const Durin::DSkeleton& Skeleton,
		uint32 MaterialSlotCount,
		Durin::FByteArray& OutBytes,
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
		Durin::FByteArray& OutBytes,
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
		std::span<const std::byte> Bytes,
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

	auto MakeMeshPayload(uint16 BoneIndex = 1) -> std::shared_ptr<const Durin::FSkeletalMeshPayloadData>
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

	auto InitializeMesh(
		Durin::DSkeletalMesh& Mesh,
		Durin::DSkeleton& Skeleton,
		std::string_view SlotName = "Body",
		std::string DerivedDataKey = {}) -> void
	{
		Durin::FSkeletalMeshAssetData Data{
			.Skeleton = &Skeleton,
			.SkeletonCompatibilityIdentity = Skeleton.GetCompatibilityIdentity(),
			.MeshNodeBindTransform = MakeTransform(),
			.MaterialSlots = {{
				.Name = Durin::FName(SlotName),
				.SourceName = std::string(SlotName),
				.SourceMaterialIndex = 0}},
			.Payload = MakeMeshPayload()};
		std::string Error;
		if (!DerivedDataKey.empty())
		{
			GetTestProvider();
			Durin::FSkeletalMeshImportedData Canonical;
			ASSERT_TRUE(Canonical.Capture(*Data.Payload, Skeleton.GetBoneCount(), static_cast<uint32>(Data.MaterialSlots.size()), Error)) << Error;
			Durin::FSkeletalMeshDerivedDataResult Product;
			ASSERT_TRUE(Durin::BuildSkeletalMeshDerivedData({
				.ImportedDataIdentity = Canonical.GetIdentity(),
				.SkeletonCompatibilityIdentity = Skeleton.GetCompatibilityIdentity(),
				.Context = {.SkeletonBoneCount = Skeleton.GetBoneCount(), .MaterialSlotCount = static_cast<uint32>(Data.MaterialSlots.size()),
					.TargetPlatform = Durin::ESkeletalPayloadTargetPlatform::Win64,
					.TargetProfile = Durin::ESkeletalPayloadTargetProfile::Game},
				.Payload = Data.Payload}, Product, Error)) << Error;
			Data.Payload = std::move(Product.Payload);
		}
		ASSERT_TRUE(Mesh.SetAssetData(std::move(Data), Error)) << Error;
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

	auto MakeLargeClipPayload() -> std::shared_ptr<const Durin::FAnimationClipPayloadData>
	{
		constexpr uint32 KeyCount = 20'000;
		auto Payload = std::make_shared<Durin::FAnimationClipPayloadData>();
		Payload->DurationSeconds = static_cast<float>(KeyCount - 1);
		Durin::FAnimationTrackData Track{
			.BoneIndex = 1,
			.Path = Durin::EAnimationTrackPath::Translation,
			.Interpolation = Durin::EAnimationInterpolation::Linear};
		Track.Times.reserve(KeyCount);
		Track.VectorValues.reserve(KeyCount);
		for (uint32 KeyIndex = 0; KeyIndex < KeyCount; ++KeyIndex)
		{
			Track.Times.push_back(static_cast<float>(KeyIndex));
			Track.VectorValues.emplace_back(
				static_cast<float>(KeyIndex), 0.0f, 1.0f);
		}
		Payload->Tracks.push_back(std::move(Track));
		return Payload;
	}

	auto InitializeClip(
		Durin::DAnimationClip& Clip,
		Durin::DSkeleton& Skeleton,
		std::string_view Name = "Walk",
		float EndValue = 1.0f,
		std::string DerivedDataKey = {}) -> void
	{
		Durin::FAnimationClipAssetData Data{
			.Skeleton = &Skeleton,
			.SkeletonCompatibilityIdentity = Skeleton.GetCompatibilityIdentity(),
			.ClipName = Durin::FName(Name),
			.Payload = MakeClipPayload(EndValue)};
		std::string Error;
		if (!DerivedDataKey.empty())
		{
			GetTestProvider();
			Durin::FAnimationClipImportedData Canonical;
			ASSERT_TRUE(Canonical.Capture(*Data.Payload, Skeleton.GetBoneCount(), Error)) << Error;
			Durin::FAnimationClipDerivedDataResult Product;
			ASSERT_TRUE(Durin::BuildAnimationClipDerivedData({
				.ImportedDataIdentity = Canonical.GetIdentity(),
				.SkeletonCompatibilityIdentity = Skeleton.GetCompatibilityIdentity(),
				.Context = {.SkeletonBoneCount = Skeleton.GetBoneCount(),
					.TargetPlatform = Durin::ESkeletalPayloadTargetPlatform::Win64,
					.TargetProfile = Durin::ESkeletalPayloadTargetProfile::Game},
				.Payload = Data.Payload}, Product, Error)) << Error;
			Data.Payload = std::move(Product.Payload);
		}
		ASSERT_TRUE(Clip.SetAssetData(std::move(Data), Error)) << Error;
	}

	auto InitializeAssetMount() -> std::filesystem::path
	{
		InitializeDObjectSystem();
		check(GetTestProvider().Registration.IsValid());
		const std::filesystem::path Root =
			Durin::Testing::GetTestWorkDirectory() / "SkeletalAssets";
		Durin::Testing::RemoveTestWorkDirectory(Root);
		Durin::Testing::RegisterMountPointForTests(
			"/SkeletalAssetTests/", Root.generic_string() + "/");
		requiref(Durin::RefreshAssetRegistry(
			Durin::EAssetRegistryScanMode::FullValidation),
			"Skeletal test catalog refresh must succeed.");
		return Root;
	}

	auto ReadWireU64(const Durin::FByteArray& Bytes, size_t Offset) -> uint64
	{
		uint64 Value = 0;
		for (uint32 Byte = 0; Byte < 8; ++Byte)
			Value |= std::to_integer<uint64>(Bytes[Offset + Byte]) << (Byte * 8);
		return Value;
	}

	auto WriteWireU32(
		Durin::FByteArray& Bytes,
		size_t Offset,
		uint32 Value) -> void
	{
		for (uint32 Byte = 0; Byte < 4; ++Byte)
			Bytes[Offset + Byte] = static_cast<std::byte>(Value >> (Byte * 8));
	}

	auto WriteWireU64(
		Durin::FByteArray& Bytes,
		size_t Offset,
		uint64 Value) -> void
	{
		for (uint32 Byte = 0; Byte < 8; ++Byte)
			Bytes[Offset + Byte] = static_cast<std::byte>(Value >> (Byte * 8));
	}

	auto RefreshSkeletalPayloadHash(Durin::FByteArray& Bytes) -> void
	{
		const uint64 Hash = Durin::FXxHash64::HashBuffer(
			std::span<const std::byte>(Bytes).subspan(
				Durin::SkeletalPayloadHeaderSize)).HashValue;
		WriteWireU64(Bytes, 56, Hash);
	}

	auto ContainsText(
		std::span<const std::byte> Bytes,
		std::string_view Text) -> bool
	{
		const std::span<const std::byte> TextBytes =
			std::as_bytes(std::span{Text.data(), Text.size()});
		return std::search(Bytes.begin(), Bytes.end(),
			TextBytes.begin(), TextBytes.end()) != Bytes.end();
	}

	auto RestartAssetManager(const std::filesystem::path& CookRoot = {}) -> void
	{
		Durin::ShutdownAssetManager();
		Durin::CollectGarbage();
		if (CookRoot.empty())
		{
			ASSERT_TRUE(Durin::InitializeAssetManager());
			return;
		}
		auto Configuration = Durin::FAssetRuntimeConfiguration::Authored();
		ASSERT_TRUE(Durin::FAssetRuntimeConfiguration::Cooked(
			CookRoot, Configuration));
		ASSERT_TRUE(Durin::InitializeAssetManager(std::move(Configuration)));
	}
}

TEST(FSkeletalMeshCookedProductTests,
	DetachedCodecMatchesBaselineAndClassifiesSummaryMismatch)
{
	InitializeDObjectSystem();
	auto* Skeleton = Durin::NewObject<Durin::DSkeleton>(nullptr, "DetachedCodecSkeleton");
	InitializeSkeleton(*Skeleton);
	const auto Payload = MakeMeshPayload();
	Durin::FByteArray Bytes;
	std::string Error;
	ASSERT_TRUE(SerializeMeshPayload(*Payload, *Skeleton, 1, Bytes, Error)) << Error;
	const std::vector<Durin::FMeshMaterialSlotDefinition> Slots{
		{.Name = Durin::FName("Body"), .SourceMaterialIndex = 3}};
	const Durin::FSkeletonTransform BindTransform = MakeTransform();
	std::unique_ptr<Durin::FSkeletalMeshRenderData> Baseline;
	ASSERT_TRUE(Durin::BuildSkeletalMeshRenderData(
		*Payload, *Skeleton, BindTransform, Slots, Baseline, Error)) << Error;
	const Durin::FSkeletalMeshSummary Summary{
		.VertexCount = static_cast<uint32>(Payload->Positions.size()),
		.IndexCount = static_cast<uint32>(Payload->Indices.size()),
		.SectionCount = static_cast<uint32>(Payload->Sections.size()),
		.LocalBounds = Durin::FSkeletalMeshBounds::FromBox(Payload->LocalBounds)};

	Durin::FSkeletalMeshCookedProduct Product;
	Durin::FCookedMeshProductError ProductError;
	ASSERT_TRUE(Durin::DecodeSkeletalMeshCookedProduct(
		Bytes, Skeleton->GetBones(), BindTransform, Slots, Summary,
		Product, ProductError)) << ProductError.Message;
	ASSERT_TRUE(Product.Payload);
	EXPECT_EQ(*Product.Payload, *Payload);
	ASSERT_NE(Product.RenderData, nullptr);
	EXPECT_EQ(Product.RenderData->IndexBuffer.GetIndices(),
		Baseline->IndexBuffer.GetIndices());
	EXPECT_EQ(Product.RenderData->VertexBuffers.Geometry.PositionVertexBuffer.GetPositions(),
		Baseline->VertexBuffers.Geometry.PositionVertexBuffer.GetPositions());
	EXPECT_EQ(Product.RenderData->PaletteBoneIndices, Baseline->PaletteBoneIndices);

	Durin::FSkeletalMeshSummary WrongSummary = Summary;
	++WrongSummary.VertexCount;
	Durin::FSkeletalMeshCookedProduct Rejected;
	EXPECT_FALSE(Durin::DecodeSkeletalMeshCookedProduct(
		Bytes, Skeleton->GetBones(), BindTransform, Slots, WrongSummary,
		Rejected, ProductError));
	EXPECT_EQ(ProductError.Category, Durin::ECookedMeshProductFailure::Metadata);

	Durin::FByteArray Truncated(Bytes.begin(), Bytes.end() - 1);
	EXPECT_FALSE(Durin::DecodeSkeletalMeshCookedProduct(
		Truncated, Skeleton->GetBones(), BindTransform, Slots, Summary,
		Rejected, ProductError));
	EXPECT_EQ(ProductError.Category, Durin::ECookedMeshProductFailure::Schema);
	Durin::FByteArray Incompatible = Bytes;
	WriteWireU32(Incompatible, 4, 99);
	EXPECT_FALSE(Durin::DecodeSkeletalMeshCookedProduct(
		Incompatible, Skeleton->GetBones(), BindTransform, Slots, Summary,
		Rejected, ProductError));
	EXPECT_EQ(ProductError.Category, Durin::ECookedMeshProductFailure::Schema);
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
	EXPECT_FALSE(Mesh->SetAssetData({
		.Skeleton = Skeleton,
		.SkeletonCompatibilityIdentity = Skeleton->GetCompatibilityIdentity(),
		.MeshNodeBindTransform = MakeTransform(),
		.MaterialSlots = {{.Name = Durin::FName("Body"), .SourceMaterialIndex = 0}},
		.Payload = std::move(InvalidPayload)}, Error));
	EXPECT_EQ(Mesh->GetRenderData(), Previous);
	EXPECT_EQ(Mesh->GetRenderData()->IndexBuffer.GetIndices(),
		std::vector<uint32>({0, 1, 2}));
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
	Durin::FSkeletalMeshAssetData InvalidMesh{
		.Skeleton = Skeleton,
		.SkeletonCompatibilityIdentity = Skeleton->GetCompatibilityIdentity(),
		.MeshNodeBindTransform = MakeTransform(),
		.MaterialSlots = {{.Name = Durin::FName("Body"), .SourceMaterialIndex = 0}},
		.Payload = InvalidPayload};
	std::string Error;
	EXPECT_FALSE(Mesh->SetAssetData(std::move(InvalidMesh), Error));
	EXPECT_EQ(Mesh->GetSummary(), MeshSummary);

	auto* Clip = Durin::NewObject<Durin::DAnimationClip>(nullptr, "PayloadClip");
	InitializeClip(*Clip, *Skeleton);
	const Durin::FAnimationClipSummary ClipSummary = Clip->GetSummary();
	auto InvalidClipPayload = std::make_shared<Durin::FAnimationClipPayloadData>(*MakeClipPayload());
	InvalidClipPayload->Tracks[0].Times[1] = 0.0f;
	Durin::FAnimationClipAssetData InvalidClip{
		.Skeleton = Skeleton,
		.SkeletonCompatibilityIdentity = Skeleton->GetCompatibilityIdentity(),
		.ClipName = Durin::FName("Invalid"),
		.Payload = InvalidClipPayload};
	EXPECT_FALSE(Clip->SetAssetData(std::move(InvalidClip), Error));
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

TEST(FSkeletalAssetTests, ProspectiveSkeletonValidatesDetachedDependentProducts)
{
	InitializeDObjectSystem();
	auto* CurrentSkeleton = Durin::NewObject<Durin::DSkeleton>(
		nullptr, "CurrentSkeleton");
	auto* ProspectiveSkeleton = Durin::NewObject<Durin::DSkeleton>(
		nullptr, "ProspectiveSkeleton");
	InitializeSkeleton(*CurrentSkeleton);
	InitializeSkeleton(*ProspectiveSkeleton, MakeAlternateBones());
	std::string Error;

	auto* CandidateMesh = Durin::NewObject<Durin::DSkeletalMesh>(
		nullptr, "CandidateMesh");
	ASSERT_TRUE(CandidateMesh->SetAssetData({
		.Skeleton = CurrentSkeleton,
		.ValidationSkeleton = ProspectiveSkeleton,
		.SkeletonCompatibilityIdentity =
			ProspectiveSkeleton->GetCompatibilityIdentity(),
		.MeshNodeBindTransform = MakeTransform(),
		.MaterialSlots = {{
			.Name = Durin::FName("Armor"), .SourceMaterialIndex = 0}},
		.Payload = MakeMeshPayload()}, Error)) << Error;
	EXPECT_FALSE(CandidateMesh->Validate(Error));
	Error.clear();
	EXPECT_TRUE(CandidateMesh->ValidateAgainstSkeleton(
		*ProspectiveSkeleton, Error)) << Error;

	auto* CandidateClip = Durin::NewObject<Durin::DAnimationClip>(
		nullptr, "CandidateClip");
	ASSERT_TRUE(CandidateClip->SetAssetData({
		.Skeleton = CurrentSkeleton,
		.ValidationSkeleton = ProspectiveSkeleton,
		.SkeletonCompatibilityIdentity =
			ProspectiveSkeleton->GetCompatibilityIdentity(),
		.ClipName = Durin::FName("Run"),
		.Payload = MakeClipPayload(2.0f)}, Error)) << Error;
	EXPECT_FALSE(CandidateClip->Validate(Error));
	Error.clear();
	EXPECT_TRUE(CandidateClip->ValidateAgainstSkeleton(
		*ProspectiveSkeleton, Error)) << Error;
}

TEST(FSkeletalAssetTests, EngineProviderPathSkipsWarmRecipesAndRecoversCorruptPayloads)
{
	using namespace Durin;
	InitializeDObjectSystem();
	struct FCacheRestore
	{
		std::string Previous = FPaths::DerivedDataCacheDir();
		~FCacheRestore() { FPaths::SetDerivedDataCacheDirForTests(Previous); }
	} CacheRestore;
	const auto Root = Testing::CreateTestFixtureDirectory("SkeletalEngineProvider");
	FPaths::SetDerivedDataCacheDirForTests(Root.generic_string());
	auto& Shared = GetTestProvider();
	Shared.Registration.Reset();
	struct FRestoreProvider
	{
		FTestSkeletalProvider& State;
		~FRestoreProvider()
		{
			State.Registration = State.Owner.RegisterFeature<ISkeletalBuildProvider>(State.Provider);
		}
	} RestoreProvider{Shared};
	FModuleTestOwner Owner("SkeletalEngineProviderTests");
	FCountingSkeletalBuildProvider Provider;
	auto Registration = Owner.RegisterFeature<ISkeletalBuildProvider>(Provider);
	ASSERT_TRUE(Registration.IsValid());
	const FSkeletalPayloadSerializationContext Context{
		.SkeletonBoneCount = 5, .MaterialSlotCount = 1,
		.TargetPlatform = ESkeletalPayloadTargetPlatform::Win64,
		.TargetProfile = ESkeletalPayloadTargetProfile::Game};
	FSkeletalMeshDerivedDataRequest MeshRequest{
		.ImportedDataIdentity = FXxHash128::HashBuffer("skeletal-engine-provider"),

		.SkeletonCompatibilityIdentity = "skeleton-compatibility",
		.Context = Context, .Payload = MakeMeshPayload()};
	FAnimationClipDerivedDataRequest ClipRequest{
		.ImportedDataIdentity = FXxHash128::HashBuffer("animation-engine-provider"),

		.SkeletonCompatibilityIdentity = "skeleton-compatibility",
		.Context = Context, .Payload = MakeClipPayload()};
	FSkeletalMeshDerivedDataResult Mesh;
	FAnimationClipDerivedDataResult Clip;
	std::string Error;
	ASSERT_TRUE(BuildSkeletalMeshDerivedData(MeshRequest, Mesh, Error)) << Error;
	ASSERT_TRUE(BuildAnimationClipDerivedData(ClipRequest, Clip, Error)) << Error;
	EXPECT_EQ(Mesh.Origin, ESkeletalDerivedDataOrigin::Rebuilt);
	EXPECT_EQ(Clip.Origin, ESkeletalDerivedDataOrigin::Rebuilt);
	EXPECT_EQ(Provider.MeshBuilds, 1u);
	EXPECT_EQ(Provider.ClipBuilds, 1u);
	const auto MeshPath = Root / "SkeletalMesh/Objects" / Mesh.Key.substr(0, 2) / (Mesh.Key + ".bin");
	const auto ClipPath = Root / "AnimationClip/Objects" / Clip.Key.substr(0, 2) / (Clip.Key + ".bin");
	FByteArray MeshBytes;
	FByteArray ClipBytes;
	ASSERT_TRUE(FFileHelper::LoadFileToArray(MeshBytes, MeshPath));
	ASSERT_TRUE(FFileHelper::LoadFileToArray(ClipBytes, ClipPath));
	ASSERT_TRUE(BuildSkeletalMeshDerivedData(MeshRequest, Mesh, Error)) << Error;
	ASSERT_TRUE(BuildAnimationClipDerivedData(ClipRequest, Clip, Error)) << Error;
	EXPECT_EQ(Mesh.Origin, ESkeletalDerivedDataOrigin::CacheHit);
	EXPECT_EQ(Clip.Origin, ESkeletalDerivedDataOrigin::CacheHit);
	EXPECT_EQ(Provider.MeshBuilds, 1u);
	EXPECT_EQ(Provider.ClipBuilds, 1u);
	FByteArray CorruptMesh = MeshBytes;
	const auto ExpectedMesh = MeshRequest.Payload;
	const auto ExpectedClip = ClipRequest.Payload;
	uint32 MeshLoads = 0;
	uint32 ClipLoads = 0;
	MeshRequest.Payload.reset();
	ClipRequest.Payload.reset();
	MeshRequest.LoadPayload = [&](std::string&) { ++MeshLoads; return ExpectedMesh; };
	ClipRequest.LoadPayload = [&](std::string&) { ++ClipLoads; return ExpectedClip; };
	ASSERT_TRUE(BuildSkeletalMeshDerivedData(MeshRequest, Mesh, Error)) << Error;
	ASSERT_TRUE(BuildAnimationClipDerivedData(ClipRequest, Clip, Error)) << Error;
	EXPECT_EQ(MeshLoads, 0u);
	EXPECT_EQ(ClipLoads, 0u);
	CorruptMesh.push_back(std::byte{1});
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(CorruptMesh, MeshPath));
	FByteArray CorruptClip = ClipBytes;
	CorruptClip.resize(CorruptClip.size() / 2);
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(CorruptClip, ClipPath));
	ASSERT_TRUE(BuildSkeletalMeshDerivedData(MeshRequest, Mesh, Error)) << Error;
	ASSERT_TRUE(BuildAnimationClipDerivedData(ClipRequest, Clip, Error)) << Error;
	EXPECT_EQ(Provider.MeshBuilds, 2u);
	EXPECT_EQ(Provider.ClipBuilds, 2u);
	EXPECT_EQ(MeshLoads, 1u);
	EXPECT_EQ(ClipLoads, 1u);
	EXPECT_FALSE(Mesh.Diagnostic.empty());
	EXPECT_FALSE(Clip.Diagnostic.empty());
	EXPECT_TRUE(Error.empty());
	EXPECT_EQ(*Mesh.Payload, *ExpectedMesh);
	EXPECT_EQ(*Clip.Payload, *ExpectedClip);
	FByteArray Rebuilt;
	ASSERT_TRUE(FFileHelper::LoadFileToArray(Rebuilt, MeshPath));
	EXPECT_EQ(Rebuilt, MeshBytes);
	ASSERT_TRUE(FFileHelper::LoadFileToArray(Rebuilt, ClipPath));
	EXPECT_EQ(Rebuilt, ClipBytes);
	uint32 CancellationChecks = 0;
	MeshRequest.ShouldCancel = [&] { return ++CancellationChecks >= 2; };
	EXPECT_FALSE(BuildSkeletalMeshDerivedData(MeshRequest, Mesh, Error));
	EXPECT_EQ(Mesh.Payload, nullptr);
	EXPECT_NE(Error.find("canceled"), std::string::npos);
	EXPECT_EQ(Provider.MeshBuilds, 2u);
	CancellationChecks = 0;
	ClipRequest.ShouldCancel = [&] { return ++CancellationChecks >= 2; };
	EXPECT_FALSE(BuildAnimationClipDerivedData(ClipRequest, Clip, Error));
	EXPECT_EQ(Clip.Payload, nullptr);
	EXPECT_NE(Error.find("canceled"), std::string::npos);
	EXPECT_EQ(Provider.ClipBuilds, 2u);
	MeshRequest.ShouldCancel = {};
	Registration.Reset();
	EXPECT_FALSE(BuildSkeletalMeshDerivedData(MeshRequest, Mesh, Error));
	EXPECT_EQ(Mesh.Payload, nullptr);
	EXPECT_NE(Error.find("unavailable"), std::string::npos);
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
	Durin::FByteArray MeshBytes;
	Durin::FByteArray RepeatedMeshBytes;
	Durin::FByteArray ClipBytes;
	Durin::FByteArray RepeatedClipBytes;
	std::string Error;
	ASSERT_TRUE(SerializeMeshPayload(*MeshPayload, *Skeleton, 1, MeshBytes, Error)) << Error;
	ASSERT_TRUE(SerializeMeshPayload(*MeshPayload, *Skeleton, 1, RepeatedMeshBytes, Error)) << Error;
	ASSERT_TRUE(SerializeClipPayload(*ClipPayload, *Skeleton, ClipBytes, Error)) << Error;
	ASSERT_TRUE(SerializeClipPayload(*ClipPayload, *Skeleton, RepeatedClipBytes, Error)) << Error;
	EXPECT_EQ(MeshBytes, RepeatedMeshBytes);
	EXPECT_EQ(ClipBytes, RepeatedClipBytes);
	EXPECT_EQ(Durin::FXxHash128::HashBuffer(MeshBytes).ToString(),
		"c9b34549e31c18d6d80fffe38134e18a");
	EXPECT_EQ(Durin::FXxHash128::HashBuffer(ClipBytes).ToString(),
		"46de1c43345e7a1a6e0957742c7dcbe5");
	ASSERT_GE(MeshBytes.size(), Durin::SkeletalPayloadHeaderSize);
	ASSERT_GE(ClipBytes.size(), Durin::SkeletalPayloadHeaderSize);
	EXPECT_EQ(MeshBytes[0], std::byte{});
	EXPECT_EQ(MeshBytes[1], std::byte{});
	EXPECT_EQ(MeshBytes[2], std::byte{});
	EXPECT_EQ(MeshBytes[3], std::byte{});
	EXPECT_EQ(ClipBytes[0], std::byte{});
	EXPECT_EQ(ClipBytes[1], std::byte{});
	EXPECT_EQ(ClipBytes[2], std::byte{});
	EXPECT_EQ(ClipBytes[3], std::byte{});

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
	Durin::FSkeletalMeshBuildKeyInput MeshKeyInput;
	auto& KeyInput = static_cast<Durin::FSkeletalBuildKeyFields&>(MeshKeyInput);
	KeyInput.ProviderIdentity = "Durin.Scene";
	KeyInput.ProviderVersion = 3;
	KeyInput.ImportedDataIdentity = Durin::FXxHash128::HashBuffer("imported-data");
	KeyInput.PayloadInputFingerprint = MeshFingerprint;

	KeyInput.SkeletonCompatibilityIdentity = Skeleton->GetCompatibilityIdentity();
	KeyInput.TargetPlatform = Durin::ESkeletalPayloadTargetPlatform::Win64;
	KeyInput.TargetProfile = Durin::ESkeletalPayloadTargetProfile::Game;
	const std::string MeshKey = Durin::BuildSkeletalMeshDerivedDataKey(
		MeshKeyInput, Error);
	EXPECT_EQ(MeshKey, "42cb71c0c15ef952b4b48f8c295b9d47");
	EXPECT_EQ(MeshKey.size(), 32u);
	EXPECT_EQ(Durin::BuildSkeletalMeshDerivedDataKey(MeshKeyInput, Error), MeshKey);
	Durin::FAnimationClipBuildKeyInput ClipKeyInput;
	static_cast<Durin::FSkeletalBuildKeyFields&>(ClipKeyInput) = KeyInput;
	ClipKeyInput.PayloadInputFingerprint = ClipFingerprint;

	const std::string ClipKey = Durin::BuildAnimationClipDerivedDataKey(
		ClipKeyInput, Error);
	EXPECT_EQ(ClipKey, "b668252374be109c2c2084c947ac0718");
	EXPECT_EQ(ClipKey.size(), 32u);
	EXPECT_NE(ClipKey, MeshKey);
	// Schema 4 changes only the version and removes the old path field.
	// Reconstruct schema 3 to check both frozen legacy identities independently.
	auto LegacyKey = [&](Durin::FByteArray Bytes, std::string_view Path) {
		EXPECT_EQ(Bytes.front(), std::byte{4});
		WriteWireU32(Bytes, 0, 3);
		Durin::FByteArray PathBytes(4 + Path.size());
		WriteWireU32(PathBytes, 0, static_cast<uint32>(Path.size()));
		std::memcpy(PathBytes.data() + 4, Path.data(), Path.size());
		Bytes.insert(Bytes.end() - 4 - KeyInput.SkeletonCompatibilityIdentity.size(),
			PathBytes.begin(), PathBytes.end());
		return Durin::FXxHash128::HashBuffer(Bytes).ToString();
	};
	EXPECT_EQ(LegacyKey(Durin::BuildSkeletalMeshDerivedDataKeyBytes(MeshKeyInput, Error),
		"skeletal-mesh:node/1/mesh/0"), "660abdcaef3c3096b0cee1e1655f6bc8");
	EXPECT_EQ(LegacyKey(Durin::BuildAnimationClipDerivedDataKeyBytes(ClipKeyInput, Error),
		"animation-clip:animation/0/skin/0"), "f1949ebcd286b684b0246923ca55d328");
}

TEST(FSkeletalAssetTests, PayloadDecodersRejectMalformedContainersTransactionally)
{
	InitializeDObjectSystem();
	auto* Skeleton = Durin::NewObject<Durin::DSkeleton>(nullptr, "MalformedCodecSkeleton");
	InitializeSkeleton(*Skeleton);
	Durin::FByteArray MeshBytes;
	Durin::FByteArray ClipBytes;
	std::string Error;
	ASSERT_TRUE(SerializeMeshPayload(*MakeMeshPayload(), *Skeleton, 1, MeshBytes, Error)) << Error;
	ASSERT_TRUE(SerializeClipPayload(*MakeClipPayload(), *Skeleton, ClipBytes, Error)) << Error;
	const Durin::FSkeletalMeshPayloadData MeshSentinel = *MakeMeshPayload();
	const Durin::FAnimationClipPayloadData ClipSentinel = *MakeClipPayload();
	auto ExpectMeshFailure = [&](Durin::FByteArray Corrupt) {
		Durin::FSkeletalMeshPayloadData Output = MeshSentinel;
		EXPECT_FALSE(DeserializePayload(Corrupt, {
			.SkeletonBoneCount = Skeleton->GetBoneCount(),
			.MaterialSlotCount = 1,
			.TargetPlatform = Durin::ESkeletalPayloadTargetPlatform::Win64,
			.TargetProfile = Durin::ESkeletalPayloadTargetProfile::Game}, Output));
		EXPECT_EQ(Output, MeshSentinel);
	};
	auto ExpectClipFailure = [&](Durin::FByteArray Corrupt) {
		Durin::FAnimationClipPayloadData Output = ClipSentinel;
		EXPECT_FALSE(DeserializePayload(Corrupt, {
			.SkeletonBoneCount = Skeleton->GetBoneCount(),
			.TargetPlatform = Durin::ESkeletalPayloadTargetPlatform::Win64,
			.TargetProfile = Durin::ESkeletalPayloadTargetProfile::Game}, Output));
		EXPECT_EQ(Output, ClipSentinel);
	};

	auto WrongMagic = MeshBytes;
	WrongMagic[0] ^= std::byte{0xff};
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
	const uint64 FirstOffset = ReadWireU64(
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

TEST(FSkeletalAssetTests, AuthoredReloadReusesCanonicalKeysAndRecoversMissingOrCorruptDerivedData)
{
	const std::filesystem::path Root = InitializeAssetMount();
	const std::filesystem::path CacheRoot = Root / "DerivedDataCache";
	Durin::FPaths::SetDerivedDataCacheDirForTests(CacheRoot.generic_string());
	Durin::FPackagePath SkeletonPath;
	Durin::FPackagePath MeshPath;
	Durin::FPackagePath ClipPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/SkeletalAssetTests/DdcSkeleton", SkeletonPath));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/SkeletalAssetTests/DdcMesh", MeshPath));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/SkeletalAssetTests/DdcClip", ClipPath));
	std::string MeshKey = Durin::FXxHash128::HashBuffer("mesh-ddc-key").ToString();
	std::string ClipKey = Durin::FXxHash128::HashBuffer("clip-ddc-key").ToString();

	Durin::DSkeleton* Skeleton = nullptr;
	Durin::DSkeletalMesh* Mesh = nullptr;
	Durin::DAnimationClip* Clip = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(SkeletonPath, Skeleton));
	InitializeSkeleton(*Skeleton);
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(MeshPath, Mesh));
	InitializeMesh(*Mesh, *Skeleton, "Body", MeshKey);
	MeshKey = GetAssetKey(*Mesh);
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(ClipPath, Clip));
	InitializeClip(*Clip, *Skeleton, "Walk", 1.0f, ClipKey);
	ClipKey = GetAssetKey(*Clip);
	const Durin::FSkeletalMeshPayloadData ExpectedMesh = *Mesh->GetPayloadData();
	const Durin::FAnimationClipPayloadData ExpectedClip = *Clip->GetPayloadData();
	ASSERT_TRUE(Durin::SavePackage(Skeleton->GetPackage()));
	ASSERT_TRUE(Durin::SavePackage(Mesh->GetPackage()));
	ASSERT_TRUE(Durin::SavePackage(Clip->GetPackage()));
	ASSERT_TRUE(Durin::UnloadPackage(ClipPath));
	ASSERT_TRUE(Durin::UnloadPackage(MeshPath));
	ASSERT_TRUE(Durin::UnloadPackage(SkeletonPath));

	const Durin::FAssetResult MeshLoad = Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(MeshPath), Mesh);
	ASSERT_TRUE(MeshLoad) << MeshLoad.Message;
	ASSERT_NE(Mesh, nullptr);
	ASSERT_NE(Mesh->GetPayloadData(), nullptr);
	EXPECT_EQ(*Mesh->GetPayloadData(), ExpectedMesh);
	EXPECT_EQ(GetAssetKey(*Mesh), MeshKey);
	MeshKey = GetAssetKey(*Mesh);
	ASSERT_TRUE(Durin::UnloadPackage(MeshPath));
	ASSERT_TRUE(Durin::UnloadPackage(SkeletonPath));
	ASSERT_TRUE(Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(MeshPath), Mesh));
	EXPECT_EQ(GetAssetKey(*Mesh), MeshKey);
	ASSERT_TRUE(Durin::UnloadPackage(MeshPath));
	ASSERT_TRUE(Durin::UnloadPackage(SkeletonPath));
	ASSERT_TRUE(Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(ClipPath), Clip));
	ASSERT_NE(Clip, nullptr);
	ASSERT_NE(Clip->GetPayloadData(), nullptr);
	EXPECT_EQ(*Clip->GetPayloadData(), ExpectedClip);
	EXPECT_EQ(GetAssetKey(*Clip), ClipKey);
	ClipKey = GetAssetKey(*Clip);
	ASSERT_TRUE(Durin::UnloadPackage(ClipPath));
	ASSERT_TRUE(Durin::UnloadPackage(SkeletonPath));
	ASSERT_TRUE(Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(ClipPath), Clip));
	EXPECT_EQ(GetAssetKey(*Clip), ClipKey);
	ASSERT_TRUE(Durin::UnloadPackage(ClipPath));
	ASSERT_TRUE(Durin::UnloadPackage(SkeletonPath));
	const std::filesystem::path ClipObject = CacheRoot / "AnimationClip/Objects"
		/ ClipKey.substr(0, 2) / (ClipKey + ".bin");
	ASSERT_TRUE(std::filesystem::remove(ClipObject));
	ASSERT_TRUE(Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(ClipPath), Clip));
	ASSERT_NE(Clip, nullptr);
	ASSERT_NE(Clip->GetPayloadData(), nullptr);
	EXPECT_EQ(*Clip->GetPayloadData(), ExpectedClip);
	ASSERT_TRUE(Durin::UnloadPackage(ClipPath));
	ASSERT_TRUE(Durin::UnloadPackage(SkeletonPath));

	const std::filesystem::path MeshObject = CacheRoot / "SkeletalMesh/Objects"
		/ MeshKey.substr(0, 2) / (MeshKey + ".bin");
	Durin::FByteArray CorruptBytes;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(CorruptBytes, MeshObject));
	ASSERT_FALSE(CorruptBytes.empty());
	CorruptBytes.back() ^= std::byte{0x80};
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(
		std::as_bytes(std::span(CorruptBytes)), MeshObject));
	Mesh = nullptr;
	const Durin::FAssetResult CorruptLoad = Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(MeshPath), Mesh);
	ASSERT_TRUE(CorruptLoad) << CorruptLoad.Message;
	ASSERT_NE(Mesh, nullptr);
	ASSERT_NE(Mesh->GetPayloadData(), nullptr);
	EXPECT_EQ(*Mesh->GetPayloadData(), ExpectedMesh);
	Durin::FPaths::SetDerivedDataCacheDirForTests({});
}

TEST(FSkeletalAssetTests, RelocationReusesIndependentOutputsAndMissingAuthoredBulkFailsLoad)
{
	const std::filesystem::path Root = InitializeAssetMount();
	Durin::FPaths::SetDerivedDataCacheDirForTests(
		(Root / "RelocationDerivedDataCache").generic_string());
	Durin::FPackagePath SkeletonPath;
	Durin::FPackagePath MeshPath;
	Durin::FPackagePath ClipPath;
	Durin::FPackagePath RelocatedMeshPath;
	Durin::FPackagePath RelocatedClipPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/SkeletalAssetTests/RelocationSkeleton", SkeletonPath));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/SkeletalAssetTests/RelocationMesh", MeshPath));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/SkeletalAssetTests/RelocationClip", ClipPath));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/SkeletalAssetTests/Moved/RelocationMesh", RelocatedMeshPath));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/SkeletalAssetTests/Moved/RelocationClip", RelocatedClipPath));

	Durin::DSkeleton* Skeleton = nullptr;
	Durin::DSkeletalMesh* Mesh = nullptr;
	Durin::DAnimationClip* Clip = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(SkeletonPath, Skeleton));
	InitializeSkeleton(*Skeleton);
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(MeshPath, Mesh));
	InitializeMesh(*Mesh, *Skeleton);
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(ClipPath, Clip));
	Durin::FAnimationClipAssetData ClipCandidate{
		.Skeleton = Skeleton,
		.SkeletonCompatibilityIdentity = Skeleton->GetCompatibilityIdentity(),
		.ClipName = Durin::FName("LargeClip"),
		.Payload = MakeLargeClipPayload()};
	std::string Error;
	ASSERT_TRUE(Clip->SetAssetData(std::move(ClipCandidate), Error)) << Error;
	const Durin::FXxHash128 MeshImportedIdentity =
		Mesh->GetImportedData().GetIdentity();
	const Durin::FXxHash128 ClipImportedIdentity =
		Clip->GetImportedData().GetIdentity();
	ASSERT_TRUE(Durin::SavePackage(Skeleton->GetPackage()));
	ASSERT_TRUE(Durin::SavePackage(Mesh->GetPackage()));
	ASSERT_TRUE(Durin::SavePackage(Clip->GetPackage()));
	const Durin::FAssetCatalogEntry ClipEntry =
		Durin::FindAssetExact(ClipPath);
	ASSERT_TRUE(ClipEntry);
	Durin::FAssetPackageInspection Inspection;
	ASSERT_TRUE(Durin::InspectAssetPackage(
		ClipEntry->PhysicalPath, Inspection));
	std::vector<std::filesystem::path> Companions;
	ASSERT_TRUE(Durin::InspectEditorBulkDataCompanionPaths(
		ClipEntry->PhysicalPath, Inspection, Companions, &Error)) << Error;
	ASSERT_EQ(Companions.size(), 1u);
	EXPECT_TRUE(std::filesystem::is_regular_file(Companions.front()));
	ASSERT_TRUE(Durin::UnloadPackage(ClipPath));
	ASSERT_TRUE(Durin::UnloadPackage(MeshPath));
	ASSERT_TRUE(Durin::UnloadPackage(SkeletonPath));

	ASSERT_TRUE(Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(MeshPath), Mesh));
	const std::string OriginalMeshKey = GetAssetKey(*Mesh);
	ASSERT_TRUE(Durin::UnloadPackage(MeshPath));
	ASSERT_TRUE(Durin::UnloadPackage(SkeletonPath));
	ASSERT_TRUE(Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(ClipPath), Clip));
	const std::string OriginalClipKey = GetAssetKey(*Clip);
	ASSERT_TRUE(Durin::UnloadPackage(ClipPath));
	ASSERT_TRUE(Durin::UnloadPackage(SkeletonPath));

	const std::array Mappings{
		Durin::FAssetRelocationMapping{MeshPath, RelocatedMeshPath},
		Durin::FAssetRelocationMapping{ClipPath, RelocatedClipPath}};
	Durin::FAssetRelocationSummary Summary;
	Durin::FAssetMutationJob Transaction;
	ASSERT_TRUE(Durin::PrepareAssetRelocationJob(
		Mappings, Summary, Transaction));
	ASSERT_TRUE(Transaction.ResumeForward());

	ASSERT_TRUE(Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(RelocatedMeshPath), Mesh));
	EXPECT_EQ(Mesh->GetImportedData().GetIdentity(), MeshImportedIdentity);
	EXPECT_EQ(GetAssetKey(*Mesh), OriginalMeshKey);
	ASSERT_TRUE(Durin::UnloadPackage(RelocatedMeshPath));
	ASSERT_TRUE(Durin::UnloadPackage(SkeletonPath));
	ASSERT_TRUE(Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(RelocatedClipPath), Clip));
	EXPECT_EQ(Clip->GetImportedData().GetIdentity(), ClipImportedIdentity);
	EXPECT_EQ(GetAssetKey(*Clip), OriginalClipKey);
	ASSERT_TRUE(Durin::UnloadPackage(RelocatedClipPath));
	ASSERT_TRUE(Durin::UnloadPackage(SkeletonPath));

	const Durin::FAssetCatalogEntry RelocatedClipEntry =
		Durin::FindAssetExact(RelocatedClipPath);
	ASSERT_TRUE(RelocatedClipEntry);
	Inspection = {};
	ASSERT_TRUE(Durin::InspectAssetPackage(
		RelocatedClipEntry->PhysicalPath, Inspection));
	Companions.clear();
	ASSERT_TRUE(Durin::InspectEditorBulkDataCompanionPaths(
		RelocatedClipEntry->PhysicalPath, Inspection, Companions, &Error)) << Error;
	ASSERT_EQ(Companions.size(), 1u);
	std::filesystem::path HeldCompanion = Companions.front();
	HeldCompanion += ".held";
	std::filesystem::rename(Companions.front(), HeldCompanion);
	Clip = nullptr;
	const Durin::FAssetResult MissingBulk =
		Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(RelocatedClipPath), Clip);
	EXPECT_FALSE(MissingBulk);
	EXPECT_EQ(Clip, nullptr);
	std::filesystem::rename(HeldCompanion, Companions.front());
	ASSERT_TRUE(Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(RelocatedClipPath), Clip));
	EXPECT_EQ(Clip->GetImportedData().GetIdentity(), ClipImportedIdentity);
	const Durin::FPackageResourceHandle WarmResource =
		Durin::GetPackageResourceManager().FindPackage(
			RelocatedClipPath.ToString());
	ASSERT_TRUE(WarmResource);
	EXPECT_EQ(WarmResource->GetReadStats().RequestCount, 0u);
	ASSERT_TRUE(Durin::UnloadPackage(RelocatedClipPath));
	ASSERT_TRUE(Durin::UnloadPackage(SkeletonPath));
	Durin::FPaths::SetDerivedDataCacheDirForTests({});
}

TEST(FSkeletalAssetTests, AuthoredLoadRebuildsCompleteDependencyGraphWithoutDerivedData)
{
	const std::filesystem::path Root = InitializeAssetMount();
	const std::filesystem::path CacheRoot = Root / "MigrationDerivedDataCache";
	Durin::FPaths::SetDerivedDataCacheDirForTests(CacheRoot.generic_string());
	Durin::FPackagePath SkeletonPath;
	Durin::FPackagePath MeshPath;
	Durin::FPackagePath ClipPath;
	Durin::FPackagePath LevelPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/SkeletalAssetTests/MigrationSkeleton", SkeletonPath));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/SkeletalAssetTests/MigrationMesh", MeshPath));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/SkeletalAssetTests/MigrationClip", ClipPath));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/SkeletalAssetTests/MigrationLevel", LevelPath));
	std::string MeshKey = Durin::FXxHash128::HashBuffer("migration-mesh-key").ToString();
	std::string ClipKey = Durin::FXxHash128::HashBuffer("migration-clip-key").ToString();

	Durin::DSkeleton* Skeleton = nullptr;
	Durin::DSkeletalMesh* Mesh = nullptr;
	Durin::DAnimationClip* Clip = nullptr;
	Durin::DLevel* Level = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(SkeletonPath, Skeleton));
	InitializeSkeleton(*Skeleton);
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(MeshPath, Mesh));
	InitializeMesh(*Mesh, *Skeleton, "Body", MeshKey);
	MeshKey = GetAssetKey(*Mesh);
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(ClipPath, Clip));
	InitializeClip(*Clip, *Skeleton, "Walk", 1.0f, ClipKey);
	ClipKey = GetAssetKey(*Clip);
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(LevelPath, Level));
	auto* Actor = Level->SpawnActor<Durin::ASkeletalMeshActor>("AnimatedActor");
	ASSERT_NE(Actor, nullptr);
	std::string Error;
	ASSERT_TRUE(Actor->GetSkeletalMeshComponent()->SetSkeletalMesh(Mesh, Error)) << Error;
	ASSERT_TRUE(Actor->GetSkeletalMeshComponent()->SetAnimationClip(Clip, Error)) << Error;
	ASSERT_TRUE(Durin::SavePackage(Skeleton->GetPackage()));
	ASSERT_TRUE(Durin::SavePackage(Mesh->GetPackage()));
	ASSERT_TRUE(Durin::SavePackage(Clip->GetPackage()));
	ASSERT_TRUE(Durin::SavePackage(Level->GetPackage()));
	ASSERT_TRUE(Durin::UnloadPackage(LevelPath));
	ASSERT_TRUE(Durin::UnloadPackage(ClipPath));
	ASSERT_TRUE(Durin::UnloadPackage(MeshPath));
	ASSERT_TRUE(Durin::UnloadPackage(SkeletonPath));
	ASSERT_TRUE(std::filesystem::remove(CacheRoot / "SkeletalMesh/Objects"
		/ MeshKey.substr(0, 2) / (MeshKey + ".bin")));
	ASSERT_TRUE(std::filesystem::remove(CacheRoot / "AnimationClip/Objects"
		/ ClipKey.substr(0, 2) / (ClipKey + ".bin")));

	Durin::DLevel* ReloadedLevel = nullptr;
	const Durin::FAssetResult Load =
		Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(LevelPath), ReloadedLevel);
	ASSERT_TRUE(Load) << Load.Message;
	ASSERT_NE(ReloadedLevel, nullptr);
	auto* ReloadedActor = Durin::Cast<Durin::ASkeletalMeshActor>(
		ReloadedLevel->FindActorByName(Durin::FName("AnimatedActor")));
	ASSERT_NE(ReloadedActor, nullptr);
	ASSERT_NE(ReloadedActor->GetSkeletalMeshComponent()->GetSkeletalMesh(), nullptr);
	ASSERT_NE(ReloadedActor->GetSkeletalMeshComponent()->GetAnimationClip(), nullptr);
	EXPECT_NE(ReloadedActor->GetSkeletalMeshComponent()->GetSkeletalMesh()->GetPayloadData(), nullptr);
	EXPECT_NE(ReloadedActor->GetSkeletalMeshComponent()->GetAnimationClip()->GetPayloadData(), nullptr);
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
	const std::array<uint8, 1> BlockedBytes{0xff};
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(
		std::as_bytes(std::span(BlockedBytes)), BlockedRoot));
	Durin::FPaths::SetDerivedDataCacheDirForTests(BlockedRoot.generic_string());
	auto* Skeleton = Durin::NewObject<Durin::DSkeleton>(nullptr, "WriteFailureSkeleton");
	InitializeSkeleton(*Skeleton);
	auto* Mesh = Durin::NewObject<Durin::DSkeletalMesh>(nullptr, "WriteFailureMesh");
	InitializeMesh(*Mesh, *Skeleton, "Body",
		Durin::FXxHash128::HashBuffer("write-failure").ToString());
	ASSERT_NE(Mesh->GetPayloadData(), nullptr);
	Durin::FSkeletalMeshDerivedDataResult Product;
	std::string BuildError;
	ASSERT_TRUE(Durin::BuildSkeletalMeshDerivedData({
		.ImportedDataIdentity = Mesh->GetImportedData().GetIdentity(),
		.SkeletonCompatibilityIdentity = Skeleton->GetCompatibilityIdentity(),
		.Context = {.SkeletonBoneCount = Skeleton->GetBoneCount(), .MaterialSlotCount = 1,
					.TargetPlatform = Durin::ESkeletalPayloadTargetPlatform::Win64,
					.TargetProfile = Durin::ESkeletalPayloadTargetProfile::Game},
		.Payload = Mesh->GetPayloadData()}, Product, BuildError)) << BuildError;
	EXPECT_FALSE(Product.Diagnostic.empty());
	EXPECT_LE(Product.Diagnostic.size(), 2048u);
	EXPECT_NE(Product.Payload, nullptr);
	std::string Error;
	EXPECT_TRUE(Mesh->Validate(Error)) << Error;
	Durin::FPaths::SetDerivedDataCacheDirForTests({});
}

TEST(FSkeletalAssetTests, AuthoredPackagesRoundTripHardReferencesAndSummaries)
{
	InitializeAssetMount();
	Durin::FPackagePath SkeletonPath;
	Durin::FPackagePath MeshPath;
	Durin::FPackagePath ClipPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/SkeletalAssetTests/RoundTripSkeleton", SkeletonPath));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/SkeletalAssetTests/RoundTripMesh", MeshPath));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/SkeletalAssetTests/RoundTripClip", ClipPath));

	Durin::DSkeleton* Skeleton = nullptr;
	Durin::DSkeletalMesh* Mesh = nullptr;
	Durin::DAnimationClip* Clip = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(SkeletonPath, Skeleton));
	InitializeSkeleton(*Skeleton);
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(MeshPath, Mesh));
	InitializeMesh(*Mesh, *Skeleton);
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(ClipPath, Clip));
	InitializeClip(*Clip, *Skeleton);
	const std::string Identity = Skeleton->GetCompatibilityIdentity();
	const Durin::FSkeletalMeshSummary MeshSummary = Mesh->GetSummary();
	const Durin::FAnimationClipSummary ClipSummary = Clip->GetSummary();
	ASSERT_TRUE(Durin::SavePackage(Skeleton->GetPackage()));
	ASSERT_TRUE(Durin::SavePackage(Mesh->GetPackage()));
	ASSERT_TRUE(Durin::SavePackage(Clip->GetPackage()));
	ASSERT_TRUE(Durin::UnloadPackage(ClipPath));
	ASSERT_TRUE(Durin::UnloadPackage(MeshPath));
	ASSERT_TRUE(Durin::UnloadPackage(SkeletonPath));

	ASSERT_TRUE(Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(MeshPath), Mesh));
	ASSERT_NE(Mesh->GetSkeleton(), nullptr);
	EXPECT_EQ(Mesh->GetSkeleton()->GetCompatibilityIdentity(), Identity);
	EXPECT_EQ(Mesh->GetSkeletonCompatibilityIdentity(), Identity);
	EXPECT_EQ(Mesh->GetSummary(), MeshSummary);
	ASSERT_EQ(Mesh->GetMaterialSlots().size(), 1u);
	EXPECT_EQ(Mesh->GetMaterialSlots()[0].SourceName, "Body");
	EXPECT_NE(Mesh->GetPayloadData(), nullptr);
	ASSERT_TRUE(Durin::UnloadPackage(MeshPath));
	ASSERT_TRUE(Durin::UnloadPackage(SkeletonPath));

	ASSERT_TRUE(Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(ClipPath), Clip));
	ASSERT_NE(Clip->GetSkeleton(), nullptr);
	EXPECT_EQ(Clip->GetSkeletonCompatibilityIdentity(), Identity);
	EXPECT_EQ(Clip->GetSummary(), ClipSummary);
	ASSERT_EQ(Clip->GetSkeleton()->GetBones().size(), 5u);
	EXPECT_EQ(Clip->GetSkeleton()->GetBones()[4].Name, Durin::FName("Hand"));
	EXPECT_NE(Clip->GetPayloadData(), nullptr);
	ASSERT_TRUE(Durin::UnloadPackage(ClipPath));
	ASSERT_TRUE(Durin::UnloadPackage(SkeletonPath));
	ASSERT_TRUE(Durin::Testing::RemoveAssetPackageForTests(ClipPath));
	ASSERT_TRUE(Durin::Testing::RemoveAssetPackageForTests(MeshPath));
	ASSERT_TRUE(Durin::Testing::RemoveAssetPackageForTests(SkeletonPath));
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

	auto* MeshDuplicate = Durin::Cast<Durin::DSkeletalMesh>(
		Durin::DuplicateObject(Mesh, nullptr, "MeshDuplicate"));
	ASSERT_NE(MeshDuplicate, nullptr);
	EXPECT_EQ(MeshDuplicate->GetSkeleton(), Skeleton);
	EXPECT_EQ(MeshDuplicate->GetSummary(), Mesh->GetSummary());
	EXPECT_NE(MeshDuplicate->GetPayloadData(), nullptr);
	EXPECT_EQ(MeshDuplicate->GetImportedData().GetIdentity(),
		Mesh->GetImportedData().GetIdentity());
	auto* ClipDuplicate = Durin::Cast<Durin::DAnimationClip>(
		Durin::DuplicateObject(Clip, nullptr, "ClipDuplicate"));
	ASSERT_NE(ClipDuplicate, nullptr);
	EXPECT_EQ(ClipDuplicate->GetSkeleton(), Skeleton);
	EXPECT_EQ(ClipDuplicate->GetSummary(), Clip->GetSummary());
	EXPECT_NE(ClipDuplicate->GetPayloadData(), nullptr);
	EXPECT_EQ(ClipDuplicate->GetImportedData().GetIdentity(),
		Clip->GetImportedData().GetIdentity());
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
	Durin::Testing::RegisterMountPointForTests(
		"/Game/", ContentRoot.generic_string() + "/");
	Durin::FPaths::SetDerivedDataCacheDirForTests(CacheRoot.generic_string());
	Durin::FPackagePath SkeletonPath;
	Durin::FPackagePath MeshPath;
	Durin::FPackagePath ClipPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/Game/Skeleton", SkeletonPath));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/Game/Mesh", MeshPath));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/Game/Clip", ClipPath));
	Durin::DSkeleton* Skeleton = nullptr;
	Durin::DSkeletalMesh* Mesh = nullptr;
	Durin::DAnimationClip* Clip = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(SkeletonPath, Skeleton));
	InitializeSkeleton(*Skeleton);
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(MeshPath, Mesh));
	InitializeMesh(*Mesh, *Skeleton, "Body",
		Durin::FXxHash128::HashBuffer("cook-mesh").ToString());
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(ClipPath, Clip));
	InitializeClip(*Clip, *Skeleton, "Walk", 1.0f,
		Durin::FXxHash128::HashBuffer("cook-clip").ToString());
	const Durin::FSkeletalMeshPayloadData ExpectedMesh = *Mesh->GetPayloadData();
	const Durin::FAnimationClipPayloadData ExpectedClip = *Clip->GetPayloadData();
	ASSERT_TRUE(Durin::SavePackage(Skeleton->GetPackage()));
	ASSERT_TRUE(Durin::SavePackage(Mesh->GetPackage()));
	ASSERT_TRUE(Durin::SavePackage(Clip->GetPackage()));

	std::string Error;
	auto Cook = [&](const std::filesystem::path& CookRoot) {
		Durin::FCookContext Context(
			CookRoot, Durin::ECookTargetPlatform::Win64,
			Durin::ECookTargetProfile::Game);
		ASSERT_TRUE(Durin::ContributeEngineCookAsset(
			*Skeleton, "/Game/Skeleton", Context, Error)) << Error;
		ASSERT_TRUE(Durin::ContributeEngineCookAsset(
			*Mesh, "/Game/Mesh", Context, Error)) << Error;
		ASSERT_TRUE(Durin::ContributeEngineCookAsset(
			*Clip, "/Game/Clip", Context, Error)) << Error;
		ASSERT_TRUE(Context.Publish(&Error)) << Error;
	};
	Cook(FirstCookRoot);
	Cook(SecondCookRoot);

	for (const std::filesystem::path& Relative : {
		std::filesystem::path("Game/Skeleton.dasset"),
		std::filesystem::path("Game/Mesh.dasset"),
		std::filesystem::path("Game/Clip.dasset"),
		std::filesystem::path("CookManifest.bin")})
	{
		Durin::FByteArray FirstBytes;
		Durin::FByteArray SecondBytes;
		ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
			FirstBytes, (FirstCookRoot / Relative)));
		ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
			SecondBytes, (SecondCookRoot / Relative)));
		EXPECT_EQ(FirstBytes, SecondBytes) << Relative.generic_string();
		if (Relative.extension() == ".dasset")
		{
			EXPECT_FALSE(ContainsText(FirstBytes, "DerivedDataKey"));
			EXPECT_FALSE(ContainsText(FirstBytes, "SourceName"));
			EXPECT_FALSE(ContainsText(FirstBytes, "SourceMaterialIndex"));
			EXPECT_FALSE(ContainsText(FirstBytes, "cook-mesh"));
			EXPECT_FALSE(ContainsText(FirstBytes, "cook-clip"));
		}
	}
	EXPECT_FALSE(std::filesystem::exists(FirstCookRoot / "Game/Mesh.dbulk"));
	EXPECT_FALSE(std::filesystem::exists(FirstCookRoot / "Game/Clip.dbulk"));
	Durin::FAssetPackageInspection MeshInspection;
	Durin::FAssetPackageInspection ClipInspection;
	Durin::FPackagePath CookedMeshPath;
	Durin::FPackagePath CookedClipPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreateProjectContent(
		"/Game/Mesh", CookedMeshPath));
	ASSERT_TRUE(Durin::FPackagePath::TryCreateProjectContent(
		"/Game/Clip", CookedClipPath));
	ASSERT_TRUE(Durin::InspectAssetPackage(
		(FirstCookRoot / "Game/Mesh.dasset").generic_string(),
		CookedMeshPath, MeshInspection));
	ASSERT_TRUE(Durin::InspectAssetPackage(
		(FirstCookRoot / "Game/Clip.dasset").generic_string(),
		CookedClipPath, ClipInspection));
	EXPECT_NE(MeshInspection.FindField("PlatformData"), nullptr);
	EXPECT_NE(ClipInspection.FindField("PlatformData"), nullptr);

	ASSERT_TRUE(Durin::UnloadPackage(ClipPath));
	ASSERT_TRUE(Durin::UnloadPackage(MeshPath));
	ASSERT_TRUE(Durin::UnloadPackage(SkeletonPath));
	Durin::Testing::RemoveTestWorkDirectory(ContentRoot);
	Durin::Testing::RemoveTestWorkDirectory(CacheRoot);
	RestartAssetManager(FirstCookRoot);
	Durin::Testing::RegisterMountPointForTests(
		"/Game/", (FirstCookRoot / "Game").generic_string() + "/");
	ASSERT_TRUE(Durin::RefreshAssetRegistry(
		Durin::EAssetRegistryScanMode::FullValidation));
	Mesh = nullptr;
	ASSERT_TRUE(Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(MeshPath), Mesh));
	ASSERT_NE(Mesh, nullptr);
	ASSERT_NE(Mesh->GetSkeleton(), nullptr);
	EXPECT_NE(Mesh->GetCookedPlatformData().GetMetadata().LogicalSize, 0u);
	ASSERT_EQ(Mesh->GetRenderData(), nullptr);
	EXPECT_EQ(Mesh->RequestRenderDataAndResources().CpuPhase,
		Durin::ECookedMeshCpuPhase::Unloaded);
	Clip = nullptr;
	ASSERT_TRUE(Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(ClipPath), Clip));
	ASSERT_NE(Clip, nullptr);
	EXPECT_EQ(Clip->GetSkeleton(), Mesh->GetSkeleton());
	EXPECT_NE(Clip->GetCookedPlatformData().GetMetadata().LogicalSize, 0u);
	const Durin::DAnimationClip& ConstClip = *Clip;
	const auto ClipBulkState = Clip->GetCookedPlatformData().GetState();
	EXPECT_EQ(ConstClip.GetPayloadData(), nullptr);
	EXPECT_EQ(ConstClip.GetPayloadData(), nullptr);
	EXPECT_EQ(Clip->GetCookedPlatformData().GetState(), ClipBulkState);
	ASSERT_TRUE(Clip->EnsurePayloadLoadedBlocking());
	ASSERT_NE(Clip->GetPayloadData(), nullptr);
	EXPECT_EQ(*Clip->GetPayloadData(), ExpectedClip);
	const auto InstalledClip = Clip->GetPayloadData();
	ASSERT_TRUE(Clip->EnsurePayloadLoadedBlocking());
	EXPECT_EQ(Clip->GetPayloadData(), InstalledClip);
	auto* MissingClip = Durin::NewObject<Durin::DAnimationClip>(nullptr, "MissingCookedClipPayload");
	EXPECT_FALSE(MissingClip->EnsurePayloadLoadedBlocking());
	EXPECT_EQ(MissingClip->GetPayloadData(), nullptr);
	ASSERT_TRUE(Durin::UnloadPackage(ClipPath));
	Clip = nullptr;
	ASSERT_TRUE(Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(ClipPath), Clip));
	EXPECT_EQ(Clip->GetPayloadData(), nullptr);
	auto* FirstConsumer = Durin::NewObject<Durin::DSkeletalMeshComponent>(
		nullptr, Durin::FName("CookedSkeletalMeshFirstConsumer"));
	ASSERT_TRUE(Durin::InitializeCookedMeshLoadManager());
	ASSERT_TRUE(FirstConsumer->SetSkeletalMesh(Mesh, Error)) << Error;
	FirstConsumer->RegisterComponent();
	ASSERT_TRUE(FirstConsumer->SetAnimationClip(Clip, Error)) << Error;
	auto* ReassignedConsumer = Durin::NewObject<Durin::DSkeletalMeshComponent>(
		nullptr, Durin::FName("CookedSkeletalMeshReassignedConsumer"));
	ASSERT_TRUE(ReassignedConsumer->SetSkeletalMesh(Mesh, Error)) << Error;
	ReassignedConsumer->RegisterComponent();
	ASSERT_TRUE(ReassignedConsumer->SetAnimationClip(Clip, Error)) << Error;
	ASSERT_TRUE(ReassignedConsumer->SetAnimationClip(nullptr, Error)) << Error;
	ASSERT_TRUE(ReassignedConsumer->SetSkeletalMesh(nullptr, Error)) << Error;
	EXPECT_EQ(Mesh->RequestRenderDataAndResources().CpuPhase,
		Durin::ECookedMeshCpuPhase::IoQueued);
	EXPECT_EQ(FirstConsumer->CreateSceneProxy(), nullptr);
	Durin::ShutdownCookedMeshLoadManager();
	const auto CancelledStatus = Mesh->RequestRenderDataAndResources();
	EXPECT_EQ(CancelledStatus.CpuPhase, Durin::ECookedMeshCpuPhase::Cancelled);
	ASSERT_TRUE(Durin::InitializeCookedMeshLoadManager());
	const Durin::FCookedMeshBlockingResult RetryResult =
		Mesh->EnsureRenderDataLoadedBlocking();
	ASSERT_TRUE(RetryResult) << RetryResult.Message;
	EXPECT_GT(RetryResult.Status.Generation, CancelledStatus.Generation);
	auto FirstProxy = FirstConsumer->CreateSceneProxy();
	if (!FirstProxy) Durin::ShutdownCookedMeshLoadManager();
	ASSERT_NE(FirstProxy, nullptr) << RetryResult.Message;
	ASSERT_NE(Clip->GetPayloadData(), nullptr);
	EXPECT_EQ(*Clip->GetPayloadData(), ExpectedClip);
	EXPECT_EQ(ReassignedConsumer->GetSkeletalMesh(), nullptr);
	EXPECT_EQ(ReassignedConsumer->GetLatestPosePalette(), nullptr);
	EXPECT_EQ(ReassignedConsumer->CreateSceneProxy(), nullptr);
	ASSERT_TRUE(ReassignedConsumer->SetSkeletalMesh(Mesh, Error)) << Error;
	ASSERT_TRUE(ReassignedConsumer->SetAnimationClip(Clip, Error)) << Error;
	ASSERT_NE(ReassignedConsumer->CreateSceneProxy(), nullptr);
	ASSERT_NE(Mesh->GetPayloadData(), nullptr);
	ASSERT_NE(Mesh->GetRenderData(), nullptr);
	EXPECT_EQ(*Mesh->GetPayloadData(), ExpectedMesh);
	Durin::ShutdownCookedMeshLoadManager();

	ASSERT_TRUE(Durin::UnloadPackage(ClipPath));
	ASSERT_TRUE(Durin::UnloadPackage(MeshPath));
	ASSERT_TRUE(Durin::UnloadPackage(SkeletonPath));
	Durin::FByteArray MeshPackage;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		MeshPackage, FirstCookRoot / "Game/Mesh.dasset"));
	MeshPackage.back() ^= std::byte{0x80};
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(
		std::as_bytes(std::span(MeshPackage)), FirstCookRoot / "Game/Mesh.dasset"));
	RestartAssetManager(FirstCookRoot);
	Durin::Testing::RegisterMountPointForTests(
		"/Game/", (FirstCookRoot / "Game").generic_string() + "/");
	ASSERT_TRUE(Durin::RefreshAssetRegistry(
		Durin::EAssetRegistryScanMode::FullValidation));
	Durin::DSkeletalMesh* CorruptMesh = nullptr;
	EXPECT_FALSE(Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(MeshPath), CorruptMesh));
	EXPECT_EQ(CorruptMesh, nullptr);

	RestartAssetManager();
	Durin::FPaths::SetDerivedDataCacheDirForTests({});
}
