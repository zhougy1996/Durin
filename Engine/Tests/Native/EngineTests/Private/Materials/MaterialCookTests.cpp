#include "MaterialResourceTestSupport.h"
#include "NativeRHIBackendTestSupport.h"
#include "Materials/MaterialShaderMap.h"
#include "AssetForge/Builtins/PBRMaterialParameters.h"
#include "MaterialRenderRepresentationTestFixture.h"
#include "Materials/MaterialCustomVersion.h"
#include "Shader/ShaderCompilingManager.h"
#include "Shader/IShaderBuildModule.h"
#include "MaterialCookedProgramTestSupport.h"
#include "Threading/TaskRuntime.h"

static auto QualifyDefaultMaterialCook(bool Metal) -> void
{
	Durin::Testing::FScopedRHIBackendOverride Backend(Metal ? "metal" : "vulkan");
	InitializeDObjectSystem();
	ASSERT_TRUE(Durin::FMountPaths::InitDefaultMountPoints());
	ASSERT_TRUE(Durin::RefreshAssetRegistry(
		Durin::EAssetRegistryScanMode::FullValidation));
	Durin::FPackagePath Path;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		Durin::DefaultMaterialPackagePath, Path));
	Durin::DMaterial* Source = nullptr;
	auto SourceLoad = Durin::LoadObject<Durin::DMaterial>(Durin::Testing::MakePackageLeafAssetObjectPathForTests(Path));
	Source = SourceLoad.value_or(nullptr);
	ASSERT_TRUE(SourceLoad) << SourceLoad.error().Message;
	ASSERT_NE(Source, nullptr);
	const Durin::FMaterialProgramIdentity ExpectedIdentity =
		Durin::Testing::ExpectedGameMaterialIdentity(*Source, Metal ? Durin::MetalLibraryShaderTarget : Durin::VulkanShaderTarget);

	const std::filesystem::path CookRoot = std::filesystem::absolute(
		Durin::Testing::CreateTestFixtureDirectory("DefaultMaterialCook"));
	Durin::FCookContext Cook(
		Metal ? Durin::ECookTargetPlatform::MacOS : Durin::ECookTargetPlatform::Win64,
		Durin::ECookTargetProfile::Game);
	std::string Error;
	ASSERT_TRUE(Durin::SaveCookPackage((*Source).GetPackage(), Durin::DefaultMaterialPackagePath, Cook)) << Error;
	ASSERT_TRUE(Durin::PublishCookContext(Cook, CookRoot)) << Error;
	EXPECT_TRUE(std::filesystem::is_regular_file(
		CookRoot / "Engine/Materials/DefaultMaterial.dasset"));
	EXPECT_FALSE(std::filesystem::is_regular_file(
		CookRoot / "Engine/Materials/DefaultMaterial.dbulk"));
	Durin::ShutdownAssetManager();
	Durin::CollectGarbage();
	auto RuntimeConfiguration = Durin::FAssetRuntimeConfiguration::Authored();
	auto Result = Durin::FAssetRuntimeConfiguration::Cooked(
		CookRoot, RuntimeConfiguration);
	ASSERT_TRUE(Result) << Result.Message;
	const auto Initialized = Durin::InitializeAssetManager(std::move(RuntimeConfiguration));
	ASSERT_TRUE(Initialized) << Initialized.Message;
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
	EXPECT_NE(Inspection.FindField("ProgramData"), nullptr);
	const auto CookedFile = CookRoot / "Engine/Materials/DefaultMaterial.dasset";
	Durin::FByteBuffer OriginalBytes;
	auto OriginalBytesRead = Durin::FFileHelper::LoadFileToArray(CookedFile);
	ASSERT_TRUE(OriginalBytesRead) << OriginalBytesRead.error().ToString();
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
	const auto MissingVersion = Durin::LoadObject<Durin::DMaterial>(Durin::Testing::MakePackageLeafAssetObjectPathForTests(Path));
	Rejected = MissingVersion.value_or(nullptr);
	EXPECT_EQ((MissingVersion ? Durin::EAssetReadError::None : MissingVersion.error().Code), Durin::EAssetReadError::UnsupportedVersion) << (MissingVersion ? std::string{} : MissingVersion.error().Message);
	EXPECT_EQ(Rejected, nullptr);
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(OriginalBytes, CookedFile));
	ASSERT_TRUE(Durin::RefreshAssetRegistry(
		Durin::EAssetRegistryScanMode::FullValidation));
	Durin::DMaterial* Cooked = nullptr;
	{
		auto Loaded = Durin::LoadObject<Durin::DMaterial>(Durin::Testing::MakePackageLeafAssetObjectPathForTests(Path));
		Cooked = Loaded.value_or(nullptr);
		Result = Loaded ? Durin::FAssetReadResult{} : Durin::AssetReadResultFromError(Loaded.error());
	}
	ASSERT_TRUE(Result) << Result.Message;
	ASSERT_NE(Cooked, nullptr);
	// Loading starts owner preparation; queries do not decode the payload.
	auto& CookedData = const_cast<Durin::FBulkData&>(Cooked->GetCookedProgramData());
	const auto SavedCookedData = CookedData;
	const std::array InvalidProgramBytes{std::byte{0}};
	auto InvalidData = Durin::FBulkData::TryCreateDetached(InvalidProgramBytes);
	ASSERT_TRUE(InvalidData);
	CookedData = std::move(*InvalidData);
	Cooked->PostLoad();
	EXPECT_EQ(Cooked->GetAcceptedCompiledProgram(), nullptr);
	EXPECT_TRUE(Cooked->GetMaterialCookDiagnostic().empty()); // Queries do not read/decode payloads.
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
	EXPECT_EQ(Cooked->GetMaterialCompileStatus().State, Durin::EMaterialCompileState::Canceled);
	EXPECT_FALSE(Cooked->GetMaterialCookDiagnostic().empty());
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
	EXPECT_EQ(Cooked->GetMaterialCompileStatus().State, Durin::EMaterialCompileState::Canceled);
	EXPECT_EQ(Cooked->GetAcceptedCompiledProgram(), nullptr);
	// Multiple owners exceed the decode admission limit and finish through the
	// ordinary compiler queue. Repeated cache calls must not duplicate work.
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
	Durin::FShaderCompilingManager::Get().ProcessAsyncTasks({.MaximumCompletions = 1});
	Cooked->CacheResourceShadersForRendering(Durin::EMaterialShaderPrecompileMode::Synchronous);
	ASSERT_TRUE(Cooked->IsComplete()) << Cooked->GetMaterialCookDiagnostic();
	const auto DefaultProgram = Cooked->GetAcceptedCompiledProgram();
	Cooked->CacheResourceShadersForRendering(Durin::ERHIFeatureLevel::ES3_1,
		Durin::EMaterialShaderPrecompileMode::Synchronous);
	const auto LowProgram = Cooked->GetAcceptedCompiledProgram(Durin::ERHIFeatureLevel::ES3_1);
	ASSERT_TRUE(LowProgram) << Cooked->GetMaterialCookDiagnostic(Durin::ERHIFeatureLevel::ES3_1);
	EXPECT_EQ(LowProgram->FeatureLevel, Durin::ERHIFeatureLevel::ES3_1);
	EXPECT_EQ(Cooked->GetAcceptedCompiledProgram(), DefaultProgram);
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
		EXPECT_EQ(Pending->GetMaterialCompileStatus().State, Durin::EMaterialCompileState::Failed);
		EXPECT_FALSE(Pending->IsCompiling());
		EXPECT_FALSE(Pending->GetMaterialCookDiagnostic().empty());
		Durin::TStrongObjectPtr<Durin::DMaterial> Offline(
			Durin::NewObject<Durin::DMaterial>(nullptr, "OfflineCookedProgram"));
		const_cast<Durin::FBulkData&>(Offline->GetCookedProgramData()) = SavedCookedData;
		const auto BeforeNone = Offline->GetMaterialCompileStatus().State;
		Offline->CacheResourceShadersForRendering(Durin::EMaterialShaderPrecompileMode::None);
		EXPECT_EQ(Offline->GetMaterialCompileStatus().State, BeforeNone);
		Offline->CacheResourceShadersForRendering();
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
	EXPECT_EQ(Cooked->GetAcceptedCompiledProgram()->GetShaderMap().CompiledShaders.size(), 6u);
	EXPECT_EQ(Cooked->GetAcceptedCompiledProgram()->GetShaderMap().ShaderMapId, ExpectedIdentity);
	for (const auto& Shader : Cooked->GetAcceptedCompiledProgram()->GetShaderMap().CompiledShaders)
	{
		EXPECT_EQ(Shader.Target, Metal ? Durin::MetalLibraryShaderTarget : Durin::VulkanShaderTarget);
		EXPECT_TRUE(Durin::ShaderFormat::ValidateShaderCode(Shader));
	}
	const auto ProgramBeforeCanceledResave = Cooked->GetAcceptedCompiledProgram();
	const Durin::FAssetBuildTaskContext CanceledControl{.Cancellation = Durin::FCancellationToken([] { return true; })};
	const auto CanceledResave = Cooked->PrepareCookedProgram(Metal ? Durin::ECookTargetPlatform::MacOS : Durin::ECookTargetPlatform::Win64, {}, CanceledControl);
	EXPECT_EQ(CanceledResave.Status, Durin::EShaderBuildStatus::Canceled);
	EXPECT_EQ(Cooked->GetAcceptedCompiledProgram(), ProgramBeforeCanceledResave);
	const auto BuildsBeforeResave = Durin::IShaderBuildModule::Get()->GetStats().Compilations;
	auto Resaved = Cooked->PrepareCookedProgram(Metal ? Durin::ECookTargetPlatform::MacOS : Durin::ECookTargetPlatform::Win64, {});
	ASSERT_TRUE(Resaved) << Resaved.Error;
	auto ResavedRead = Resaved->AcquireRead();
	ASSERT_TRUE(ResavedRead);
	Durin::FMaterialStaticProperties ResavedProperties;
	std::shared_ptr<const Durin::FMaterialCompilerResult> ResavedProgram;
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
		Durin::FVector4f(0.5f, 0.5f, 0.5f, 1.0f));
	}

	Durin::ShutdownAssetManager();
	Durin::CollectGarbage();
	ASSERT_TRUE(Durin::InitializeAssetManager());
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
	Durin::Testing::FScopedRHIBackendOverride Backend("vulkan");
	InitializeDObjectSystem();
	ASSERT_TRUE(Durin::FMountPaths::InitDefaultMountPoints());
	ASSERT_TRUE(Durin::RefreshAssetRegistry(
		Durin::EAssetRegistryScanMode::FullValidation));
	Durin::FPackagePath Path;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		Durin::DefaultMaterialPackagePath, Path));
	Durin::DMaterial* Source = nullptr;
	auto SourceLoad = Durin::LoadObject<Durin::DMaterial>(Durin::Testing::MakePackageLeafAssetObjectPathForTests(Path));
	Source = SourceLoad.value_or(nullptr);
	ASSERT_TRUE(SourceLoad) << SourceLoad.error().Message;
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
	Durin::FObjectPath InstancePath;
	ASSERT_TRUE(Durin::FObjectPath::TryCreate(AuthoredInstance->GetObjectPath(), InstancePath));
	const Durin::FMaterialProgramIdentity ExpectedIdentity =
		Durin::Testing::ExpectedGameMaterialIdentity(*Source);
	std::array<Durin::FObjectPath, 3> VariantPaths;
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
		ASSERT_TRUE(Durin::FObjectPath::TryCreate(Variant->GetObjectPath(), VariantPaths[Index]));
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
	ASSERT_TRUE(Durin::SaveCookPackage((*Source).GetPackage(), Durin::DefaultMaterialPackagePath, Cook)) << Error;
	ASSERT_TRUE(Durin::PublishCookContext(Cook, CookRoot)) << Error;
	EXPECT_TRUE(std::filesystem::is_regular_file(
		CookRoot / "Engine/Materials/DefaultMaterial.dasset"));
	EXPECT_FALSE(std::filesystem::is_regular_file(
		CookRoot / "Engine/Materials/DefaultMaterial.dbulk"));
	Durin::ShutdownAssetManager();
	Durin::CollectGarbage();
	auto RuntimeConfiguration = Durin::FAssetRuntimeConfiguration::Authored();
	auto Result = Durin::FAssetRuntimeConfiguration::Cooked(
		CookRoot, RuntimeConfiguration);
	ASSERT_TRUE(Result) << Result.Message;
	const auto Initialized = Durin::InitializeAssetManager(std::move(RuntimeConfiguration));
	ASSERT_TRUE(Initialized) << Initialized.Message;
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
		auto Loaded = Durin::LoadObject<Durin::DMaterial>(Durin::Testing::MakePackageLeafAssetObjectPathForTests(Path));
		Cooked = Loaded.value_or(nullptr);
		Result = Loaded ? Durin::FAssetReadResult{} : Durin::AssetReadResultFromError(Loaded.error());
	}
	ASSERT_TRUE(Result) << Result.Message;
	ASSERT_NE(Cooked, nullptr);
	const std::array<Durin::DObject*, 1> CookedOwners{Cooked};
	const auto BudgetDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
	uint64 CookedCompletions = 0;
	while (Cooked->IsCompiling() && std::chrono::steady_clock::now() < BudgetDeadline)
	{
		const auto Tick = Durin::FShaderCompilingManager::Get().ProcessAsyncTasks({.MaximumCompletions = 1});
		EXPECT_LE(Tick.ProcessedCompletionCount, 1u);
		CookedCompletions += Tick.ProcessedCompletionCount;
		std::this_thread::yield();
	}
	EXPECT_GT(CookedCompletions, 0u); // Cooked admissions participate in normal compiler feedback.
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
		auto Loaded = Durin::LoadObject<Durin::DMaterialInstance>(InstancePath);
		Instance = Loaded.value_or(nullptr);
		Result = Loaded ? Durin::FAssetReadResult{} : Durin::AssetReadResultFromError(Loaded.error());
	}
	ASSERT_TRUE(Result) << Result.Message;
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
			auto LoadedValue = Durin::LoadObject<Durin::DMaterialInstance>(VariantPaths[Index]);
			Variant = LoadedValue.value_or(nullptr);
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
	EXPECT_EQ(Cooked->GetAcceptedCompiledProgram(), AcceptedProgram);
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
	const bool bOwnsRenderingThread = Durin::GetRenderCommandAdmissionState()
		== Durin::ERenderCommandAdmissionState::Stopped;
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
	Durin::CollectGarbage();
	WaitForRenderingThread();
	if (bOwnsRenderingThread) Durin::ShutdownRenderingThread();
	}

	Durin::ShutdownAssetManager();
	Durin::CollectGarbage();
	ASSERT_TRUE(Durin::InitializeAssetManager());
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
		auto LoadedValue = LoadObject<DMaterial>(Testing::MakePackageLeafAssetObjectPathForTests(Path));
		Source = LoadedValue.value_or(nullptr);
		ASSERT_TRUE(LoadedValue);
	}
	ASSERT_NE(Source, nullptr);
	auto UsageProperties = Source->GetStaticProperties();
	UsageProperties.GeometryUsage = 1; UsageProperties.PassUsage = 7;
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
	Graph.Outputs.BaseColor = Add(EMaterialProgramOpcode::Swizzle, EMaterialProgramValueType::Float3, {}, {TintNode});
	const auto TextureNode = Add(EMaterialProgramOpcode::TextureParameter, EMaterialProgramValueType::Texture2D, Texture.Id);
	const auto MeshUV = Add(EMaterialProgramOpcode::Constant, EMaterialProgramValueType::Float2);
	const auto UV = Add(EMaterialProgramOpcode::VertexInterpolator, EMaterialProgramValueType::Float2, {}, {MeshUV});
	auto* PrimitiveData = NewObject<DMaterialExpressionPrimitiveCustomData>(nullptr, NAME_None);
	PrimitiveData->Id = FGuid::NewGuid();
	PrimitiveData->ResultType = EMaterialProgramValueType::Float3;
	PrimitiveData->DataIndex = 29;
	Graph.Expressions.emplace_back(PrimitiveData);
	Graph.Outputs.WorldPositionOffset = Testing::MakeLink(*PrimitiveData);
	const auto Sample = Add(EMaterialProgramOpcode::TextureSample2D, EMaterialProgramValueType::Float4, {}, {TextureNode, UV});
	Graph.Outputs.Emissive = Add(EMaterialProgramOpcode::Swizzle, EMaterialProgramValueType::Float3, {}, {Sample});
	auto* InstanceData = NewObject<DMaterialExpressionInstanceCustomData>(nullptr, NAME_None);
	InstanceData->Id = FGuid::NewGuid();
	InstanceData->ResultType = EMaterialProgramValueType::Float3;
	InstanceData->DataIndex = 3;
	Graph.Expressions.emplace_back(InstanceData);
	Graph.Outputs.Emissive = Add(EMaterialProgramOpcode::Multiply, EMaterialProgramValueType::Float3,
		{}, {Graph.Outputs.Emissive.Connection, Testing::MakeLink(*InstanceData)});
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
	FObjectPath InstancePath;
	ASSERT_TRUE(FObjectPath::TryCreate(Instance->GetObjectPath(), InstancePath));
	const auto CookRoot = std::filesystem::absolute(Testing::CreateTestFixtureDirectory("CustomLayoutMaterialCook"));
	FCookContext Cook(ECookTargetPlatform::Win64, ECookTargetProfile::Game);
	std::string Error;
	ASSERT_TRUE(SaveCookPackage((*Source).GetPackage(), DefaultMaterialPackagePath, Cook)) << Error;
	ASSERT_TRUE(PublishCookContext(Cook, CookRoot)) << Error;
	ShutdownAssetManager(); CollectGarbage();
	auto Configuration = FAssetRuntimeConfiguration::Authored();
	ASSERT_TRUE(FAssetRuntimeConfiguration::Cooked(CookRoot, Configuration));
	ASSERT_TRUE(InitializeAssetManager(std::move(Configuration)));
	{
		const std::array Mounts{FMountPoint{.VirtualRoot = "/Engine/", .Owner = EMountOwner::Test,
			.Root = CookRoot / "Engine", .bAutoScan = true}};
		Testing::FScopedMountRegistryFixture CookMounts(Mounts);
		ASSERT_TRUE(CookMounts.IsValid());
		ASSERT_TRUE(RefreshAssetRegistry(EAssetRegistryScanMode::FullValidation));
		DMaterial* Loaded = nullptr;
		{
			auto LoadedValue = LoadObject<DMaterial>(Testing::MakePackageLeafAssetObjectPathForTests(Path));
			Loaded = LoadedValue.value_or(nullptr);
			ASSERT_TRUE(LoadedValue);
		}
		ASSERT_NE(Loaded, nullptr);
	const std::array<Durin::DObject*, 1> LoadedOwners{Loaded};
	Durin::FShaderCompilingManager::Get().FinishCompilationForObjects(LoadedOwners);
	ASSERT_NE(Loaded->GetAcceptedCompiledProgram(), nullptr);
		EXPECT_EQ(Loaded->GetAcceptedCompiledProgram()->GetShaderMap().Layout, ExpectedLayout);
		EXPECT_EQ(Loaded->GetStaticProperties().GeometryUsage, 1u);
		EXPECT_EQ(Loaded->GetStaticProperties().PassUsage, 7u);
		EXPECT_TRUE(Loaded->GetExpressionCollection().Expressions.empty());
		EXPECT_TRUE(Loaded->GetAcceptedCompiledProgram()->GetDebugData().IR.Nodes.empty());
		EXPECT_TRUE(Loaded->GetAcceptedCompiledProgram()->GetDebugData().GeneratedSource.empty());
		EXPECT_EQ(Loaded->GetAcceptedCompiledProgram()->GetShaderMap().RequiredStages,
			SelectMaterialShaderStages(Loaded->GetStaticProperties(), false));
		EXPECT_EQ(Loaded->GetAcceptedCompiledProgram()->GetShaderMap().CompiledShaders.size(), 4u);
		EXPECT_FALSE(std::ranges::any_of(Loaded->GetAcceptedCompiledProgram()->GetShaderMap().CompiledShaders,
			[](const auto& Stage) { return Stage.SourceEntryPoint == "HitProxyFragmentMain"; }));
		DMaterialInstance* LoadedInstance = nullptr;
		{
			auto LoadedValue = LoadObject<DMaterialInstance>(InstancePath);
			LoadedInstance = LoadedValue.value_or(nullptr);
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
	ShutdownAssetManager(); CollectGarbage();
	ASSERT_TRUE(InitializeAssetManager());
}
