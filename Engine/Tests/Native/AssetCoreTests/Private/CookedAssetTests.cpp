#include <gtest/gtest.h>

#include "Asset/AssetOperations.h"
#include "Asset/Mutation.h"
#include "Asset/PackageSerialization.h"
#include "AssetCook.h"
#include "Asset/CanonicalResave.h"
#include "Asset/Compatibility.h"
#include "Asset/PackageObjectStreamWriter.h"
#include "Animation/AnimationClip.h"
#include "CoreGlobals.h"
#include "DObject/Class.h"
#include "DObject/DObjectArray.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/Object.h"
#include "DObject/Package.h"
#include "EnvironmentLighting/EnvironmentLighting.h"
#include "HAL/PlatformLTS.h"
#include "Hash/XxHash.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Materials/Material.h"
#include "NativeTestSupport.h"
#include "NativeDObjectTestSupport.h"
#include "SkeletalMesh/SkeletalMesh.h"
#include "SkeletalMesh/Skeleton.h"
#include "StaticMesh/StaticMesh.h"
#include "Terrain/TerrainHeightmap.h"
#include "Texture/Texture2D.h"
#include "Texture/TextureCube.h"
#include "Texture/VolumeTexture.h"

namespace
{
	using namespace Durin;
	using namespace Durin::Asset;

	template<typename T>
	concept HasPublicAddToCook = requires(
		T& Value, FCookContext& Context, std::string& Error)
	{
		Value.AddToCook(Context, std::string_view{}, Error);
	};

	auto MakePackageBytes() -> std::vector<std::byte>
	{
		static const bool bInitialized = [] {
			Testing::InitializeDObjectSystemForTests();
			return true;
		}();
		(void)bInitialized;
		static const bool bMounted = [] {
			const std::filesystem::path Root = Durin::Testing::GetTestWorkDirectory()
											   / "CookPackageFixtureMount";
			std::filesystem::create_directories(Root);
			PathUtilities::RegisterMountPointForTests(
				"/TestCook/", Root.generic_string() + "/"
			);
			return true;
		}();
		(void)bMounted;
		static uint64 NextPackage = 1;
		FAssetPath Path;
		EXPECT_TRUE(FAssetPath::TryCreate(std::format("/TestCook/Fixture{}", NextPackage++), Path));
		DPackage* Package = CreatePackage(Path);
		EXPECT_NE(Package, nullptr);
		DObject* Asset = NewObject<DObject>(Package, "Root");
		EXPECT_NE(Asset, nullptr);
		EXPECT_TRUE(Package->SetAsset(Asset));
		std::vector<std::byte> Bytes;
		const FAssetResult Result = SerializeAssetPackageBytes(Package, Bytes);
		EXPECT_TRUE(Result) << Result.Message;
		EXPECT_TRUE(UnloadPackage(Package, EAssetPackageUnloadPolicy::DiscardUnsaved));
		return Bytes;
	}

} // namespace

TEST(FCookedPathTests, ResolvesMountedCompanionsAndRejectsTraversal)
{
	const std::filesystem::path Root = Durin::Testing::GetTestWorkDirectory() / "CookedPath";
	std::filesystem::path Package, Companion;
	ASSERT_TRUE(ResolveCookedPackagePath(std::filesystem::absolute(Root), "/Game/Textures/T", Package));
	ASSERT_TRUE(ResolveCookedCompanionPath(std::filesystem::absolute(Root), Package, Companion));
	EXPECT_EQ(Package.filename(), "T.dasset");
	EXPECT_EQ(Companion.filename(), "T.dbulk");
	ASSERT_TRUE(ResolveCookedPackagePath(
		std::filesystem::absolute(Root), "/Plugins/PCG/Textures/T", Package
	));
	EXPECT_NE(Package.generic_string().find("Plugins/PCG/Textures/T.dasset"), std::string::npos);
	EXPECT_FALSE(ResolveCookedPackagePath(std::filesystem::absolute(Root), "/Game/../Escape", Package));
	EXPECT_FALSE(ResolveCookedPackagePath(std::filesystem::absolute(Root), "/Game/CON", Package));
	EXPECT_FALSE(ResolveCookedPackagePath(std::filesystem::absolute(Root), "G:/Source/T", Package));
}

TEST(FCookedPathTests, ImmutableRuntimeConfigurationRejectsReplacementAndPackageMutation)
{
	const std::filesystem::path Root = std::filesystem::absolute(
		Durin::Testing::GetTestWorkDirectory() / "CookedMode"
	);
	FAssetRuntimeConfiguration Runtime = FAssetRuntimeConfiguration::Authored();
	FAssetRuntimeConfiguration Invalid = Runtime;
	EXPECT_FALSE(FAssetRuntimeConfiguration::Cooked("relative/cook", Invalid));
	EXPECT_EQ(Invalid, Runtime);
	ASSERT_TRUE(FAssetRuntimeConfiguration::Cooked(Root, Runtime));
	EXPECT_FALSE(Runtime.AllowsSourceFallback());
	EXPECT_FALSE(Runtime.AllowsDerivedDataFallback());
	ShutdownAssetManager();
	ASSERT_TRUE(InitializeAssetManager(Runtime));
	EXPECT_TRUE(GetAssetRuntimeConfiguration().RequiresCookedPayload());
	EXPECT_FALSE(InitializeAssetManager(FAssetRuntimeConfiguration::Authored()));
	EXPECT_EQ(SavePackage(nullptr).Error, EAssetError::ReadOnlyMode);
	ShutdownAssetManager();
	ASSERT_TRUE(InitializeAssetManager());
}

TEST(FCookManifestTests, IsDeterministicAndRejectsCorruptRecords)
{
	FCookManifest Manifest{
		ECookTargetPlatform::Win64,
		ECookTargetProfile::Game,
		{{ECookManifestEntryKind::CookedBulk, 1, "Game/B.dbulk", 2, 3, 4},
		 {ECookManifestEntryKind::CookedPackage, 1, "Game/A.dasset", 1, 5, 6}}
	};
	std::vector<std::byte> First, Second;
	ASSERT_TRUE(EncodeCookManifest(Manifest, First));
	ASSERT_TRUE(EncodeCookManifest(Manifest, Second));
	EXPECT_EQ(First, Second);
	const FXxHash128 GoldenHash = FXxHash128::HashBuffer(First);
	EXPECT_EQ(First.size(), 137u);
	EXPECT_EQ(GoldenHash.HashLow, 1127403949174504654ull);
	EXPECT_EQ(GoldenHash.HashHigh, 9302219320893799974ull);
	FCookManifest Decoded;
	ASSERT_TRUE(DecodeCookManifest(First, Decoded));
	ASSERT_EQ(Decoded.Entries.size(), 2u);
	EXPECT_EQ(Decoded.Entries[0].RelativePath, "Game/A.dasset");
	First.back() ^= std::byte{1};
	EXPECT_FALSE(DecodeCookManifest(First, Decoded));
}

TEST(FCookStateTests, IsCanonicalVersionedAndRejectsCorruption)
{
	FCookState State{
		ECookTargetPlatform::Win64,
		ECookTargetProfile::Game,
		{{"/Game/B", {1, 2}, {3, 4}, {5, 6}, 12, 8, 2, 3, "texture", "ddc-hit"},
		 {"/Game/A", {7, 8}, {9, 10}, {}, 11, 0, 4, 5, "generic", "captured"}}
	};
	std::vector<std::byte> First, Second;
	std::string Error;
	ASSERT_TRUE(EncodeCookState(State, First, &Error)) << Error;
	ASSERT_TRUE(EncodeCookState(State, Second, &Error)) << Error;
	EXPECT_EQ(First, Second);
	FCookState Decoded;
	ASSERT_TRUE(DecodeCookState(First, Decoded, &Error)) << Error;
	ASSERT_EQ(Decoded.Entries.size(), 2u);
	EXPECT_EQ(Decoded.Entries[0].VirtualPackagePath, "/Game/A");
	First[4] ^= std::byte{1};
	EXPECT_FALSE(DecodeCookState(First, Decoded, &Error));
}

TEST(FCookContributorTests, RejectsDuplicatesAndAllowsOwnerRetirement)
{
	const FCookContributor Contributor = [](DObject&, std::string_view,
											FCookContext&) -> FAssetResult { return {}; };
	const FCookContributorHandle First = RegisterCookContributor(
		DObject::StaticClass(), {"generic-test", 1, 1, Contributor}
	);
	ASSERT_NE(First, 0u);
	EXPECT_EQ(RegisterCookContributor(DObject::StaticClass(), {"duplicate", 1, 1, Contributor}), 0u);
	UnregisterCookContributor(First);
	const FCookContributorHandle Replacement = RegisterCookContributor(
		DObject::StaticClass(), {"replacement", 1, 1, Contributor}
	);
	EXPECT_NE(Replacement, 0u);
	UnregisterCookContributor(Replacement);
}

TEST(FCookContributorTests, FamilyCookHelpersAreNotPublicApi)
{
	EXPECT_FALSE(HasPublicAddToCook<DTexture2D>);
	EXPECT_FALSE(HasPublicAddToCook<DTextureCube>);
	EXPECT_FALSE(HasPublicAddToCook<DVolumeTexture>);
	EXPECT_FALSE(HasPublicAddToCook<DStaticMesh>);
	EXPECT_FALSE(HasPublicAddToCook<DSkeletalMesh>);
	EXPECT_FALSE(HasPublicAddToCook<DSkeleton>);
	EXPECT_FALSE(HasPublicAddToCook<DAnimationClip>);
	EXPECT_FALSE(HasPublicAddToCook<DTerrainHeightmap>);
	EXPECT_FALSE(HasPublicAddToCook<DMaterial>);
	EXPECT_FALSE(HasPublicAddToCook<DEnvironmentLighting>);
}

TEST(FCookSavePlanTests, CapturesWithoutAnOutputRootAndIsDeterministic)
{
	std::string Error;
	FCookContext First({}, ECookTargetPlatform::Win64, ECookTargetProfile::Game);
	FCookContext Second({}, ECookTargetPlatform::Win64, ECookTargetProfile::Game);
	ASSERT_TRUE(First.AddRawPackage("/Game/Detached", MakePackageBytes(), {std::byte{1}, std::byte{2}, std::byte{3}}, &Error)) << Error;
	ASSERT_TRUE(Second.AddRawPackage("/Game/Detached", MakePackageBytes(), {std::byte{1}, std::byte{2}, std::byte{3}}, &Error)) << Error;
	std::vector<FCookSavePlan> FirstPlans, SecondPlans;
	ASSERT_TRUE(First.TakeSavePlans(FirstPlans, &Error)) << Error;
	ASSERT_TRUE(Second.TakeSavePlans(SecondPlans, &Error)) << Error;
	EXPECT_EQ(FirstPlans, SecondPlans);
	ASSERT_EQ(FirstPlans.size(), 1u);
	EXPECT_EQ(FirstPlans[0].VirtualPath, "/Game/Detached");
	EXPECT_FALSE(FirstPlans[0].PackageDigest.IsZero());
	EXPECT_FALSE(FirstPlans[0].SegmentDigest.IsZero());
}

TEST(FCookOutputStoreTests, RestoresEveryPriorFileAfterMidCommitFailure)
{
	const std::filesystem::path Root = std::filesystem::absolute(
		Durin::Testing::GetTestWorkDirectory() / "CookTransactionalRollback"
	);
	Durin::Testing::RemoveTestWorkDirectory(Root);
	std::string Error;
	auto Capture = [&](std::initializer_list<std::byte> Segment) {
		FCookContext Context({}, ECookTargetPlatform::Win64, ECookTargetProfile::Game);
		EXPECT_TRUE(Context.AddRawPackage("/Game/Transactional", MakePackageBytes(), std::vector<std::byte>(Segment), &Error)) << Error;
		std::vector<FCookSavePlan> Plans;
		EXPECT_TRUE(Context.TakeSavePlans(Plans, &Error)) << Error;
		Plans[0].Contributor = "opaque-test";
		Plans[0].BuildProvenance = "captured";
		return Plans;
	};
	auto MakeState = [](const FCookSavePlan& Plan) {
		return FCookState{ECookTargetPlatform::Win64, ECookTargetProfile::Game, {{Plan.VirtualPath, Plan.InputFingerprint, Plan.PackageDigest, Plan.SegmentDigest, Plan.PackageFileSize, Plan.SegmentFileSize, Plan.ContributorVersion, Plan.FamilyProducerVersion, Plan.Contributor, Plan.BuildProvenance}}};
	};
	std::vector<FCookSavePlan> First = Capture({std::byte{1}, std::byte{2}});
	auto Store = CreateLocalLooseCookOutputStore(Root, ECookTargetPlatform::Win64, ECookTargetProfile::Game);
	FCookRunResult Result;
	ASSERT_TRUE(Store->Publish(First, MakeState(First[0]), Result, {}, {}, Error)) << Error;
	std::vector<std::byte> PriorPackage, PriorSegment, PriorManifest, PriorState;
	ASSERT_TRUE(FFileHelper::LoadFileToArray(PriorPackage, Root / "Game/Transactional.dasset"));
	ASSERT_TRUE(FFileHelper::LoadFileToArray(PriorSegment, Root / "Game/Transactional.dbulk"));
	ASSERT_TRUE(FFileHelper::LoadFileToArray(PriorManifest, Root / "CookManifest.bin"));
	ASSERT_TRUE(FFileHelper::LoadFileToArray(PriorState, Root / "CookState.bin"));

	std::vector<FCookSavePlan> Second = Capture(
		{std::byte{9}, std::byte{8}, std::byte{7}}
	);
	for (const ECookOperationStage FailureStage : {
			 ECookOperationStage::CommitPackage,
			 ECookOperationStage::CommitState,
			 ECookOperationStage::CommitManifest
		 })
	{
		EXPECT_FALSE(Store->Publish(Second, MakeState(Second[0]), Result, {}, [FailureStage](ECookOperationStage Stage, size_t, std::string& OutError) {
				if (Stage != FailureStage) return false;
				OutError = "injected commit failure";
				return true; }, Error));
		std::vector<std::byte> Bytes;
		ASSERT_TRUE(FFileHelper::LoadFileToArray(Bytes, Root / "Game/Transactional.dasset"));
		EXPECT_EQ(Bytes, PriorPackage);
		ASSERT_TRUE(FFileHelper::LoadFileToArray(Bytes, Root / "Game/Transactional.dbulk"));
		EXPECT_EQ(Bytes, PriorSegment);
		ASSERT_TRUE(FFileHelper::LoadFileToArray(Bytes, Root / "CookManifest.bin"));
		EXPECT_EQ(Bytes, PriorManifest);
		ASSERT_TRUE(FFileHelper::LoadFileToArray(Bytes, Root / "CookState.bin"));
		EXPECT_EQ(Bytes, PriorState);
	}

	bool bCancelled = false;
	EXPECT_FALSE(Store->Publish(Second, MakeState(Second[0]), Result, [&bCancelled] { return bCancelled; }, [&bCancelled](ECookOperationStage Stage, size_t, std::string&) {
			if (Stage == ECookOperationStage::CommitSegment) bCancelled = true;
			return false; }, Error));
	EXPECT_NE(Error.find("CookCancelledDuringCommit"), std::string::npos);
	std::vector<std::byte> Bytes;
	ASSERT_TRUE(FFileHelper::LoadFileToArray(Bytes, Root / "Game/Transactional.dasset"));
	EXPECT_EQ(Bytes, PriorPackage);
	ASSERT_TRUE(FFileHelper::LoadFileToArray(Bytes, Root / "Game/Transactional.dbulk"));
	EXPECT_EQ(Bytes, PriorSegment);
	ASSERT_TRUE(FFileHelper::LoadFileToArray(Bytes, Root / "CookManifest.bin"));
	EXPECT_EQ(Bytes, PriorManifest);
	ASSERT_TRUE(FFileHelper::LoadFileToArray(Bytes, Root / "CookState.bin"));
	EXPECT_EQ(Bytes, PriorState);
}

TEST(FCookOutputStoreTests, RepairsCorruptReusedOutputAndRejectsCompetingWriter)
{
	const std::filesystem::path Root = std::filesystem::absolute(
		Durin::Testing::GetTestWorkDirectory() / "CookRepairAndLock"
	);
	Durin::Testing::RemoveTestWorkDirectory(Root);
	FCookContext Context({}, ECookTargetPlatform::Win64, ECookTargetProfile::Game);
	std::string Error;
	ASSERT_TRUE(Context.AddRawPackage("/Game/Repair", MakePackageBytes(), {std::byte{4}, std::byte{5}, std::byte{6}}, &Error)) << Error;
	std::vector<FCookSavePlan> Plans;
	ASSERT_TRUE(Context.TakeSavePlans(Plans, &Error)) << Error;
	Plans[0].Contributor = "repair-test";
	Plans[0].BuildProvenance = "captured";
	FCookState State{ECookTargetPlatform::Win64, ECookTargetProfile::Game, {{Plans[0].VirtualPath, Plans[0].InputFingerprint, Plans[0].PackageDigest, Plans[0].SegmentDigest, Plans[0].PackageFileSize, Plans[0].SegmentFileSize, Plans[0].ContributorVersion, Plans[0].FamilyProducerVersion, Plans[0].Contributor, Plans[0].BuildProvenance}}};
	auto Store = CreateLocalLooseCookOutputStore(Root, ECookTargetPlatform::Win64, ECookTargetProfile::Game);
	FCookRunResult Result;
	ASSERT_TRUE(Store->Publish(Plans, State, Result, {}, {}, Error)) << Error;
	const std::array<std::byte, 1> Corrupt{std::byte{0}};
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(Corrupt, Root / "Game/Repair.dbulk"));
	Plans[0].bReuseExistingOutput = true;
	ASSERT_TRUE(Store->Publish(Plans, State, Result, {}, {}, Error)) << Error;
	std::vector<std::byte> Repaired;
	ASSERT_TRUE(FFileHelper::LoadFileToArray(Repaired, Root / "Game/Repair.dbulk"));
	EXPECT_EQ(Repaired, Plans[0].BulkBytes);

	ASSERT_TRUE(std::filesystem::create_directory(Root / ".durin-cook-writer"));
	EXPECT_FALSE(Store->Publish(Plans, State, Result, {}, {}, Error));
	EXPECT_NE(Error.find("CookCompetingWriter"), std::string::npos);
	std::filesystem::remove(Root / ".durin-cook-writer");
}

TEST(FCookOutputStoreTests, CleansOnlyPreviousManifestOwnedStaleFiles)
{
	const std::filesystem::path Root = std::filesystem::absolute(
		Durin::Testing::GetTestWorkDirectory() / "CookStaleCleanup"
	);
	Durin::Testing::RemoveTestWorkDirectory(Root);
	auto Capture = [](std::string Path, std::byte Value) {
		FCookContext Context({}, ECookTargetPlatform::Win64, ECookTargetProfile::Game);
		std::string Error;
		EXPECT_TRUE(Context.AddRawPackage(std::move(Path), MakePackageBytes(), {Value}, &Error)) << Error;
		std::vector<FCookSavePlan> Plans;
		EXPECT_TRUE(Context.TakeSavePlans(Plans, &Error)) << Error;
		Plans[0].Contributor = "stale-test";
		Plans[0].BuildProvenance = "captured";
		return Plans[0];
	};
	FCookSavePlan Keep = Capture("/Game/Keep", std::byte{1});
	FCookSavePlan Stale = Capture("/Game/Stale", std::byte{2});
	auto StateFor = [](std::span<const FCookSavePlan> Plans) {
		FCookState State{ECookTargetPlatform::Win64, ECookTargetProfile::Game};
		for (const FCookSavePlan& Plan : Plans)
			State.Entries.push_back({Plan.VirtualPath, Plan.InputFingerprint, Plan.PackageDigest, Plan.SegmentDigest, Plan.PackageFileSize, Plan.SegmentFileSize, Plan.ContributorVersion, Plan.FamilyProducerVersion, Plan.Contributor, Plan.BuildProvenance});
		return State;
	};
	std::vector<FCookSavePlan> First{Keep, Stale};
	auto Store = CreateLocalLooseCookOutputStore(Root, ECookTargetPlatform::Win64, ECookTargetProfile::Game);
	FCookRunResult Result;
	std::string Error;
	ASSERT_TRUE(Store->Publish(First, StateFor(First), Result, {}, {}, Error)) << Error;
	const std::array<std::byte, 1> UnownedBytes{std::byte{9}};
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(UnownedBytes, Root / "unowned.bin"));
	std::vector<FCookSavePlan> Second{Keep};
	ASSERT_TRUE(Store->Publish(Second, StateFor(Second), Result, {}, {}, Error)) << Error;
	EXPECT_FALSE(std::filesystem::exists(Root / "Game/Stale.dasset"));
	EXPECT_FALSE(std::filesystem::exists(Root / "Game/Stale.dbulk"));
	EXPECT_TRUE(std::filesystem::exists(Root / "Game/Keep.dasset"));
	EXPECT_TRUE(std::filesystem::exists(Root / "unowned.bin"));
}
