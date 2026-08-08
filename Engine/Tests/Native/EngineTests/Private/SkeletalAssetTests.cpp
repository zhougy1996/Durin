#include <gtest/gtest.h>

#include "Animation/AnimationClip.h"
#include "AssetSystem.h"
#include "EngineTestSupport.h"
#include "Misc/Paths.h"
#include "NativeTestSupport.h"
#include "SkeletalMesh/SkeletalMesh.h"

namespace
{
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
		std::string_view SlotName = "Body") -> void
	{
		Durin::FSkeletalMeshImportedData Data{
			.Skeleton = &Skeleton,
			.SkeletonCompatibilityIdentity = Skeleton.GetCompatibilityIdentity(),
			.MeshNodeBindTransform = MakeTransform(),
			.MaterialSlots = {{
				.Name = Durin::FName(SlotName),
				.SourceName = std::string(SlotName),
				.SourceMaterialIndex = 0}},
			.Payload = MakeMeshPayload(),
			.CookedPayload = MakeCookedDescriptor(100)};
		std::string Error;
		ASSERT_TRUE(Mesh.InitializeFromImportedData(std::move(Data), Error)) << Error;
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
		float EndValue = 1.0f) -> void
	{
		Durin::FAnimationClipImportedData Data{
			.Skeleton = &Skeleton,
			.SkeletonCompatibilityIdentity = Skeleton.GetCompatibilityIdentity(),
			.ClipName = Durin::FName(Name),
			.Payload = MakeClipPayload(EndValue),
			.CookedPayload = MakeCookedDescriptor(200)};
		std::string Error;
		ASSERT_TRUE(Clip.InitializeFromImportedData(std::move(Data), Error)) << Error;
	}

	auto InitializeAssetMount() -> std::filesystem::path
	{
		InitializeDObjectSystem();
		const std::filesystem::path Root =
			Durin::Testing::GetTestWorkDirectory() / "SkeletalAssets";
		Durin::Testing::RemoveTestWorkDirectory(Root);
		Durin::PathUtilities::RegisterMountPointForTests(
			"/SkeletalAssetTests/", Root.generic_string() + "/");
		return Root;
	}
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
	Durin::FSkeletalMeshImportedData InvalidMesh{
		.Skeleton = Skeleton,
		.SkeletonCompatibilityIdentity = Skeleton->GetCompatibilityIdentity(),
		.MeshNodeBindTransform = MakeTransform(),
		.MaterialSlots = {{.Name = Durin::FName("Body"), .SourceMaterialIndex = 0}},
		.Payload = InvalidPayload};
	std::string Error;
	EXPECT_FALSE(Mesh->InitializeFromImportedData(std::move(InvalidMesh), Error));
	EXPECT_EQ(Mesh->GetSummary(), MeshSummary);

	auto* Clip = Durin::NewObject<Durin::DAnimationClip>(nullptr, "PayloadClip");
	InitializeClip(*Clip, *Skeleton);
	const Durin::FAnimationClipSummary ClipSummary = Clip->GetSummary();
	auto InvalidClipPayload = std::make_shared<Durin::FAnimationClipPayloadData>(*MakeClipPayload());
	InvalidClipPayload->Tracks[0].Times[1] = 0.0f;
	Durin::FAnimationClipImportedData InvalidClip{
		.Skeleton = Skeleton,
		.SkeletonCompatibilityIdentity = Skeleton->GetCompatibilityIdentity(),
		.ClipName = Durin::FName("Invalid"),
		.Payload = InvalidClipPayload};
	EXPECT_FALSE(Clip->InitializeFromImportedData(std::move(InvalidClip), Error));
	EXPECT_EQ(Clip->GetSummary(), ClipSummary);
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
	ASSERT_TRUE(Durin::Asset::DeleteAsset(ClipPath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(MeshPath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(SkeletonPath));
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
