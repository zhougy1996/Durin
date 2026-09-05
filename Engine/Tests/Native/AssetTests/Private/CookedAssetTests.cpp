#include <gtest/gtest.h>
#include <gtest/gtest-spi.h>

#include "Asset/PackageSerialization.h"
#include "Asset/Mutation.h"
#include "Asset/AssetCook.h"
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
#include "Misc/MountPathTestSupport.h"
#include "Materials/Material.h"
#include "NativeTestSupport.h"
#include "NativeAssetRuntimeTestSupport.h"
#include "NativeDObjectTestSupport.h"
#include "StaticMesh/StaticMesh.h"
#include "Texture/Texture2D.h"
#include "Texture/TextureCube.h"
#include "Texture/TextureCookedData.h"
#include "Texture/VolumeTexture.h"

namespace
{
	using namespace Durin;

	template<typename T>
	concept HasPublicAddToCook = requires(
		T& Value, FCookContext& Context, std::string& Error)
	{
		Value.AddToCook(Context, std::string_view{}, Error);
	};

	auto MakePackageBytes() -> Durin::FByteArray
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
			Testing::RegisterMountPointForTests(
				"/TestCook/", Root.generic_string() + "/"
			);
			return true;
		}();
		(void)bMounted;
		static uint64 NextPackage = 1;
		FPackagePath Path;
		EXPECT_TRUE(FPackagePath::TryCreate(std::format("/TestCook/Fixture{}", NextPackage++), Path));
		DPackage* Package = CreatePackage(Path);
		EXPECT_NE(Package, nullptr);
		DObject* Asset = NewObject<DObject>(Package, "Root");
		EXPECT_NE(Asset, nullptr);
		EXPECT_EQ(Package->FindTopLevelAsset(Asset->GetFName()), Asset);
		Durin::FByteArray Bytes;
		const FAssetResult Result = SerializeAssetPackageBytes(Package, Bytes);
		EXPECT_TRUE(Result) << Result.Message;
		EXPECT_TRUE(UnloadPackage(Package, EAssetPackageUnloadPolicy::DiscardUnsaved));
		return Bytes;
	}

	template<class TTexture, class TPlatformData>
	auto ExpectCookedTextureDecodeBoundaries(
		TPlatformData PlatformData, std::string_view Family) -> void
	{
		auto* Texture = NewObject<TTexture>(nullptr, "CookedDecodeBoundary");
		ASSERT_NE(Texture, nullptr);
		std::string Error;
		ASSERT_TRUE(Texture->SetPlatformData(std::make_unique<TPlatformData>(PlatformData), Error))
			<< Error;
		const auto* Installed = Texture->GetPlatformData();
		const uint64 Revision = Texture->GetBuildRevision();
		FByteArray ValidBytes;
		FCanonicalMemoryWriter Writer(ValidBytes, EArchivePurpose::CookedPayload);
		PlatformData.Serialize(Writer, {.TargetPlatform = ECookTargetPlatform::Win64,
			.TargetProfile = ECookTargetProfile::Game});
		ASSERT_FALSE(Writer.HasError()) << Writer.GetError();
		FByteArray TrailingBytes = ValidBytes;
		TrailingBytes.push_back(std::byte{0x7f});

		FBulkData Bulk;
		EXPECT_FALSE(TexturePrivate::LoadCookedPlatformData<TPlatformData>(
			*Texture, Bulk, Family, Error));
		for (const FByteArray& InvalidBytes : {FByteArray{std::byte{0xff}}, TrailingBytes})
		{
			ASSERT_TRUE(FBulkData::TryCreateDetached(InvalidBytes, Bulk, &Error)) << Error;
			EXPECT_FALSE(TexturePrivate::LoadCookedPlatformData<TPlatformData>(
				*Texture, Bulk, Family, Error));
			EXPECT_NE(Error.find(std::format("Cooked {} '{}'", Family, Texture->GetObjectPath())),
				std::string::npos) << Error;
			EXPECT_EQ(Texture->GetPlatformData(), Installed);
			EXPECT_EQ(Texture->GetBuildRevision(), Revision);
			// A failed decoder must release its lock so the payload can be retried.
			std::span<const std::byte> LockedBytes;
			ASSERT_TRUE(Bulk.LockReadOnly(LockedBytes, &Error)) << Error;
			ASSERT_TRUE(Bulk.UnlockReadOnly(&Error)) << Error;
		}
		ASSERT_TRUE(FBulkData::TryCreateDetached(ValidBytes, Bulk, &Error)) << Error;
		ASSERT_TRUE(TexturePrivate::LoadCookedPlatformData<TPlatformData>(
			*Texture, Bulk, Family, Error)) << Error;
		EXPECT_TRUE(Texture->HasPlatformData());
		EXPECT_EQ(Texture->GetBuildRevision(), Revision + 1);
		EXPECT_NE(Bulk.GetState(), EBulkDataState::ReadLocked);
		EXPECT_TRUE(Error.empty());
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

TEST(FCookedPathTests, ScopedRuntimeRejectsInvalidRootAndRestoresNestedConfigurations)
{
	Testing::InitializeDObjectSystemForTests();
	ASSERT_TRUE(InitializeAssetManager());
	const FAssetRuntimeConfiguration Original = GetAssetRuntimeConfiguration();
	const auto FirstRoot = Testing::CreateTestFixtureDirectory("ScopedCookedRuntimeFirst");
	const auto SecondRoot = Testing::CreateTestFixtureDirectory("ScopedCookedRuntimeSecond");
	Testing::FScopedAssetRuntimeForTests Outer;
	ASSERT_TRUE(Outer.RestartCooked(FirstRoot));
	const FAssetRuntimeConfiguration First = GetAssetRuntimeConfiguration();
	{
		Testing::FScopedAssetRuntimeForTests Inner;
		const auto Invalid = Inner.RestartCooked("relative/cook");
		EXPECT_FALSE(Invalid);
		EXPECT_EQ(GetAssetRuntimeConfiguration(), First);
		EXPECT_FALSE(InitializeAssetManager(Original));
		ASSERT_TRUE(Inner.Restore());
		EXPECT_EQ(GetAssetRuntimeConfiguration(), First);
		ASSERT_TRUE(Inner.RestartCooked(SecondRoot));
		EXPECT_EQ(GetAssetRuntimeConfiguration().GetCookRoot(), SecondRoot);
	}
	EXPECT_EQ(GetAssetRuntimeConfiguration(), First);
	ASSERT_TRUE(Outer.Restore());
	EXPECT_EQ(GetAssetRuntimeConfiguration(), Original);
	ASSERT_TRUE(Outer.Restore());
	EXPECT_EQ(GetAssetRuntimeConfiguration(), Original);
	// The same scope can switch again after an explicit restore.
	ASSERT_TRUE(Outer.RestartCooked(SecondRoot));
	ASSERT_TRUE(Outer.Restore());
	EXPECT_EQ(GetAssetRuntimeConfiguration(), Original);
}

TEST(FCookedPathTests, ScopedRuntimeRestoresAfterFatalAssertion)
{
	Testing::InitializeDObjectSystemForTests();
	ASSERT_TRUE(InitializeAssetManager());
	const FAssetRuntimeConfiguration Original = GetAssetRuntimeConfiguration();
	EXPECT_FATAL_FAILURE({
		Testing::FScopedAssetRuntimeForTests Runtime;
		ASSERT_TRUE(Runtime.RestartCooked(
			Testing::CreateTestFixtureDirectory("ScopedCookedRuntimeEarlyExit")));
		FAIL() << "intentional runtime scope exit";
	}, "intentional runtime scope exit");
	EXPECT_EQ(GetAssetRuntimeConfiguration(), Original);
	ASSERT_TRUE(InitializeAssetManager(Original));
}

TEST(FCookedTextureDataTests, DecodeFailureUnlocksBulkAndPreservesInstalledFamilyData)
{
	Testing::InitializeDObjectSystemForTests();
	FTexturePlatformData Texture2D;
	Texture2D.PixelFormat = EPixelFormat::BC1_UNORM;
	const FPixelFormatLayout Texture2DLayout =
		GetPixelFormatLayout(Texture2D.PixelFormat, 1, 1);
	Texture2D.Mips.push_back({FByteArray(Texture2DLayout.DataSize, std::byte{0x7f}),
		1, 1, static_cast<uint32>(Texture2DLayout.RowPitch)});
	ASSERT_NO_FATAL_FAILURE(ExpectCookedTextureDecodeBoundaries<DTexture2D>(Texture2D, "Texture2D"));
	FTextureCubePlatformData Cube;
	Cube.PixelFormat = Texture2D.PixelFormat;
	Cube.Faces.fill(Texture2D);
	ASSERT_NO_FATAL_FAILURE(ExpectCookedTextureDecodeBoundaries<DTextureCube>(Cube, "TextureCube"));
	FVolumeTexturePlatformData Volume;
	Volume.PixelFormat = EPixelFormat::R8_UNORM;
	Volume.Mips.push_back({FByteArray(1, std::byte{0x7f}), 1, 1, 1, 1, 1});
	ASSERT_NO_FATAL_FAILURE(ExpectCookedTextureDecodeBoundaries<DVolumeTexture>(Volume, "volume texture"));
	CollectGarbage();
}

TEST(FCookManifestTests, IsDeterministicAndRejectsCorruptRecords)
{
	FCookManifest Manifest{
		ECookTargetPlatform::Win64,
		ECookTargetProfile::Game,
		{{ECookManifestEntryKind::CookedBulk, 1, "Game/B.dbulk", 2, 3, 4},
		 {ECookManifestEntryKind::CookedPackage, 1, "Game/A.dasset", 1, 5, 6}}
	};
	Durin::FByteArray First, Second;
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
	Durin::FByteArray First, Second;
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
	EXPECT_FALSE(HasPublicAddToCook<DMaterial>);
	EXPECT_FALSE(HasPublicAddToCook<DEnvironmentLighting>);
}

TEST(FCookSavePlanTests, CapturesWithoutAnOutputRootAndIsDeterministic)
{
	std::string Error;
	FCookContext First({}, ECookTargetPlatform::Win64, ECookTargetProfile::Game);
	FCookContext Second({}, ECookTargetPlatform::Win64, ECookTargetProfile::Game);
	const Durin::FByteArray PackageBytes = MakePackageBytes();
	ASSERT_TRUE(First.AddRawPackage("/Game/Detached", PackageBytes, {std::byte{1}, std::byte{2}, std::byte{3}}, &Error)) << Error;
	ASSERT_TRUE(Second.AddRawPackage("/Game/Detached", PackageBytes, {std::byte{1}, std::byte{2}, std::byte{3}}, &Error)) << Error;
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
		EXPECT_TRUE(Context.AddRawPackage("/Game/Transactional", MakePackageBytes(), Durin::FByteArray(Segment), &Error)) << Error;
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
	std::vector<FCookAuxiliaryOutput> FirstAuxiliary{{
		ECookManifestEntryKind::ShaderLibrary, "Shaders/ShaderLibrary.dslb",
		{std::byte{1}, std::byte{3}}}};
	FirstAuxiliary[0].Digest = FXxHash128::HashBuffer(FirstAuxiliary[0].Bytes);
	auto Store = CreateLocalLooseCookOutputStore(Root, ECookTargetPlatform::Win64, ECookTargetProfile::Game);
	FCookRunResult Result;
	FCookPublishResult PublishResult = Store->Publish(
		First, FirstAuxiliary, MakeState(First[0]), Result, {}, {});
	ASSERT_TRUE(PublishResult) << PublishResult.Diagnostic;
	Durin::FByteArray PriorPackage, PriorSegment, PriorLibrary, PriorManifest, PriorState;
	ASSERT_TRUE(FFileHelper::LoadFileToArray(PriorPackage, Root / "Game/Transactional.dasset"));
	ASSERT_TRUE(FFileHelper::LoadFileToArray(PriorSegment, Root / "Game/Transactional.dbulk"));
	ASSERT_TRUE(FFileHelper::LoadFileToArray(PriorLibrary, Root / "Shaders/ShaderLibrary.dslb"));
	ASSERT_TRUE(FFileHelper::LoadFileToArray(PriorManifest, Root / "CookManifest.bin"));
	ASSERT_TRUE(FFileHelper::LoadFileToArray(PriorState, Root / "CookState.bin"));

	std::vector<FCookSavePlan> Second = Capture(
		{std::byte{9}, std::byte{8}, std::byte{7}}
	);
	std::vector<FCookAuxiliaryOutput> SecondAuxiliary{{
		ECookManifestEntryKind::ShaderLibrary, "Shaders/ShaderLibrary.dslb",
		{std::byte{9}, std::byte{7}, std::byte{5}}}};
	SecondAuxiliary[0].Digest = FXxHash128::HashBuffer(SecondAuxiliary[0].Bytes);
	for (const ECookOperationStage FailureStage : {
			 ECookOperationStage::CommitPackage,
			 ECookOperationStage::CommitAuxiliary,
			 ECookOperationStage::CommitState,
			 ECookOperationStage::CommitManifest
		 })
	{
		PublishResult = Store->Publish(Second, SecondAuxiliary, MakeState(Second[0]), Result, {}, [FailureStage](ECookOperationStage Stage, size_t, std::string& OutError) {
				if (Stage != FailureStage) return false;
				OutError = "injected commit failure";
				return true; });
		EXPECT_FALSE(PublishResult);
		EXPECT_EQ(PublishResult.Status, ECookPublishStatus::Failed);
		Durin::FByteArray Bytes;
		ASSERT_TRUE(FFileHelper::LoadFileToArray(Bytes, Root / "Game/Transactional.dasset"));
		EXPECT_EQ(Bytes, PriorPackage);
		ASSERT_TRUE(FFileHelper::LoadFileToArray(Bytes, Root / "Game/Transactional.dbulk"));
		EXPECT_EQ(Bytes, PriorSegment);
		ASSERT_TRUE(FFileHelper::LoadFileToArray(Bytes, Root / "Shaders/ShaderLibrary.dslb"));
		EXPECT_EQ(Bytes, PriorLibrary);
		ASSERT_TRUE(FFileHelper::LoadFileToArray(Bytes, Root / "CookManifest.bin"));
		EXPECT_EQ(Bytes, PriorManifest);
		ASSERT_TRUE(FFileHelper::LoadFileToArray(Bytes, Root / "CookState.bin"));
		EXPECT_EQ(Bytes, PriorState);
	}

	bool bCancelled = false;
	PublishResult = Store->Publish(Second, SecondAuxiliary, MakeState(Second[0]), Result, [&bCancelled] { return bCancelled; }, [&bCancelled](ECookOperationStage Stage, size_t, std::string&) {
			if (Stage == ECookOperationStage::CommitSegment) bCancelled = true;
			return false; });
	EXPECT_FALSE(PublishResult);
	EXPECT_EQ(PublishResult.Status, ECookPublishStatus::Cancelled);
	EXPECT_NE(PublishResult.Diagnostic.find("CookCancelledDuringCommit"), std::string::npos);
	Durin::FByteArray Bytes;
	ASSERT_TRUE(FFileHelper::LoadFileToArray(Bytes, Root / "Game/Transactional.dasset"));
	EXPECT_EQ(Bytes, PriorPackage);
	ASSERT_TRUE(FFileHelper::LoadFileToArray(Bytes, Root / "Game/Transactional.dbulk"));
	EXPECT_EQ(Bytes, PriorSegment);
	ASSERT_TRUE(FFileHelper::LoadFileToArray(Bytes, Root / "Shaders/ShaderLibrary.dslb"));
	EXPECT_EQ(Bytes, PriorLibrary);
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
	FCookPublishResult PublishResult = Store->Publish(Plans, State, Result, {}, {});
	ASSERT_TRUE(PublishResult) << PublishResult.Diagnostic;
	const std::array<std::byte, 1> Corrupt{std::byte{0}};
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(Corrupt, Root / "Game/Repair.dbulk"));
	Plans[0].bReuseExistingOutput = true;
	PublishResult = Store->Publish(Plans, State, Result, {}, {});
	ASSERT_TRUE(PublishResult) << PublishResult.Diagnostic;
	Durin::FByteArray Repaired;
	ASSERT_TRUE(FFileHelper::LoadFileToArray(Repaired, Root / "Game/Repair.dbulk"));
	EXPECT_EQ(Repaired, Plans[0].BulkBytes);

	ASSERT_TRUE(std::filesystem::create_directory(Root / ".durin-cook-writer"));
	PublishResult = Store->Publish(Plans, State, Result, {}, {});
	EXPECT_FALSE(PublishResult);
	EXPECT_EQ(PublishResult.Status, ECookPublishStatus::Failed);
	EXPECT_NE(PublishResult.Diagnostic.find("CookCompetingWriter"), std::string::npos);
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
	FCookPublishResult PublishResult = Store->Publish(First, StateFor(First), Result, {}, {});
	ASSERT_TRUE(PublishResult) << PublishResult.Diagnostic;
	const std::array<std::byte, 1> UnownedBytes{std::byte{9}};
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(UnownedBytes, Root / "unowned.bin"));
	std::vector<FCookSavePlan> Second{Keep};
	PublishResult = Store->Publish(Second, StateFor(Second), Result, {}, {});
	ASSERT_TRUE(PublishResult) << PublishResult.Diagnostic;
	EXPECT_FALSE(std::filesystem::exists(Root / "Game/Stale.dasset"));
	EXPECT_FALSE(std::filesystem::exists(Root / "Game/Stale.dbulk"));
	EXPECT_TRUE(std::filesystem::exists(Root / "Game/Keep.dasset"));
	EXPECT_TRUE(std::filesystem::exists(Root / "unowned.bin"));
}
