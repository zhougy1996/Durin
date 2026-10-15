#include "NativeRHIBackendTestSupport.h"
#include "NativeAssetBuildTestSupport.h"
#include "Asset/Cook.h"
#include "Asset/AssetCompilingManager.h"
#include "AssetRegistry/Scan.h"
#include "Asset/Load.h"
#include "Asset/PackageSerialization.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/Object.h"
#include "DObject/Package.h"
#include "DObject/Class.h"
#include "DObject/DurinPropertyTypes.h"
#include "Asset/OfflinePreparation.h"
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
#include "StaticMesh/StaticMesh.h"
#include "Threading/Task.h"

#include <gtest/gtest.h>

namespace
{
	using namespace Durin;

	class DCookReferenceFixture : public DObject
	{
	public:
		explicit DCookReferenceFixture(const FObjectInitializer& Initializer = FObjectInitializer::Get()) : DObject(Initializer) {}
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
			static const DurinCodeGen::FClassParams Params = {&StaticClassNoRegister, "Tests::DCookReferenceFixture", "DCookReferenceFixture", Properties, std::size(Properties)};
			static DClass* Class = DurinCodeGen::ConstructDClass(Params);
			return Class;
		}
		TObjectPtr<DObject> RuntimeReference;
		TObjectPtr<DObject> EditorReference;
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

	// Owns contributor registration while each test supplies an isolated content mount.
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
		auto SetUp() -> void override
		{
			std::string Error;
			ASSERT_TRUE(RegisterEngineCookContributors(Handles)) << Error;
			const auto Generic = RegisterCookContributor(DObject::StaticClass(), {
				.Name = "generic-package",
				.Prepare = [](DObject& Object, std::string_view Path, FCookPreparationContext& Context) -> FCookContributionResult {
					std::string Error;

					return {};
				},
				.DeclareDependencies = [](const FCookDependencyRequest&, std::vector<FCookDependencyDeclaration>&) -> FCookContributionResult { return {}; }}).Handle;
			ASSERT_NE(Generic, 0u);
			Handles.push_back(Generic);
		}
		auto TearDown() -> void override
		{
			for (const auto Handle : Handles) UnregisterCookContributor(Handle);
		}
		FScopedOfflinePreparation Offline;
		std::vector<FCookContributorHandle> Handles;
	};
}

TEST_F(FCookFunctionalTests, DiscoveryAndPreparationDoNotCopyProviderCallbacks)
{
	const auto Fixture = Testing::CreateTestFixtureDirectory("CookCallbackCopies");
	Testing::FScopedMountRegistryFixture Mounts;
	Testing::RegisterMountPointForTests("/CookTests/", (Fixture / "Content").generic_string());
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/CookTests/CallbackCopies", Path));
	auto* Package = CreatePackage(Path);
	ASSERT_NE(NewObject<DObject>(Package, "First"), nullptr);
	ASSERT_NE(NewObject<DObject>(Package, "Second"), nullptr);
	ASSERT_TRUE(SavePackage(Package));
	ASSERT_TRUE(UnloadPackage(Package));
	ASSERT_TRUE(RefreshAssetRegistry(EAssetRegistryScanMode::FullValidation));
	UnregisterCookContributor(Handles.back());
	Handles.pop_back();
	struct FCopyProbe
	{
		int& Copies;
		int& Calls;
		FCopyProbe(int& InCopies, int& InCalls) : Copies(InCopies), Calls(InCalls) {}
		FCopyProbe(const FCopyProbe& Other) : Copies(Other.Copies), Calls(Other.Calls) { ++Copies; }
		auto operator()(DObject&, std::string_view, FCookPreparationContext&) const -> FCookContributionResult
		{
			++Calls;
			return {};
		}
		auto operator()(const FCookDependencyRequest&, std::vector<FCookDependencyDeclaration>&) const -> FCookContributionResult
		{
			++Calls;
			return {};
		}
	};
	int Copies = 0;
	int Preparations = 0;
	int Declarations = 0;
	const auto Registered = RegisterCookContributor(DObject::StaticClass(), {
		.Name = "copy-probe",
		.Prepare = FCopyProbe(Copies, Preparations),
		.DeclareDependencies = FCopyProbe(Copies, Declarations)});
	ASSERT_TRUE(Registered);
	Handles.push_back(Registered.Handle);
	const int Before = Copies;
	FCookRequest Request{.TargetPlatform = ECookTargetPlatform::Win64,
		.TargetProfile = ECookTargetProfile::Game, .ExplicitRoots = {Path}, .bDryRun = true};
	FCookRunResult Result;
	EXPECT_TRUE(FCookCoordinator().Run(Request, Result)) << FormatCookRunError(Result);
	EXPECT_EQ(Result.Status, ECookRunStatus::Succeeded);
	EXPECT_EQ(Copies, Before);
	EXPECT_EQ(Declarations, 1);
	EXPECT_EQ(Preparations, 2);
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
	EXPECT_NE(FormatCookPlanError(Rejected.Error).find("unsupported for the requested target"), std::string::npos);
}

TEST_F(FCookFunctionalTests, DeclarationFailuresRetainIdentityAndPathCause)
{
	const auto Fixture = Testing::CreateTestFixtureDirectory("CookDeclarationCause");
	Testing::FScopedMountRegistryFixture Mounts;
	Testing::RegisterMountPointForTests("/CookTests/", (Fixture / "Content").generic_string());
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/CookTests/Declarations", Path));
	auto* Package = CreatePackage(Path);
	ASSERT_NE(NewObject<DObject>(Package, "Declarations"), nullptr);
	ASSERT_TRUE(SavePackage(Package));
	ASSERT_TRUE(UnloadPackage(Package));
	ASSERT_TRUE(RefreshAssetRegistry(EAssetRegistryScanMode::FullValidation));
	UnregisterCookContributor(Handles.back());
	Handles.pop_back();
	std::vector<FCookDependencyDeclaration> Declarations;
	uint32 Contributions = 0;
	const auto Registered = RegisterCookContributor(DObject::StaticClass(), {
		.Name = "declaration-provider",
		.Prepare = [&](DObject&, std::string_view, FCookPreparationContext&) -> FCookContributionResult { ++Contributions; return {}; },
		.DeclareDependencies = [&](const FCookDependencyRequest&, std::vector<FCookDependencyDeclaration>& Out) -> FCookContributionResult { Out = Declarations; return {}; }});
	ASSERT_TRUE(Registered);
	Handles.push_back(Registered.Handle);
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
	ASSERT_NE(NewObject<DObject>(Package, "Frozen"), nullptr);
	ASSERT_TRUE(SavePackage(Package));
	ASSERT_TRUE(UnloadPackage(Package));
	ASSERT_TRUE(RefreshAssetRegistry(EAssetRegistryScanMode::FullValidation));
	UnregisterCookContributor(Handles.back());
	Handles.pop_back();
	const FMaterialCookConfiguration Selected{EMaterialQualityLevel::High, ERHIFeatureLevel::SM5};
	std::vector<FMaterialCookConfiguration> Observed;
	const auto Registered = RegisterCookContributor(DObject::StaticClass(), {
		.Name = "frozen-policy",
		.Prepare = [](DObject& Object, std::string_view Path, FCookPreparationContext& Context) -> FCookContributionResult {
			EXPECT_EQ(Context.GetTargetPlatform(), ECookTargetPlatform::Win64);
			return {};
		},
		.DeclareDependencies = [&](const FCookDependencyRequest& Captured, std::vector<FCookDependencyDeclaration>&) -> FCookContributionResult {
			EXPECT_EQ(Captured.TargetPlatform, ECookTargetPlatform::Win64);
			Observed.assign(Captured.MaterialConfigurations.begin(), Captured.MaterialConfigurations.end());
			return {};
		}});
	ASSERT_TRUE(Registered);
	Handles.push_back(Registered.Handle);
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
	ASSERT_TRUE(Prepared) << Prepared.error();
	EXPECT_EQ(Material->GetAcceptedCompiledProgram(), Accepted);
	EXPECT_EQ(Material->GetPackage()->GetEditRevision(), Revision);
	const auto BeforeSerialization = GetShaderBuildStats().Compilations;
	FByteBuffer UnpreparedBytes;
	FCanonicalMemoryWriter UnpreparedWriter(UnpreparedBytes, EArchivePurpose::CookedPayload,
		{.bCooking = true, .Target = {"Win64", "Game"}});
	Material->SerializeCooked(UnpreparedWriter);
	ASSERT_TRUE(UnpreparedWriter.IsError());
	EXPECT_NE(UnpreparedWriter.GetFailure()->Message.find("prepared Cook data"), std::string::npos);
	EXPECT_TRUE(UnpreparedBytes.empty());
	EXPECT_EQ(GetShaderBuildStats().Compilations, BeforeSerialization);
	auto Read = Prepared->AcquireRead();
	ASSERT_TRUE(Read);
	FMaterialStaticProperties Properties;
	std::shared_ptr<const FMaterialCompilerResult> Program;
	EXPECT_TRUE(DecodeMaterialCookedProgramFamily(Read.Lock.GetBytes(), ECookTargetPlatform::Win64,
		ECookTargetProfile::Game, EMaterialQualityLevel::High, ERHIFeatureLevel::SM5, {}, Properties, Program));
	EXPECT_FALSE(DecodeMaterialCookedProgramFamily(Read.Lock.GetBytes(), ECookTargetPlatform::Win64,
		ECookTargetProfile::Game, EMaterialQualityLevel::Low, ERHIFeatureLevel::SM5, {}, Properties, Program));
	FAssetBuildTaskContext Cancelled;
	Cancelled.Cancellation.Cancel();
	EXPECT_FALSE(Material->PrepareCookedProgram(ECookTargetPlatform::Win64, Configurations, Cancelled));
}

TEST_F(FCookFunctionalTests, PreparationOmitsTransientAssetSubtrees)
{
	const auto Fixture = Testing::CreateTestFixtureDirectory("TransientPreparation");
	Testing::FScopedMountRegistryFixture Mounts;
	Testing::RegisterMountPointForTests("/CookTests/", (Fixture / "Content").generic_string());
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/CookTests/TransientPreparation", Path));
	auto* Package = CreatePackage(Path);
	auto* Root = NewObject<DObject>(Package, "Root");
	ASSERT_NE(NewObject<DStaticMesh>(Root, "InvalidTransientMesh", EObjectFlags::Transient), nullptr);
	auto* Temporary = NewObject<DObject>(Root, "Temporary", EObjectFlags::Transient);
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
	ASSERT_TRUE(SubmitTexture2DCompilation(*Texture,
		{.Build = std::move(*Build), .ResultApplication = {.bMarkPackageDirty = false}},
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
		ASSERT_NE(NewObject<DObject>(Package, SourceFirst ? "ZLast" : "AFirst"), nullptr);
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
		EXPECT_EQ(Inspection.Objects.front().ClassName, DObject::StaticClass()->GetQualifiedName().ToString());
	}
}

TEST_F(FCookFunctionalTests, SourceOnlyReferenceAdmissionPrecedesReuse)
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
		EXPECT_FALSE(FCookCoordinator().Run(Request, Result));
		EXPECT_NE(FormatCookRunError(Result).find("RuntimeReference"), std::string::npos);
		EXPECT_EQ(Inventory(Request.OutputRoot), Before);
		FCookContext Direct(ECookTargetPlatform::Win64, ECookTargetProfile::Game, Retain);
		EXPECT_FALSE(Direct.AddPackage(Path.ToString(), Package));
	}
}

TEST_F(FCookFunctionalTests, SourceOnlyExplicitRootFailsWithoutPublishing)
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
	EXPECT_FALSE(FCookCoordinator().Run(Request, Result));
	ASSERT_TRUE(Result.InputFailure.GetDiagnostic());
	EXPECT_EQ(Result.InputFailure.GetDiagnostic()->Error, ECookInputError::EmptyRuntimeRoot);
	EXPECT_FALSE(std::filesystem::exists(Request.OutputRoot / "CookManifest.bin"));
	FCookContext Direct(ECookTargetPlatform::Win64, ECookTargetProfile::Game);
	const auto Saved = SaveCookPackage(Package, Path.GetView(), Direct);
	EXPECT_EQ(Saved.Error, ECookContributionError::EmptyRuntimeRoot);
}

TEST_F(FCookFunctionalTests, SecondaryFamilyDeclarationsControlReuseAndConflicts)
{
	const auto Fixture = Testing::CreateTestFixtureDirectory("SecondaryDeclarations");
	Testing::FScopedMountRegistryFixture Mounts;
	Testing::RegisterMountPointForTests("/CookTests/", (Fixture / "Content").generic_string());
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/CookTests/SecondaryDeclarations", Path));
	auto* Package = CreatePackage(Path);
	ASSERT_NE(NewObject<DObject>(Package, "AFirst"), nullptr);
	ASSERT_NE(NewObject<DMaterialFunction>(Package, "Source"), nullptr);
	ASSERT_TRUE(SavePackage(Package));
	ASSERT_TRUE(UnloadPackage(Package));
	ASSERT_TRUE(RefreshAssetRegistry(EAssetRegistryScanMode::FullValidation));
	for (const auto Handle : Handles) UnregisterCookContributor(Handle);
	Handles.clear();
	const FCookContributor Capture = [](DObject& Object, std::string_view Path,
		FCookPreparationContext& Context) -> FCookContributionResult {
		return {};
	};
	const auto Declare = [](const FCookDependencyRequest&, std::vector<FCookDependencyDeclaration>& Out)
		-> FCookContributionResult {
		Out.push_back({ECookBuildDependencyKind::ConfigurationValue, "shared-policy", {}, {std::byte{1}}});
		return {};
	};
	const auto Generic = RegisterCookContributor(DObject::StaticClass(),
		{"generic-package", 1, Capture, {}, Declare});
	ASSERT_TRUE(Generic);
	Handles.push_back(Generic.Handle);
	auto Secondary = RegisterCookContributor(DMaterialFunction::StaticClass(),
		{"secondary-source", 1, Capture});
	ASSERT_TRUE(Secondary);
	Handles.push_back(Secondary.Handle);
	FCookRequest Request{.OutputRoot = Fixture / "Output", .TargetPlatform = ECookTargetPlatform::Win64,
		.TargetProfile = ECookTargetProfile::Game, .ExplicitRoots = {Path}};
	FCookRunResult Result;
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << FormatCookRunError(Result);
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << FormatCookRunError(Result);
	ASSERT_EQ(Result.Packages.size(), 1u);
	EXPECT_NE(Result.Packages.front().Status, ECookPackageStatus::CookHit);
	UnregisterCookContributor(Secondary.Handle);
	Handles.pop_back();
	Secondary = RegisterCookContributor(DMaterialFunction::StaticClass(),
		{"secondary-source", 1, Capture, {}, Declare});
	ASSERT_TRUE(Secondary);
	Handles.push_back(Secondary.Handle);
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << FormatCookRunError(Result);
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << FormatCookRunError(Result);
	EXPECT_EQ(Result.Packages.front().Status, ECookPackageStatus::CookHit);
	const auto Before = Inventory(Request.OutputRoot);
	UnregisterCookContributor(Secondary.Handle);
	Handles.pop_back();
	Secondary = RegisterCookContributor(DMaterialFunction::StaticClass(),
		{"secondary-source", 1, Capture, {},
			[](const FCookDependencyRequest&, std::vector<FCookDependencyDeclaration>& Out) -> FCookContributionResult {
				Out.push_back({ECookBuildDependencyKind::ConfigurationValue, "shared-policy", {}, {std::byte{2}}});
				return {};
			}});
	ASSERT_TRUE(Secondary);
	Handles.push_back(Secondary.Handle);
	EXPECT_FALSE(FCookCoordinator().Run(Request, Result));
	ASSERT_TRUE(Result.InputFailure.GetDiagnostic());
	EXPECT_EQ(Result.InputFailure.GetDiagnostic()->Error, ECookInputError::DuplicateDeclaration);
	EXPECT_EQ(Inventory(Request.OutputRoot), Before);
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
			ASSERT_NE(NewObject<DObject>(Package, "Generic"), nullptr);
		} else {
			ASSERT_NE(NewObject<DObject>(Package, "Generic"), nullptr);
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
	auto* Root = NewObject<DObject>(Package, "AFirst");
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
	auto* NestedRoot = NewObject<DObject>(NestedPackage, "Nested");
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
		EXPECT_EQ(Entry.Contributor, "engine-package");
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
	ASSERT_TRUE(Subset) << Subset.error();
	auto Read = Subset->AcquireRead();
	ASSERT_TRUE(Read);
	FMaterialStaticProperties Properties;
	std::shared_ptr<const FMaterialCompilerResult> Program;
	EXPECT_TRUE(DecodeMaterialCookedProgramFamily(Read.Lock.GetBytes(), ECookTargetPlatform::Win64,
		ECookTargetProfile::Game, Low.Quality, Low.FeatureLevel, {}, Properties, Program));
	EXPECT_FALSE(DecodeMaterialCookedProgramFamily(Read.Lock.GetBytes(), ECookTargetPlatform::Win64,
		ECookTargetProfile::Game, High.Quality, High.FeatureLevel, {}, Properties, Program));
	auto Same = Material->PrepareCookedProgram(ECookTargetPlatform::Win64, Request.MaterialConfigurations);
	ASSERT_TRUE(Same) << Same.error();
	auto SameRead = Same->AcquireRead();
	auto Original = Material->GetCookedProgramData();
	auto OriginalRead = Original.AcquireRead();
	ASSERT_TRUE(SameRead);
	ASSERT_TRUE(OriginalRead);
	EXPECT_TRUE(std::ranges::equal(SameRead.Lock.GetBytes(), OriginalRead.Lock.GetBytes()));
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
	ASSERT_NE(NewObject<DObject>(Package, "Rejected"), nullptr);
	ASSERT_TRUE(SavePackage(Package));
	ASSERT_TRUE(UnloadPackage(Package));
	ASSERT_TRUE(RefreshAssetRegistry(EAssetRegistryScanMode::FullValidation));
	UnregisterCookContributor(Handles.back());
	Handles.pop_back();
	const auto Registered = RegisterCookContributor(DObject::StaticClass(), {
		.Name = "rejecting-provider",
		.Prepare = [](DObject& Object, std::string_view VirtualPath, FCookPreparationContext&) -> FCookContributionResult {
			return FCookContributionResult{.Error = ECookContributionError::SourceMutation,
				.ObjectPath = Object.GetObjectPath(), .VirtualPath = std::string(VirtualPath)};
		},
		.DeclareDependencies = [](const FCookDependencyRequest&, std::vector<FCookDependencyDeclaration>&) -> FCookContributionResult { return {}; }});
	ASSERT_TRUE(Registered);
	Handles.push_back(Registered.Handle);
	FCookRequest Request{.OutputRoot = Fixture / "Output", .TargetPlatform = ECookTargetPlatform::Win64,
		.TargetProfile = ECookTargetProfile::Game, .ExplicitRoots = {Path}};
	FCookRunResult Result;
	EXPECT_FALSE(FCookCoordinator().Run(Request, Result));
	ASSERT_TRUE(Result.ContributionCause);
	EXPECT_EQ(Result.ContributionCause->Error, ECookContributionError::SourceMutation);
	EXPECT_EQ(Result.ContributionPackage, Path);
	EXPECT_EQ(Result.ContributionProvider, "rejecting-provider");

	EXPECT_FALSE(std::filesystem::exists(Request.OutputRoot / "CookManifest.bin"));
	const auto Retained = Result.ContributionCause;
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
	ASSERT_NE(NewObject<DObject>(Package, "Capture"), nullptr);
	ASSERT_TRUE(SavePackage(Package));
	ASSERT_TRUE(UnloadPackage(Package));
	ASSERT_TRUE(RefreshAssetRegistry(EAssetRegistryScanMode::FullValidation));
	UnregisterCookContributor(Handles.back());
	Handles.pop_back();
	uint32 Prepared = 0;
	const auto Registered = RegisterCookContributor(DObject::StaticClass(), {
		.Name = "capture-provider",
		.Prepare = [&](DObject&, std::string_view, FCookPreparationContext&) -> FCookContributionResult { ++Prepared; return {}; },
		.DeclareDependencies = [](const FCookDependencyRequest&, std::vector<FCookDependencyDeclaration>&) -> FCookContributionResult { return {}; }});
	ASSERT_TRUE(Registered);
	Handles.push_back(Registered.Handle);
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
	ASSERT_NE(NewObject<DObject>(Package, "Saved"), nullptr);
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
	ASSERT_NE(NewObject<DObject>(Package, "Added"), nullptr);
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
	for (auto& Entry : LegacyState.Entries) Entry.Contributor = "retired-package-publisher";
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
	Request.MaterialConfigurations.clear();
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << FormatCookRunError(Result);
	EXPECT_EQ(Inventory(Output / "Game"), First);
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
		EXPECT_EQ(Entry.Contributor, "engine-package");
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
	EXPECT_EQ(Inventory(Output / "Game"), First);
	Request.IncrementalPolicy = ECookIncrementalPolicy::Disabled;
	const auto WarmCompilations = GetShaderBuildStats().Compilations;
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << Durin::FormatCookRunError(Result);
	EXPECT_EQ(GetShaderBuildStats().Compilations, WarmCompilations);
	EXPECT_EQ(Inventory(Output / "Game"), First);
	EXPECT_EQ(Inventory(Source), Before);
	Testing::FScopedAssetRuntimeForTests Runtime;
	ASSERT_TRUE(Runtime.RestartCooked(Output));
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
			const auto Program = LoadedMaterial->GetAcceptedCompiledProgram();
			ASSERT_TRUE(Program) << Durin::FormatMaterialError(LoadedMaterial->GetMaterialCookDiagnostic());
			EXPECT_TRUE(LoadedMaterial->GetMaterialCompileStatus().IsCurrent());
			EXPECT_TRUE(Program->IR.Nodes.empty());
			EXPECT_TRUE(Program->GeneratedSource.empty());
			EXPECT_GT(LoadedMaterial->GetCookedProgramData().GetMetadata().LogicalSize, 0u);
			EXPECT_FALSE(LoadedMaterial->GetRenderData().Representation.IsError());
		}

		if (Asset->IsA(DTexture::StaticClass()))
			EXPECT_TRUE(static_cast<DTexture*>(Asset)->EnsurePlatformDataLoadedBlocking());
		if (Asset->IsA(DStaticMesh::StaticClass()))
			EXPECT_TRUE(static_cast<DStaticMesh*>(Asset)->EnsureRenderDataLoadedBlocking());
		if (Path.GetView() == "/Game/Environment")
		{
			auto* Hdr = Cast<DTextureCube>(Asset);
			ASSERT_NE(Hdr, nullptr);
			EXPECT_EQ(Hdr->GetBuiltPixelFormat(), EPixelFormat::RGBA32_FLOAT);
			float Value = 0;
			std::memcpy(&Value, Hdr->GetPlatformData()->Faces[0].Mips[0].Pixels.data(), sizeof(float));
			EXPECT_FLOAT_EQ(Value, 4.0f);
		}
	}
	EXPECT_EQ(Inventory(Source), Before);
}
