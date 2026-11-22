#include "MaterialResourceTestSupport.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionIO.h"
#include "Materials/MaterialExpressionMaterialAttributes.h"
#include "Materials/MaterialExpressionCustomData.h"
#include "Materials/MaterialExpressionArithmetic.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionScene.h"
#include "Materials/MaterialExpressionVector.h"
#include "Materials/MaterialExpressionVector4Parameter.h"
#include "Misc/UnifiedError.h"
#include "Asset/LoadErrors.h"
#include "NativeRHIBackendTestSupport.h"
#include "Materials/MaterialShaderMap.h"
#include "AssetForge/Builtins/PBRMaterialParameters.h"
#include "MaterialRenderRepresentationTestFixture.h"
#include "Materials/MaterialCustomVersion.h"
#include "Shader/ShaderCompilingManager.h"
#include "Shader/IShaderBuildModule.h"
#include "Shader/ShaderData.h"
#include "MaterialCookedProgramTestSupport.h"
#include "Threading/TaskRuntime.h"
#include "Asset/OfflinePreparation.h"
#include "NativePackageUnloadTestSupport.h"

static auto QualifyDefaultMaterialCook(bool Metal) -> void
{
	std::optional<Durin::UnifiedError::FError> SourceLoadError;
	Durin::Testing::FScopedRHIBackendOverride Backend(Metal ? "metal" : "vulkan");
	InitializeDObjectSystem();
	ASSERT_TRUE(Durin::FMountPaths::InitDefaultMountPoints());
	ASSERT_TRUE(Durin::RefreshAssetRegistry(
		Durin::EAssetRegistryScanMode::FullValidation));
	Durin::FPackagePath Path;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		Durin::DefaultMaterialPackagePath, Path));
	Durin::DMaterial* Source = nullptr;
	auto SourceLoad = Durin::LoadObject<Durin::DMaterial>(Durin::Testing::MakePackageLeafAssetObjectPathForTests(Path), nullptr, &SourceLoadError);
	Source = SourceLoad;
	ASSERT_TRUE(SourceLoad) << (*SourceLoadError);
	ASSERT_NE(Source, nullptr);
	const Durin::FMaterialProgramIdentity ExpectedIdentity =
		Durin::Testing::ExpectedGameMaterialIdentity(*Source, Metal ? Durin::MetalLibraryShaderTarget : Durin::VulkanShaderTarget);

	const std::filesystem::path CookRoot = std::filesystem::absolute(
		Durin::Testing::CreateTestFixtureDirectory("DefaultMaterialCook"));
	Durin::FCookContext Cook(
		Metal ? Durin::ECookTargetPlatform::MacOS : Durin::ECookTargetPlatform::Win64,
		Durin::ECookTargetProfile::Game);
	std::string Error;
	ASSERT_TRUE(Cook.AddPackage(std::string(Durin::DefaultMaterialPackagePath), (*Source).GetPackage())) << Error;
	ASSERT_TRUE(Durin::PublishCookContext(Cook, CookRoot)) << Error;
	EXPECT_TRUE(std::filesystem::is_regular_file(
		CookRoot / "Engine/Materials/DefaultMaterial.dasset"));
	EXPECT_FALSE(std::filesystem::is_regular_file(
		CookRoot / "Engine/Materials/DefaultMaterial.dbulk"));
	Durin::ShutdownAssetRuntime();
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
	auto RuntimeConfiguration = Durin::FAssetRuntimeConfiguration::Authored();
	auto Result = Durin::FAssetRuntimeConfiguration::Cooked(
		CookRoot, RuntimeConfiguration);
	ASSERT_TRUE(Result) << (Result ? std::string{} : Result.error().ToString());
	const auto Initialized = Durin::InitializeAssetRuntime(std::move(RuntimeConfiguration));
	ASSERT_TRUE(Initialized) << (Initialized ? std::string{} : Initialized.error().ToString());
	{
	std::optional<Durin::UnifiedError::FError> MissingVersionError;
	const std::array CookMountDefinitions{
		Durin::FMountPoint{
			.VirtualRoot = "/Engine/",
			.Owner = Durin::EMountOwner::Test,
			.Root = CookRoot / "Engine",
			.bAutoScan = true}};
	Durin::Testing::FScopedMountRegistryFixture CookMounts(
		CookMountDefinitions);
	ASSERT_TRUE(CookMounts.IsValid()) << CookMounts.GetError();
	Durin::FAssetPackageInspection Inspection;
	ASSERT_TRUE(Durin::InspectAssetPackage(
		(CookRoot / "Engine/Materials/DefaultMaterial.dasset").generic_string(),
		Inspection));
	EXPECT_NE(Inspection.FindField("ProgramData"), nullptr);
	const auto CookedFile = CookRoot / "Engine/Materials/DefaultMaterial.dasset";
	Durin::FByteBuffer OriginalBytes;
	auto OriginalBytesRead = Durin::FFileHelper::LoadFileToArray(CookedFile);
	ASSERT_TRUE(OriginalBytesRead) << OriginalBytesRead.error().GetMessage();
	OriginalBytes = std::move(*OriginalBytesRead);
	Durin::ObjectPackage::FLinkerTables VersionLinker;
	ASSERT_TRUE(Durin::ObjectPackage::ReadPackage(OriginalBytes, {}, Path, VersionLinker));
	ASSERT_EQ(VersionLinker.CustomVersions, (std::vector<Durin::FCustomVersion>{
		{Durin::FMaterialGraphVersion::Guid, Durin::FMaterialGraphVersion::CurrentVersion},
		{Durin::FMaterialOutputVersion::Guid, Durin::FMaterialOutputVersion::CurrentVersion}}));
	VersionLinker.CustomVersions.clear();
	Durin::FByteBuffer MissingVersionBytes, UnusedBulk;
	ASSERT_TRUE(Durin::ObjectPackage::WritePackage(VersionLinker, MissingVersionBytes, UnusedBulk));
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(MissingVersionBytes, CookedFile));
	ASSERT_TRUE(Durin::RefreshAssetRegistry(Durin::EAssetRegistryScanMode::FullValidation));
	Durin::DMaterial* Rejected = nullptr;
	const auto MissingVersion = Durin::LoadObject<Durin::DMaterial>(Durin::Testing::MakePackageLeafAssetObjectPathForTests(Path), nullptr, &MissingVersionError);
	Rejected = MissingVersion;
	EXPECT_FALSE(MissingVersion) << (MissingVersionError ? MissingVersionError->ToString() : "");
	EXPECT_EQ(Rejected, nullptr);
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(OriginalBytes, CookedFile));
	ASSERT_TRUE(Durin::RefreshAssetRegistry(
		Durin::EAssetRegistryScanMode::FullValidation));
	Durin::DMaterial* Cooked = nullptr;
	{
		std::optional<Durin::UnifiedError::FError> LoadedError;
		auto Loaded = Durin::LoadObject<Durin::DMaterial>(Durin::Testing::MakePackageLeafAssetObjectPathForTests(Path), nullptr, &LoadedError);
		Cooked = Loaded;
		ASSERT_NE(Loaded, nullptr) << (LoadedError ? LoadedError->ToString() : "");
	}
	ASSERT_NE(Cooked, nullptr);
	// Loading starts owner preparation; queries do not decode the payload.
	auto& CookedData = const_cast<Durin::FBulkData&>(Cooked->GetCookedProgramData());
	const auto SavedCookedData = CookedData;
	const std::array InvalidProgramBytes{std::byte{0}};
	auto InvalidData = Durin::FBulkData::TryCreateDetached(InvalidProgramBytes);
	ASSERT_TRUE(InvalidData);
	CookedData = std::move(*InvalidData);
	Cooked->PostLoad();
	// PostLoad can observe the completed asynchronous read on its first poll.
	const auto DiagnosticAfterLoad = Cooked->GetMaterialCookDiagnostic();
	EXPECT_EQ(Cooked->GetAcceptedCompiledProgram(), nullptr);
	EXPECT_EQ(Cooked->GetMaterialCookDiagnostic(), DiagnosticAfterLoad);
	const std::array<Durin::DObject*, 1> Owners{Cooked};
	Durin::FShaderCompilingManager::Get().FinishCompilationForObjects(Owners);
	EXPECT_EQ(Cooked->GetMaterialCompileStatus().State, Durin::EMaterialCompileState::Failed);
	EXPECT_FALSE(Cooked->GetMaterialCookDiagnostic().empty());
	EXPECT_EQ(Cooked->GetAcceptedCompiledProgram(), nullptr);
	CookedData = SavedCookedData;
	Cooked->CacheResourceShadersForRendering();
	Durin::FShaderCompilingManager::Get().MarkCompilationAsCanceled(Owners);
	EXPECT_FALSE(Cooked->IsCompiling());
	EXPECT_EQ(Cooked->GetMaterialCompileStatus().State, Durin::EMaterialCompileState::Canceled);
	Cooked->CacheResourceShadersForRendering();
	const auto OriginalQuality = Durin::GetMaterialQualityLevel();
	ASSERT_TRUE(Durin::SetMaterialQualityLevel(OriginalQuality == Durin::EMaterialQualityLevel::High
		? Durin::EMaterialQualityLevel::Low : Durin::EMaterialQualityLevel::High));
	Durin::FShaderCompilingManager::Get().FinishCompilationForObjects(Owners);
	EXPECT_EQ(Cooked->GetMaterialCompileStatus().State, Durin::EMaterialCompileState::Ready);
	ASSERT_TRUE(Cooked->GetAcceptedCompiledProgram());
	EXPECT_EQ(Cooked->GetAcceptedCompiledProgram()->Quality, OriginalQuality);
	EXPECT_TRUE(Cooked->GetMaterialCookDiagnostic().empty());
	ASSERT_TRUE(Durin::SetMaterialQualityLevel(OriginalQuality));
	Cooked->CacheResourceShadersForRendering();
	const auto VersionBeforeReplace = Cooked->GetRenderChangeCount();
	// Deliberately replace identical inline storage without a material edit event.
	auto& Storage = const_cast<Durin::FBulkData&>(Cooked->GetCookedProgramData());
	Durin::FBulkData StorageSnapshot = Storage;
	const auto SizeBeforeReplace = Storage.GetMetadata().LogicalSize;
	Storage = StorageSnapshot;
	EXPECT_EQ(Storage.GetMetadata().LogicalSize, SizeBeforeReplace);
	EXPECT_EQ(Cooked->GetRenderChangeCount(), VersionBeforeReplace);
	Durin::FShaderCompilingManager::Get().FinishCompilationForObjects(Owners);
	EXPECT_EQ(Cooked->GetMaterialCompileStatus().State, Durin::EMaterialCompileState::Ready);
	EXPECT_TRUE(Cooked->GetAcceptedCompiledProgram());
	// Multiple owners decode through the ordinary task scheduler. Repeated cache
	// calls reuse the current resource association.
	std::vector<Durin::TStrongObjectPtr<Durin::DMaterial>> Burst;
	std::vector<Durin::DObject*> BurstOwners;
	for (uint32 Index = 0; Index < 24; ++Index)
	{
		auto* Material = Durin::NewObject<Durin::DMaterial>(nullptr,
			Durin::FName(std::format("CookedAdmission{}", Index)));
		const_cast<Durin::FBulkData&>(Material->GetCookedProgramData()) = SavedCookedData;
		Material->PostLoad();
		Material->CacheResourceShadersForRendering();
		Burst.emplace_back(Material);
		BurstOwners.push_back(Material);
	}
	Durin::FShaderCompilingManager::Get().FinishCompilationForObjects(BurstOwners);
	for (const auto& Material : Burst)
	{
		EXPECT_FALSE(Material->IsCompiling());
		EXPECT_TRUE(Material->IsComplete()) << Material->GetMaterialCookDiagnostic();
	}
	Cooked->CacheResourceShadersForRendering();
	Durin::FShaderCompilingManager::Get().ProcessAsyncTasks({});
	Cooked->CacheResourceShadersForRendering(Durin::EMaterialShaderPrecompileMode::Synchronous);
	ASSERT_TRUE(Cooked->IsComplete()) << Cooked->GetMaterialCookDiagnostic();
	const auto DefaultProgram = Cooked->GetAcceptedCompiledProgram();
	Durin::TStrongObjectPtr<Durin::DMaterialInstance> RuntimeInstance(
		Durin::NewObject<Durin::DMaterialInstance>(nullptr, "AdoptedCookedResource", Durin::EObjectFlags::Transient));
	ASSERT_TRUE(RuntimeInstance->SetParent(Cooked));
	EXPECT_EQ(RuntimeInstance->GetAcceptedCompiledProgram(), DefaultProgram);
	RuntimeInstance->CacheResourceShadersForRendering(Durin::EMaterialShaderPrecompileMode::Synchronous);
	EXPECT_TRUE(RuntimeInstance->IsComplete()) << RuntimeInstance->GetMaterialCookDiagnostic();
	EXPECT_EQ(RuntimeInstance->GetAcceptedCompiledProgram(), DefaultProgram);
	Cooked->CacheResourceShadersForRendering(Durin::ERHIFeatureLevel::ES3_1,
		Durin::EMaterialShaderPrecompileMode::Synchronous);
	const auto LowProgram = Cooked->GetAcceptedCompiledProgram(Durin::ERHIFeatureLevel::ES3_1);
	ASSERT_TRUE(LowProgram) << Cooked->GetMaterialCookDiagnostic(Durin::ERHIFeatureLevel::ES3_1);
	EXPECT_EQ(LowProgram->FeatureLevel, Durin::ERHIFeatureLevel::ES3_1);
	EXPECT_EQ(Cooked->GetAcceptedCompiledProgram(), DefaultProgram);
	{
		using namespace Durin;
		ASSERT_EQ(GetMaterialCompilationStatistics().OutstandingConsumerCount, 0u);
		FShaderCompilingManager::Get().FinishAllCompilation();
		std::vector<TStrongObjectPtr<DMaterial>> Budgeted;
		std::vector<FWeakObjectPtr> NotifiedAssets;
		struct FPostCompileScope
		{
			FDelegateHandle Handle;
			~FPostCompileScope() { FAssetCompilingManager::Get().OnAssetPostCompile().Remove(Handle); }
		} Notifications{FAssetCompilingManager::Get().OnAssetPostCompile().AddLambda([&](const FAssetPostCompileData& Data) {
			NotifiedAssets.insert(NotifiedAssets.end(), Data.Assets.begin(), Data.Assets.end());
		})};
		for (uint32 Index = 0; Index < 2; ++Index)
		{
			auto* Owner = NewObject<DMaterial>(nullptr, FName(std::format("BudgetedCookedResource{}", Index)));
			Budgeted.emplace_back(Owner);
			const_cast<FBulkData&>(Owner->GetCookedProgramData()) = SavedCookedData;
			Owner->CacheResourceShadersForRendering();
			Owner->PostLoad(); // Repeated loading must detach the prior logical read as well.
			Owner->CacheResourceShadersForRendering(ERHIFeatureLevel::ES3_1);
		}
		const auto ReadyCount = [&] {
			uint32 Count = 0;
			for (const auto& Owner : Budgeted)
				for (const auto Level : {ERHIFeatureLevel::SM5, ERHIFeatureLevel::ES3_1})
					Count += Owner->GetMaterialCompileStatus(Level).State == EMaterialCompileState::Ready;
			return Count;
		};
		const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (ReadyCount() != 4 && std::chrono::steady_clock::now() < Deadline)
		{
			FShaderCompilingManager::Get().ProcessAsyncTasks({});
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		EXPECT_EQ(ReadyCount(), 4u);
		for (const auto& Owner : Budgeted)
			EXPECT_TRUE(std::ranges::any_of(NotifiedAssets, [&](const auto& Asset) { return Asset.Get() == Owner.Get(); }));
		const size_t BeforeEmptyPump = NotifiedAssets.size();
		FShaderCompilingManager::Get().ProcessAsyncTasks({});
		EXPECT_EQ(NotifiedAssets.size(), BeforeEmptyPump);

		TStrongObjectPtr<DMaterial> FailedTier(NewObject<DMaterial>(nullptr, "CookedTierFailureEvent"));
		const_cast<FBulkData&>(FailedTier->GetCookedProgramData()) = SavedCookedData;
		FailedTier->CacheResourceShadersForRendering(EMaterialShaderPrecompileMode::Synchronous);
		ASSERT_TRUE(FailedTier->IsComplete());
		auto BadPayload = FBulkData::TryCreateDetached(InvalidProgramBytes);
		ASSERT_TRUE(BadPayload);
		const_cast<FBulkData&>(FailedTier->GetCookedProgramData()) = std::move(*BadPayload);
		FailedTier->CacheResourceShadersForRendering(ERHIFeatureLevel::ES3_1);
		const size_t BeforeFailedPump = NotifiedAssets.size();
		const auto FailureDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (FailedTier->IsCompiling() && std::chrono::steady_clock::now() < FailureDeadline)
		{
			FShaderCompilingManager::Get().ProcessAsyncTasks({});
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		EXPECT_EQ(NotifiedAssets.size(), BeforeFailedPump);
		EXPECT_EQ(FailedTier->GetMaterialCompileStatus(ERHIFeatureLevel::ES3_1).State, EMaterialCompileState::Failed);
		EXPECT_TRUE(FailedTier->IsComplete());
	}
	{
		struct FRestoreTasks
		{
			bool bDeferred = Durin::GetGameThreadDeferredWorkQueueDiagnostics().bAccepting;
			~FRestoreTasks()
			{
				EXPECT_TRUE(Durin::InitializeTaskScheduler(2));
				if (bDeferred) EXPECT_TRUE(Durin::InitializeGameThreadDeferredExecutor());
			}
		} Restore;
		Durin::FAssetCompilingManager::Get().FinishAllCompilation();
		Durin::TStrongObjectPtr<Durin::DMaterial> Pending(
			Durin::NewObject<Durin::DMaterial>(nullptr, "PendingCookedProgram"));
		const_cast<Durin::FBulkData&>(Pending->GetCookedProgramData()) = SavedCookedData;
		Pending->CacheResourceShadersForRendering();
		ASSERT_TRUE(Pending->IsCompiling());
		Durin::ShutdownTaskSystem(Durin::ETaskShutdownMode::Drain);
		const std::array<Durin::DObject*, 1> PendingOwners{Pending.Get()};
		Durin::FShaderCompilingManager::Get().FinishCompilationForObjects(PendingOwners);
		EXPECT_EQ(Pending->GetMaterialCompileStatus().State, Durin::EMaterialCompileState::Ready);
		EXPECT_FALSE(Pending->IsCompiling());
		EXPECT_TRUE(Pending->GetMaterialCookDiagnostic().empty());
		Durin::TStrongObjectPtr<Durin::DMaterial> Offline(
			Durin::NewObject<Durin::DMaterial>(nullptr, "OfflineCookedProgram"));
		const_cast<Durin::FBulkData&>(Offline->GetCookedProgramData()) = SavedCookedData;
		const auto BeforeNone = Offline->GetMaterialCompileStatus().State;
		Offline->CacheResourceShadersForRendering(Durin::EMaterialShaderPrecompileMode::None);
		EXPECT_EQ(Offline->GetMaterialCompileStatus().State, BeforeNone);
		Offline->CacheResourceShadersForRendering();
		Durin::FShaderCompilingManager::Get().ProcessAsyncTasks({});
		EXPECT_EQ(Offline->GetMaterialCompileStatus().State, Durin::EMaterialCompileState::Failed);
		EXPECT_FALSE(Offline->IsCompiling());
		EXPECT_FALSE(Offline->GetMaterialCookDiagnostic().empty());
		const std::array<Durin::DObject*, 1> OfflineOwners{Offline.Get()};
		Durin::FShaderCompilingManager::Get().FinishCompilationForObjects(OfflineOwners);
		Offline->CacheResourceShadersForRendering(Durin::EMaterialShaderPrecompileMode::Synchronous);
		EXPECT_TRUE(Offline->IsComplete()) << Offline->GetMaterialCookDiagnostic();
		ASSERT_TRUE(Offline->GetAcceptedCompiledProgram());
		EXPECT_EQ(Offline->GetAcceptedCompiledProgram()->GetShaderMap().ShaderMapId, ExpectedIdentity);
		Durin::TStrongObjectPtr<Durin::DMaterial> Invalid(
			Durin::NewObject<Durin::DMaterial>(nullptr, "InvalidOfflineCookedProgram"));
		auto InvalidPayload = Durin::FBulkData::TryCreateDetached(InvalidProgramBytes);
		ASSERT_TRUE(InvalidPayload);
		const_cast<Durin::FBulkData&>(Invalid->GetCookedProgramData()) = std::move(*InvalidPayload);
		Invalid->CacheResourceShadersForRendering(Durin::EMaterialShaderPrecompileMode::Synchronous);
		EXPECT_EQ(Invalid->GetMaterialCompileStatus().State, Durin::EMaterialCompileState::Failed);
		EXPECT_FALSE(Invalid->IsCompiling());
		EXPECT_FALSE(Invalid->GetMaterialCookDiagnostic().empty());
	}
	ASSERT_TRUE(Cooked->GetAcceptedCompiledProgram()) << Cooked->GetMaterialCookDiagnostic();
	EXPECT_TRUE(Cooked->GetMaterialCookDiagnostic().empty());
	EXPECT_EQ(Cooked->GetAcceptedCompiledProgram()->GetShaderMap().CompiledShaders.size(), 5u);
	EXPECT_EQ(Cooked->GetAcceptedCompiledProgram()->GetShaderMap().ShaderMapId, ExpectedIdentity);
	for (const auto& Shader : Cooked->GetAcceptedCompiledProgram()->GetShaderMap().CompiledShaders)
	{
		EXPECT_EQ(Shader.Target, Metal ? Durin::MetalLibraryShaderTarget : Durin::VulkanShaderTarget);
		EXPECT_TRUE(Durin::ShaderFormat::ValidateShaderCode(Shader));
	}
	const auto ProgramBeforeCanceledResave = Cooked->GetAcceptedCompiledProgram();
	const Durin::FAssetBuildTaskContext CanceledControl{.IsCancellationRequested = [] { return true; }};
	Durin::FBulkData ResavedData;
	const auto CanceledResave = Cooked->PrepareCookedProgram(Metal ? Durin::ECookTargetPlatform::MacOS : Durin::ECookTargetPlatform::Win64, {}, ResavedData, CanceledControl);
	EXPECT_EQ(CanceledResave.Error, Durin::EObjectCookError::Cancelled);
	EXPECT_EQ(Cooked->GetAcceptedCompiledProgram(), ProgramBeforeCanceledResave);
	const auto BuildsBeforeResave = Durin::IShaderBuildModule::Get()->GetStats().Compilations;
	auto Resaved = Cooked->PrepareCookedProgram(Metal ? Durin::ECookTargetPlatform::MacOS : Durin::ECookTargetPlatform::Win64, {}, ResavedData);
	ASSERT_TRUE(Resaved) << Resaved.Diagnostic;
	auto ResavedRead = ResavedData.AcquireRead();
	ASSERT_TRUE(ResavedRead);
	Durin::FMaterialStaticProperties ResavedProperties;
	std::shared_ptr<const Durin::FMaterialProgram> ResavedProgram;
	ASSERT_TRUE(Durin::DecodeMaterialCookedProgramFamily(ResavedRead->GetBytes(),
		Metal ? Durin::ECookTargetPlatform::MacOS : Durin::ECookTargetPlatform::Win64,
		Durin::ECookTargetProfile::Game, Durin::GetMaterialQualityLevel(), Durin::ERHIFeatureLevel::SM5, {}, ResavedProperties, ResavedProgram));
	EXPECT_EQ(ResavedProgram->GetShaderMap().ShaderMapId, ExpectedIdentity);
	EXPECT_EQ(Durin::IShaderBuildModule::Get()->GetStats().Compilations, BuildsBeforeResave);
	EXPECT_TRUE(Cooked->GetMaterialCompileStatus().IsCurrent());
	const auto CookedStatistics = Durin::GetMaterialProgramStatistics(*Cooked);
	EXPECT_EQ(CookedStatistics.State,
		Durin::EMaterialProgramStatisticsState::Current);
	EXPECT_EQ(CookedStatistics.NormalizedIRNodeCount, 0u);
	EXPECT_EQ(CookedStatistics.GeneratedSourceBytes, 0u);
	EXPECT_GT(CookedStatistics.CompiledCodeBytes, 0u);
	EXPECT_EQ(
		GetMaterialBinding(Durin::Testing::CaptureMaterialRenderData(*Cooked)).BaseColor,
		Durin::FVector4f(0.f, 0.f, 0.f, 1.f));
	}

	Durin::ShutdownAssetRuntime();
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
	ASSERT_TRUE(Durin::InitializeAssetRuntime());
	ASSERT_TRUE(Durin::RefreshAssetRegistry(
		Durin::EAssetRegistryScanMode::FullValidation));
}

TEST(FDefaultMaterialCookTests, UnreferencedBuiltInRootPublishesAndLoadsCooked)
{
	QualifyDefaultMaterialCook(false);
}

#if defined(__APPLE__)
TEST(FDefaultMaterialCookTests, MacOSBuiltInRootPublishesAndLoadsBinaryStages)
{
	QualifyDefaultMaterialCook(true);
}
#endif

TEST(FDefaultMaterialCookTests, ActiveParametersSurviveGraphStripping)
{
	std::optional<Durin::UnifiedError::FError> SourceLoadError;
	Durin::Testing::FScopedRHIBackendOverride Backend("vulkan");
	InitializeDObjectSystem();
	ASSERT_TRUE(Durin::FMountPaths::InitDefaultMountPoints());
	ASSERT_TRUE(Durin::RefreshAssetRegistry(
		Durin::EAssetRegistryScanMode::FullValidation));
	Durin::FPackagePath Path;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		Durin::DefaultMaterialPackagePath, Path));
	Durin::DMaterial* Source = nullptr;
	auto SourceLoad = Durin::LoadObject<Durin::DMaterial>(Durin::Testing::MakePackageLeafAssetObjectPathForTests(Path), nullptr, &SourceLoadError);
	Source = SourceLoad;
	ASSERT_TRUE(SourceLoad) << (*SourceLoadError);
	ASSERT_NE(Source, nullptr);
	auto Validation = Durin::Testing::MakePBRMaterialExpressionsForTest().Apply(*Source);
	ASSERT_TRUE(Validation);
	ASSERT_TRUE(Source->SetVectorParameterValue(
		Durin::AssetForge::Builtins::MaterialParameters::BaseColorName(), Durin::FVector3(0.2, 0.4, 0.7)));
	auto* AuthoredInstance = Durin::NewObject<Durin::DMaterialInstance>(
		Source->GetPackage(), "CookedOverrides");
	ASSERT_TRUE(AuthoredInstance->SetParent(Source));
	ASSERT_TRUE(AuthoredInstance->SetVectorParameterValue(
		Durin::AssetForge::Builtins::MaterialParameters::BaseColorName(), Durin::FVector3(0.8, 0.3, 0.1)));
	Durin::FSoftObjectPath InstancePath;
	ASSERT_TRUE(Durin::FSoftObjectPath::TryCreate(AuthoredInstance->GetObjectPath(), InstancePath));
	const Durin::FMaterialProgramIdentity ExpectedIdentity =
		Durin::Testing::ExpectedGameMaterialIdentity(*Source);
	std::array<Durin::FSoftObjectPath, 3> VariantPaths;
	std::array<Durin::FMaterialProgramIdentity, 3> VariantIdentities;
	for (size_t Index = 0; Index < VariantPaths.size(); ++Index)
	{
		auto* Variant = Durin::NewObject<Durin::DMaterialInstance>(Source->GetPackage(),
			Durin::FName(std::format("CookedVariant{}", Index)));
		Durin::FMaterialPropertyOverrides Overrides;
		Overrides.bOverrideBlendMode = true;
		Overrides.bOverrideOpacityMaskThreshold = true;
		Overrides.Values.BlendMode = Index < 2 ? Durin::EMaterialBlendMode::Masked : Durin::EMaterialBlendMode::Translucent;
		Overrides.Values.OpacityMaskThreshold = Index == 0 ? 0.25f : 0.75f;
		ASSERT_TRUE(Variant->SetParentAndPropertyOverrides(AuthoredInstance, Overrides));
		ASSERT_TRUE(Variant->GetAcceptedCompiledProgram());
		VariantIdentities[Index] = Durin::Testing::ExpectedGameMaterialIdentity(*Variant);
		ASSERT_TRUE(Durin::FSoftObjectPath::TryCreate(Variant->GetObjectPath(), VariantPaths[Index]));
	}

	// A transient instance inside the authored package must not need its own
	// compiled payload or enter the cooked package graph.
	auto* Preview = Durin::DMaterialInstance::CreateDynamic(AuthoredInstance, Source, "TransientCookPreview");
	ASSERT_TRUE(Preview);
	ASSERT_TRUE(Preview->SetScalarParameterValue(
		Durin::AssetForge::Builtins::MaterialParameters::RoughnessName(), 0.91f));
	const std::filesystem::path CookRoot = std::filesystem::absolute(
		Durin::Testing::CreateTestFixtureDirectory("ActiveMaterialCook"));
	Durin::FCookContext Cook(
		Durin::ECookTargetPlatform::Win64,
		Durin::ECookTargetProfile::Game);
	std::string Error;
	ASSERT_TRUE(Cook.AddPackage(std::string(Durin::DefaultMaterialPackagePath), (*Source).GetPackage())) << Error;
	ASSERT_TRUE(Durin::PublishCookContext(Cook, CookRoot)) << Error;
	EXPECT_TRUE(std::filesystem::is_regular_file(
		CookRoot / "Engine/Materials/DefaultMaterial.dasset"));
	EXPECT_FALSE(std::filesystem::is_regular_file(
		CookRoot / "Engine/Materials/DefaultMaterial.dbulk"));
	Durin::ShutdownAssetRuntime();
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
	auto RuntimeConfiguration = Durin::FAssetRuntimeConfiguration::Authored();
	auto Result = Durin::FAssetRuntimeConfiguration::Cooked(
		CookRoot, RuntimeConfiguration);
	ASSERT_TRUE(Result) << (Result ? std::string{} : Result.error().ToString());
	const auto Initialized = Durin::InitializeAssetRuntime(std::move(RuntimeConfiguration));
	ASSERT_TRUE(Initialized) << (Initialized ? std::string{} : Initialized.error().ToString());
	{
	const std::array CookMountDefinitions{
		Durin::FMountPoint{
			.VirtualRoot = "/Engine/",
			.Owner = Durin::EMountOwner::Test,
			.Root = CookRoot / "Engine",
			.bAutoScan = true}};
	Durin::Testing::FScopedMountRegistryFixture CookMounts(
		CookMountDefinitions);
	ASSERT_TRUE(CookMounts.IsValid()) << CookMounts.GetError();
	Durin::FAssetPackageInspection Inspection;
	ASSERT_TRUE(Durin::InspectAssetPackage(
		(CookRoot / "Engine/Materials/DefaultMaterial.dasset").generic_string(),
		Inspection));
	EXPECT_TRUE(std::ranges::any_of(Inspection.Objects, [](const auto& Object) {
		return Object.FindField("ProgramData") != nullptr;
	}));
	ASSERT_TRUE(Durin::RefreshAssetRegistry(
		Durin::EAssetRegistryScanMode::FullValidation));
	Durin::DMaterial* Cooked = nullptr;
	{
		std::optional<Durin::UnifiedError::FError> LoadedError;
		auto Loaded = Durin::LoadObject<Durin::DMaterial>(Durin::Testing::MakePackageLeafAssetObjectPathForTests(Path), nullptr, &LoadedError);
		Cooked = Loaded;
		ASSERT_NE(Loaded, nullptr) << (LoadedError ? LoadedError->ToString() : "");
	}
	ASSERT_NE(Cooked, nullptr);
	const std::array<Durin::DObject*, 1> CookedOwners{Cooked};
	const auto BudgetDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
	bool bCookedNotified = false;
	struct FPostCompileScope
	{
		Durin::FDelegateHandle Handle;
		~FPostCompileScope() { Durin::FAssetCompilingManager::Get().OnAssetPostCompile().Remove(Handle); }
	} Notifications{Durin::FAssetCompilingManager::Get().OnAssetPostCompile().AddLambda([&](const Durin::FAssetPostCompileData& Data) {
		for (const auto& Asset : Data.Assets) bCookedNotified |= Asset.Get() == Cooked;
	})};
	while (Cooked->IsCompiling() && std::chrono::steady_clock::now() < BudgetDeadline)
	{
		Durin::FShaderCompilingManager::Get().ProcessAsyncTasks({});
		std::this_thread::yield();
	}
	EXPECT_TRUE(bCookedNotified);
	Durin::FShaderCompilingManager::Get().FinishCompilationForObjects(CookedOwners);
	ASSERT_TRUE(Cooked->GetAcceptedCompiledProgram());
	EXPECT_EQ(Cooked->GetAcceptedCompiledProgram()->GetShaderMap().ShaderMapId, ExpectedIdentity);
	EXPECT_TRUE(Cooked->GetMaterialCompileStatus().IsCurrent());
	EXPECT_EQ(
		GetMaterialBinding(Durin::Testing::CaptureMaterialRenderData(*Cooked)).BaseColor,
		Durin::FVector4f(0.2f, 0.4f, 0.7f, 1.0f));
	EXPECT_TRUE(std::ranges::none_of(Inspection.Objects, [](const auto& Object) {
		return Object.FindField("Program") != nullptr;
	}));
	EXPECT_TRUE(Cooked->GetExpressionCollection().Expressions.empty());
	EXPECT_FALSE(Cooked->GetAcceptedCompiledProgram()->GetShaderMap().ActiveParameters.empty());
	Durin::DMaterialInstance* Instance = nullptr;
	{
		std::optional<Durin::UnifiedError::FError> LoadedError;
		auto Loaded = Durin::LoadObject<Durin::DMaterialInstance>(InstancePath, nullptr, &LoadedError);
		Instance = Loaded;
		ASSERT_NE(Loaded, nullptr) << (LoadedError ? LoadedError->ToString() : "");
	}
	ASSERT_NE(Instance, nullptr);
	EXPECT_EQ(Instance->GetParent(), Cooked);
	Instance->PostLoad(); // Exercise initialization before the owner queue advances.
	ASSERT_TRUE(Instance->IsCompiling());
	auto* PendingChild = Durin::DMaterialInstance::CreateDynamic(Instance, nullptr, "PendingCookedChild");
	ASSERT_NE(PendingChild, nullptr);
	ASSERT_TRUE(PendingChild->SetVectorParameterValue(
		Durin::AssetForge::Builtins::MaterialParameters::BaseColorName(), Durin::FVector3(0.3, 0.6, 0.8)));
	for (size_t Index = 0; Index < VariantPaths.size(); ++Index)
	{
		Durin::DMaterialInstance* Variant = nullptr;
		{
			std::optional<Durin::UnifiedError::FError> LoadedValueError;
			auto LoadedValue = Durin::LoadObject<Durin::DMaterialInstance>(VariantPaths[Index], nullptr, &LoadedValueError);
			Variant = LoadedValue;
			ASSERT_TRUE(LoadedValue);
		}
		ASSERT_TRUE(Variant);
		ASSERT_GT(Variant->GetCookedProgramData().GetMetadata().LogicalSize, 0u);
		const std::array<Durin::DObject*, 1> VariantOwners{Variant};
		Durin::FShaderCompilingManager::Get().FinishCompilationForObjects(VariantOwners);
		ASSERT_TRUE(Variant->GetAcceptedCompiledProgram());
		EXPECT_EQ(Variant->GetAcceptedCompiledProgram()->GetShaderMap().ShaderMapId, VariantIdentities[Index]);
		EXPECT_NE(Variant->GetAcceptedCompiledProgram()->GetShaderMap().ShaderMapId, ExpectedIdentity);
		EXPECT_TRUE(Variant->GetAcceptedCompiledProgram()->GetDebugData().IR.Nodes.empty());
		EXPECT_TRUE(Variant->GetAcceptedCompiledProgram()->GetDebugData().GeneratedSource.empty());
		EXPECT_FALSE(Durin::Testing::CaptureMaterialRenderData(*Variant).Representation.IsError());
		ExpectColorNear(GetMaterialBinding(Durin::Testing::CaptureMaterialRenderData(*Variant)).BaseColor,
			Durin::FVector4f(0.8f, 0.3f, 0.1f, 1.0f));
	}

	const std::array<Durin::DObject*, 1> InstanceOwners{Instance};
	Durin::FShaderCompilingManager::Get().FinishCompilationForObjects(InstanceOwners);
	ExpectColorNear(GetMaterialBinding(Durin::Testing::CaptureMaterialRenderData(*PendingChild)).BaseColor,
		Durin::FVector4f(0.3f, 0.6f, 0.8f, 1.0f));
	auto* Child = Durin::DMaterialInstance::CreateDynamic(Instance, nullptr, "CookedChild");
	ASSERT_TRUE(Child);
	EXPECT_TRUE(Child->IsDynamicInstance());
	EXPECT_EQ(Child->GetAcceptedCompiledProgram(), Instance->GetAcceptedCompiledProgram());
	EXPECT_FALSE(Durin::RequestMaterialRecompile(*Child));
	auto* Texture = Durin::NewObject<Durin::DTexture2D>(nullptr, "CookedDynamicTexture");
	ASSERT_TRUE(Texture->GetTextureReferenceRHI());
	const auto AcceptedProgram = Cooked->GetAcceptedCompiledProgram();
	ASSERT_TRUE(Cooked->SetTextureParameterValue(
		Durin::AssetForge::Builtins::MaterialParameters::BaseColorTextureName(), Texture));
	Cooked->CacheResourceShadersForRendering();
	EXPECT_FALSE(Cooked->IsCompiling());
	EXPECT_EQ(Cooked->GetAcceptedCompiledProgram()->ShaderMap, AcceptedProgram->ShaderMap);
	Instance->GetPackage()->ClearDirty();
	ASSERT_TRUE(Child->SetTextureParameterValue(
		Durin::AssetForge::Builtins::MaterialParameters::BaseColorTextureName(), Texture));
	ASSERT_TRUE(Child->SetScalarParameterValue(
		Durin::AssetForge::Builtins::MaterialParameters::RoughnessName(), 0.23f));
	EXPECT_FALSE(Instance->GetPackage()->IsDirty());
	EXPECT_EQ(Child->GetCookedProgramData().GetMetadata().LogicalSize, 0u);
	EXPECT_FALSE(Instance->IsParameterValueOrphan(
		Durin::AssetForge::Builtins::MaterialParameters::GetBuiltinParameterIds(
			Durin::AssetForge::Builtins::MaterialParameters::EMaterialBuiltinParameterRole::BaseColor).Value));
	const bool bOwnsRenderingThread = !Durin::IsRenderingThreadRunning();
	if (bOwnsRenderingThread) Durin::InitRenderingThread();
	Durin::FMaterialRenderData BaseData, ChildData;
	auto BaseProxy = Cooked->GetRenderProxy();
	auto ChildProxy = Child->GetRenderProxy();
	struct FCaptureCookedBindingsCommand
	{
		static constexpr auto GetName() -> const char* { return "CaptureCookedBindings"; }
	};
	Durin::EnqueueRenderCommand<FCaptureCookedBindingsCommand>(
		[&](Durin::FRHICommandListImmediate&) {
			BaseData = BaseProxy->GetMaterialWithFallback();
			ChildData = ChildProxy->GetMaterialWithFallback();
		});
	WaitForRenderingThread();
	ExpectColorNear(GetMaterialBinding(BaseData).BaseColor,
		Durin::FVector4f(0.2f, 0.4f, 0.7f, 1.0f));
	ExpectColorNear(GetMaterialBinding(ChildData).BaseColor,
		Durin::FVector4f(0.8f, 0.3f, 0.1f, 1.0f));
	EXPECT_EQ(GetMaterialBinding(ChildData).Textures[0], Texture->GetTextureReferenceRHI());
	EXPECT_EQ(GetMaterialBinding(BaseData).Textures[0], Texture->GetTextureReferenceRHI());
	EXPECT_FLOAT_EQ(GetMaterialBinding(ChildData).Roughness, 0.23f);
	EXPECT_EQ(ChildData.Representation.GetUniformPayload().size(),
		Durin::Testing::CaptureMaterialRenderData(*Child).Representation.GetUniformPayload().size());
	EXPECT_TRUE(std::ranges::equal(ChildData.Representation.GetUniformPayload(),
		Durin::Testing::CaptureMaterialRenderData(*Child).Representation.GetUniformPayload()));
	Durin::ReleaseMaterialRenderProxy_GameThread(std::move(ChildProxy));
	Durin::ReleaseMaterialRenderProxy_GameThread(std::move(BaseProxy));
	Durin::MarkAsGarbage(Child);
	Durin::MarkAsGarbage(Instance);
	Durin::MarkAsGarbage(Texture);
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
	WaitForRenderingThread();
	if (bOwnsRenderingThread) Durin::ShutdownRenderingThread();
	}

	Durin::ShutdownAssetRuntime();
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
	ASSERT_TRUE(Durin::InitializeAssetRuntime());
	ASSERT_TRUE(Durin::RefreshAssetRegistry(
		Durin::EAssetRegistryScanMode::FullValidation));
}

TEST(FDefaultMaterialCookTests, CustomLayoutAndSamplingSurvivePackageCookAndGraphStripping)
{
	Durin::Testing::FScopedRHIBackendOverride Backend("vulkan");
	using namespace Durin;
	InitializeDObjectSystem();
	ASSERT_TRUE(FMountPaths::InitDefaultMountPoints());
	ASSERT_TRUE(RefreshAssetRegistry(EAssetRegistryScanMode::FullValidation));
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate(DefaultMaterialPackagePath, Path));
	DMaterial* Source = nullptr;
	{
		std::optional<Durin::UnifiedError::FError> LoadedValueError;
		auto LoadedValue = LoadObject<DMaterial>(Testing::MakePackageLeafAssetObjectPathForTests(Path), nullptr, &LoadedValueError);
		Source = LoadedValue;
		ASSERT_TRUE(LoadedValue);
	}
	ASSERT_NE(Source, nullptr);
	auto UsageProperties = Source->GetStaticProperties();
	UsageProperties.GeometryUsage = 1; UsageProperties.PassUsage = 7;
	// Pipeline-only properties stay in the package, while the program family
	// contains canonical Shader properties. Their difference must load normally.
	UsageProperties.bTwoSided = true;
	UsageProperties.DepthWritePolicy = EMaterialDepthWritePolicy::Disabled;
	ASSERT_TRUE(Source->SetStaticProperties(UsageProperties));
	FMaterialParameterDefinition Tint, Texture;
	Tint.Id = FGuid::NewGuid(); Tint.Name = FName("CookedTint"); Tint.DisplayName = "Cooked Tint";
	Tint.Type = EMaterialParameterType::Vector4; Tint.Value = FMaterialParameterValue::MakeVector4(FVector4(0.2, 0.4, 0.7, 1.0));
	Texture.Id = FGuid::NewGuid(); Texture.Name = FName("CookedLayer"); Texture.DisplayName = "Cooked Layer";
	Texture.Type = EMaterialParameterType::Texture;
	Texture.Value = FMaterialParameterValue::MakeTexture(nullptr, {}, EMaterialTextureFallback::FlatRGNormal);
	Texture.Value.GetTexture().SamplerState.AddressU = EMaterialSamplerAddressMode::ClampToEdge;
	Testing::FTestMaterialExpressionGraph Graph;
	const std::array Definitions{Tint, Texture};
	auto Add = [&](EMaterialProgramOpcode Opcode, EMaterialProgramValueType Type, FGuid Id = {},
		std::vector<FMaterialExpressionInput> Inputs = {}) {
		return Testing::MakeLink(Graph.Add(Opcode, Type, std::move(Inputs), Id, {}, Definitions));
	};
	const auto TintNode = Add(EMaterialProgramOpcode::Parameter, EMaterialProgramValueType::Float4, Tint.Id);
	Graph.Outputs.Get(EMaterialProperty::MP_BaseColor) = Add(EMaterialProgramOpcode::Swizzle, EMaterialProgramValueType::Float3, {}, {TintNode});
	const auto TextureNode = Add(EMaterialProgramOpcode::TextureParameter, EMaterialProgramValueType::Texture2D, Texture.Id);
	const auto MeshUV = Add(EMaterialProgramOpcode::Constant, EMaterialProgramValueType::Float2);
	const auto UV = Add(EMaterialProgramOpcode::VertexInterpolator, EMaterialProgramValueType::Float2, {}, {MeshUV});
	auto* PrimitiveData = NewObject<DMaterialExpressionPrimitiveCustomData>(nullptr, NAME_None);
	PrimitiveData->Id = FGuid::NewGuid();
	PrimitiveData->ResultType = EMaterialProgramValueType::Float3;
	PrimitiveData->DataIndex = 29;
	Graph.Expressions.emplace_back(PrimitiveData);
	Graph.Outputs.Get(EMaterialProperty::MP_WorldPositionOffset) = Testing::MakeLink(*PrimitiveData);
	const auto Sample = Add(EMaterialProgramOpcode::TextureSample2D, EMaterialProgramValueType::Float4, {}, {TextureNode, UV});
	Graph.Outputs.Get(EMaterialProperty::MP_EmissiveColor) = Add(EMaterialProgramOpcode::Swizzle, EMaterialProgramValueType::Float3, {}, {Sample});
	auto* InstanceData = NewObject<DMaterialExpressionInstanceCustomData>(nullptr, NAME_None);
	InstanceData->Id = FGuid::NewGuid();
	InstanceData->ResultType = EMaterialProgramValueType::Float3;
	InstanceData->DataIndex = 3;
	Graph.Expressions.emplace_back(InstanceData);
	Graph.Outputs.Get(EMaterialProperty::MP_EmissiveColor) = Add(EMaterialProgramOpcode::Multiply, EMaterialProgramValueType::Float3,
		{}, {Graph.Outputs.Get(EMaterialProperty::MP_EmissiveColor).Connection, Testing::MakeLink(*InstanceData)});
	ASSERT_TRUE(Graph.Apply(*Source));
	ASSERT_NE(Source->GetAcceptedCompiledProgram(), nullptr);
	const auto ExpectedLayout = Source->GetAcceptedCompiledProgram()->GetShaderMap().Layout;
	auto* Instance = NewObject<DMaterialInstance>(Source->GetPackage(), "CustomCookedOverrides");
	ASSERT_TRUE(Instance->SetParent(Source));
	ASSERT_TRUE(Instance->SetParameterValue(Tint.Id, FMaterialParameterValue::MakeVector4(FVector4(0.8, 0.3, 0.1, 1.0))));
	auto Sampling = Texture.Value;
	Sampling.GetTexture().SamplerState.AddressV = EMaterialSamplerAddressMode::MirroredRepeat;
	Sampling.GetTexture().TextureFallback = EMaterialTextureFallback::Black;
	ASSERT_TRUE(Instance->SetParameterValue(Texture.Id, Sampling));
	FSoftObjectPath InstancePath;
	ASSERT_TRUE(FSoftObjectPath::TryCreate(Instance->GetObjectPath(), InstancePath));
	const auto CookRoot = std::filesystem::absolute(Testing::CreateTestFixtureDirectory("CustomLayoutMaterialCook"));
	FCookContext Cook(ECookTargetPlatform::Win64, ECookTargetProfile::Game);
	std::string Error;
	ASSERT_TRUE(Cook.AddPackage(std::string(DefaultMaterialPackagePath), (*Source).GetPackage())) << Error;
	ASSERT_TRUE(PublishCookContext(Cook, CookRoot)) << Error;
	ShutdownAssetRuntime(); CollectGarbage(Durin::EObjectFlags::Standalone);
	auto Configuration = FAssetRuntimeConfiguration::Authored();
	ASSERT_TRUE(FAssetRuntimeConfiguration::Cooked(CookRoot, Configuration));
	ASSERT_TRUE(InitializeAssetRuntime(std::move(Configuration)));
	{
		const std::array Mounts{FMountPoint{.VirtualRoot = "/Engine/", .Owner = EMountOwner::Test,
			.Root = CookRoot / "Engine", .bAutoScan = true}};
		Testing::FScopedMountRegistryFixture CookMounts(Mounts);
		ASSERT_TRUE(CookMounts.IsValid());
		ASSERT_TRUE(RefreshAssetRegistry(EAssetRegistryScanMode::FullValidation));
		DMaterial* Loaded = nullptr;
		{
			std::optional<Durin::UnifiedError::FError> LoadedValueError;
			auto LoadedValue = LoadObject<DMaterial>(Testing::MakePackageLeafAssetObjectPathForTests(Path), nullptr, &LoadedValueError);
			Loaded = LoadedValue;
			ASSERT_TRUE(LoadedValue);
		}
		ASSERT_NE(Loaded, nullptr);
	const std::array<Durin::DObject*, 1> LoadedOwners{Loaded};
	Durin::FShaderCompilingManager::Get().FinishCompilationForObjects(LoadedOwners);
	ASSERT_NE(Loaded->GetAcceptedCompiledProgram(), nullptr);
		EXPECT_EQ(Loaded->GetAcceptedCompiledProgram()->GetShaderMap().Layout, ExpectedLayout);
		EXPECT_EQ(Loaded->GetStaticProperties().GeometryUsage, 1u);
		EXPECT_EQ(Loaded->GetStaticProperties().PassUsage, 7u);
		EXPECT_TRUE(Loaded->GetStaticProperties().bTwoSided);
		EXPECT_EQ(Loaded->GetStaticProperties().DepthWritePolicy, EMaterialDepthWritePolicy::Disabled);
		EXPECT_TRUE(Loaded->GetExpressionCollection().Expressions.empty());
		EXPECT_TRUE(Loaded->GetAcceptedCompiledProgram()->GetDebugData().IR.Nodes.empty());
		EXPECT_TRUE(Loaded->GetAcceptedCompiledProgram()->GetDebugData().GeneratedSource.empty());
		EXPECT_EQ(Loaded->GetAcceptedCompiledProgram()->GetShaderMap().RequiredEntries,
			SelectMaterialShaderEntries(Loaded->GetStaticProperties(), false));
		EXPECT_EQ(Loaded->GetAcceptedCompiledProgram()->GetShaderMap().CompiledShaders.size(), 4u);
		EXPECT_FALSE(std::ranges::any_of(Loaded->GetAcceptedCompiledProgram()->GetShaderMap().CompiledShaders,
			[](const auto& Stage) { return Stage.SourceEntryPoint == "HitProxyFragmentMain"; }));
		DMaterialInstance* LoadedInstance = nullptr;
		{
			std::optional<Durin::UnifiedError::FError> LoadedValueError;
			auto LoadedValue = LoadObject<DMaterialInstance>(InstancePath, nullptr, &LoadedValueError);
			LoadedInstance = LoadedValue;
			ASSERT_TRUE(LoadedValue);
		}
		ASSERT_NE(LoadedInstance, nullptr);
		FShaderCompilingManager::Get().FinishAllCompilation();
		EXPECT_FALSE(LoadedInstance->IsParameterValueOrphan(Texture.Id));
		const auto RootBinding = GetMaterialBinding(Durin::Testing::CaptureMaterialRenderData(*Loaded));
		const auto InstanceBinding = GetMaterialBinding(Durin::Testing::CaptureMaterialRenderData(*LoadedInstance));
		EXPECT_EQ(RootBinding.LayoutIdentity, ExpectedLayout.Identity);
		EXPECT_EQ(InstanceBinding.LayoutIdentity, ExpectedLayout.Identity);
		ASSERT_EQ(InstanceBinding.CompiledSamplers.size(), 1u);
		EXPECT_EQ(RootBinding.CompiledSamplers[0], Texture.Value.GetTexture().SamplerState);
		EXPECT_EQ(InstanceBinding.CompiledSamplers[0], Sampling.GetTexture().SamplerState);
		EXPECT_EQ(InstanceBinding.CompiledTextureFallbacks[0], EMaterialTextureFallback::Black);
		for (const auto& Field : ExpectedLayout.Fields) if (Field.ParameterId == Tint.Id)
		{
			EXPECT_FLOAT_EQ(ReadFloat(RootBinding.CompiledUniformPayload, Field.Offset), 0.2f);
			EXPECT_FLOAT_EQ(ReadFloat(InstanceBinding.CompiledUniformPayload, Field.Offset), 0.8f);
		}
	}
	ShutdownAssetRuntime(); CollectGarbage(Durin::EObjectFlags::Standalone);
	ASSERT_TRUE(InitializeAssetRuntime());
}

TEST(FMaterialIntegratedCookTests, FinalCorpusPreservesAllocationsAndUpdatesWithoutSourceWork)
{
	using namespace Durin;
	InitializeDObjectSystem();
	Testing::FScopedRHIBackendOverride Backend("vulkan");
	FScopedOfflinePreparation Offline;
	Testing::FScopedMountRegistryFixture MountRegistry;
	const auto Root=Testing::GetTestWorkDirectory()/"IntegratedCook";
	Testing::RemoveTestWorkDirectory(Root);
	Testing::RegisterMountPointForTests("/Game/",(Root/"Source").generic_string()+"/");
	const std::array Names{"M_Specular","M_ClearCoat","M_Unlit","M_PostNeutral","M_PostColor","M_PostDepth","M_Cube","M_CubeWPO"};
	std::array<FPackagePath,Names.size()> Paths;
	std::array<std::shared_ptr<const FMaterialProgram>,Names.size()> EditorPrograms,GamePrograms;
	std::array<FMaterialConfiguration,Names.size()> Configurations;
	std::array<FGuid,Names.size()> ColorIds;
	FCookContext Cook(ECookTargetPlatform::Win64,ECookTargetProfile::Game);
	Cook.SetMaterialConfigurations(std::array{FMaterialCookConfiguration{GetMaterialQualityLevel(),ERHIFeatureLevel::SM5}});
	for (uint32 Variant=0;Variant<Names.size();++Variant)
	{
		SCOPED_TRACE(Names[Variant]);
		ASSERT_TRUE(FPackagePath::TryCreate(std::string("/Game/MaterialContracts/")+Names[Variant],Paths[Variant]));
		DMaterial* Material=nullptr; ASSERT_TRUE(CreatePackageLeafAssetForTesting(Paths[Variant],Material));
		Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
		auto& Configuration=Configurations[Variant];
		if (Variant==1) Configuration.Properties.ShadingModel=EMaterialShadingModel::ClearCoat;
		if (Variant>=2) Configuration.Properties.ShadingModel=EMaterialShadingModel::Unlit;
		if (Variant>=3 && Variant<=5) Configuration.Domain=EMaterialDomain::PostProcess;
		ASSERT_TRUE(Material->SetMaterialConfiguration(Configuration));
		std::vector<TStrongObjectPtr<DMaterialExpression>> Nodes;
		const auto Add=[&]<class T>() { auto* E=NewObject<T>(nullptr,NAME_None); E->Id=FGuid::NewGuid(); Nodes.emplace_back(E); return E; };
		auto* Color=Add.template operator()<DMaterialExpressionVector4Parameter>();
		Color->Metadata={.Id=FGuid::NewGuid(),.Name="Color"}; Color->DefaultValue={.5,1,2,1}; ColorIds[Variant]=Color->Metadata.Id;
		auto* RGB=Add.template operator()<DMaterialExpressionSwizzle>(); RGB->Input={Color}; RGB->Components={0,1,2};
		FMaterialRootInputs Outputs; Outputs.Get(EMaterialProperty::MP_EmissiveColor)={RGB};
		if (Variant<2)
		{
			auto* Weight=Add.template operator()<DMaterialExpressionScalarParameter>();
			Weight->Metadata={.Id=FGuid::NewGuid(),.Name="Weight"}; Weight->DefaultValue=.4f;
			Outputs.Get(EMaterialProperty::MP_Specular)={Weight};
			Outputs.Get(EMaterialProperty::MP_ClearCoat)={Weight};
			Outputs.Get(EMaterialProperty::MP_ClearCoatRoughness).SetConstant({.2f});
		}
		if (Variant==3 || Variant==4)
		{
			auto* Scene=Add.template operator()<DMaterialExpressionSceneTexture>();
			auto* SceneRGB=Add.template operator()<DMaterialExpressionSwizzle>(); SceneRGB->Input={Scene}; SceneRGB->Components={0,1,2};
			Outputs.Get(EMaterialProperty::MP_EmissiveColor)={SceneRGB};
			if (Variant==4)
			{
				auto* Multiply=Add.template operator()<DMaterialExpressionMultiply>(); Multiply->ResultType=EMaterialProgramValueType::Float3;
				Multiply->A.Connection={SceneRGB}; Multiply->B.Connection={RGB};
				auto* Make = Add.template operator()<DMaterialExpressionMakeMaterialAttributes>();
				Make->EmissiveColor = {Multiply};
				Outputs.Attributes = {Make};
				Outputs.bUseMaterialAttributes = true;
			}
		}
		if (Variant==5)
		{
			auto* Depth=Add.template operator()<DMaterialExpressionSceneDepth>(); Outputs.Get(EMaterialProperty::MP_EmissiveColor)={Depth};
		}
		if (Variant>=6)
		{
			auto* Cube=Add.template operator()<DMaterialExpressionTextureSampleParameterCube>();
			Cube->Metadata={.Id=FGuid::NewGuid(),.Name="Cube"};
			Cube->ReadMode=EMaterialTextureReadMode::Level;
			Cube->MipValue.SetConstant({1.f});
			auto* SampleRGB=Add.template operator()<DMaterialExpressionSwizzle>();
			SampleRGB->Input={Cube}; SampleRGB->Components={0,1,2};
			auto* Multiply=Add.template operator()<DMaterialExpressionMultiply>();
			Multiply->ResultType=EMaterialProgramValueType::Float3;
			Multiply->A.Connection={SampleRGB}; Multiply->B.Connection={RGB};
			Outputs.Get(EMaterialProperty::MP_EmissiveColor)={Multiply};
			if (Variant==7) Outputs.Get(EMaterialProperty::MP_WorldPositionOffset)={SampleRGB};
		}
		std::vector<DMaterialExpression*> Expressions; for (const auto& E:Nodes) Expressions.push_back(E.Get());
		ASSERT_TRUE(Material->SetMaterialExpressions(Expressions,Outputs));
		ASSERT_TRUE(Material->CompileEdits()) << FormatMaterialProgramDiagnostics(Material->GetMaterialCompileDiagnostics());
		EditorPrograms[Variant]=Material->GetAcceptedCompiledProgram(); ASSERT_TRUE(EditorPrograms[Variant]);
		FMaterialCompilerEnvironment Environment; ASSERT_TRUE(BuildDefaultMaterialCompilerEnvironment(Environment));
		Environment.bIncludeEditorPasses=false;
		auto Snapshot=SnapshotMaterialCompilerInput(*Material,Environment); ASSERT_TRUE(Snapshot);
		auto Game=MIR::Compile(Snapshot.Snapshot->Input); ASSERT_TRUE(Game) << FormatMaterialProgramDiagnostics(Game.Diagnostics);
		GamePrograms[Variant]=std::make_shared<FMaterialProgram>(std::move(Game.Resource));
		const auto& EditorMap=EditorPrograms[Variant]->GetShaderMap(); const auto& GameMap=GamePrograms[Variant]->GetShaderMap();
		EXPECT_EQ(EditorMap.Layout,GameMap.Layout); EXPECT_EQ(EditorMap.Domain,GameMap.Domain);
		EXPECT_EQ(std::ranges::count(EditorMap.Entries,"FHitProxyFragmentShader",[](const auto& E) { return E.Shader.ShaderType; }),Configuration.Domain==EMaterialDomain::Surface ? 1 : 0);
		EXPECT_EQ(std::ranges::count(GameMap.Entries,"FHitProxyFragmentShader",[](const auto& E) { return E.Shader.ShaderType; }),0);
		for (const auto& Entry:GameMap.Entries)
		{
			const auto Peer=std::ranges::find(EditorMap.Entries,Entry.Shader.ShaderType,[](const auto& E) { return E.Shader.ShaderType; }); ASSERT_NE(Peer,EditorMap.Entries.end());
			EXPECT_EQ(Peer->Requirements,Entry.Requirements); EXPECT_EQ(Peer->Shader.OutputLayout,Entry.Shader.OutputLayout);
			EXPECT_EQ(bool(Peer->Mesh),bool(Entry.Mesh));
			const auto Code=std::ranges::find(GameMap.CompiledShaders,Entry.Shader.EntryPoint,&FShaderCode::SourceEntryPoint);
			const auto EditorCode=std::ranges::find(EditorMap.CompiledShaders,Entry.Shader.EntryPoint,&FShaderCode::SourceEntryPoint);
			ASSERT_NE(Code,GameMap.CompiledShaders.end()); ASSERT_NE(EditorCode,EditorMap.CompiledShaders.end());
			EXPECT_EQ(Code->CodeHash,EditorCode->CodeHash); EXPECT_TRUE(std::ranges::equal(Code->Code,EditorCode->Code));
		}
		for (uint32 Other=0;Other<Variant;++Other)
		{
			EXPECT_NE(GameMap.ShaderMapId,GamePrograms[Other]->GetShaderMap().ShaderMapId);
			EXPECT_NE(EditorMap.ShaderMapId,EditorPrograms[Other]->GetShaderMap().ShaderMapId);
		}
		ASSERT_TRUE(SavePackage(Material->GetPackage()));
		ASSERT_TRUE(Cook.AddPackage(std::string("/Game/MaterialContracts/")+Names[Variant], Material->GetPackage()));
	}
	const auto CookRoot=std::filesystem::absolute(Root/"Cooked");
	ASSERT_TRUE(PublishCookContext(Cook,CookRoot));
	ShutdownAssetRuntime(); CollectGarbage(EObjectFlags::Standalone);
	// Replay cannot accidentally consult the authored package tree.
	std::filesystem::rename(Root/"Source",Root/"UnavailableSource");
	auto Runtime=FAssetRuntimeConfiguration::Authored(); ASSERT_TRUE(FAssetRuntimeConfiguration::Cooked(CookRoot,Runtime));
	ASSERT_TRUE(InitializeAssetRuntime(std::move(Runtime)));
	{
		const std::array Mounts{FMountPoint{.VirtualRoot="/Game/",.Owner=EMountOwner::Test,.Root=CookRoot/"Game",.bAutoScan=true}};
		Testing::FScopedMountRegistryFixture CookMounts(Mounts); ASSERT_TRUE(CookMounts.IsValid()); ASSERT_TRUE(RefreshAssetRegistry(EAssetRegistryScanMode::FullValidation));
		const auto BuildStats=GetShaderBuildStats();
		std::string Evidence="fixture,editor_identity,game_identity,editor_entries,game_entries,loaded_entries,package_bytes,shader_bytes,retained_bytes,accepted_bindings\n";
		for (uint32 Variant=0;Variant<Names.size();++Variant)
		{
			SCOPED_TRACE(Names[Variant]);
			auto* Loaded=LoadObject<DMaterial>(Testing::MakePackageLeafAssetObjectPathForTests(Paths[Variant])); ASSERT_TRUE(Loaded);
			Loaded->CacheResourceShadersForRendering(ERHIFeatureLevel::SM5,GetMaterialQualityLevel(),EMaterialShaderPrecompileMode::Synchronous);
			FShaderCompilingManager::Get().FinishCompilationForObjects(std::array<DObject*,1>{Loaded});
			const auto Program=Loaded->GetAcceptedCompiledProgram(); ASSERT_TRUE(Program) << Loaded->GetMaterialCookDiagnostic();
			const auto& Expected=GamePrograms[Variant]->GetShaderMap(); const auto& Actual=Program->GetShaderMap();
			EXPECT_EQ(Loaded->GetDomain(),Configurations[Variant].Domain); EXPECT_EQ(Loaded->GetStaticProperties(),Configurations[Variant].Properties);
			EXPECT_TRUE(Loaded->GetExpressionCollection().Expressions.empty()); EXPECT_TRUE(Program->GetDebugData().IR.Nodes.empty()); EXPECT_TRUE(Program->GetDebugData().GeneratedSource.empty());
			EXPECT_EQ(Actual.ShaderMapId,Expected.ShaderMapId); EXPECT_EQ(Actual.Entries,Expected.Entries); EXPECT_EQ(Actual.Layout,Expected.Layout);
			ASSERT_EQ(Actual.CompiledShaders.size(),Expected.CompiledShaders.size());
			for (size_t I=0;I<Actual.CompiledShaders.size();++I) { EXPECT_EQ(Actual.CompiledShaders[I].CodeHash,Expected.CompiledShaders[I].CodeHash); EXPECT_TRUE(std::ranges::equal(Actual.CompiledShaders[I].Code,Expected.CompiledShaders[I].Code)); }
			const auto Data=Testing::CaptureMaterialRenderData(*Loaded); EXPECT_FALSE(Data.Representation.IsError());
			const auto Field=std::ranges::find(Actual.Layout.Fields,ColorIds[Variant],&FMaterialRenderField::ParameterId);
			if (Field!=Actual.Layout.Fields.end())
			{
				const auto RequestsBefore=GetMaterialCompilationStatistics().AcceptedRequests;
				float Value; std::memcpy(&Value,Data.Representation.GetUniformPayload().data()+Field->Offset,sizeof(Value)); EXPECT_FLOAT_EQ(Value,.5f);
				auto* First=DMaterialInstance::CreateDynamic(Loaded,Loaded,"First");
				auto* Second=DMaterialInstance::CreateDynamic(Loaded,Loaded,"Second");
				ASSERT_TRUE(First && Second);
				ASSERT_TRUE(First->SetParameterValue(ColorIds[Variant],FMaterialParameterValue(FVector4(.8,1,2,1))));
				const auto Updated=Testing::CaptureMaterialRenderData(*First);
				const auto Unchanged=Testing::CaptureMaterialRenderData(*Second);
				std::memcpy(&Value,Updated.Representation.GetUniformPayload().data()+Field->Offset,sizeof(Value)); EXPECT_FLOAT_EQ(Value,.8f);
				std::memcpy(&Value,Unchanged.Representation.GetUniformPayload().data()+Field->Offset,sizeof(Value)); EXPECT_FLOAT_EQ(Value,.5f);
				EXPECT_EQ(First->GetAcceptedCompiledProgram(),Program); EXPECT_EQ(Second->GetAcceptedCompiledProgram(),Program);
				EXPECT_EQ(GetMaterialCompilationStatistics().AcceptedRequests,RequestsBefore);
			}
			uint64 ShaderBytes=0,Bindings=0;
			for (const auto& Shader:Actual.CompiledShaders) ShaderBytes+=Shader.Code.size();
			for (const auto& Entry:Actual.Entries) Bindings+=Entry.Bindings.size();
			const auto PackageBytes=std::filesystem::file_size(CookRoot/"Game"/"MaterialContracts"/(std::string(Names[Variant])+".dasset"));
			Evidence+=std::format("{},{},{},{},{},{},{},{},{},{}\n",Names[Variant],EditorPrograms[Variant]->GetShaderMap().ShaderMapId.ToString(),Actual.ShaderMapId.ToString(),EditorPrograms[Variant]->GetShaderMap().Entries.size(),Expected.Entries.size(),Actual.Entries.size(),PackageBytes,ShaderBytes,Actual.GetRetainedBytes(),Bindings);
			ASSERT_TRUE(UnloadPackage(Paths[Variant]));
		}
		const auto After=GetShaderBuildStats();
		EXPECT_EQ(After.Compilations,BuildStats.Compilations); EXPECT_EQ(After.DependencyResolutions,BuildStats.DependencyResolutions); EXPECT_EQ(After.ContentReads,BuildStats.ContentReads);
		if (const auto* Directory=std::getenv("DURIN_MATERIAL_STAGE4_CLOSURE_EVIDENCE_DIR"))
		{
			FByteBuffer ShaderLibrary;
			ASSERT_TRUE(BuildCookedShaderLibrary(EShaderTargetPlatform::Win64,EShaderTargetProfile::Game,ShaderLibrary));
			ASSERT_TRUE(FFileHelper::SaveArrayToFile(ShaderLibrary,std::filesystem::path(Directory)/ShaderCookedLibraryRelativePath));
			ASSERT_TRUE(FFileHelper::SaveArrayToFile(std::as_bytes(std::span(Evidence)),std::filesystem::path(Directory)/"cook-contracts.csv"));
			std::filesystem::copy(CookRoot,std::filesystem::path(Directory)/"Cooked",std::filesystem::copy_options::recursive|std::filesystem::copy_options::overwrite_existing);
		}
	}
	ShutdownAssetRuntime(); CollectGarbage(EObjectFlags::Standalone); ASSERT_TRUE(InitializeAssetRuntime());
}
