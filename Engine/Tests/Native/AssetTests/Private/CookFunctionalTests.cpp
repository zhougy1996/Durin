#include "Texture/Texture2DCompilationTestSupport.h"
#include "NativeRHIBackendTestSupport.h"
#include "NativeAssetBuildTestSupport.h"
#include "Asset/Cook.h"
#include "Asset/AssetCompilingManager.h"
#include "AssetRegistry/Scan.h"
#include "Asset/Load.h"
#include "Asset/PackageSerialization.h"
#include "Asset/PackageInspection.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/Object.h"
#include "DObject/Package.h"
#include "DObject/Class.h"
#include "DObject/DurinPropertyTypes.h"
#include "Asset/OfflinePreparation.h"
#include "DerivedDataCache/DerivedDataCacheDiagnostics.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/MountPathTestSupport.h"
#include "Modules/ModuleManager.h"
#include "Shader/GlobalShader.h"
#include "Shader/IShaderBuildModule.h"
#include "ShaderBuild/ShaderPaths.h"
#include "NativeTestSupport.h"
#include "NativeDObjectTestSupport.h"
#include "NativeAssetTestSupport.h"
#include "NativeAssetRuntimeTestSupport.h"
#include "Texture/Texture2D.h"
#include "Texture/Texture2DCompilation.h"
#include "Texture/TextureCube.h"
#include "Texture/VolumeTexture.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialCookedProgram.h"
#include "Serialization/BinaryFormat.h"
#include "Serialization/CustomVersion.h"
#include "StaticMesh/StaticMesh.h"
#include "Physics/BodySetup.h"
#include "Threading/TaskRuntime.h"

#include <gtest/gtest.h>
#include "../../../../Source/Runtime/Engine/Private/Materials/MaterialTranslation.h"
#include "Shader/ShaderCompilingManager.h"

namespace
{
	using namespace Durin;

	class DCookReferenceFixture : public DObject
	{
	public:
		static inline std::function<void(FObjectCookContext&)> SchemaHook;
		static inline std::function<void(FObjectCookContext&)> InstanceHook;
		static inline std::function<void(FObjectPreSaveContext&)> SaveHook;
		static inline bool bCompletionReady = true;
		static inline std::function<FCookContributionResult(DObject&, std::string_view, FCookPreparationContext&)> PrepareHook;
		static inline uint32 Constructions = 0, Begins = 0, Clears = 0;
		explicit DCookReferenceFixture(const FObjectInitializer& Initializer = FObjectInitializer::Get()) : DObject(Initializer) { ++Constructions; }
		static auto AppendToClassSchema(FObjectCookContext& Context) -> void { if (SchemaHook) SchemaHook(Context); }
		auto OnCookEvent(ECookEvent Event, FObjectCookContext& Context) const -> void override {
			DObject::OnCookEvent(Event, Context);
			if (Context.bCooking && InstanceHook) InstanceHook(Context);
		}
		auto BeginCacheForCookedPlatformData(FObjectCookContext& Context) -> void override {
			DObject::BeginCacheForCookedPlatformData(Context); ++Begins;
			if (PrepareHook) {
				auto* Preparation = FCookPreparationContext::Get(*this, Context);
				ASSERT_NE(Preparation, nullptr);
				Preparation->Failure = PrepareHook(*this, Context.Package, *Preparation);
			}
		}
		auto ClearCachedCookedPlatformData(const FObjectCookContext& Context) -> void override {
			++Clears; DObject::ClearCachedCookedPlatformData(Context);
		}
		auto IsCachedCookedPlatformDataLoaded(const FObjectCookContext&) const -> bool override { return bCompletionReady; }
		auto PreSave(FObjectPreSaveContext& Context) -> void override { DObject::PreSave(Context); if (SaveHook) SaveHook(Context); }
		static auto Reset() -> void { SchemaHook = {}; InstanceHook = {}; PrepareHook = {}; SaveHook = {}; bCompletionReady = true; Constructions = Begins = Clears = 0; }
		static void __DefaultConstructor(const FObjectInitializer& X) { new (X.GetObj()) DCookReferenceFixture(X); }
		static auto StaticClassNoRegister() -> DClass*
		{
			static DClass* Class = nullptr;
			if (!Class) {
				Class = new DClass(EC_StaticConstructor, "DCookReferenceFixture", sizeof(DCookReferenceFixture), alignof(DCookReferenceFixture), EObjectFlags::NoFlags, EClassFlags::None, EClassCastFlags::DClass, (DClass::ClassConstructorType)InternalConstructor<DCookReferenceFixture>);
				Class->SetSuperStructBase(DObject::StaticClass());
				Class->Register(DClass::StaticClass, "", "DCookReferenceFixture");
			}
			return Class;
		}
		static auto StaticClass() -> DClass*
		{
			static const auto Runtime = DurinCodeGen::FObjectPropertyParams::ObjectPtr<DObject>("RuntimeReference", EPropertyFlags::None, 1, STRUCT_OFFSET_UINT16(DCookReferenceFixture, RuntimeReference), &DObject::StaticClass);
			static const auto Editor = DurinCodeGen::FObjectPropertyParams::ObjectPtr<DObject>("EditorReference", EPropertyFlags::EditorOnly, 1, STRUCT_OFFSET_UINT16(DCookReferenceFixture, EditorReference), &DObject::StaticClass);
			static const DurinCodeGen::FPropertyParamsBase* Properties[] = {&Runtime, &Editor};
			static const DurinCodeGen::FClassParams Params = {
				.ClassNoRegisterFunc = &StaticClassNoRegister, .QualifiedClassName = "Tests::DCookReferenceFixture",
				.ShortClassName = "DCookReferenceFixture", .PropertyParams = Properties, .NumProperties = std::size(Properties),
				.CookSchema = &AppendToClassSchema};
			static DClass* Class = DurinCodeGen::ConstructDClass(Params);
			return Class;
		}
		auto NeedsLoadForTargetPlatform(const FArchiveTarget& Target) const -> bool override
		{ return !bWin64Only || Target.Platform == GetTargetPlatformName(ECookTargetPlatform::Win64); }
		bool bWin64Only = false;
		TObjectPtr<DObject> RuntimeReference;
		TObjectPtr<DObject> EditorReference;
	};

	class DCookSecondaryFixture : public DObject
	{
	public:
		static inline std::function<void(FObjectCookContext&)> SecondarySchema;
		static inline std::function<void(FObjectCookContext&)> SecondaryInstance;
		explicit DCookSecondaryFixture(const FObjectInitializer& Initializer = FObjectInitializer::Get()) : DObject(Initializer) {}
		static auto __DefaultConstructor(const FObjectInitializer& X) -> void { new (X.GetObj()) DCookSecondaryFixture(X); }
		auto IsEditorOnly() const -> bool override { return true; }
		static auto AppendToClassSchema(FObjectCookContext& Context) -> void { if (SecondarySchema) SecondarySchema(Context); }
		auto OnCookEvent(ECookEvent Event, FObjectCookContext& Context) const -> void override {
			DObject::OnCookEvent(Event, Context);
			if (Context.bCooking && SecondaryInstance) SecondaryInstance(Context);
		}
		static auto StaticClassNoRegister() -> DClass* {
			static DClass* Class = [] {
				auto* C = new DClass(EC_StaticConstructor, "DCookSecondaryFixture", sizeof(DCookSecondaryFixture), alignof(DCookSecondaryFixture),
					EObjectFlags::NoFlags, EClassFlags::None, EClassCastFlags::DClass, &InternalConstructor<DCookSecondaryFixture>);
				C->SetSuperStructBase(DObject::StaticClass()); C->Register(DClass::StaticClass, "", "DCookSecondaryFixture"); return C;
			}(); return Class;
		}
		static auto StaticClass() -> DClass* {
			static const DurinCodeGen::FClassParams Params{.ClassNoRegisterFunc = &StaticClassNoRegister,
				.QualifiedClassName = "Tests::DCookSecondaryFixture", .ShortClassName = "DCookSecondaryFixture", .CookSchema = &AppendToClassSchema};
			static DClass* Class = DurinCodeGen::ConstructDClass(Params); return Class;
		}
	};

	// Keeps the Cook shader library independent of the renderer's production inventory.
	class FCookFixtureShader : public FGlobalShader
	{
	public:
		DURIN_DECLARE_GLOBAL_SHADER(FCookFixtureShader, FGlobalShader,
			"/CookFixture/Minimal", EShaderFrequency::Compute, "Main");
	};
	DURIN_IMPLEMENT_GLOBAL_SHADER(FCookFixtureShader);

	auto Inventory(const std::filesystem::path& Root) -> std::map<std::string, FXxHash128>
	{
		std::map<std::string, FXxHash128> Result;
		for (const auto& Entry : std::filesystem::recursive_directory_iterator(Root))
		{
			if (!Entry.is_regular_file()) continue;
			FByteBuffer Bytes;
			auto BytesRead = FFileHelper::LoadFileToArray(Entry.path());
			EXPECT_TRUE(BytesRead) << BytesRead.error().ToString();
			if (BytesRead) Bytes = std::move(*BytesRead);
			Result.emplace(Entry.path().lexically_relative(Root).generic_string(), FXxHash128::HashBuffer(Bytes));
		}
		return Result;
	}

	// Owns type-fixture hooks while each test supplies an isolated content mount.
	class FCookFunctionalTests : public ::testing::Test
	{
	protected:
		static auto SetUpTestSuite() -> void
		{
			Testing::InitializeDObjectSystemForTests();
			ASSERT_TRUE(InitializeTaskScheduler(2));
			ASSERT_TRUE(InitializeAssetCompilingManager());
			FModuleManager::Get().LoadModuleChecked("TextureCompressor");
			FModuleManager::Get().LoadModuleChecked("MeshBuilder");
			static const FGlobalShaderSetRegistration Shaders("CookFixture", "CookFixture.Minimal",
				EShaderRequestEligibility::GameAndEditor, {&FCookFixtureShader::StaticType()});
			ASSERT_TRUE(Shaders.IsValid());
			const auto ShaderRoot = Testing::GetTestWorkDirectory() / "CookShaders";
			std::filesystem::create_directories(ShaderRoot);
			const std::string Source = "[shader(\"compute\")] [numthreads(1,1,1)] void Main() {}";
			ASSERT_TRUE(FFileHelper::SaveArrayToFile(std::as_bytes(std::span(Source)), ShaderRoot / "Minimal.slang"));
			FShaderPaths::RegisterMountPoint("/CookFixture/", ShaderRoot.generic_string(),
				(Testing::GetTestWorkDirectory() / "CookShaderCache").generic_string());
			FModuleManager::Get().LoadModuleChecked("ShaderBuild");
		}
		static auto TearDownTestSuite() -> void
		{
			FAssetCompilingManager::Get().FinishAllCompilation();
			ShutdownAssetCompilingManager();
			ShutdownTaskSystem(ETaskShutdownMode::Drain);
		}
		auto SetUp() -> void override { DCookReferenceFixture::Reset(); DCookSecondaryFixture::SecondarySchema = {}; DCookSecondaryFixture::SecondaryInstance = {}; }
		auto TearDown() -> void override { DCookReferenceFixture::Reset(); DCookSecondaryFixture::SecondarySchema = {}; DCookSecondaryFixture::SecondaryInstance = {}; }
		FScopedOfflinePreparation Offline;
	};
}

TEST_F(FCookFunctionalTests, ObjectHooksPrepareEveryExportAndWarmReuseSkipsConstruction)
{
	const auto Fixture = Testing::CreateTestFixtureDirectory("CookObjectHooks");
	Testing::FScopedMountRegistryFixture Mounts;
	Testing::RegisterMountPointForTests("/CookTests/", (Fixture / "Content").generic_string());
	FPackagePath Path; ASSERT_TRUE(FPackagePath::TryCreate("/CookTests/ObjectHooks", Path));
	auto* Package = CreatePackage(Path);
	ASSERT_NE(NewObject<DCookReferenceFixture>(Package, "First"), nullptr);
	ASSERT_NE(NewObject<DCookReferenceFixture>(Package, "Second"), nullptr);
	ASSERT_TRUE(SavePackage(Package)); ASSERT_TRUE(UnloadPackage(Package));
	ASSERT_TRUE(RefreshAssetRegistry(EAssetRegistryScanMode::FullValidation));
	uint32 SchemaCalls = 0;
	DCookReferenceFixture::SchemaHook = [&](FObjectCookContext& Context) { EXPECT_TRUE(Context.Package.empty()); ++SchemaCalls; };
	FCookRequest Request{.OutputRoot = Fixture / "Output", .TargetPlatform = ECookTargetPlatform::Win64,
		.TargetProfile = ECookTargetProfile::Game, .ExplicitRoots = {Path}};
	FCookRunResult Result;
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << FormatCookRunError(Result);
	EXPECT_EQ(SchemaCalls, 1u); EXPECT_EQ(DCookReferenceFixture::Begins, 2u); EXPECT_EQ(DCookReferenceFixture::Clears, 2u);
	const auto Constructions = DCookReferenceFixture::Constructions;
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << FormatCookRunError(Result);
	ASSERT_EQ(Result.Packages.size(), 1u); EXPECT_EQ(Result.Packages.front().Status, ECookPackageStatus::CookHit);
	EXPECT_EQ(DCookReferenceFixture::Constructions, Constructions);
	EXPECT_EQ(DCookReferenceFixture::Begins, 2u); EXPECT_EQ(DCookReferenceFixture::Clears, 2u);
}

TEST_F(FCookFunctionalTests, ReusedOutputsCarryOnlyMetadataAndCorruptionRecaptures)
{
	const auto Fixture = Testing::CreateTestFixtureDirectory("CookReuseReferences");
	Testing::FScopedMountRegistryFixture Mounts;
	Testing::RegisterMountPointForTests("/CookTests/", (Fixture / "Content").generic_string());
	FPackagePath Path; ASSERT_TRUE(FPackagePath::TryCreate("/CookTests/Reuse", Path));
	auto* Package = CreatePackage(Path);
	ASSERT_NE(NewObject<DCookReferenceFixture>(Package, "Reuse"), nullptr);
	ASSERT_TRUE(SavePackage(Package)); ASSERT_TRUE(UnloadPackage(Package));
	ASSERT_TRUE(RefreshAssetRegistry(EAssetRegistryScanMode::FullValidation));
	FCookRequest Request{.OutputRoot = Fixture / "Output", .TargetPlatform = ECookTargetPlatform::Win64,
		.TargetProfile = ECookTargetProfile::Game, .ExplicitRoots = {Path}};
	FCookRunResult Result;
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << FormatCookRunError(Result);
	const auto Before = Inventory(Request.OutputRoot);
	class FCheckingStore final : public ICookOutputStore
	{
	public:
		bool bCalled = false;
		std::unique_ptr<ICookOutputStore> Store;
		auto Publish(std::span<const FCookSavePlan> Plans, std::span<const FCookAuxiliaryOutput> Auxiliary,
			const FCookState& State, FCookRunResult& Result, const FCookCancellationCheck& Cancel,
			const FCookFailureInjection& Fail) -> FCookPublishResult override
		{
			bCalled = true;
			EXPECT_EQ(Plans.size(), 1u);
			for (const auto& Plan : Plans) {
				EXPECT_TRUE(Plan.bReuseExistingOutput);
				EXPECT_TRUE(Plan.PackageBytes.empty()); EXPECT_TRUE(Plan.BulkBytes.empty());
				EXPECT_GT(Plan.PackageFileSize, 0u);
			}
			return Store->Publish(Plans, Auxiliary, State, Result, Cancel, Fail);
		}
	} Store;
	Store.Store = CreateLocalLooseCookOutputStore(Request.OutputRoot, Request.TargetPlatform, Request.TargetProfile);
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result, &Store)) << FormatCookRunError(Result);
	EXPECT_TRUE(Store.bCalled);
	EXPECT_EQ(Result.Packages.front().Status, ECookPackageStatus::CookHit);
	EXPECT_EQ(Inventory(Request.OutputRoot), Before);
	const std::array Corrupt{std::byte{0}};
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(Corrupt, Request.OutputRoot / "CookTests/Reuse.dasset"));
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << FormatCookRunError(Result);
	EXPECT_EQ(Result.Packages.front().Status, ECookPackageStatus::Captured);
	EXPECT_EQ(Inventory(Request.OutputRoot), Before);
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(Corrupt, Request.OutputRoot / "CookManifest.bin"));
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << FormatCookRunError(Result);
	EXPECT_EQ(Result.Packages.front().Status, ECookPackageStatus::Captured);
	EXPECT_EQ(Inventory(Request.OutputRoot), Before);
}

TEST_F(FCookFunctionalTests, ClassSchemasDeclareMaterialInputsOnceAndOtherFamiliesIgnoreThem)
{
	FObjectCookContext Material;
	Material.RequestValues["shader-build"] = {std::byte{1}};
	FBinaryWriter Configurations;
	Configurations.WriteU32(1);
	Configurations.WriteU32(static_cast<uint32>(EMaterialQualityLevel::High));
	Configurations.WriteU32(static_cast<uint32>(ERHIFeatureLevel::SM5));
	Material.RequestValues["material/configurations"] = Configurations.TakeBytes();
	DMaterial::StaticClass()->AppendCookSchema(Material);
	ASSERT_TRUE(Material.Result) << Material.Result.Diagnostic;
	for (const auto Name : {"shader-build", "material/configurations"}) {
		EXPECT_EQ(std::ranges::count(Material.Dependencies, "request/" + std::string(Name), &FCookDependencyDeclaration::LogicalName), 1);
		EXPECT_EQ(std::ranges::count(Material.Dependencies, std::string(Name), &FCookDependencyDeclaration::LogicalName), 0);
	}
	FObjectCookContext Missing;
	DMaterial::StaticClass()->AppendCookSchema(Missing);
	EXPECT_FALSE(Missing.Result);
	for (auto* Class : {DTexture2D::StaticClass(), DTextureCube::StaticClass(), DVolumeTexture::StaticClass(), DStaticMesh::StaticClass()}) {
		FObjectCookContext Context;
		Context.RequestValues["material/configurations"] = {std::byte{0xff}};
		Class->AppendCookSchema(Context);
		EXPECT_TRUE(Context.Result) << Context.Result.Diagnostic;
		EXPECT_TRUE(Context.RequestedValues.empty());
	}
}

TEST_F(FCookFunctionalTests, BeginsEveryObjectBeforeWaitingAndClearsAfterCapture)
{
	const auto Fixture = Testing::CreateTestFixtureDirectory("CookLifecycleOrder");
	Testing::FScopedMountRegistryFixture Mounts;
	Testing::RegisterMountPointForTests("/CookTests/", (Fixture / "Content").generic_string());
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/CookTests/LifecycleOrder", Path));
	auto* Package = CreatePackage(Path);
	NewObject<DCookReferenceFixture>(Package, "First");
	NewObject<DCookReferenceFixture>(Package, "Second");
	uint32 Joined = 0, Saves = 0;
	DCookReferenceFixture::PrepareHook = [&](DObject&, std::string_view, FCookPreparationContext& Context) -> FCookContributionResult {
		Context.Lifecycle.WaitForCompletion = [&] { EXPECT_EQ(DCookReferenceFixture::Begins, 2u); ++Joined; };
		return {};
	};
	DCookReferenceFixture::SaveHook = [&](FObjectPreSaveContext& Context) {
		EXPECT_TRUE(Context.bCooking);
		EXPECT_EQ(Joined, 2u);
		EXPECT_EQ(DCookReferenceFixture::Clears, 0u);
		++Saves;
	};
	FCookContext Cook(ECookTargetPlatform::Win64, ECookTargetProfile::Game);
	ASSERT_TRUE(Cook.AddPackage(Path.ToString(), Package));
	EXPECT_EQ(Saves, 2u);
	EXPECT_EQ(DCookReferenceFixture::Clears, 2u);
}

TEST_F(FCookFunctionalTests, CancellationJoinsAndClearsEveryStartedOperationWithoutCapture)
{
	const auto Fixture = Testing::CreateTestFixtureDirectory("CookLifecycleCancellation");
	Testing::FScopedMountRegistryFixture Mounts;
	Testing::RegisterMountPointForTests("/CookTests/", (Fixture / "Content").generic_string());
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/CookTests/LifecycleCancellation", Path));
	auto* Package = CreatePackage(Path);
	NewObject<DCookReferenceFixture>(Package, "First");
	NewObject<DCookReferenceFixture>(Package, "Second");
	uint32 Joined = 0, Saves = 0;
	bool Cancel = false;
	DCookReferenceFixture::PrepareHook = [&](DObject&, std::string_view, FCookPreparationContext& Context) -> FCookContributionResult {
		Context.Lifecycle.WaitForCompletion = [&, Invocation = &Context.Lifecycle] {
			++Joined;
			Cancel = true;
			(void)Invocation->CheckCancellation();
		};
		return {};
	};
	DCookReferenceFixture::SaveHook = [&](FObjectPreSaveContext&) { ++Saves; };
	FCookContext Cook(ECookTargetPlatform::Win64, ECookTargetProfile::Game);
	Cook.SetCancellationCheck([&] { return Cancel; });
	EXPECT_FALSE(Cook.AddPackage(Path.ToString(), Package));
	EXPECT_EQ(Joined, 2u);
	EXPECT_EQ(DCookReferenceFixture::Clears, 2u);
	EXPECT_EQ(Saves, 0u);
	EXPECT_EQ(Cook.GetPreparationFailure().Status, ECookContributionStatus::Canceled);
	EXPECT_TRUE(Cook.GetSavePlans().empty());
}

TEST_F(FCookFunctionalTests, DetachedPreparationOverlapsAndAdmissionCanDrainOneSlot)
{
	const auto Fixture = Testing::CreateTestFixtureDirectory("CookAsyncAdmission");
	Testing::FScopedMountRegistryFixture Mounts;
	Testing::RegisterMountPointForTests("/CookTests/", (Fixture / "Content").generic_string());
	FPackagePath Path; ASSERT_TRUE(FPackagePath::TryCreate("/CookTests/AsyncAdmission", Path));
	auto* Package = CreatePackage(Path);
	NewObject<DCookReferenceFixture>(Package, "First");
	NewObject<DCookReferenceFixture>(Package, "Second");
	std::mutex Mutex; std::condition_variable Changed;
	uint32 Entered = 0;
	bool RequireOverlap = true;
	DCookReferenceFixture::PrepareHook = [&](DObject&, std::string_view, FCookPreparationContext& Context) -> FCookContributionResult {
		if (Context.ReserveWorkingSet(1024)) Context.StartDetachedWork([&](const FAssetBuildTaskContext&) -> std::expected<FCookPreparationProducts, std::string> {
			std::unique_lock Lock(Mutex); ++Entered; Changed.notify_all();
			if (RequireOverlap && !Changed.wait_for(Lock, std::chrono::seconds(5), [&] { return Entered == 2; }))
				return std::unexpected("Independent Cook tasks failed to overlap.");
			return FCookPreparationProducts{};
		});
		return {};
	};
	FAssetPackageSerializationOptions Options{.Domain = EAssetPackageSaveDomain::Cooked,
		.TargetPlatform = ECookTargetPlatform::Win64, .TargetProfile = ECookTargetProfile::Game};
	Options.MaximumConcurrentPreparation = 2; Options.MaximumPreparationWorkingSetBytes = 2048;
	FCookPreparationMetrics Metrics; Options.PreparationMetrics = &Metrics;
	FByteBuffer Bytes;
	ASSERT_TRUE(SerializeAssetPackageBytes(Package, Bytes, Options));
	EXPECT_EQ(Entered, 2u); EXPECT_EQ(DCookReferenceFixture::Clears, 2u);
	EXPECT_EQ(Metrics.AdmittedOperations, 2u); EXPECT_EQ(Metrics.PeakAdmittedOperations, 2u);
	EXPECT_EQ(Metrics.PeakReservedBytes, 2048u);
	RequireOverlap = false; Entered = 0;
	Options.MaximumConcurrentPreparation = 1; Options.MaximumPreparationWorkingSetBytes = 1024;
	ASSERT_TRUE(SerializeAssetPackageBytes(Package, Bytes, Options));
	EXPECT_EQ(Entered, 2u); EXPECT_EQ(DCookReferenceFixture::Clears, 4u);
	EXPECT_EQ(Metrics.PeakAdmittedOperations, 1u); EXPECT_EQ(Metrics.PeakReservedBytes, 1024u);
	RecordProperty("single_slot_peak_reserved_bytes", Metrics.PeakReservedBytes);
	Options.MaximumPreparationWorkingSetBytes = 512;
	Entered = 0;
	EXPECT_FALSE(SerializeAssetPackageBytes(Package, Bytes, Options));
	EXPECT_EQ(Entered, 0u);
}

TEST_F(FCookFunctionalTests, DetachedFailureCancelsAndJoinsOtherStartedWork)
{
	const auto Fixture = Testing::CreateTestFixtureDirectory("CookAsyncFailure");
	Testing::FScopedMountRegistryFixture Mounts;
	Testing::RegisterMountPointForTests("/CookTests/", (Fixture / "Content").generic_string());
	FPackagePath Path; ASSERT_TRUE(FPackagePath::TryCreate("/CookTests/AsyncFailure", Path));
	auto* Package = CreatePackage(Path);
	NewObject<DCookReferenceFixture>(Package, "First"); NewObject<DCookReferenceFixture>(Package, "Second");
	std::atomic<uint32> Ended = 0;
	uint32 Index = 0;
	DCookReferenceFixture::PrepareHook = [&](DObject&, std::string_view, FCookPreparationContext& Context) -> FCookContributionResult {
		const bool Fail = Index++ == 0;
		if (Context.ReserveWorkingSet(1024)) Context.StartDetachedWork([&, Fail](const FAssetBuildTaskContext& Control) -> std::expected<FCookPreparationProducts, std::string> {
			if (!Fail) {
				const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
				while (!Control.IsCancelled() && std::chrono::steady_clock::now() < Deadline) std::this_thread::yield();
			}
			++Ended; return std::unexpected("Injected Cook failure.");
		});
		return {};
	};
	FCookContext Cook(ECookTargetPlatform::Win64, ECookTargetProfile::Game);
	EXPECT_FALSE(Cook.AddPackage(Path.ToString(), Package));
	// A queued canceled operation may exit before invoking its work body.
	EXPECT_GE(Ended.load(), 1u); EXPECT_EQ(DCookReferenceFixture::Clears, 2u);
	EXPECT_TRUE(Cook.GetSavePlans().empty());
}

TEST_F(FCookFunctionalTests, IncompleteOperationsFailWithoutPollingOrPublishing)
{
	const auto Fixture = Testing::CreateTestFixtureDirectory("CookLifecycleIncomplete");
	Testing::FScopedMountRegistryFixture Mounts;
	Testing::RegisterMountPointForTests("/CookTests/", (Fixture / "Content").generic_string());
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/CookTests/LifecycleIncomplete", Path));
	auto* Package = CreatePackage(Path);
	NewObject<DCookReferenceFixture>(Package, "Root");
	DCookReferenceFixture::bCompletionReady = false;
	FCookContext Cook(ECookTargetPlatform::Win64, ECookTargetProfile::Game);
	EXPECT_FALSE(Cook.AddPackage(Path.ToString(), Package));
	EXPECT_EQ(DCookReferenceFixture::Clears, 1u);
	EXPECT_EQ(Cook.GetPreparationFailure().Error, ECookContributionError::Preparation);
	EXPECT_TRUE(Cook.GetSavePlans().empty());
}

TEST_F(FCookFunctionalTests, PreparationCannotAddAnUnpreparedPersistentObject)
{
	const auto Fixture = Testing::CreateTestFixtureDirectory("CookLifecycleGraphGrowth");
	Testing::FScopedMountRegistryFixture Mounts;
	Testing::RegisterMountPointForTests("/CookTests/", (Fixture / "Content").generic_string());
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/CookTests/LifecycleGraphGrowth", Path));
	auto* Package = CreatePackage(Path);
	NewObject<DCookReferenceFixture>(Package, "Root");
	DCookReferenceFixture::PrepareHook = [](DObject& Object, std::string_view, FCookPreparationContext&) -> FCookContributionResult {
		NewObject<DObject>(&Object, "UnpreparedChild");
		return {};
	};
	FCookContext Cook(ECookTargetPlatform::Win64, ECookTargetProfile::Game);
	EXPECT_FALSE(Cook.AddPackage(Path.ToString(), Package));
	EXPECT_EQ(DCookReferenceFixture::Clears, 1u);
	EXPECT_TRUE(Cook.GetSavePlans().empty());
}

TEST_F(FCookFunctionalTests, MultipleMeshesProduceIdenticalBytesAcrossAdmissionBudgets)
{
	const auto Fixture = Testing::CreateTestFixtureDirectory("CookMeshBudgets");
	struct FRestoreCacheRoot
	{
		std::string Previous = FPaths::DerivedDataCacheDir();
		~FRestoreCacheRoot() { FPaths::SetDerivedDataCacheDirForTests(Previous); }
	} RestoreCacheRoot;
	const auto CacheRoot = Fixture / "DerivedDataCache";
	FPaths::SetDerivedDataCacheDirForTests(CacheRoot.generic_string());
	const auto Counters = [](std::string_view Name) {
		for (const auto& Bucket : DerivedData::GetCacheDiagnostics().Buckets)
			if (Bucket.Bucket == Name) return Bucket.Counters;
		return DerivedData::FCacheDiagnosticCounters{};
	};
	Testing::FScopedMountRegistryFixture Mounts;
	Testing::RegisterMountPointForTests("/CookTests/", (Fixture / "Content").generic_string());
	FPackagePath Path; ASSERT_TRUE(FPackagePath::TryCreate("/CookTests/MeshBudgets", Path));
	auto* Package = CreatePackage(Path);
	struct FBodyState
	{
		DBodySetup* Body;
		uint64 Revision, BuildRevision, RequestGeneration;
		EPhysicsMeshBuildStatus Status;
		std::optional<std::string> Error;
	};
	std::vector<FBodyState> BodyStates;
	for (uint32 Index = 0; Index < 3; ++Index) {
		auto* Mesh = NewObject<DStaticMesh>(Package, FName(std::format("Mesh{}", Index)));
		FMeshDescription Geometry; Geometry.PolygonGroups.push_back({"Material", 0, "Material"});
		auto& Section = Geometry.Sections.emplace_back(); Section.Name = "Tetrahedron";
		Section.Positions = {{0,0,0}, {1,0,0}, {0,1,0}, {0,0,1}};
		Section.Indices = {0,2,1, 0,1,3, 1,2,3, 2,0,3};
		FStaticMeshSource Source; ASSERT_TRUE(Source.Initialize(std::move(Geometry)));
		*DStaticMesh::StaticClass()->FindPropertyByName("Source")->ContainerPtrToValuePtr<FStaticMeshSource>(Mesh) = std::move(Source);
		*DStaticMesh::StaticClass()->FindPropertyByName("MaterialSlots")->ContainerPtrToValuePtr<std::vector<FMeshMaterialSlotDefinition>>(Mesh) = {{.Name = "Material"}};
		if (Index) {
			auto* Body = NewObject<DBodySetup>(Mesh, "BodySetup");
			ASSERT_TRUE(Mesh->SetBodySetup(Body));
			// Initialize stored settings without scheduling an editor collision build.
			*DBodySetup::StaticClass()->FindPropertyByName("CollisionSourceMode")->ContainerPtrToValuePtr<EBodySetupCollisionSourceMode>(Body) =
				Index == 1 ? EBodySetupCollisionSourceMode::ConvexHullFromLOD0 : EBodySetupCollisionSourceMode::TriangleMeshFromLOD0;
			ASSERT_FALSE(Body->GetResidentSimpleGeometry());
			ASSERT_FALSE(Body->GetResidentComplexGeometry());
			BodyStates.push_back({Body, Body->GetRevision(), Body->GetCollisionBuildRevision(),
				Body->GetPhysicsMeshRequestGeneration(), Body->GetPhysicsMeshBuildStatus(), Body->GetPhysicsMeshBuildError()});
		}
	}
	FAssetPackageSerializationOptions Options{.Domain = EAssetPackageSaveDomain::Cooked,
		.TargetPlatform = ECookTargetPlatform::Win64, .TargetProfile = ECookTargetProfile::Game};
	Package->ClearDirty();
	FByteBuffer AuthoredBefore;
	ASSERT_TRUE(SerializeAssetPackageBytes(Package, AuthoredBefore));
	const auto CheckAssetState = [&] {
		FByteBuffer AuthoredAfter;
		ASSERT_TRUE(SerializeAssetPackageBytes(Package, AuthoredAfter));
		EXPECT_EQ(AuthoredBefore, AuthoredAfter);
		EXPECT_FALSE(Package->IsDirty());
		for (const auto& State : BodyStates) {
			EXPECT_EQ(State.Body->GetRevision(), State.Revision);
			EXPECT_EQ(State.Body->GetCollisionBuildRevision(), State.BuildRevision);
			EXPECT_EQ(State.Body->GetPhysicsMeshRequestGeneration(), State.RequestGeneration);
			EXPECT_EQ(State.Body->GetPhysicsMeshBuildStatus(), State.Status);
			EXPECT_EQ(State.Body->GetPhysicsMeshBuildError(), State.Error);
			EXPECT_FALSE(State.Body->GetResidentSimpleGeometry());
			EXPECT_FALSE(State.Body->GetResidentComplexGeometry());
		}
		for (auto* Object : Package->GetTopLevelAssets()) {
			auto* Mesh = Cast<DStaticMesh>(Object); ASSERT_NE(Mesh, nullptr);
			EXPECT_FALSE(Mesh->GetCookedRenderData().HasData());
			EXPECT_FALSE(Mesh->GetCookedCollisionData().HasData());
			EXPECT_EQ(Mesh->GetRenderData(), nullptr);
		}
	};
	const auto Before = Counters("StaticMesh");
	const auto CollisionBefore = Counters("StaticMeshCollision");
	FByteBuffer Concurrent, Sequential;
	ASSERT_TRUE(SerializeAssetPackageBytes(Package, Concurrent, Options));
	CheckAssetState();
	const auto Cold = Counters("StaticMesh");
	EXPECT_GT(Cold.Misses, Before.Misses);
	EXPECT_GT(Cold.Puts, Before.Puts);
	EXPECT_GT(Cold.WrittenBytes, Before.WrittenBytes);
	const auto CollisionCold = Counters("StaticMeshCollision");
	// The convex and triangle modes each build and persist an independent record.
	EXPECT_EQ(CollisionCold.Misses - CollisionBefore.Misses, 2u);
	EXPECT_EQ(CollisionCold.BuildFallbacks - CollisionBefore.BuildFallbacks, 2u);
	EXPECT_EQ(CollisionCold.Puts - CollisionBefore.Puts, 2u);
	EXPECT_GT(CollisionCold.WrittenBytes, CollisionBefore.WrittenBytes);
	EXPECT_EQ(CollisionCold.StoreFailures, CollisionBefore.StoreFailures);
	const auto ColdCache = Inventory(CacheRoot);
	Options.MaximumConcurrentPreparation = 1;
	ASSERT_TRUE(SerializeAssetPackageBytes(Package, Sequential, Options));
	CheckAssetState();
	const auto Warm = Counters("StaticMesh");
	EXPECT_GT(Warm.Gets, Cold.Gets);
	EXPECT_EQ(Warm.Gets - Cold.Gets, Warm.Hits - Cold.Hits);
	EXPECT_EQ(Warm.Misses, Cold.Misses);
	EXPECT_EQ(Warm.BuildFallbacks, Cold.BuildFallbacks);
	EXPECT_EQ(Warm.Puts, Cold.Puts);
	const auto CollisionWarm = Counters("StaticMeshCollision");
	EXPECT_EQ(CollisionWarm.Gets - CollisionCold.Gets, 2u);
	EXPECT_EQ(CollisionWarm.Hits - CollisionCold.Hits, 2u);
	EXPECT_EQ(CollisionWarm.Misses, CollisionCold.Misses);
	EXPECT_EQ(CollisionWarm.BuildFallbacks, CollisionCold.BuildFallbacks);
	EXPECT_EQ(CollisionWarm.Puts, CollisionCold.Puts);
	EXPECT_EQ(CollisionWarm.WrittenBytes, CollisionCold.WrittenBytes);
	EXPECT_EQ(Inventory(CacheRoot), ColdCache);
	EXPECT_EQ(Concurrent, Sequential);
}

TEST_F(FCookFunctionalTests, MaterialVariantsOverlapAndShuffledDeliveryKeepsMixedPackageBytes)
{
	Testing::FScopedRHIBackendOverride Backend("vulkan");
	const auto Fixture = Testing::CreateTestFixtureDirectory("CookMaterialAsyncVariants");
	Testing::FScopedMountRegistryFixture Mounts;
	Testing::RegisterMountPointForTests("/CookTests/", (Fixture / "Content").generic_string());
	FPackagePath Path; ASSERT_TRUE(FPackagePath::TryCreate("/CookTests/AsyncVariants", Path));
	auto* Package = CreatePackage(Path);
	auto* Material = NewObject<DMaterial>(Package, "Material");
	auto Properties = Material->GetStaticProperties(); Properties.BlendMode = EMaterialBlendMode::Masked; Properties.OpacityMaskThreshold = 0.473237f;
	ASSERT_TRUE(Material->SetStaticProperties(Properties));
	auto* Mesh = NewObject<DStaticMesh>(Package, "Mesh");
	FMeshDescription Geometry; Geometry.PolygonGroups.push_back({"Material", 0, "Material"});
	auto& Section = Geometry.Sections.emplace_back(); Section.Name = "Triangle";
	Section.Positions = {{0,0,0}, {1,0,0}, {0,1,0}}; Section.Indices = {0,1,2};
	FStaticMeshSource Source; ASSERT_TRUE(Source.Initialize(std::move(Geometry)));
	*DStaticMesh::StaticClass()->FindPropertyByName("Source")->ContainerPtrToValuePtr<FStaticMeshSource>(Mesh) = std::move(Source);
	*DStaticMesh::StaticClass()->FindPropertyByName("MaterialSlots")->ContainerPtrToValuePtr<std::vector<FMeshMaterialSlotDefinition>>(Mesh) = {{.Name = "Material", .DefaultMaterial = Material}};
	const auto Accepted = Material->GetAcceptedCompiledProgram();
	const auto GameThreadProducers = MaterialBuildPrivate::GetTranslationStats().GameThreadProducers;
	std::mutex Mutex; std::condition_variable Delivered;
	bool HighDelivered = false;
	std::vector<EMaterialQualityLevel> Order;
	struct FResetHook { ~FResetHook() { MaterialBuildPrivate::SetMaterialBuildCompletionHookForTests({}); } } Reset;
	MaterialBuildPrivate::SetMaterialBuildCompletionHookForTests([&](EMaterialQualityLevel Quality, bool Before) {
		std::unique_lock Lock(Mutex);
		if (Before && Quality == EMaterialQualityLevel::Low)
			EXPECT_TRUE(Delivered.wait_for(Lock, std::chrono::seconds(5), [&] { return HighDelivered; }));
		if (!Before) {
			Order.push_back(Quality);
			if (Quality == EMaterialQualityLevel::High) { HighDelivered = true; Delivered.notify_all(); }
		}
	});
	FAssetPackageSerializationOptions Options{.Domain = EAssetPackageSaveDomain::Cooked,
		.TargetPlatform = ECookTargetPlatform::Win64, .TargetProfile = ECookTargetProfile::Game};
	Options.MaterialConfigurations = {{EMaterialQualityLevel::Low, ERHIFeatureLevel::SM5}, {EMaterialQualityLevel::High, ERHIFeatureLevel::SM5}};
	FByteBuffer Shuffled, Sequential;
	ASSERT_TRUE(SerializeAssetPackageBytes(Package, Shuffled, Options));
	ASSERT_FALSE(Order.empty()); EXPECT_EQ(Order.front(), EMaterialQualityLevel::High);
	MaterialBuildPrivate::SetMaterialBuildCompletionHookForTests({});
	Options.MaximumConcurrentPreparation = 1;
	ASSERT_TRUE(SerializeAssetPackageBytes(Package, Sequential, Options));
	EXPECT_EQ(Shuffled, Sequential);
	// Qualify several material owners separately from the controlled callback stall,
	// so a two-worker scheduler cannot be occupied by two deliberately stalled lows.
	auto* Instance = NewObject<DMaterialInstance>(Package, "Instance"); ASSERT_TRUE(Instance->SetParent(Material));
	NewObject<DMaterial>(Package, "Duplicate");
	Options.MaximumConcurrentPreparation = 4;
	ASSERT_TRUE(SerializeAssetPackageBytes(Package, Shuffled, Options));
	Options.MaximumConcurrentPreparation = 1;
	ASSERT_TRUE(SerializeAssetPackageBytes(Package, Sequential, Options));
	EXPECT_EQ(Shuffled, Sequential);
	EXPECT_EQ(MaterialBuildPrivate::GetTranslationStats().GameThreadProducers, GameThreadProducers);
	EXPECT_EQ(Material->GetAcceptedCompiledProgram(), Accepted);
	EXPECT_FALSE(Material->GetCookedProgramData().HasData());
	EXPECT_EQ(Mesh->GetRenderData(), nullptr);
}

TEST_F(FCookFunctionalTests, MaterialCancellationAfterResultReadinessJoinsBeforeReturning)
{
	Testing::FScopedRHIBackendOverride Backend("vulkan");
	const auto Fixture = Testing::CreateTestFixtureDirectory("CookMaterialCancelReady");
	Testing::FScopedMountRegistryFixture Mounts;
	Testing::RegisterMountPointForTests("/CookTests/", (Fixture / "Content").generic_string());
	FPackagePath Path; ASSERT_TRUE(FPackagePath::TryCreate("/CookTests/CancelReady", Path));
	auto* Package = CreatePackage(Path); NewObject<DMaterial>(Package, "Material");
	std::atomic<bool> Ready = false, Observed = false;
	struct FResetHook { ~FResetHook() { MaterialBuildPrivate::SetMaterialBuildCompletionHookForTests({}); } } Reset;
	MaterialBuildPrivate::SetMaterialBuildCompletionHookForTests([&](EMaterialQualityLevel, bool Before) {
		if (Before && !Ready.exchange(true)) {
			const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (!Observed.load() && std::chrono::steady_clock::now() < Deadline) std::this_thread::yield();
			EXPECT_TRUE(Observed.load());
		}
	});
	FAssetPackageSerializationOptions Options{.Domain = EAssetPackageSaveDomain::Cooked,
		.TargetPlatform = ECookTargetPlatform::Win64, .TargetProfile = ECookTargetProfile::Game};
	Options.MaterialConfigurations = {{EMaterialQualityLevel::Low, ERHIFeatureLevel::SM5}, {EMaterialQualityLevel::High, ERHIFeatureLevel::SM5}};
	Options.IsCancelled = [&] { if (!Ready.load()) return false; Observed = true; return true; };
	FByteBuffer Bytes;
	EXPECT_FALSE(SerializeAssetPackageBytes(Package, Bytes, Options));
	EXPECT_TRUE(Observed.load()); EXPECT_TRUE(Bytes.empty());
}

TEST_F(FCookFunctionalTests, PreSaveStillRejectsGraphGrowthAndInputMutation)
{
	const auto Fixture = Testing::CreateTestFixtureDirectory("CookPreSaveFreeze");
	Testing::FScopedMountRegistryFixture Mounts;
	Testing::RegisterMountPointForTests("/CookTests/", (Fixture / "Content").generic_string());
	FPackagePath Path; ASSERT_TRUE(FPackagePath::TryCreate("/CookTests/PreSaveFreeze", Path));
	auto* Package = CreatePackage(Path); auto* Root = NewObject<DCookReferenceFixture>(Package, "Root");
	DCookReferenceFixture::SaveHook = [&](FObjectPreSaveContext&) { Package->MarkDirty(); };
	FCookContext Cook(ECookTargetPlatform::Win64, ECookTargetProfile::Game);
	EXPECT_FALSE(Cook.AddPackage(Path.ToString(), Package)); EXPECT_TRUE(Cook.GetSavePlans().empty());
	DCookReferenceFixture::SaveHook = [&](FObjectPreSaveContext&) { NewObject<DObject>(Root, "Unexpected"); };
	FCookContext SecondCook(ECookTargetPlatform::Win64, ECookTargetProfile::Game);
	EXPECT_FALSE(SecondCook.AddPackage(Path.ToString(), Package)); EXPECT_TRUE(SecondCook.GetSavePlans().empty());
	EXPECT_EQ(DCookReferenceFixture::Begins, 2u);
}

TEST_F(FCookFunctionalTests, MacOSMeshCollisionTargetBaseline)
{
	const auto Fixture = Testing::CreateTestFixtureDirectory("MacOSCollisionTarget");
	Testing::FScopedMountRegistryFixture Mounts;
	Testing::RegisterMountPointForTests("/CookTests/", (Fixture / "Content").generic_string());
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/CookTests/Mesh", Path));
	auto* Mesh = NewObject<DStaticMesh>(CreatePackage(Path), "Mesh");
	FMeshDescription Geometry;
	Geometry.PolygonGroups.push_back({"Material", 0, "Material"});
	auto& Triangle = Geometry.Sections.emplace_back();
	Triangle.Name = "Triangle";
	Triangle.Positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
	Triangle.Indices = {0, 1, 2};
	FStaticMeshSource Source;
	ASSERT_TRUE(Source.Initialize(std::move(Geometry)));
	*DStaticMesh::StaticClass()->FindPropertyByName("Source")
		->ContainerPtrToValuePtr<FStaticMeshSource>(Mesh) = std::move(Source);
	*DStaticMesh::StaticClass()->FindPropertyByName("MaterialSlots")
		->ContainerPtrToValuePtr<std::vector<FMeshMaterialSlotDefinition>>(Mesh) = {{.Name = "Material"}};
	Mesh->SetCollisionSourceMode(EBodySetupCollisionSourceMode::None);
	FCookContext WithoutCollision(ECookTargetPlatform::MacOS, ECookTargetProfile::Game);
	const auto Saved = WithoutCollision.AddPackage("/CookTests/Mesh", Mesh->GetPackage());
	ASSERT_TRUE(Saved) << FormatCookPlanError(Saved.Error);
	Mesh->SetCollisionSourceMode(EBodySetupCollisionSourceMode::TriangleMeshFromLOD0);
	FCookContext WithCollision(ECookTargetPlatform::MacOS, ECookTargetProfile::Game);
	const auto Rejected = WithCollision.AddPackage("/CookTests/Mesh", Mesh->GetPackage());
	ASSERT_FALSE(Rejected);
}

TEST_F(FCookFunctionalTests, DeclarationFailuresRetainIdentityAndPathCause)
{
	const auto Fixture = Testing::CreateTestFixtureDirectory("CookDeclarationCause");
	Testing::FScopedMountRegistryFixture Mounts;
	Testing::RegisterMountPointForTests("/CookTests/", (Fixture / "Content").generic_string());
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/CookTests/Declarations", Path));
	auto* Package = CreatePackage(Path);
	ASSERT_NE(NewObject<DCookReferenceFixture>(Package, "Declarations"), nullptr);
	ASSERT_TRUE(SavePackage(Package));
	ASSERT_TRUE(UnloadPackage(Package));
	ASSERT_TRUE(RefreshAssetRegistry(EAssetRegistryScanMode::FullValidation));
	std::vector<FCookDependencyDeclaration> Declarations;
	uint32 Contributions = 0;
	DCookReferenceFixture::PrepareHook = [&](DObject&, std::string_view, FCookPreparationContext&) -> FCookContributionResult { ++Contributions; return {}; };
	DCookReferenceFixture::InstanceHook = [&](FObjectCookContext& Context) { Context.Dependencies = Declarations; };
	FCookRequest Request{.OutputRoot = Fixture / "Output", .TargetPlatform = ECookTargetPlatform::Win64,
		.TargetProfile = ECookTargetProfile::Game, .ExplicitRoots = {Path}};
	FCookRunResult Result;
	Declarations = {{ECookBuildDependencyKind::ConfigurationValue, "quality"},
		{ECookBuildDependencyKind::ConfigurationValue, "quality"}};
	EXPECT_FALSE(FCookCoordinator().Run(Request, Result));
	ASSERT_TRUE(Result.InputFailure.GetDiagnostic());
	EXPECT_EQ(Result.InputFailure.GetDiagnostic()->Error, ECookInputError::DuplicateDeclaration);
	EXPECT_EQ(Result.InputFailure.GetDiagnostic()->Package, Path);
	EXPECT_EQ(Result.InputFailure.GetDiagnostic()->Kind, ECookBuildDependencyKind::ConfigurationValue);
	EXPECT_EQ(Result.InputFailure.GetDiagnostic()->Name, "quality");
	const auto Retained = Result.InputFailure.GetDiagnostic();
	Declarations = {{ECookBuildDependencyKind::DirectPackage, "invalid-package"}};
	EXPECT_FALSE(FCookCoordinator().Run(Request, Result));
	ASSERT_TRUE(Result.InputFailure.GetDiagnostic());
	EXPECT_EQ(Result.InputFailure.Status, ECookInputStatus::InvalidDependency);
	EXPECT_EQ(Result.InputFailure.GetDiagnostic()->Error, ECookInputError::PackageDeclaration);
	ASSERT_TRUE(Result.InputFailure.GetDiagnostic()->PathCause);
	EXPECT_EQ(Result.InputFailure.GetDiagnostic()->PathCause->Code, EObjectPathError::NotAbsolute);
	EXPECT_EQ(Result.InputFailure.GetDiagnostic()->Name, "invalid-package");
	EXPECT_EQ(Retained->Name, "quality");
	EXPECT_EQ(Contributions, 0u);
	EXPECT_FALSE(std::filesystem::exists(Request.OutputRoot / "CookManifest.bin"));
}

TEST_F(FCookFunctionalTests, RequestValuesStayFrozenAcrossCallbacks)
{
	const auto Fixture = Testing::CreateTestFixtureDirectory("CookFrozenPolicy");
	Testing::FScopedMountRegistryFixture Mounts;
	Testing::RegisterMountPointForTests("/CookTests/", (Fixture / "Content").generic_string());
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/CookTests/Frozen", Path));
	auto* Package = CreatePackage(Path);
	ASSERT_NE(NewObject<DCookReferenceFixture>(Package, "Frozen"), nullptr);
	ASSERT_TRUE(SavePackage(Package));
	ASSERT_TRUE(UnloadPackage(Package));
	ASSERT_TRUE(RefreshAssetRegistry(EAssetRegistryScanMode::FullValidation));
	const FMaterialCookConfiguration Selected{EMaterialQualityLevel::High, ERHIFeatureLevel::SM5};
	std::vector<FMaterialCookConfiguration> Observed;
	DCookReferenceFixture::PrepareHook = [](DObject&, std::string_view, FCookPreparationContext& Context) -> FCookContributionResult {
		EXPECT_EQ(Context.GetTargetPlatform(), ECookTargetPlatform::Win64); return {};
	};
	DCookReferenceFixture::SchemaHook = [&](FObjectCookContext& Context) {
		EXPECT_EQ(Context.Target.Platform, "Win64");
		FBinaryReader Reader(Context.FindRequestValue("material/configurations")); uint32 Count = 0;
		ASSERT_TRUE(Reader.ReadU32(Count));
		for (uint32 Index=0; Index<Count; ++Index) { uint32 Quality=0, Feature=0; ASSERT_TRUE(Reader.ReadU32(Quality)); ASSERT_TRUE(Reader.ReadU32(Feature));
			Observed.push_back({static_cast<EMaterialQualityLevel>(Quality), static_cast<ERHIFeatureLevel>(Feature)}); }
	};
	FCookRequest Request{.OutputRoot = Fixture / "Output", .TargetPlatform = ECookTargetPlatform::Win64,
		.TargetProfile = ECookTargetProfile::Game, .ExplicitRoots = {Path}, .MaterialConfigurations = {Selected}};
	Request.IsCancelled = [&] {
		Request.TargetPlatform = ECookTargetPlatform::MacOS;
		Request.MaterialConfigurations = {{EMaterialQualityLevel::Low, ERHIFeatureLevel::ES3_1}};
		return false;
	};
	FCookRunResult Result;
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << FormatCookRunError(Result);
	EXPECT_EQ(Result.TargetPlatform, ECookTargetPlatform::Win64);
	EXPECT_EQ(Observed, (std::vector<FMaterialCookConfiguration>{Selected}));
	Request.IsCancelled = {};
	Request.MaterialConfigurations = {Selected, Selected};
	EXPECT_FALSE(FCookCoordinator().Run(Request, Result));
	EXPECT_EQ(Result.Error, ECookRunError::InvalidRequest);
}

TEST_F(FCookFunctionalTests, DetachedMaterialPreparationHonorsSelectedVariants)
{
	const auto Fixture = Testing::CreateTestFixtureDirectory("CookSelectedMaterial");
	Testing::FScopedMountRegistryFixture Mounts;
	Testing::RegisterMountPointForTests("/CookTests/", (Fixture / "Content").generic_string());
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/CookTests/Material", Path));
	auto* Material = NewObject<DMaterial>(CreatePackage(Path), "Material");
	const auto Accepted = Material->GetAcceptedCompiledProgram();
	const auto Revision = Material->GetPackage()->GetEditRevision();
	const std::array Configurations{FMaterialCookConfiguration{EMaterialQualityLevel::High, ERHIFeatureLevel::SM5}};
	auto Prepared = Material->PrepareCookedProgram(ECookTargetPlatform::Win64, Configurations);
	ASSERT_TRUE(Prepared) << Prepared.Error;
	EXPECT_EQ(Material->GetAcceptedCompiledProgram(), Accepted);
	EXPECT_EQ(Material->GetPackage()->GetEditRevision(), Revision);
	const auto BeforeSerialization = GetShaderBuildStats().Compilations;
	FByteBuffer UnpreparedBytes;
	FCanonicalMemoryWriter UnpreparedWriter(UnpreparedBytes, EArchivePurpose::CookedPayload,
		{.bCooking = true, .Target = {"Win64", "Game"}});
	Material->SerializeCooked(UnpreparedWriter);
	ASSERT_TRUE(UnpreparedWriter.IsError());
	EXPECT_TRUE(UnpreparedBytes.empty());
	EXPECT_EQ(GetShaderBuildStats().Compilations, BeforeSerialization);
	auto Read = Prepared->AcquireRead();
	ASSERT_TRUE(Read);
	FMaterialStaticProperties Properties;
	std::shared_ptr<const FMaterialCompilerResult> Program;
	EXPECT_TRUE(DecodeMaterialCookedProgramFamily(Read->GetBytes(), ECookTargetPlatform::Win64,
		ECookTargetProfile::Game, EMaterialQualityLevel::High, ERHIFeatureLevel::SM5, {}, Properties, Program));
	EXPECT_FALSE(DecodeMaterialCookedProgramFamily(Read->GetBytes(), ECookTargetPlatform::Win64,
		ECookTargetProfile::Game, EMaterialQualityLevel::Low, ERHIFeatureLevel::SM5, {}, Properties, Program));
	FAssetBuildTaskContext Cancelled;
	Cancelled.Cancellation.Cancel();
	const auto Canceled = Material->PrepareCookedProgram(ECookTargetPlatform::Win64, Configurations, Cancelled);
	EXPECT_EQ(Canceled.Status, EShaderBuildStatus::Canceled);
	EXPECT_FALSE(Canceled.Value);
	EXPECT_TRUE(Canceled.Error.empty());
	MaterialBuildPrivate::CloseTranslationSessions();
	struct FReopen { ~FReopen() { MaterialBuildPrivate::OpenTranslationSessions(); } } Reopen;
	const auto Failed = Material->PrepareCookedProgram(ECookTargetPlatform::Win64, Configurations);
	EXPECT_EQ(Failed.Status, EShaderBuildStatus::Error);
	EXPECT_FALSE(Failed.Value);
}

TEST_F(FCookFunctionalTests, PreparationOmitsTransientAssetSubtrees)
{
	const auto Fixture = Testing::CreateTestFixtureDirectory("TransientPreparation");
	Testing::FScopedMountRegistryFixture Mounts;
	Testing::RegisterMountPointForTests("/CookTests/", (Fixture / "Content").generic_string());
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/CookTests/TransientPreparation", Path));
	auto* Package = CreatePackage(Path);
	auto* Root = NewObject<DCookReferenceFixture>(Package, "Root");
	ASSERT_NE(NewObject<DStaticMesh>(Root, "InvalidTransientMesh", EObjectFlags::Transient), nullptr);
	auto* Temporary = NewObject<DCookReferenceFixture>(Root, "Temporary", EObjectFlags::Transient);
	ASSERT_NE(NewObject<DStaticMesh>(Temporary, "InvalidMesh"), nullptr);
	ASSERT_NE(NewObject<DMaterial>(Temporary, "Material"), nullptr);
	ASSERT_NE(NewObject<DTexture2D>(Temporary, "Invalid2D"), nullptr);
	ASSERT_NE(NewObject<DTextureCube>(Temporary, "InvalidCube"), nullptr);
	ASSERT_NE(NewObject<DVolumeTexture>(Temporary, "InvalidVolume"), nullptr);
	const auto Before = GetShaderBuildStats().Compilations;
	FCookContext Context(ECookTargetPlatform::Win64, ECookTargetProfile::Game);
	const auto Saved = Context.AddPackage(Path.ToString(), Package);
	ASSERT_TRUE(Saved) << FormatCookPlanError(Saved.Error);
	EXPECT_EQ(GetShaderBuildStats().Compilations, Before);
	auto* Omitted = NewObject<DStaticMesh>(Root, "ExplicitlyOmittedMesh");
	auto Overrides = std::make_shared<FObjectSaveOverrides>();
	ASSERT_TRUE(Overrides->AddObjectOmission(*Omitted));
	ASSERT_TRUE(Overrides->AddObjectOmission(*NewObject<DTexture2D>(Root, "Omitted2D")));
	ASSERT_TRUE(Overrides->AddObjectOmission(*NewObject<DTextureCube>(Root, "OmittedCube")));
	ASSERT_TRUE(Overrides->AddObjectOmission(*NewObject<DVolumeTexture>(Root, "OmittedVolume")));
	FByteBuffer Bytes, Bulk;
	const auto WithOverride = SerializeAssetPackageClosure(Package, Bytes, Bulk,
		{.Domain = EAssetPackageSaveDomain::Cooked, .TargetPlatform = ECookTargetPlatform::Win64,
			.TargetProfile = ECookTargetProfile::Game, .SaveOverrides = Overrides});
	ASSERT_TRUE(WithOverride) << ObjectPackage::FormatPackageError(WithOverride);
	EXPECT_EQ(GetShaderBuildStats().Compilations, Before);
}

namespace
{
template<class T>
auto ExpectDirectTexturePreparation(bool Nested) -> void
{
	const auto Fixture = Testing::CreateTestFixtureDirectory("SecondaryTexturePreparation");
	Testing::FScopedMountRegistryFixture Mounts;
	Testing::RegisterMountPointForTests("/CookTests/", (Fixture / "Content").generic_string());
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate(std::format("/CookTests/Secondary{}{}", int(std::is_same_v<T, DTextureCube>) + 2 * int(std::is_same_v<T, DVolumeTexture>), Nested), Path));
	auto* Package = CreatePackage(Path);
	auto* Root = NewObject<DCookReferenceFixture>(Package, "AFirst");
	ASSERT_NE(Root, nullptr);
	auto* Texture = NewObject<T>(Nested ? Root : static_cast<DObject*>(Package), "Texture");
	ASSERT_NE(Texture, nullptr);
	Root->RuntimeReference = Texture;
	if constexpr (std::is_same_v<T, DVolumeTexture>)
	{
		FVolumeTextureSourceData Voxels;
		Voxels.Width = Voxels.Height = Voxels.Depth = 4;
		ASSERT_TRUE(Voxels.Voxels.UpdatePayload(FByteBuffer(64, std::byte{0x7f})));
		auto Source = PrepareVolumeTextureSource(Voxels);
		ASSERT_TRUE(Source);
		Texture->SetSource(std::move(*Source));
	}
	else
	{
		auto Pixels = Image::FImage::TryCreate({.Width = 4, .Height = 4,
			.Format = Image::ERawImageFormat::RGBA8}, FByteBuffer(64, std::byte{0xff}));
		ASSERT_TRUE(Pixels);
		if constexpr (std::is_same_v<T, DTexture2D>)
		{
			FTextureSource Source;
			ASSERT_TRUE(Source.Init2D(Pixels->GetView(), 4));
			Texture->SetSource(std::move(Source));
		}
		else
		{
			FTextureCubeFaceImages Faces;
			for (auto& Face : Faces.Faces) Face = *Pixels;
			Faces.SourceChannelCount = 4;
			auto Source = PrepareTextureCubeSource(Faces);
			ASSERT_TRUE(Source);
			Texture->SetSource(std::move(*Source));
			Texture->SetBuildSettings(ETextureCubeSourceLayout::SixFaces, 4, 0, 4, 4, true);
		}
	}
	ASSERT_FALSE(Texture->HasPlatformData());
	std::promise<void> Started, Release;
	auto StartedFuture = Started.get_future();
	auto ReleaseFuture = Release.get_future().share();
	bool Released = false;
	bool Completed = false;
	struct FDrainCompilation
	{
		std::promise<void>& Release;
		bool& Released;
		~FDrainCompilation()
		{
			if (!Released) { Released = true; Release.set_value(); }
			FAssetCompilingManager::Get().FinishAllCompilation();
			AssetPrivate::SetTexture2DCompilationPhaseHookForTests({});
		}
	} Drain{Release, Released};
	if constexpr (std::is_same_v<T, DTexture2D>)
	{
	AssetPrivate::SetTexture2DCompilationPhaseHookForTests(
		[&](uint64, ETexture2DCompilationPhase Phase) {
			if (Phase != ETexture2DCompilationPhase::Building) return;
			Started.set_value();
			ReleaseFuture.wait();
		});
	auto Build = MakeTexture2DBuildRequest(Texture->GetSource());
	ASSERT_TRUE(Build);
	ASSERT_TRUE(Texture->BeginCachePlatformData({.Build = std::move(*Build), .ResultApplication = {.bMarkPackageDirty = false}},
		[&](FTextureOperationResult Result) { Completed = Result.Succeeded(); }));
	ASSERT_EQ(StartedFuture.wait_for(std::chrono::seconds(10)), std::future_status::ready);
	ASSERT_FALSE(Completed);
	}
	// A runtime reference keeps the nested object in the stripped cooked graph.
	FCookContext Context(ECookTargetPlatform::Win64, ECookTargetProfile::Game, false);
	Context.SetCancellationCheck([&] {
		if (!Released) { Released = true; Release.set_value(); }
		return false;
	});
	const auto Saved = Context.AddPackage(Path.ToString(), Package);
	ASSERT_TRUE(Saved) << FormatCookPlanError(Saved.Error);
	EXPECT_TRUE(Texture->HasPlatformData());
	if constexpr (std::is_same_v<T, DTexture2D>) EXPECT_TRUE(Completed);
	ASSERT_EQ(Context.GetSavePlans().size(), 1u);
	FCookContext Warm(ECookTargetPlatform::Win64, ECookTargetProfile::Game, false);
	ASSERT_TRUE(Warm.AddPackage(Path.ToString(), Package));
	ASSERT_EQ(Warm.GetSavePlans().size(), 1u);
	EXPECT_EQ(Context.GetSavePlans().front().PackageBytes, Warm.GetSavePlans().front().PackageBytes);
}

}

TEST_F(FCookFunctionalTests, DirectSavePreparesSecondaryAndNestedTexture2D)
{
	ASSERT_NO_FATAL_FAILURE(ExpectDirectTexturePreparation<DTexture2D>(false));
	ASSERT_NO_FATAL_FAILURE(ExpectDirectTexturePreparation<DTexture2D>(true));
}

TEST_F(FCookFunctionalTests, DirectSavePreparesSecondaryAndNestedTextureCube)
{
	ASSERT_NO_FATAL_FAILURE(ExpectDirectTexturePreparation<DTextureCube>(false));
	ASSERT_NO_FATAL_FAILURE(ExpectDirectTexturePreparation<DTextureCube>(true));
}

TEST_F(FCookFunctionalTests, DirectSavePreparesSecondaryAndNestedVolumeTexture)
{
	ASSERT_NO_FATAL_FAILURE(ExpectDirectTexturePreparation<DVolumeTexture>(false));
	ASSERT_NO_FATAL_FAILURE(ExpectDirectTexturePreparation<DVolumeTexture>(true));
}

TEST_F(FCookFunctionalTests, MixedAuthoringOnlyExportsAreFilteredForEitherRetentionPolicy)
{
	const auto Fixture = Testing::CreateTestFixtureDirectory("MixedSourceOnly");
	Testing::FScopedMountRegistryFixture Mounts;
	Testing::RegisterMountPointForTests("/CookTests/", (Fixture / "Content").generic_string());
	for (const bool Retain : {false, true}) for (const bool SourceFirst : {false, true})
	{
		FPackagePath Path;
		ASSERT_TRUE(FPackagePath::TryCreate(std::format("/CookTests/Mixed{}{}", Retain, SourceFirst), Path));
		auto* Package = CreatePackage(Path);
		if (SourceFirst) ASSERT_NE(NewObject<DMaterialFunction>(Package, "AFirst"), nullptr);
		ASSERT_NE(NewObject<DCookReferenceFixture>(Package, SourceFirst ? "ZLast" : "AFirst"), nullptr);
		if (!SourceFirst) ASSERT_NE(NewObject<DMaterialFunction>(Package, "ZLast"), nullptr);
		FByteBuffer Bytes;
		const auto Saved = SerializeAssetPackageBytes(Package, Bytes,
			{.Domain = EAssetPackageSaveDomain::Cooked, .TargetPlatform = ECookTargetPlatform::Win64,
				.TargetProfile = ECookTargetProfile::Game, .bRetainEditorOnlyData = Retain});
		ASSERT_TRUE(Saved) << ObjectPackage::FormatPackageError(Saved);
		const auto File = Fixture / (Retain ? "retained.dasset" : "stripped.dasset");
		ASSERT_TRUE(FFileHelper::SaveArrayToFile(Bytes, File));
		FAssetPackageInspection Inspection;
		const auto Inspected = InspectAssetPackage(File.generic_string(), Path, Inspection);
		ASSERT_TRUE(Inspected) << Inspected.Message;
		EXPECT_EQ(Inspection.Objects.size(), 1u);
		EXPECT_EQ(Inspection.Objects.front().ClassName, DCookReferenceFixture::StaticClass()->GetQualifiedName().ToString());
	}
}

TEST_F(FCookFunctionalTests, TargetPolicyStripsDescendantsAndReferencesWithoutChangingLiveObjects)
{
	const auto Fixture = Testing::CreateTestFixtureDirectory("CookTargetPolicy");
	Testing::FScopedMountRegistryFixture Mounts;
	Testing::RegisterMountPointForTests("/CookTests/", (Fixture / "Content").generic_string());
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/CookTests/TargetPolicy", Path));
	auto* Package = CreatePackage(Path);
	auto* Root = NewObject<DCookReferenceFixture>(Package, "Root");
	auto* Parent = NewObject<DCookReferenceFixture>(Package, "PlatformParent");
	Parent->bWin64Only = true;
	auto* Child = NewObject<DCookReferenceFixture>(Parent, "Child");
	Root->RuntimeReference = Child;
	for (const bool Retain : {false, true})
		for (const auto Target : {ECookTargetPlatform::Win64, ECookTargetPlatform::MacOS})
		{
			const bool Included = Target == ECookTargetPlatform::Win64;
			const auto Before = DCookReferenceFixture::Begins;
			FByteBuffer Bytes;
			const auto Saved = SerializeAssetPackageBytes(Package, Bytes,
				{.Domain = EAssetPackageSaveDomain::Cooked, .TargetPlatform = Target,
					.TargetProfile = ECookTargetProfile::Game, .bRetainEditorOnlyData = Retain});
			ASSERT_TRUE(Saved) << ObjectPackage::FormatPackageError(Saved);
			EXPECT_EQ(DCookReferenceFixture::Begins - Before, Included ? 3u : 1u);
			const auto File = Fixture / std::format("{}-{}.dasset", GetTargetPlatformName(Target), Retain);
			ASSERT_TRUE(FFileHelper::SaveArrayToFile(Bytes, File));
			FAssetPackageInspection Inspection;
			ASSERT_TRUE(InspectAssetPackage(File.generic_string(), Path, Inspection));
			EXPECT_EQ(Inspection.Objects.size(), Included ? 3u : 1u);
			const auto Record = std::ranges::find(Inspection.Objects, std::string("Root"), &FAssetPackageObjectInspection::ObjectPath);
			ASSERT_NE(Record, Inspection.Objects.end());
			const auto* Field = Record->FindField("RuntimeReference");
			ASSERT_NE(Field, nullptr);
			FAssetPackageObjectReference Reference;
			ASSERT_TRUE(Field->TryReadObjectReference(Reference));
			EXPECT_EQ(Reference.Kind == EAssetPackageObjectReferenceKind::Null, !Included);
			EXPECT_EQ(Root->RuntimeReference.Get(), Child);
		}
}

TEST_F(FCookFunctionalTests, PolicyStrippedReferencesSerializeNullBeforeReuse)
{
	const auto Fixture = Testing::CreateTestFixtureDirectory("SourceOnlyReferences");
	Testing::FScopedMountRegistryFixture Mounts;
	Testing::RegisterMountPointForTests("/CookTests/", (Fixture / "Content").generic_string());
	for (const bool Retain : {false, true}) for (const bool External : {false, true})
	{
		FPackagePath Path, SourcePath;
		ASSERT_TRUE(FPackagePath::TryCreate(std::format("/CookTests/References{}{}", Retain, External), Path));
		ASSERT_TRUE(FPackagePath::TryCreate(std::format("/CookTests/Source{}{}", Retain, External), SourcePath));
		auto* Package = CreatePackage(Path);
		auto* Root = NewObject<DCookReferenceFixture>(Package, "Root");
		auto* SourcePackage = External ? CreatePackage(SourcePath) : Package;
		auto* Source = NewObject<DMaterialFunction>(SourcePackage, "Source");
		Root->EditorReference = Source;
		if (External) ASSERT_TRUE(SavePackage(SourcePackage));
		ASSERT_TRUE(SavePackage(Package));
		ASSERT_TRUE(RefreshAssetRegistry(EAssetRegistryScanMode::FullValidation));
		FCookRequest Request{.OutputRoot = Fixture / std::format("Output{}{}", Retain, External), .TargetPlatform = ECookTargetPlatform::Win64,
			.TargetProfile = ECookTargetProfile::Game, .ExplicitRoots = {Path}, .bRetainEditorOnlyData = Retain};
		FCookRunResult Result;
		ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << FormatCookRunError(Result);
		ASSERT_EQ(Result.Packages.size(), 1u);
		ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << FormatCookRunError(Result);
		EXPECT_EQ(Result.Packages.front().Status, ECookPackageStatus::CookHit);
		const auto Before = Inventory(Request.OutputRoot);
		Root->RuntimeReference = Source;
		ASSERT_TRUE(SavePackage(Package));
		ASSERT_TRUE(RefreshAssetRegistry(EAssetRegistryScanMode::FullValidation));
		ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << FormatCookRunError(Result);
		EXPECT_EQ(Root->RuntimeReference.Get(), Source);
		FAssetPackageInspection Inspection;
		const auto File = Request.OutputRoot / (Path.ToString().substr(1) + ".dasset");
		ASSERT_TRUE(InspectAssetPackage(File.generic_string(), Path, Inspection));
		const auto RootRecord = std::ranges::find(Inspection.Objects, std::string("Root"), &FAssetPackageObjectInspection::ObjectPath);
		ASSERT_NE(RootRecord, Inspection.Objects.end());
		const auto* Field = RootRecord->FindField("RuntimeReference"); ASSERT_NE(Field, nullptr);
		FAssetPackageObjectReference Reference; ASSERT_TRUE(Field->TryReadObjectReference(Reference));
		EXPECT_EQ(Reference.Kind, EAssetPackageObjectReferenceKind::Null);
		FCookContext Direct(ECookTargetPlatform::Win64, ECookTargetProfile::Game, Retain);
		EXPECT_TRUE(Direct.AddPackage(Path.ToString(), Package));
	}
}

TEST_F(FCookFunctionalTests, SourceOnlyExplicitRootIsOmittedSuccessfully)
{
	const auto Fixture = Testing::CreateTestFixtureDirectory("SourceOnlyRoot");
	Testing::FScopedMountRegistryFixture Mounts;
	Testing::RegisterMountPointForTests("/CookTests/", (Fixture / "Content").generic_string());
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/CookTests/SourceOnly", Path));
	auto* Package = CreatePackage(Path);
	ASSERT_NE(NewObject<DMaterialFunction>(Package, "Source"), nullptr);
	ASSERT_TRUE(SavePackage(Package));
	ASSERT_TRUE(RefreshAssetRegistry(EAssetRegistryScanMode::FullValidation));
	FCookRequest Request{.OutputRoot = Fixture / "Output", .TargetPlatform = ECookTargetPlatform::Win64,
		.TargetProfile = ECookTargetProfile::Game, .ExplicitRoots = {Path}};
	FCookRunResult Result;
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << FormatCookRunError(Result);
	ASSERT_EQ(Result.Packages.size(), 1u); EXPECT_EQ(Result.Packages.front().Status, ECookPackageStatus::Omitted);
	EXPECT_TRUE(std::filesystem::exists(Request.OutputRoot / "CookManifest.bin"));
	FCookManifest Manifest; auto Bytes = FFileHelper::LoadFileToArray(Request.OutputRoot / "CookManifest.bin");
	ASSERT_TRUE(Bytes); ASSERT_TRUE(DecodeCookManifest(*Bytes, Manifest)); EXPECT_TRUE(Manifest.Entries.empty());
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)); EXPECT_EQ(Result.Packages.front().Status, ECookPackageStatus::Omitted);
	FCookContext Direct(ECookTargetPlatform::Win64, ECookTargetProfile::Game);
	const auto Saved = SaveCookPackage(Package, Path.GetView(), Direct);
	EXPECT_TRUE(Saved);
	EXPECT_TRUE(Direct.GetSavePlans().empty());
	FPackagePath RuntimePath;
	ASSERT_TRUE(FPackagePath::TryCreate("/CookTests/Runtime", RuntimePath));
	auto* RuntimePackage = CreatePackage(RuntimePath);
	NewObject<DCookReferenceFixture>(RuntimePackage, "Runtime");
	ASSERT_TRUE(SavePackage(RuntimePackage));
	ASSERT_TRUE(RefreshAssetRegistry(EAssetRegistryScanMode::FullValidation));
	Request.ExplicitRoots = {Path, RuntimePath};
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << FormatCookRunError(Result);
	ASSERT_EQ(Result.Packages.size(), 2u);
	EXPECT_EQ(Result.Packages.front().PackagePath, RuntimePath);
	EXPECT_EQ(Result.Packages.back().PackagePath, Path);
	EXPECT_EQ(Result.Packages.back().Status, ECookPackageStatus::Omitted);
}

TEST_F(FCookFunctionalTests, SecondaryClassSchemasControlReuseAndConflicts)
{
	const auto Fixture = Testing::CreateTestFixtureDirectory("SecondarySchemas");
	Testing::FScopedMountRegistryFixture Mounts;
	Testing::RegisterMountPointForTests("/CookTests/", (Fixture / "Content").generic_string());
	FPackagePath Path; ASSERT_TRUE(FPackagePath::TryCreate("/CookTests/SecondarySchemas", Path));
	auto* Package = CreatePackage(Path);
	ASSERT_NE(NewObject<DCookReferenceFixture>(Package, "First"), nullptr);
	ASSERT_NE(NewObject<DCookSecondaryFixture>(Package, "Source"), nullptr);
	ASSERT_TRUE(SavePackage(Package)); ASSERT_TRUE(UnloadPackage(Package));
	ASSERT_TRUE(RefreshAssetRegistry(EAssetRegistryScanMode::FullValidation));
	DCookReferenceFixture::SchemaHook = [](FObjectCookContext& Context) { Context.Dependencies.push_back({ECookBuildDependencyKind::ConfigurationValue,"shared-policy",{}, {std::byte{1}}}); };
	uint8 Version = 1, Policy = 1;
	DCookSecondaryFixture::SecondarySchema = [&](FObjectCookContext& Context) {
		Context.Dependencies.push_back({ECookBuildDependencyKind::SchemaProducerVersion,"secondary-version",{}, {std::byte{Version}}});
		Context.Dependencies.push_back({ECookBuildDependencyKind::ConfigurationValue,"shared-policy",{}, {std::byte{Policy}}});
	};
	FCookRequest Request{.OutputRoot = Fixture / "Output", .TargetPlatform = ECookTargetPlatform::Win64,
		.TargetProfile = ECookTargetProfile::Game, .ExplicitRoots = {Path}};
	FCookRunResult Result;
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << FormatCookRunError(Result);
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << FormatCookRunError(Result);
	ASSERT_EQ(Result.Packages.size(), 1u); EXPECT_EQ(Result.Packages.front().Status, ECookPackageStatus::CookHit);
	Version = 2;
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << FormatCookRunError(Result);
	EXPECT_EQ(Result.Packages.front().Status, ECookPackageStatus::Captured);
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << FormatCookRunError(Result);
	EXPECT_EQ(Result.Packages.front().Status, ECookPackageStatus::CookHit);
	const auto Before = Inventory(Request.OutputRoot); Policy = 2;
	EXPECT_FALSE(FCookCoordinator().Run(Request, Result)); ASSERT_TRUE(Result.InputFailure.GetDiagnostic());
	EXPECT_EQ(Result.InputFailure.GetDiagnostic()->Error, ECookInputError::DuplicateDeclaration);
	EXPECT_EQ(Inventory(Request.OutputRoot), Before);
	Policy = 1;
	DCookSecondaryFixture::SecondaryInstance = [](FObjectCookContext& Context) { Context.Dependencies.push_back({ECookBuildDependencyKind::ConfigurationValue,"instance-value",{}, {std::byte{1}}}); };
	// Invalidate the stored declaration-code identity before installing a new event behavior.
	Version = 3;
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << FormatCookRunError(Result);
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << FormatCookRunError(Result);
	EXPECT_EQ(Result.Packages.front().Status, ECookPackageStatus::CookHit);
}

TEST_F(FCookFunctionalTests, LoadedInstanceValuesCanHitButIncompleteInputsCannot)
{
	const auto Fixture = Testing::CreateTestFixtureDirectory("LoadedValueReuse");
	Testing::FScopedMountRegistryFixture Mounts;
	Testing::RegisterMountPointForTests("/CookTests/", (Fixture / "Content").generic_string());
	FPackagePath Path; ASSERT_TRUE(FPackagePath::TryCreate("/CookTests/LoadedValue", Path));
	auto* Package = CreatePackage(Path);
	ASSERT_NE(NewObject<DCookReferenceFixture>(Package, "Source"), nullptr);
	ASSERT_TRUE(SavePackage(Package)); ASSERT_TRUE(UnloadPackage(Package));
	ASSERT_TRUE(RefreshAssetRegistry(EAssetRegistryScanMode::FullValidation));
	uint8 Value = 1;
	uint32 Evaluations = 0;
	bool Complete = true;
	DCookReferenceFixture::InstanceHook = [&](FObjectCookContext& Context) {
		++Evaluations;
		Context.bDependenciesComplete = Complete;
		Context.Dependencies.push_back({ECookBuildDependencyKind::ConfigurationValue, "instance-value", {}, {std::byte{Value}}});
	};
	FCookRequest Request{.OutputRoot = Fixture / "Output", .TargetPlatform = ECookTargetPlatform::Win64,
		.TargetProfile = ECookTargetProfile::Game, .ExplicitRoots = {Path}};
	FCookRunResult Result;
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << FormatCookRunError(Result);
	EXPECT_EQ(DCookReferenceFixture::Begins, 1u);
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << FormatCookRunError(Result);
	EXPECT_EQ(Evaluations, 2u);
	EXPECT_EQ(Result.Packages.front().Status, ECookPackageStatus::CookHit);
	EXPECT_EQ(DCookReferenceFixture::Begins, 1u);
	Value = 2;
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << FormatCookRunError(Result);
	EXPECT_EQ(Result.Packages.front().Status, ECookPackageStatus::Captured);
	EXPECT_EQ(DCookReferenceFixture::Begins, 2u);
	Complete = false;
	for (uint32 Index = 0; Index < 2; ++Index) {
		ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << FormatCookRunError(Result);
		EXPECT_EQ(Result.Packages.front().Status, ECookPackageStatus::Captured);
	}
}

TEST_F(FCookFunctionalTests, QualifiedSchemasIgnoreUnrelatedInputsAndObserveConstructedClasses)
{
	const auto Fixture = Testing::CreateTestFixtureDirectory("ScopedCookVersions");
	Testing::FScopedMountRegistryFixture Mounts;
	Testing::RegisterMountPointForTests("/CookTests/", (Fixture / "Content").generic_string());
	FPackagePath Path; ASSERT_TRUE(FPackagePath::TryCreate("/CookTests/ScopedVersions", Path));
	auto* Package = CreatePackage(Path);
	ASSERT_NE(NewObject<DCookReferenceFixture>(Package, "Source"), nullptr);
	ASSERT_TRUE(SavePackage(Package)); ASSERT_TRUE(UnloadPackage(Package));
	ASSERT_TRUE(RefreshAssetRegistry(EAssetRegistryScanMode::FullValidation));
	auto Version = FGuid::NewGuid();
	ASSERT_TRUE(FCustomVersionRegistry::Register({Version, 7, "ScopedCookVersion"}));
	bool CompleteCoverage = true;
	DCookReferenceFixture::SchemaHook = [&](FObjectCookContext& Context) {
		if (CompleteCoverage) Context.VersionCoveredClasses.insert(DCookReferenceFixture::StaticClass()->GetQualifiedName().ToString());
		Context.UsedCustomVersions.insert(Version);
		Context.ConstructedClasses.insert(DCookSecondaryFixture::StaticClass()->GetQualifiedName().ToString());
	};
	uint8 ConstructedVersion = 1;
	DCookSecondaryFixture::SecondarySchema = [&](FObjectCookContext& Context) {
		Context.VersionCoveredClasses.insert(DCookSecondaryFixture::StaticClass()->GetQualifiedName().ToString());
		Context.Dependencies.push_back({ECookBuildDependencyKind::SchemaProducerVersion, "constructed-version", {}, {std::byte{ConstructedVersion}}});
	};
	FCookRequest Request{.OutputRoot = Fixture / "Output", .TargetPlatform = ECookTargetPlatform::Win64,
		.TargetProfile = ECookTargetProfile::Game, .ExplicitRoots = {Path}};
	FCookRunResult Result;
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << FormatCookRunError(Result);
	const auto Constructions = DCookReferenceFixture::Constructions;
	ASSERT_TRUE(FCustomVersionRegistry::Register({FGuid::NewGuid(), 1, "UnrelatedCookVersion"}));
	Request.MaterialConfigurations = {{EMaterialQualityLevel::Low, ERHIFeatureLevel::SM5}};
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << FormatCookRunError(Result);
	EXPECT_EQ(Result.Packages.front().Status, ECookPackageStatus::CookHit);
	EXPECT_EQ(DCookReferenceFixture::Constructions, Constructions);
	// A Cook-only declared version is current even though it was absent in the source file.
	Version = FGuid::NewGuid();
	ASSERT_TRUE(FCustomVersionRegistry::Register({Version, 8, "ScopedCookOnlyVersion"}));
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << FormatCookRunError(Result);
	EXPECT_EQ(Result.Packages.front().Status, ECookPackageStatus::Captured);
	ConstructedVersion = 2;
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << FormatCookRunError(Result);
	EXPECT_EQ(Result.Packages.front().Status, ECookPackageStatus::Captured);
	CompleteCoverage = false;
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << FormatCookRunError(Result);
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << FormatCookRunError(Result);
	EXPECT_EQ(Result.Packages.front().Status, ECookPackageStatus::CookHit);
	ASSERT_TRUE(FCustomVersionRegistry::Register({FGuid::NewGuid(), 1, "FallbackCookVersion"}));
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << FormatCookRunError(Result);
	EXPECT_EQ(Result.Packages.front().Status, ECookPackageStatus::Captured);
}

TEST_F(FCookFunctionalTests, IncompleteNativeInputsPropagateOnlyThroughTransitiveBuildEdges)
{
	const auto Fixture = Testing::CreateTestFixtureDirectory("CookIncompleteEdges");
	Testing::FScopedMountRegistryFixture Mounts;
	Testing::RegisterMountPointForTests("/CookTests/", (Fixture / "Content").generic_string());
	FPackagePath Root, Build;
	ASSERT_TRUE(FPackagePath::TryCreate("/CookTests/EdgeRoot", Root));
	ASSERT_TRUE(FPackagePath::TryCreate("/CookTests/EdgeBuild", Build));
	for (const auto& Path : {Root, Build}) {
		auto* Package = CreatePackage(Path); NewObject<DCookReferenceFixture>(Package, "Source");
		ASSERT_TRUE(SavePackage(Package)); ASSERT_TRUE(UnloadPackage(Package));
	}
	ASSERT_TRUE(RefreshAssetRegistry(EAssetRegistryScanMode::FullValidation));
	uint8 Policy = 1, Value = 1;
	bool Complete = false;
	DCookReferenceFixture::SchemaHook = [&](FObjectCookContext& Context) {
		Context.Dependencies.push_back({ECookBuildDependencyKind::SchemaProducerVersion, "edge-policy", {}, {std::byte{Policy}}});
	};
	DCookReferenceFixture::InstanceHook = [&](FObjectCookContext& Context) {
		if (Context.Package == Root.ToString()) Context.Dependencies.push_back({Policy == 1 ? ECookBuildDependencyKind::DirectPackage : ECookBuildDependencyKind::TransitivePackage, Build.ToString()});
		else {
			Context.bDependenciesComplete = Complete;
			Context.Dependencies.push_back({ECookBuildDependencyKind::ConfigurationValue, "build-value", {}, {std::byte{Value}}});
		}
	};
	FCookRequest Request{.OutputRoot = Fixture / "Output", .TargetPlatform = ECookTargetPlatform::Win64,
		.TargetProfile = ECookTargetProfile::Game, .ExplicitRoots = {Root}};
	FCookRunResult Result;
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << FormatCookRunError(Result);
	++Value;
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << FormatCookRunError(Result);
	ASSERT_EQ(Result.Packages.size(), 1u); EXPECT_EQ(Result.Packages.front().Status, ECookPackageStatus::CookHit);
	Policy = 2;
	for (uint32 Index = 0; Index < 2; ++Index) {
		ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << FormatCookRunError(Result);
		EXPECT_EQ(Result.Packages.front().Status, ECookPackageStatus::Captured);
	}
	Complete = true;
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << FormatCookRunError(Result);
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << FormatCookRunError(Result);
	EXPECT_EQ(Result.Packages.front().Status, ECookPackageStatus::CookHit);
	++Value;
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << FormatCookRunError(Result);
	EXPECT_EQ(Result.Packages.front().Status, ECookPackageStatus::Captured);
}

TEST_F(FCookFunctionalTests, CustomVersionsInvalidateStoredObjectDeclarations)
{
	const auto Fixture = Testing::CreateTestFixtureDirectory("CookVersionProjection");
	Testing::FScopedMountRegistryFixture Mounts;
	Testing::RegisterMountPointForTests("/CookTests/", (Fixture / "Content").generic_string());
	FPackagePath Path, Dependency;
	ASSERT_TRUE(FPackagePath::TryCreate("/CookTests/VersionProjection", Path));
	ASSERT_TRUE(FPackagePath::TryCreate("/CookTests/VersionInput", Dependency));
	auto* Package = CreatePackage(Path);
	ASSERT_NE(NewObject<DCookReferenceFixture>(Package, "Root"), nullptr);
	ASSERT_TRUE(SavePackage(Package));
	auto* Input = CreatePackage(Dependency);
	ASSERT_NE(NewObject<DObject>(Input, "Input"), nullptr);
	ASSERT_TRUE(SavePackage(Input));
	ASSERT_TRUE(RefreshAssetRegistry(EAssetRegistryScanMode::FullValidation));
	uint32 Events = 0;
	const auto VersionGuid = FGuid::NewGuid();
	DCookReferenceFixture::InstanceHook = [&](FObjectCookContext& Context) {
		++Events;
		if (std::ranges::any_of(FCustomVersionRegistry::GetAll(), [&](const auto& V) { return V.Guid == VersionGuid; }))
			Context.Dependencies.push_back({ECookBuildDependencyKind::TransitivePackage, Dependency.ToString()});
	};
	FCookRequest Request{.OutputRoot = Fixture / "Output", .TargetPlatform = ECookTargetPlatform::Win64,
		.TargetProfile = ECookTargetProfile::Game, .ExplicitRoots = {Path}};
	FCookRunResult Result;
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << FormatCookRunError(Result);
	EXPECT_EQ(Events, 1u);
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result));
	EXPECT_EQ(Events, 1u);
	ASSERT_TRUE(FCustomVersionRegistry::Register({VersionGuid, 1, "CookProjectionRegression"}));
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << FormatCookRunError(Result);
	EXPECT_EQ(Events, 2u);
	const auto StateBytes = FFileHelper::LoadFileToArray(Request.OutputRoot / "CookState.bin");
	ASSERT_TRUE(StateBytes);
	FCookState State;
	ASSERT_TRUE(DecodeCookState(*StateBytes, State));
	EXPECT_TRUE(std::ranges::any_of(State.Inputs, [&](const auto& P) { return P.VirtualPackagePath == Dependency.ToString(); }));
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result));
	EXPECT_EQ(Events, 2u);
	EXPECT_EQ(Result.Packages.front().Status, ECookPackageStatus::CookHit);
}

TEST_F(FCookFunctionalTests, ClassSchemasRejectPackageAndFileDependencyDeclarations)
{
	const auto Fixture = Testing::CreateTestFixtureDirectory("ClassSchemaBoundary");
	Testing::FScopedMountRegistryFixture Mounts;
	Testing::RegisterMountPointForTests("/CookTests/", (Fixture / "Content").generic_string());
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/CookTests/SchemaBoundary", Path));
	auto* Package = CreatePackage(Path);
	ASSERT_NE(NewObject<DCookReferenceFixture>(Package, "Root"), nullptr);
	ASSERT_TRUE(SavePackage(Package));
	ASSERT_TRUE(RefreshAssetRegistry(EAssetRegistryScanMode::FullValidation));
	FCookRequest Request{.OutputRoot = Fixture / "Output", .TargetPlatform = ECookTargetPlatform::Win64,
		.TargetProfile = ECookTargetProfile::Game, .ExplicitRoots = {Path}};
	for (const auto Kind : {ECookBuildDependencyKind::DirectPackage, ECookBuildDependencyKind::TransitivePackage, ECookBuildDependencyKind::ExternalFile})
	{
		DCookReferenceFixture::SchemaHook = [&](FObjectCookContext& Context) { Context.Dependencies.push_back({Kind, Path.ToString()}); };
		FCookRunResult Result;
		EXPECT_FALSE(FCookCoordinator().Run(Request, Result));
		EXPECT_EQ(Result.InputFailure.Status, ECookInputStatus::InvalidDependency);
		EXPECT_EQ(DCookReferenceFixture::Begins, 0u);
	}
	EXPECT_FALSE(std::filesystem::exists(Request.OutputRoot / "CookManifest.bin"));
}

TEST_F(FCookFunctionalTests, MixedCreationOrderPreservesCanonicalPackageBytes)
{
	const auto Fixture = Testing::CreateTestFixtureDirectory("MixedCreationOrder");
	Testing::FScopedMountRegistryFixture Mounts;
	Testing::RegisterMountPointForTests("/CookTests/", (Fixture / "Content").generic_string());
	std::vector<FCookSavePlan> Baseline;
	for (const bool Reverse : {false, true})
	{
		FPackagePath Path;
		ASSERT_TRUE(FPackagePath::TryCreate(std::format("/CookTests/Order{}", Reverse), Path));
		auto* Package = CreatePackage(Path);
		DMaterial* Material = nullptr;
		DMaterialInstance* Instance = nullptr;
		if (Reverse) {
			Instance = NewObject<DMaterialInstance>(Package, "Instance");
			Material = NewObject<DMaterial>(Package, "Material");
			ASSERT_NE(NewObject<DCookReferenceFixture>(Package, "Generic"), nullptr);
		} else {
			ASSERT_NE(NewObject<DCookReferenceFixture>(Package, "Generic"), nullptr);
			Material = NewObject<DMaterial>(Package, "Material");
			Instance = NewObject<DMaterialInstance>(Package, "Instance");
		}
		ASSERT_TRUE(Instance->SetParent(Material));
		FCookContext Context(ECookTargetPlatform::Win64, ECookTargetProfile::Game);
		Context.SetMaterialConfigurations(std::array{FMaterialCookConfiguration{EMaterialQualityLevel::High, ERHIFeatureLevel::SM5}});
		ASSERT_TRUE(SaveCookPackage(Package, "/Game/Equivalent", Context));
		std::vector<FCookSavePlan> Plans;
		ASSERT_TRUE(Context.TakeSavePlans(Plans));
		ASSERT_EQ(Plans.size(), 1u);
		if (!Reverse) Baseline = std::move(Plans);
		else {
			EXPECT_EQ(Plans.front().PackageBytes, Baseline.front().PackageBytes);
			EXPECT_EQ(Plans.front().BulkBytes, Baseline.front().BulkBytes);
		}
	}
}

TEST_F(FCookFunctionalTests, MixedPackageDeclaresEveryMaterialFamily)
{
	const auto Fixture = Testing::CreateTestFixtureDirectory("MixedMaterialPolicy");
	Testing::FScopedMountRegistryFixture Mounts;
	Testing::RegisterMountPointForTests("/CookTests/", (Fixture / "Content").generic_string());
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/CookTests/Mixed", Path));
	auto* Package = CreatePackage(Path);
	auto* Root = NewObject<DCookReferenceFixture>(Package, "AFirst");
	auto* Material = NewObject<DMaterial>(Package, "Material");
	ASSERT_NE(Material, nullptr);
	// Nested exports must also contribute recipe identity, even when stripped later.
	ASSERT_NE(NewObject<DMaterial>(Root, "NestedMaterial"), nullptr);
	auto* Instance = NewObject<DMaterialInstance>(Package, "Instance");
	ASSERT_TRUE(Instance->SetParent(Material));
	ASSERT_TRUE(SavePackage(Package));
	ASSERT_TRUE(UnloadPackage(Package));
	FPackagePath NestedPath;
	ASSERT_TRUE(FPackagePath::TryCreate("/CookTests/Nested", NestedPath));
	auto* NestedPackage = CreatePackage(NestedPath);
	auto* NestedRoot = NewObject<DCookReferenceFixture>(NestedPackage, "Nested");
	ASSERT_NE(NewObject<DMaterial>(NestedRoot, "OnlyNestedMaterial"), nullptr);
	ASSERT_TRUE(SavePackage(NestedPackage));
	ASSERT_TRUE(UnloadPackage(NestedPackage));
	ASSERT_TRUE(RefreshAssetRegistry(EAssetRegistryScanMode::FullValidation));
	FCookRequest Request{.OutputRoot = Fixture / "Output", .TargetPlatform = ECookTargetPlatform::Win64,
		.TargetProfile = ECookTargetProfile::Game, .ExplicitRoots = {Path, NestedPath},
		.MaterialConfigurations = {{EMaterialQualityLevel::High, ERHIFeatureLevel::SM5}}};
	FCookRunResult Result;
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << FormatCookRunError(Result);
	const auto First = Inventory(Request.OutputRoot / "CookTests");
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << FormatCookRunError(Result);
	ASSERT_EQ(Result.Packages.size(), 2u);
	for (const auto& Entry : Result.Packages)
	{
		EXPECT_EQ(Entry.Serializer, "engine-package");
		EXPECT_EQ(Entry.Status, ECookPackageStatus::CookHit);
	}
	Request.MaterialConfigurations = {{EMaterialQualityLevel::Low, ERHIFeatureLevel::SM5}};
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << FormatCookRunError(Result);
	for (const auto& Entry : Result.Packages) EXPECT_NE(Entry.Status, ECookPackageStatus::CookHit);
	EXPECT_NE(Inventory(Request.OutputRoot / "CookTests"), First);
	const auto Selected = Inventory(Request.OutputRoot / "CookTests");
	Request.IncrementalPolicy = ECookIncrementalPolicy::Disabled;
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << FormatCookRunError(Result);
	EXPECT_EQ(Inventory(Request.OutputRoot / "CookTests"), Selected);
}

TEST_F(FCookFunctionalTests, CookedOnlyPreparationSelectsAvailableVariantsWithoutProducers)
{
	Testing::FScopedRHIBackendOverride Backend("vulkan");
	const auto Fixture = Testing::CreateTestFixtureDirectory("CookedMaterialSelection");
	Testing::FScopedMountRegistryFixture Mounts;
	Testing::RegisterMountPointForTests("/CookTests/", (Fixture / "Content").generic_string());
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/CookTests/ResaveMaterial", Path));
	auto* Material = NewObject<DMaterial>(CreatePackage(Path), "ResaveMaterial");
	ASSERT_NE(Material, nullptr);
	const auto Saved = SavePackage(Material->GetPackage());
	ASSERT_TRUE(Saved) << Saved.Message;
	ASSERT_TRUE(UnloadPackage(Material->GetPackage()));
	ASSERT_TRUE(RefreshAssetRegistry(EAssetRegistryScanMode::FullValidation));
	const FMaterialCookConfiguration Low{EMaterialQualityLevel::Low, ERHIFeatureLevel::SM5};
	const FMaterialCookConfiguration High{EMaterialQualityLevel::High, ERHIFeatureLevel::SM5};
	FCookRequest Request{.OutputRoot = Fixture / "Output", .TargetPlatform = ECookTargetPlatform::Win64,
		.TargetProfile = ECookTargetProfile::Game, .ExplicitRoots = {Path}, .MaterialConfigurations = {Low, High}};
	FCookRunResult Result;
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << FormatCookRunError(Result);
	Testing::FScopedAssetRuntimeForTests Runtime;
	ASSERT_TRUE(Runtime.RestartCooked(Request.OutputRoot));
	Testing::RegisterMountPointForTests("/CookTests/", (Request.OutputRoot / "CookTests").generic_string(), true, false);
	ASSERT_TRUE(RefreshAssetRegistry(EAssetRegistryScanMode::FullValidation));
	const auto Loaded = LoadPackage(Path);
	ASSERT_TRUE(Loaded);
	Material = Cast<DMaterial>((*Loaded)->FindTopLevelAsset("ResaveMaterial"));
	ASSERT_NE(Material, nullptr);
	const auto Before = GetShaderBuildStats().Compilations;
	const auto Revision = Material->GetPackage()->GetEditRevision();
	EXPECT_FALSE(Material->PrepareCookedProgram(ECookTargetPlatform::Win64, {}));
	const std::array Invalid{High, High};
	EXPECT_FALSE(Material->PrepareCookedProgram(ECookTargetPlatform::Win64, Invalid));
	EXPECT_FALSE(Material->PrepareCookedProgram(ECookTargetPlatform::MacOS, std::span(&Low, 1)));
	auto Subset = Material->PrepareCookedProgram(ECookTargetPlatform::Win64, std::span(&Low, 1));
	ASSERT_TRUE(Subset) << Subset.Error;
	auto Read = Subset->AcquireRead();
	ASSERT_TRUE(Read);
	FMaterialStaticProperties Properties;
	std::shared_ptr<const FMaterialCompilerResult> Program;
	EXPECT_TRUE(DecodeMaterialCookedProgramFamily(Read->GetBytes(), ECookTargetPlatform::Win64,
		ECookTargetProfile::Game, Low.Quality, Low.FeatureLevel, {}, Properties, Program));
	EXPECT_FALSE(DecodeMaterialCookedProgramFamily(Read->GetBytes(), ECookTargetPlatform::Win64,
		ECookTargetProfile::Game, High.Quality, High.FeatureLevel, {}, Properties, Program));
	auto Same = Material->PrepareCookedProgram(ECookTargetPlatform::Win64, Request.MaterialConfigurations);
	ASSERT_TRUE(Same) << Same.Error;
	auto SameRead = Same->AcquireRead();
	auto Original = Material->GetCookedProgramData();
	auto OriginalRead = Original.AcquireRead();
	ASSERT_TRUE(SameRead);
	ASSERT_TRUE(OriginalRead);
	EXPECT_TRUE(std::ranges::equal(SameRead->GetBytes(), OriginalRead->GetBytes()));
	EXPECT_EQ(GetShaderBuildStats().Compilations, Before);
	EXPECT_EQ(Material->GetPackage()->GetEditRevision(), Revision);
}

TEST_F(FCookFunctionalTests, RetainsContributionCauseAndClearsItForNextRun)
{
	const auto Fixture = Testing::CreateTestFixtureDirectory("CookContributionCause");
	Testing::FScopedMountRegistryFixture Mounts;
	Testing::RegisterMountPointForTests("/CookTests/", (Fixture / "Content").generic_string());
	FPackagePath Path, Missing;
	ASSERT_TRUE(FPackagePath::TryCreate("/CookTests/Rejected", Path));
	ASSERT_TRUE(FPackagePath::TryCreate("/CookTests/Missing", Missing));
	auto* Package = CreatePackage(Path);
	ASSERT_NE(NewObject<DCookReferenceFixture>(Package, "Rejected"), nullptr);
	ASSERT_TRUE(SavePackage(Package));
	ASSERT_TRUE(UnloadPackage(Package));
	ASSERT_TRUE(RefreshAssetRegistry(EAssetRegistryScanMode::FullValidation));
	bool CancelContribution = false;
	DCookReferenceFixture::PrepareHook = [&](DObject& Object, std::string_view VirtualPath, FCookPreparationContext&) -> FCookContributionResult {
		if (CancelContribution) return {.Status = ECookContributionStatus::Canceled};
		return {.Status = ECookContributionStatus::Error, .Error = ECookContributionError::SourceMutation,
			.ObjectPath = Object.GetObjectPath(), .VirtualPath = std::string(VirtualPath)};
	};
	FCookRequest Request{.OutputRoot = Fixture / "Output", .TargetPlatform = ECookTargetPlatform::Win64,
		.TargetProfile = ECookTargetProfile::Game, .ExplicitRoots = {Path}};
	FCookRunResult Result;
	EXPECT_FALSE(FCookCoordinator().Run(Request, Result));
	ASSERT_TRUE(Result.ContributionCause);
	EXPECT_EQ(Result.ContributionCause->Error, ECookContributionError::SourceMutation);
	EXPECT_EQ(Result.ContributionPackage, Path);
	EXPECT_EQ(Result.ContributionProvider, "Tests::DCookReferenceFixture");

	EXPECT_FALSE(std::filesystem::exists(Request.OutputRoot / "CookManifest.bin"));
	const auto Retained = Result.ContributionCause;
	CancelContribution = true;
	Request.IsCancelled = [] { return false; };
	EXPECT_FALSE(FCookCoordinator().Run(Request, Result));
	EXPECT_EQ(Result.Status, ECookRunStatus::Cancelled);
	EXPECT_FALSE(Result.ContributionCause);
	Request.ExplicitRoots = {Missing};
	EXPECT_FALSE(FCookCoordinator().Run(Request, Result));
	EXPECT_FALSE(Result.ContributionCause);
	EXPECT_TRUE(Result.ContributionProvider.empty());
	EXPECT_EQ(Retained->Error, ECookContributionError::SourceMutation);
}

TEST_F(FCookFunctionalTests, PreparationCallbackUsesSingleCommonPublisher)
{
	const auto Fixture = Testing::CreateTestFixtureDirectory("CookCaptureCause");
	Testing::FScopedMountRegistryFixture Mounts;
	Testing::RegisterMountPointForTests("/CookTests/", (Fixture / "Content").generic_string());
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/CookTests/Capture", Path));
	auto* Package = CreatePackage(Path);
	ASSERT_NE(NewObject<DCookReferenceFixture>(Package, "Capture"), nullptr);
	ASSERT_TRUE(SavePackage(Package));
	ASSERT_TRUE(UnloadPackage(Package));
	ASSERT_TRUE(RefreshAssetRegistry(EAssetRegistryScanMode::FullValidation));
	uint32 Prepared = 0;
	DCookReferenceFixture::PrepareHook = [&](DObject&, std::string_view, FCookPreparationContext&) -> FCookContributionResult { ++Prepared; return {}; };
	FCookRequest Request{.OutputRoot = Fixture / "Output", .TargetPlatform = ECookTargetPlatform::Win64,
		.TargetProfile = ECookTargetProfile::Game, .ExplicitRoots = {Path}};
	FCookRunResult Result;
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << FormatCookRunError(Result);
	EXPECT_EQ(Prepared, 1u);
	ASSERT_EQ(Result.Packages.size(), 1u);
	EXPECT_TRUE(std::filesystem::exists(Request.OutputRoot / "CookManifest.bin"));
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << FormatCookRunError(Result);
	EXPECT_EQ(Prepared, 1u);
	EXPECT_EQ(Result.Packages.front().Status, ECookPackageStatus::CookHit);
}

TEST_F(FCookFunctionalTests, PreservesSourcesAndPriorOutputsOnFailure)
{
	const auto Fixture = Testing::CreateTestFixtureDirectory("CookFailures");
	const auto Content = Fixture / "Content";
	Testing::FScopedMountRegistryFixture Mounts;
	Testing::RegisterMountPointForTests("/CookTests/", Content.generic_string());
	FPackagePath Path, Missing;
	ASSERT_TRUE(FPackagePath::TryCreate("/CookTests/Saved", Path));
	ASSERT_TRUE(FPackagePath::TryCreate("/CookTests/Missing", Missing));
	auto* Package = CreatePackage(Path);
	ASSERT_NE(NewObject<DCookReferenceFixture>(Package, "Saved"), nullptr);
	ASSERT_TRUE(SavePackage(Package));
	ASSERT_TRUE(UnloadPackage(Package));
	ASSERT_TRUE(RefreshAssetRegistry(EAssetRegistryScanMode::FullValidation));
	auto Before = Inventory(Content);
	FCookRequest Request{.OutputRoot = Fixture / "Output", .TargetPlatform = ECookTargetPlatform::Win64,
		.TargetProfile = ECookTargetProfile::Game, .ExplicitRoots = {Path}};
	FCookRunResult Result;
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << Durin::FormatCookRunError(Result);
	EXPECT_TRUE(Result);
	EXPECT_EQ(Result.Error, ECookRunError::None);
	EXPECT_FALSE(Result.bDryRun);
	EXPECT_EQ(CookRunCodeName(Result), "succeeded");
	const auto Published = Inventory(Request.OutputRoot);
	EXPECT_EQ(Inventory(Content), Before);
	{
		auto LoadedValue = LoadPackage(Path);
		Package = LoadedValue.value_or(nullptr);
		ASSERT_TRUE(LoadedValue);
	}
	ASSERT_NE(NewObject<DCookReferenceFixture>(Package, "Added"), nullptr);
	ASSERT_TRUE(SavePackage(Package));
	ASSERT_TRUE(UnloadPackage(Package));
	Before = Inventory(Content);
	Request.IncrementalPolicy = ECookIncrementalPolicy::Disabled;
	bool bReached = false;
	EXPECT_FALSE(FCookCoordinator().Run(Request, Result, nullptr,
		[&](ECookOperationStage Stage, size_t, std::string& Error) {
			if (Stage != ECookOperationStage::CommitManifest) return false;
			bReached = true;
			Error = "Injected manifest publication failure";
			return true;
		}));
	EXPECT_TRUE(bReached);
	ASSERT_TRUE(Result.PublicationCause);
	EXPECT_EQ(Result.PublicationCause->Status, ECookPublishStatus::Failed);
	EXPECT_EQ(Result.PublicationCause->Error, ECookPublishError::Injected);
	ASSERT_TRUE(Result.PublicationCause->InjectionCause);
	EXPECT_EQ(Result.PublicationCause->InjectionCause->Stage, ECookOperationStage::CommitManifest);
	EXPECT_EQ(Result.PublicationCause->InjectionCause->ExternalDiagnostic, "Injected manifest publication failure");
	EXPECT_EQ(Inventory(Request.OutputRoot), Published);
	Request.ExplicitRoots = {Missing};
	EXPECT_FALSE(FCookCoordinator().Run(Request, Result));
	EXPECT_FALSE(Result.PublicationCause);
	EXPECT_EQ(Inventory(Request.OutputRoot), Published);
	Request.ExplicitRoots = {Path};
	Request.IsCancelled = [] { return true; };
	EXPECT_FALSE(FCookCoordinator().Run(Request, Result));
	EXPECT_EQ(Result.Status, ECookRunStatus::Cancelled);
	EXPECT_FALSE(Result);
	EXPECT_EQ(Result.Error, ECookRunError::Cancelled);
	EXPECT_EQ(Inventory(Request.OutputRoot), Published);
	Request.IsCancelled = {};
	EXPECT_FALSE(FCookCoordinator().Run(Request, Result, nullptr,
		[](ECookOperationStage Stage, size_t, std::string& Text) {
			if (Stage != ECookOperationStage::Load) return false;
			Text.assign(4096, 'x');
			return true;
		}));
	EXPECT_EQ(Result.Error, ECookRunError::LoadInjectedFailure);
	ASSERT_TRUE(Result.InjectionCause);
	EXPECT_EQ(Result.InjectionCause->Stage, ECookOperationStage::Load);
	EXPECT_EQ(Result.InjectionCause->Index, 0u);
	EXPECT_EQ(Result.InjectionCause->ExternalDiagnostic.size(), 2048u);
	EXPECT_EQ(FormatCookRunError(Result), std::string(2048, 'x'));
	EXPECT_EQ(Inventory(Request.OutputRoot), Published);
	Request.OutputRoot = Fixture / "DryRun";
	Request.bDryRun = true;
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << Durin::FormatCookRunError(Result);
	EXPECT_TRUE(Result);
	EXPECT_EQ(Result.Error, ECookRunError::None);
	EXPECT_TRUE(Result.bDryRun);
	EXPECT_FALSE(Result.InjectionCause);
	EXPECT_EQ(CookRunCodeName(Result), "dry-run");
	EXPECT_FALSE(std::filesystem::exists(Request.OutputRoot / "CookManifest.bin"));
	EXPECT_EQ(Inventory(Content), Before);
}

TEST_F(FCookFunctionalTests, CooksSavedFamiliesAndReusesValidatedOutputs)
{
	Durin::Testing::FScopedRHIBackendOverride Backend("vulkan");
	const auto Fixture = Testing::CreateTestFixtureDirectory("CookFamilies");
	struct FRestoreCacheRoot
	{
		std::string Previous = FPaths::DerivedDataCacheDir();
		~FRestoreCacheRoot() { FPaths::SetDerivedDataCacheDirForTests(Previous); }
	} RestoreCacheRoot;
	// This fixture starts with its own empty cache and then exercises warm reuse.
	FPaths::SetDerivedDataCacheDirForTests((Fixture / "DerivedDataCache").generic_string());
	const auto Source = Fixture / "Project";
	std::filesystem::create_directories(Source / "Content");
	Testing::FScopedMountRegistryFixture Mounts;
	Testing::RegisterMountPointForTests("/Game/", (Source / "Content").generic_string());
	std::vector<FPackagePath> Paths;
	auto Make = [&]<typename T>(std::string_view Name) -> T* {
		FPackagePath Path;
		EXPECT_TRUE(FPackagePath::TryCreate(std::string("/Game/") + std::string(Name), Path));
		Paths.push_back(Path);
		return NewObject<T>(CreatePackage(Path), FName(Name));
	};
	std::string Error;
	auto* Saved = Make.operator()<DObject>("Saved");
	ASSERT_TRUE(SavePackage(Saved->GetPackage()));
	auto ImageResult1 = Image::FImage::TryCreate({.Width = 4, .Height = 4,
		.Format = Image::ERawImageFormat::RGBA8}, FByteBuffer(4 * 4 * 4, std::byte{0xff}));
	ASSERT_TRUE(ImageResult1);
	auto PixelsImage = std::move(*ImageResult1);
	FTextureSource Pixels;
	ASSERT_TRUE(Pixels.Init2D(PixelsImage.GetView(), 4));
	auto* Texture = Make.operator()<DTexture2D>("Texture");
	Texture->SetSource(std::move(Pixels));
	ASSERT_TRUE(SavePackage(Texture->GetPackage()));
	FTextureCubeFaceImages Faces;
	for (auto& Face : Faces.Faces) Face = PixelsImage;
	Faces.SourceChannelCount = 4;
	auto* Cube = Make.operator()<DTextureCube>("Cube");
	auto PreparedCubeSource = Durin::PrepareTextureCubeSource(Faces);
	ASSERT_TRUE(PreparedCubeSource);
	Cube->SetSource(std::move(*PreparedCubeSource));
	Cube->SetBuildSettings(ETextureCubeSourceLayout::SixFaces, 4, 0, 4, 4, true);
	ASSERT_TRUE(SavePackage(Cube->GetPackage()));
	FVolumeTextureSourceData Voxels;
	Voxels.Width = Voxels.Height = Voxels.Depth = 4;
	ASSERT_TRUE(Voxels.Voxels.UpdatePayload(FByteBuffer(64, std::byte{0x7f})));
	auto* Volume = Make.operator()<DVolumeTexture>("Volume");
	auto PreparedVolumeSource = Durin::PrepareVolumeTextureSource(Voxels);
	ASSERT_TRUE(PreparedVolumeSource);
	Volume->SetSource(std::move(*PreparedVolumeSource));
	ASSERT_TRUE(SavePackage(Volume->GetPackage()));
	auto* Material = Make.operator()<DMaterial>("Material");
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	DMaterialInterface* Parent = Material;
	for (int Index = 0; Index < 3; ++Index)
	{
		auto* Instance = Make.operator()<DMaterialInstance>(std::format("Variant{}", Index));
		FMaterialPropertyOverrides Overrides;
		Overrides.bOverrideBlendMode = true;
		Overrides.bOverrideOpacityMaskThreshold = true;
		Overrides.Values.BlendMode = Index < 2 ? EMaterialBlendMode::Masked : EMaterialBlendMode::Translucent;
		Overrides.Values.OpacityMaskThreshold = Index == 0 ? 0.25f : 0.75f;
		ASSERT_TRUE(Instance->SetParentAndPropertyOverrides(Parent, Overrides));
		ASSERT_TRUE(SavePackage(Instance->GetPackage()));
		Parent = Instance;
	}

	auto* Mesh = Make.operator()<DStaticMesh>("Mesh");
	FMeshDescription Geometry;
	Geometry.PolygonGroups.push_back({"Material", 0, "Material"});
	auto& Triangle = Geometry.Sections.emplace_back();
	Triangle.Name = "Triangle";
	Triangle.Positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
	Triangle.Indices = {0, 1, 2};
	FStaticMeshSource MeshInput;
	ASSERT_TRUE(MeshInput.Initialize(std::move(Geometry)));
	*DStaticMesh::StaticClass()->FindPropertyByName("Source")
		->ContainerPtrToValuePtr<FStaticMeshSource>(Mesh) = std::move(MeshInput);
	*DStaticMesh::StaticClass()->FindPropertyByName("MaterialSlots")
		->ContainerPtrToValuePtr<std::vector<FMeshMaterialSlotDefinition>>(Mesh) = {{.Name = "Material", .DefaultMaterial = Material}};
	ASSERT_TRUE(SavePackage(Mesh->GetPackage()));
	auto* Environment = Make.operator()<DTextureCube>("Environment");
	std::vector<float> HdrPixels(8 * 4 * 4, 4.0f);
	const auto HdrBytes = std::as_bytes(std::span(HdrPixels));
	auto ImageResult2 = Image::FImage::TryCreate({.Width = 8, .Height = 4, .Format = Image::ERawImageFormat::RGBA32F,
		.GammaSpace = Image::EImageGammaSpace::Linear}, FByteBuffer(HdrBytes.begin(), HdrBytes.end()));
	ASSERT_TRUE(ImageResult2);
	auto HdrImage = std::move(*ImageResult2);
	auto HdrSource = PrepareTextureCubePanoramaSource(HdrImage.GetView(), 4, 0);
	ASSERT_TRUE(HdrSource);
	Environment->SetSource(std::move(*HdrSource));
	Environment->SetBuildSettings(ETextureCubeSourceLayout::EquirectangularPanorama, 4, 0, 8, 4, false, ETextureCubeOutput::HDR);
	ASSERT_TRUE(SavePackage(Environment->GetPackage()));
	FPackagePath AliasPath;
	ASSERT_TRUE(FPackagePath::TryCreate("/Game/Alias", AliasPath));
	DAssetRedirector* Alias = nullptr;
	ASSERT_TRUE(Testing::CreateAssetRedirectorForTests(AliasPath, Paths[1], Alias));
	ASSERT_TRUE(SavePackage(Alias->GetPackage()));
	const auto Before = Inventory(Source);
	const auto Output = Fixture / "Output";
	// Only generated fixtures are mounted; repository Engine content is outside this run.
	ASSERT_TRUE(UnloadPackage(Alias->GetPackage()));
	for (const auto& Path : std::views::reverse(Paths))
	{
		const auto Unloaded = UnloadPackage(Path);
		ASSERT_TRUE(Unloaded) << Path.GetView() << ": " << Unloaded.Message;
	}
	ASSERT_TRUE(RefreshAssetRegistry(EAssetRegistryScanMode::FullValidation));
	FCookRequest Request{.OutputRoot = Output, .TargetPlatform = ECookTargetPlatform::Win64,
		.TargetProfile = ECookTargetProfile::Game};
	for (const auto Name : {"Saved", "Alias", "Cube", "Volume", "Variant2", "Mesh", "Environment"})
	{
		FPackagePath Root;
		ASSERT_TRUE(FPackagePath::TryCreate(std::format("/Game/{}", Name), Root));
		Request.ExplicitRoots.push_back(Root);
	}
	FCookRunResult Result;
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << Durin::FormatCookRunError(Result);
	EXPECT_EQ(Inventory(Source), Before);
	const auto First = Inventory(Output / "Game");
	// A retired publisher identity must invalidate otherwise valid generated state.
	const auto LegacyBytes = FFileHelper::LoadFileToArray(Output / "CookState.bin");
	ASSERT_TRUE(LegacyBytes);
	FCookState LegacyState;
	ASSERT_TRUE(DecodeCookState(*LegacyBytes, LegacyState));
	for (auto& Entry : LegacyState.Entries) Entry.Serializer = "retired-package-publisher";
	FByteBuffer LegacyEncoded;
	ASSERT_TRUE(EncodeCookState(LegacyState, LegacyEncoded));
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(LegacyEncoded, Output / "CookState.bin"));
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << FormatCookRunError(Result);
	for (const auto& Package : Result.Packages) EXPECT_NE(Package.Status, ECookPackageStatus::CookHit);
	EXPECT_EQ(Inventory(Output / "Game"), First);
	const auto Compilations = GetShaderBuildStats().Compilations;
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << Durin::FormatCookRunError(Result);
	EXPECT_EQ(GetShaderBuildStats().Compilations, Compilations);
	ASSERT_EQ(Result.Packages.size(), Paths.size());
	for (const auto& Package : Result.Packages) EXPECT_EQ(Package.Status, ECookPackageStatus::CookHit);
	EXPECT_EQ(Inventory(Source), Before);
	EXPECT_EQ(Inventory(Output / "Game"), First);
	Request.MaterialConfigurations = {{EMaterialQualityLevel::High, ERHIFeatureLevel::SM5}};
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << FormatCookRunError(Result);
	for (const auto& Package : Result.Packages)
	{
		const auto Name = Package.PackagePath.GetView();
		if (Name == "/Game/Material" || Name == "/Game/Variant0" || Name == "/Game/Variant1" || Name == "/Game/Variant2" || Name == "/Game/Mesh")
			EXPECT_NE(Package.Status, ECookPackageStatus::CookHit);
		else EXPECT_EQ(Package.Status, ECookPackageStatus::CookHit);
	}
	// Keep the selected configuration for recipe invalidation and warm reuse;
	// restoring the default matrix here would only add another full material cook.
	const auto Configured = Inventory(Output / "Game");
	FByteBuffer StateBytes;
	auto StateBytesRead = FFileHelper::LoadFileToArray(Output / "CookState.bin");
	ASSERT_TRUE(StateBytesRead) << StateBytesRead.error().ToString();
	StateBytes = std::move(*StateBytesRead);
	FCookState State;
	ASSERT_TRUE(DecodeCookState(StateBytes, State));
	ASSERT_EQ(State.Entries.size(), Paths.size());
	for (const auto& Path : Paths)
		EXPECT_TRUE(std::ranges::any_of(State.Entries, [&](const auto& Entry) { return Entry.VirtualPackagePath == Path.GetView(); })) << Path.GetView();
	uint32 MaterialRecipes = 0;
	uint32 MeshRecipes = 0;
	for (auto& Entry : State.Entries)
	{
		EXPECT_EQ(Entry.Serializer, "engine-package");
		EXPECT_FALSE(Entry.BuildDependencies.empty());
		if (std::ranges::find(Entry.BuildDependencies, "recipe/static-mesh", &FCookBuildDependency::LogicalName) != Entry.BuildDependencies.end())
		{
			++MeshRecipes;
			const auto Recipe = std::ranges::find(Entry.BuildDependencies,
				"recipe/static-mesh", &FCookBuildDependency::LogicalName);
			ASSERT_NE(Recipe, Entry.BuildDependencies.end());
			// Render and physics each contribute one producer ID and four independent versions.
			ASSERT_EQ(Recipe->Value.size(), 48u);
			FBinaryReader Reader(Recipe->Value);
			uint64 BuilderVersion = 0;
			uint32 FunctionVersion = 0, ConstantsSchema = 0;
			ASSERT_TRUE(Reader.ReadU64(BuilderVersion) && Reader.ReadU32(FunctionVersion)
				&& Reader.ReadU32(ConstantsSchema));
			EXPECT_EQ(ConstantsSchema, 2u);
			// A prior adapter constants layout must recook even when algorithms stay unchanged.
			Recipe->Value[12] ^= std::byte{1};
		}
		if (std::ranges::find(Entry.BuildDependencies, "recipe/material", &FCookBuildDependency::LogicalName) != Entry.BuildDependencies.end())
		{
			++MaterialRecipes;
			const auto Recipe = std::ranges::find(Entry.BuildDependencies,
				"recipe/material", &FCookBuildDependency::LogicalName);
			ASSERT_NE(Recipe, Entry.BuildDependencies.end());
			EXPECT_EQ(Recipe->Kind, ECookBuildDependencyKind::SchemaProducerVersion);
			FBinaryWriter Writer;
			Writer.WriteU64(GetMaterialBuildVersion());
			Writer.WriteU32(GetMaterialCompileAdapterVersion());
			Writer.WriteU32(MIR::CurrentVersion);
			Writer.WriteU32(MaterialCookedProgramPayloadSchemaVersion);
			Writer.WriteU32(CurrentMaterialPassContractVersion);
			EXPECT_EQ(Recipe->Value, Writer.TakeBytes());
			// Simulate a prior Material producer in otherwise valid incremental state.
			Recipe->Value[7] ^= std::byte{1};
		}
	}
	ASSERT_EQ(MaterialRecipes, 5u); // Includes the mesh package's transitive material recipe.
	ASSERT_EQ(MeshRecipes, 1u);
	ASSERT_TRUE(EncodeCookState(State, StateBytes));
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(StateBytes, Output / "CookState.bin"));
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << Durin::FormatCookRunError(Result);
	for (const auto& Package : Result.Packages)
		if (std::ranges::any_of(State.Entries, [&](const auto& Entry) { return Entry.VirtualPackagePath == Package.PackagePath.ToString() && std::ranges::any_of(Entry.BuildDependencies, [](const auto& Input) { return Input.LogicalName == "recipe/material" || Input.LogicalName == "recipe/static-mesh"; }); }))
			EXPECT_NE(Package.Status, ECookPackageStatus::CookHit);
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << Durin::FormatCookRunError(Result);
	for (const auto& Package : Result.Packages) EXPECT_EQ(Package.Status, ECookPackageStatus::CookHit);
	EXPECT_EQ(Inventory(Output / "Game"), Configured);
	Request.IncrementalPolicy = ECookIncrementalPolicy::Disabled;
	const auto WarmCompilations = GetShaderBuildStats().Compilations;
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << Durin::FormatCookRunError(Result);
	EXPECT_EQ(GetShaderBuildStats().Compilations, WarmCompilations);
	EXPECT_EQ(Inventory(Output / "Game"), Configured);
	EXPECT_EQ(Inventory(Source), Before);
	Testing::FScopedAssetRuntimeForTests Runtime;
	ASSERT_TRUE(Runtime.RestartCooked(Output));
	ASSERT_TRUE(InitializeGameThreadDeferredExecutor());
	Testing::RegisterMountPointForTests("/Game/", (Output / "Game").generic_string(), true, false);
	ASSERT_TRUE(RefreshAssetRegistry(EAssetRegistryScanMode::FullValidation));
	for (const auto& Path : Paths)
	{
		DPackage* Loaded = nullptr;
		const auto Result = LoadPackage(Path);
		Loaded = Result.value_or(nullptr);
		ASSERT_TRUE(Result) << Path.GetView() << ": " << (Result ? std::string{} : Result.error().Message);
		EXPECT_NE(Loaded, nullptr);
		auto* Asset = Loaded->FindTopLevelAsset(Path.GetPackageName());
		ASSERT_NE(Asset, nullptr);
		if (auto* LoadedMaterial = Cast<DMaterialInterface>(Asset))
		{
			FAssetCompilingManager::Get().FinishCompilationForObject(*LoadedMaterial);
			const auto Program = LoadedMaterial->GetAcceptedCompiledProgram();
			ASSERT_TRUE(Program) << LoadedMaterial->GetMaterialCookDiagnostic();
			EXPECT_TRUE(LoadedMaterial->GetMaterialCompileStatus().IsCurrent());
			EXPECT_TRUE(Program->GetDebugData().IR.Nodes.empty());
			EXPECT_TRUE(Program->GetDebugData().GeneratedSource.empty());
			EXPECT_GT(LoadedMaterial->GetCookedProgramData().GetMetadata().LogicalSize, 0u);
			EXPECT_TRUE(LoadedMaterial->GetMaterialResource().HasValidGameThreadShaderMap());
		}

		if (Asset->IsA(DTexture::StaticClass()))
			EXPECT_TRUE(static_cast<DTexture*>(Asset)->FinishCachePlatformData());
		if (Asset->IsA(DStaticMesh::StaticClass()))
			EXPECT_TRUE(static_cast<DStaticMesh*>(Asset)->EnsureRenderDataLoadedBlocking());
		if (Path.GetView() == "/Game/Environment")
		{
			auto* Hdr = Cast<DTextureCube>(Asset);
			ASSERT_NE(Hdr, nullptr);
			EXPECT_EQ(Hdr->GetPixelFormat(), EPixelFormat::RGBA32_FLOAT);
			float Value = 0;
			std::memcpy(&Value, Hdr->GetPlatformData()->Faces[0].Mips[0].Pixels.data(), sizeof(float));
			EXPECT_FLOAT_EQ(Value, 4.0f);
		}
	}
	EXPECT_EQ(Inventory(Source), Before);
}
