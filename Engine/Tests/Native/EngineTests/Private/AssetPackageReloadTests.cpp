#include "Asset/Asset.h"
#include "Asset/AssetCompilingManager.h"
#include "Asset/PackageSerialization.h"
#include "Asset/PackageReload.h"
#include "Components/VolumetricCloudComponent.h"
#include "DObject/StrongObjectPtr.h"
#include "Editor/Transactor.h"
#include "Editor/WorkspaceRootWindow.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Misc/MountPathTestSupport.h"
#include "Modules/ModuleManager.h"
#include "NativeDObjectTestSupport.h"
#include "NativeTestSupport.h"
#include "Texture/Texture2D.h"
#include "Texture/VolumeTexture.h"
#include "Threading/Task.h"

#include <gtest/gtest.h>

namespace
{
	class FAssetPackageReloadTests : public testing::Test
	{
	protected:
		static auto SetUpTestSuite() -> void
		{
			ASSERT_TRUE(Durin::InitializeTaskScheduler(2));
			Durin::Testing::InitializeDObjectSystemForTests();
			Durin::FModuleManager::Get().LoadModuleChecked("TextureBuild");
			ASSERT_TRUE(Durin::InitializeAssetCompilingManager());
		}

		static auto TearDownTestSuite() -> void
		{
			Durin::ShutdownAssetCompilingManager();
			Durin::CollectGarbage();
			Durin::ShutdownTaskScheduler();
		}

		auto SetUp() -> void override
		{
			const auto Root = Durin::Testing::CreateTestFixtureDirectory("AssetDiscard");
			Durin::Testing::RegisterMountPointForTests(
				"/AssetDiscardTests/", Root.generic_string() + "/");
			Cloud = Durin::TStrongObjectPtr<Durin::DVolumetricCloudComponent>(
				Durin::NewObject<Durin::DVolumetricCloudComponent>(nullptr, "DiscardConsumer"));
		}

		auto TearDown() -> void override
		{
			Cloud.Reset();
			Durin::CollectGarbage();
		}

		template<typename T>
		auto VerifyDiscard(T* Texture) -> void
		{
			const auto Path = Texture->GetPackage()->GetPackagePathIdentity();
			const auto SavedIdentity = Texture->GetSource().GetIdentity();
			ASSERT_TRUE(Durin::SavePackage(Texture->GetPackage()));
			// Establish that the expected baseline really survives a fresh load.
			ASSERT_TRUE(Durin::UnloadPackage(Path));
			Texture = nullptr;
			ASSERT_TRUE(Durin::LoadObject(
				Durin::Testing::MakePackageLeafAssetObjectPathForTests(Path), Texture));
			ASSERT_EQ(Texture->GetSource().GetIdentity(), SavedIdentity);
			if constexpr (std::is_same_v<T, Durin::DTexture2D>) Cloud->SetWeatherTexture(Texture);
			else Cloud->SetBaseDensityTexture(Texture);

			Durin::Editor::FEditableAssetDocumentModel Documents;
			ASSERT_TRUE(Documents.Activate({.Id = {1}, .ResourceId = Path.ToString()}, Texture));
			std::string Error;
			ASSERT_TRUE(SetSource(*Texture, std::byte{91}, Error)) << Error;
			Texture->GetPackage()->MarkDirty();
			ASSERT_NE(Texture->GetSource().GetIdentity(), SavedIdentity);
			ASSERT_TRUE(Documents.Discard(Texture));

			T* Referenced = nullptr;
			if constexpr (std::is_same_v<T, Durin::DTexture2D>) Referenced = Cloud->GetWeatherTexture();
			else Referenced = Cloud->GetBaseDensityTexture();
			ASSERT_NE(Referenced, nullptr);
			EXPECT_FALSE(Referenced->GetPackage()->IsDirty());
			EXPECT_EQ(Referenced->GetSource().GetIdentity(), SavedIdentity);

			ASSERT_TRUE(Durin::SavePackage(Referenced->GetPackage()));
			Cloud->SetWeatherTexture(nullptr);
			Cloud->SetBaseDensityTexture(nullptr);
			ASSERT_TRUE(Durin::UnloadPackage(Path));
			Texture = nullptr;
			ASSERT_TRUE(Durin::LoadObject(
				Durin::Testing::MakePackageLeafAssetObjectPathForTests(Path), Texture));
			EXPECT_EQ(Texture->GetSource().GetIdentity(), SavedIdentity);
			ASSERT_TRUE(Durin::UnloadPackage(Path));
		}

		static auto SetSource(Durin::DTexture2D& Texture, std::byte Value,
			std::string& Error) -> bool
		{
			Durin::Image::FImage Image;
			Durin::FTextureSource Source;
			if (!Durin::Image::FImage::TryCreate({.Width = 2, .Height = 2,
				.Format = Durin::Image::ERawImageFormat::RGBA8},
				Durin::FByteBuffer(16, Value), Image, &Error)
				|| !Source.Init2D(Image.GetView(), 4)) return false;
			Texture.SetSource(std::move(Source));
			return true;
		}

		static auto SetSource(Durin::DVolumeTexture& Texture, std::byte Value,
			std::string& Error) -> bool
		{
			Durin::FVolumeTextureSourceData Source{
				.Width = 2, .Height = 2, .Depth = 2,
				.Format = Durin::EVolumeTextureFormat::R8_UNORM};
			if (!Source.SetVoxelBytes(Durin::FByteBuffer(8, Value))) return false;
			auto PreparedSource = Durin::PrepareVolumeTextureSource(Source);
			if (!PreparedSource)
			{
				Error = "Volume source preparation failed; see log for details.";
				return false;
			}
			Texture.SetSource(std::move(*PreparedSource));
			return true;
		}

		Durin::Testing::FScopedMountRegistryFixture Mounts;
		Durin::TStrongObjectPtr<Durin::DVolumetricCloudComponent> Cloud;
	};
}

TEST_F(FAssetPackageReloadTests, Texture2DDiscardRestoresSceneReferenceAndPreservesSavedContent)
{
	Durin::FPackagePath Path;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/AssetDiscardTests/Texture", Path));
	Durin::DTexture2D* Texture = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(Path, Texture));
	std::string Error;
	ASSERT_TRUE(SetSource(*Texture, std::byte{17}, Error)) << Error;
	VerifyDiscard(Texture);
}

TEST_F(FAssetPackageReloadTests, VolumeDiscardRestoresSceneReferenceAndPreservesSavedContent)
{
	Durin::FPackagePath Path;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/AssetDiscardTests/Volume", Path));
	Durin::DVolumeTexture* Texture = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(Path, Texture));
	std::string Error;
	ASSERT_TRUE(SetSource(*Texture, std::byte{17}, Error)) << Error;
	VerifyDiscard(Texture);
}

TEST_F(FAssetPackageReloadTests, MaterialDiscardRestoresBaseAndInstanceAuthoredState)
{
	using namespace Durin;
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/AssetDiscardTests/Material", Path));
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Material));
	const FName MaterialName = Material->GetFName();
	auto* Instance = NewObject<DMaterialInstance>(Material->GetPackage(), "Instance");
	ASSERT_NE(Instance, nullptr);
	ASSERT_TRUE(Instance->SetParent(Material));
	ASSERT_TRUE(Material->SetStaticProperties({.bTwoSided = true}));
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	ASSERT_TRUE(Instance->SetParent(nullptr));
	ASSERT_TRUE(Material->SetStaticProperties({.bTwoSided = false}));
	ASSERT_FALSE(Material->GetStaticProperties().bTwoSided);

	Editor::FEditableAssetDocumentModel Documents;
	std::string ReloadError;
	ASSERT_TRUE(Documents.Discard(Material, {}, {},
		[&](std::string Message) { ReloadError = std::move(Message); })) << ReloadError;
	auto* Reloaded = Cast<DMaterial>(FindResidentPackage(Path)->FindTopLevelAsset(MaterialName));
	ASSERT_NE(Reloaded, nullptr);
	auto* ReloadedInstance = Cast<DMaterialInstance>(
		Reloaded->GetPackage()->FindTopLevelAsset("Instance"));
	ASSERT_NE(ReloadedInstance, nullptr);
	EXPECT_EQ(ReloadedInstance->GetParent(), Reloaded);
	EXPECT_TRUE(Reloaded->GetStaticProperties().bTwoSided);
	EXPECT_FALSE(Reloaded->GetPackage()->IsDirty());
	ASSERT_TRUE(UnloadPackage(Path));
}

TEST_F(FAssetPackageReloadTests, RejectsUnsavedCancelledAndOverBudgetRequestsWithoutMutation)
{
	using namespace Durin;
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/AssetDiscardTests/Admission", Path));
	DTexture2D* Texture = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Texture));
	std::string Error;
	ASSERT_TRUE(SetSource(*Texture, std::byte{23}, Error)) << Error;
	DPackage* Package = Texture->GetPackage();
	Package->MarkDirty();

	auto Unsaved = ReloadPackages({.Packages = {Package}}).GetResult();
	EXPECT_EQ(Unsaved.Status, EPackageReloadStatus::Failed);
	EXPECT_EQ(Unsaved.Failure, EPackageReloadFailure::Unsaved);
	EXPECT_EQ(FindResidentPackage(Path), Package);
	EXPECT_TRUE(Package->IsDirty());

	ASSERT_TRUE(SavePackage(Package));
	const auto SavedIdentity = Texture->GetSource().GetIdentity();
	ASSERT_TRUE(SetSource(*Texture, std::byte{45}, Error)) << Error;
	Package->MarkDirty();
	auto Cancelled = ReloadPackages({.Packages = {Package},
		.IsCancelled = [] { return true; }}).GetResult();
	EXPECT_EQ(Cancelled.Status, EPackageReloadStatus::Cancelled);
	EXPECT_EQ(FindResidentPackage(Path), Package);
	EXPECT_NE(Texture->GetSource().GetIdentity(), SavedIdentity);
	EXPECT_TRUE(Package->IsDirty());

	FPackageReloadRequest BudgetRequest{.Packages = {Package}};
	BudgetRequest.Budget.MaximumRetainedCpuBytes = 1;
	auto Budget = ReloadPackages(BudgetRequest).GetResult();
	EXPECT_EQ(Budget.Status, EPackageReloadStatus::Failed);
	EXPECT_EQ(Budget.Failure, EPackageReloadFailure::BudgetExceeded);
	EXPECT_EQ(FindResidentPackage(Path), Package);
	EXPECT_NE(Texture->GetSource().GetIdentity(), SavedIdentity);
	EXPECT_TRUE(Package->IsDirty());

	FPackageReloadRequest GpuBudgetRequest{.Packages = {Package}};
	GpuBudgetRequest.Budget.MaximumCandidateGpuBytes = 0;
	auto GpuBudget = ReloadPackages(GpuBudgetRequest).GetResult();
	EXPECT_EQ(GpuBudget.Status, EPackageReloadStatus::Failed);
	EXPECT_EQ(GpuBudget.Failure, EPackageReloadFailure::BudgetExceeded);
	EXPECT_EQ(FindResidentPackage(Path), Package);
	EXPECT_TRUE(Package->IsDirty());

	ASSERT_TRUE(SavePackage(Package));
	ASSERT_TRUE(UnloadPackage(Path));
}

TEST_F(FAssetPackageReloadTests, RejectsAnUnintegratedAssetFamilyWithoutClearingDirtyState)
{
	using namespace Durin;
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/AssetDiscardTests/StaticMesh", Path));
	DObject* Asset = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Asset));
	ASSERT_TRUE(SavePackage(Asset->GetPackage()));
	Asset->GetPackage()->MarkDirty();

	const auto Result = ReloadPackages({.Packages = {Asset->GetPackage()}}).GetResult();
	EXPECT_EQ(Result.Status, EPackageReloadStatus::Failed);
	EXPECT_EQ(Result.Failure, EPackageReloadFailure::Unsupported);
	EXPECT_EQ(FindResidentPackage(Path), Asset->GetPackage());
	EXPECT_TRUE(Asset->GetPackage()->IsDirty());

	ASSERT_TRUE(SavePackage(Asset->GetPackage()));
	ASSERT_TRUE(UnloadPackage(Path));
}

TEST_F(FAssetPackageReloadTests, ReloadsEveryTopLevelExportAndRebindsExternalReferencesTogether)
{
	using namespace Durin;
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/AssetDiscardTests/MultiTexture", Path));
	DTexture2D* Primary = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Primary));
	auto* Secondary = NewObject<DTexture2D>(Primary->GetPackage(), "Secondary");
	ASSERT_NE(Secondary, nullptr);
	std::string Error;
	ASSERT_TRUE(SetSource(*Primary, std::byte{11}, Error)) << Error;
	ASSERT_TRUE(SetSource(*Secondary, std::byte{22}, Error)) << Error;
	const auto PrimarySaved = Primary->GetSource().GetIdentity();
	const auto SecondarySaved = Secondary->GetSource().GetIdentity();
	ASSERT_TRUE(SavePackage(Primary->GetPackage()));
	Cloud->SetWeatherTexture(Secondary);
	ASSERT_TRUE(SetSource(*Primary, std::byte{33}, Error)) << Error;
	ASSERT_TRUE(SetSource(*Secondary, std::byte{44}, Error)) << Error;
	Primary->GetPackage()->MarkDirty();

	Editor::FEditableAssetDocumentModel Documents;
	ASSERT_TRUE(Documents.Discard(Primary));
	DPackage* Reloaded = FindResidentPackage(Path);
	ASSERT_NE(Reloaded, nullptr);
	ASSERT_NE(Reloaded, Primary->GetPackage());
	auto* ReloadedPrimary = Cast<DTexture2D>(Reloaded->FindTopLevelAsset(Primary->GetFName()));
	auto* ReloadedSecondary = Cast<DTexture2D>(Reloaded->FindTopLevelAsset(Secondary->GetFName()));
	ASSERT_NE(ReloadedPrimary, nullptr);
	ASSERT_NE(ReloadedSecondary, nullptr);
	EXPECT_EQ(ReloadedPrimary->GetSource().GetIdentity(), PrimarySaved);
	EXPECT_EQ(ReloadedSecondary->GetSource().GetIdentity(), SecondarySaved);
	EXPECT_EQ(Cloud->GetWeatherTexture(), ReloadedSecondary);
	EXPECT_FALSE(Reloaded->IsDirty());
	Cloud->SetWeatherTexture(nullptr);
	ASSERT_TRUE(UnloadPackage(Path));
}

TEST_F(FAssetPackageReloadTests, SecondPackagePreparationFailurePreservesTheWholeLiveBatch)
{
	using namespace Durin;
	std::array<FPackagePath, 2> Paths;
	std::array<DTexture2D*, 2> Textures{};
	std::array<FXxHash128, 2> Edited{};
	std::string Error;
	for (size_t Index = 0; Index < Paths.size(); ++Index)
	{
		ASSERT_TRUE(FPackagePath::TryCreate(
			std::format("/AssetDiscardTests/Batch{}", Index), Paths[Index]));
		ASSERT_TRUE(CreatePackageLeafAssetForTesting(Paths[Index], Textures[Index]));
		ASSERT_TRUE(SetSource(*Textures[Index], std::byte{static_cast<unsigned char>(10 + Index)}, Error)) << Error;
		ASSERT_TRUE(SavePackage(Textures[Index]->GetPackage()));
		ASSERT_TRUE(SetSource(*Textures[Index], std::byte{static_cast<unsigned char>(40 + Index)}, Error)) << Error;
		Textures[Index]->GetPackage()->MarkDirty();
		Edited[Index] = Textures[Index]->GetSource().GetIdentity();
	}
	FPackageReloadRequest Request{.Packages = {
		Textures[0]->GetPackage(), Textures[1]->GetPackage()}};
	Request.ShouldFail = [](EPackageReloadFaultPoint Point, uint64 Package, uint64) {
		return Point == EPackageReloadFaultPoint::ApplyValues && Package == 1;
	};
	const auto Result = ReloadPackages(Request).GetResult();
	EXPECT_EQ(Result.Status, EPackageReloadStatus::Failed);
	for (size_t Index = 0; Index < Paths.size(); ++Index)
	{
		EXPECT_EQ(FindResidentPackage(Paths[Index]), Textures[Index]->GetPackage());
		EXPECT_EQ(Textures[Index]->GetSource().GetIdentity(), Edited[Index]);
		EXPECT_TRUE(Textures[Index]->GetPackage()->IsDirty());
		ASSERT_TRUE(SavePackage(Textures[Index]->GetPackage()));
		ASSERT_TRUE(UnloadPackage(Paths[Index]));
	}
}

TEST_F(FAssetPackageReloadTests, ReloadRetiresCrossPackageHistoryAndInvalidatesTheOtherCheckpoint)
{
	using namespace Durin;
	std::array<FPackagePath, 2> Paths;
	std::array<DTexture2D*, 2> Textures{};
	std::string Error;
	for (size_t Index = 0; Index < Paths.size(); ++Index)
	{
		ASSERT_TRUE(FPackagePath::TryCreate(
			std::format("/AssetDiscardTests/History{}", Index), Paths[Index]));
		ASSERT_TRUE(CreatePackageLeafAssetForTesting(Paths[Index], Textures[Index]));
		ASSERT_TRUE(SetSource(*Textures[Index], std::byte{static_cast<unsigned char>(20 + Index)}, Error)) << Error;
		ASSERT_TRUE(SavePackage(Textures[Index]->GetPackage()));
	}

	auto* Transactions = NewObject<DTransBuffer>(nullptr, "ReloadHistory");
	TStrongObjectPtr<DTransBuffer> TransactionRoot(Transactions);
	Transactions->EstablishSavedState(*Textures[0]->GetPackage());
	Transactions->EstablishSavedState(*Textures[1]->GetPackage());
	{
		Editor::FScopedTransaction Scope(Transactions, {"test", "Cross-package edit"});
		for (DTexture2D* Texture : Textures)
		{
			Scope.Modify(Texture);
			Texture->SetBuildSettings(Texture->GetUsage(), !Texture->IsSRGB(),
				Texture->GetMaxResolution(), Texture->GetCompressionQuality(),
				Texture->GetAlphaMipMode(), Texture->GetAlphaCoverageThreshold());
		}
		ASSERT_TRUE(Scope.End());
	}
	ASSERT_EQ(Transactions->GetHistoryCount(), 1u);
	ASSERT_TRUE(Textures[1]->GetPackage()->IsDirty());

	FPackageReloadRequest Request{.Packages = {Textures[0]->GetPackage()}};
	Request.Participants.push_back(CreateTransactorReloadParticipant(
		*Transactions, Request.Packages));
	Request.ShouldFail = [](EPackageReloadFaultPoint Point, uint64, uint64) {
		return Point == EPackageReloadFaultPoint::PrepareHistory;
	};
	EXPECT_EQ(ReloadPackages(Request).GetResult().Status, EPackageReloadStatus::Failed);
	EXPECT_EQ(Transactions->GetHistoryCount(), 1u);
	EXPECT_TRUE(Transactions->CanUndo());
	for (size_t Index = 0; Index < Paths.size(); ++Index)
	{
		EXPECT_EQ(FindResidentPackage(Paths[Index]), Textures[Index]->GetPackage());
		EXPECT_TRUE(Textures[Index]->GetPackage()->IsDirty());
	}
	Request.ShouldFail = {};
	auto Operation = ReloadPackages(Request);
	ASSERT_TRUE(Operation.Wait());
	EXPECT_EQ(Transactions->GetHistoryCount(), 0u);
	EXPECT_FALSE(Transactions->GetPackageRevisionState(*Textures[0]->GetPackage()).has_value());
	const auto OtherState = Transactions->GetPackageRevisionState(*Textures[1]->GetPackage());
	ASSERT_TRUE(OtherState.has_value());
	EXPECT_FALSE(OtherState->bCheckpointValid);
	EXPECT_TRUE(Textures[1]->GetPackage()->IsDirty());

	ASSERT_TRUE(SavePackage(Textures[1]->GetPackage()));
	FAssetCompilingManager::Get().MarkCompilationAsCanceled(*Textures[1]);
	FAssetCompilingManager::Get().FinishCompilationForObject(*Textures[1]);
	TransactionRoot.Reset();
	CollectGarbage();
	ASSERT_TRUE(UnloadPackage(Paths[0]));
	ASSERT_TRUE(UnloadPackage(Paths[1]));
}

TEST_F(FAssetPackageReloadTests, EveryCoordinatorFailurePreservesTheEditedGraphAndAllowsRetry)
{
	using namespace Durin;
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/AssetDiscardTests/Faults", Path));
	DTexture2D* Texture = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Texture));
	std::string Error;
	ASSERT_TRUE(SetSource(*Texture, std::byte{17}, Error));
	ASSERT_TRUE(SavePackage(Texture->GetPackage()));
	const auto Saved = Texture->GetSource().GetIdentity();
	ASSERT_TRUE(SetSource(*Texture, std::byte{91}, Error));
	Texture->GetPackage()->MarkDirty();
	Cloud->SetWeatherTexture(Texture);
	const auto Edited = Texture->GetSource().GetIdentity();
	const EPackageReloadFaultPoint Points[] = {
		EPackageReloadFaultPoint::PreflightBudget,
		EPackageReloadFaultPoint::QuiesceSelected,
		EPackageReloadFaultPoint::ReadMain,
		EPackageReloadFaultPoint::ReadBulk,
		EPackageReloadFaultPoint::CreateSkeleton,
		EPackageReloadFaultPoint::ApplyValues,
		EPackageReloadFaultPoint::RestoreLedger,
		EPackageReloadFaultPoint::PreparePostLoad,
		EPackageReloadFaultPoint::PrepareReferences,
		EPackageReloadFaultPoint::PrepareNativeParticipant,
		EPackageReloadFaultPoint::PrepareHistory,
		EPackageReloadFaultPoint::PrepareRuntimeProduct,
		EPackageReloadFaultPoint::ReserveRenderPublish,
		EPackageReloadFaultPoint::RevalidateDisk,
		EPackageReloadFaultPoint::RevalidateReferencers,
		EPackageReloadFaultPoint::BeforeCommit};
	for (auto Fault : Points)
	{
		SCOPED_TRACE(static_cast<int>(Fault));
		bool Reached = false;
		FPackageReloadRequest Request{.Packages = {Texture->GetPackage()}};
		Request.ShouldFail = [&](EPackageReloadFaultPoint Point, uint64, uint64) {
			if (Point != Fault) return false;
			Reached = true;
			return true;
		};
		const auto Result = ReloadPackages(Request).GetResult();
		ASSERT_TRUE(Reached);
		ASSERT_EQ(Result.Status, EPackageReloadStatus::Failed);
		EXPECT_FALSE(Result.Diagnostics.empty());
		CollectGarbage();
		EXPECT_EQ(FindResidentPackage(Path), Texture->GetPackage());
		EXPECT_EQ(Cloud->GetWeatherTexture(), Texture);
		EXPECT_EQ(Texture->GetSource().GetIdentity(), Edited);
		EXPECT_TRUE(Texture->GetPackage()->IsDirty());
	}
	ASSERT_TRUE(ReloadPackages({.Packages = {Texture->GetPackage()}}).Wait());
	EXPECT_EQ(Cloud->GetWeatherTexture()->GetSource().GetIdentity(), Saved);
	Cloud->SetWeatherTexture(nullptr);
	ASSERT_TRUE(UnloadPackage(Path));
}

TEST_F(FAssetPackageReloadTests, DiscardUsesLatestSaveDespiteDirtyActivationAndFailedSave)
{
	using namespace Durin;
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/AssetDiscardTests/LatestSave", Path));
	DTexture2D* Texture = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Texture));
	std::string Error;
	ASSERT_TRUE(SetSource(*Texture, std::byte{17}, Error));
	ASSERT_TRUE(SavePackage(Texture->GetPackage()));
	ASSERT_TRUE(SetSource(*Texture, std::byte{41}, Error));
	Texture->GetPackage()->MarkDirty();
	ASSERT_TRUE(SavePackage(Texture->GetPackage()));
	const auto SavedB = Texture->GetSource().GetIdentity();
	ASSERT_TRUE(SetSource(*Texture, std::byte{91}, Error));
	Texture->GetPackage()->MarkDirty();
	Cloud->SetWeatherTexture(Texture);
	Editor::FEditableAssetDocumentModel Documents;
	ASSERT_TRUE(Documents.Activate({.Id = {1}, .ResourceId = Path.ToString()}, Texture));
	EXPECT_FALSE(Documents.Save(Texture, [] { return false; }, [](std::string) {}));
	EXPECT_TRUE(Texture->GetPackage()->IsDirty());
	ASSERT_TRUE(Documents.Discard(Texture));
	EXPECT_EQ(Cloud->GetWeatherTexture()->GetSource().GetIdentity(), SavedB);
	EXPECT_FALSE(Cloud->GetWeatherTexture()->GetPackage()->IsDirty());
	Cloud->SetWeatherTexture(nullptr);
	ASSERT_TRUE(UnloadPackage(Path));
}
