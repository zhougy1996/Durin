#include "NativeRHIBackendTestSupport.h"
#include "Materials/MaterialShaderMap.h"
#include "MaterialFunctionTestSupport.h"
#include "MaterialCookedProgramTestSupport.h"

namespace
{
	class FMaterialFunctionCookTests : public testing::Test
	{
	protected:
		auto SetUp() -> void override
		{
			InitializeDObjectSystem();
			// Full cook emits the fixed shader library as well as material programs.
			// Register its renderer-owned inventory independently of other test suites.
			ASSERT_EQ(Durin::GetRenderCommandAdmissionState(), Durin::ERenderCommandAdmissionState::Stopped);
			Durin::InitRenderingThread();
			RendererLifecycle.Start(RendererModule);
		}

		auto TearDown() -> void override
		{
			RendererLifecycle.Shutdown();
			Durin::FlushRenderingCommands();
			Durin::ShutdownRenderingThread();
		}

		Durin::FRendererModule RendererModule;
		Durin::FModuleTestHarness RendererLifecycle{"MaterialCookRenderer"};
	};
}

TEST_F(FMaterialFunctionCookTests, CookFingerprintsNestedFunctionsWithoutProducingRuntimeFunctionPackages)
{
	Durin::Testing::FScopedRHIBackendOverride Backend("vulkan");
	using namespace Durin;
	InitializeDObjectSystem();
	FScopedOfflinePreparation Offline;
	const auto Root = Testing::CreateTestFixtureDirectory("CookFunctionAssets");
	std::filesystem::create_directories(Root / "Content");
	const std::array Mounts{FMountPoint{.VirtualRoot = "/CookFunctionTests/", .Owner = EMountOwner::Test,
		.Root = Root / "Content", .bAutoScan = true, .bContentWritable = true}};
	Testing::FScopedMountRegistryFixture Registry(Mounts);
	ASSERT_TRUE(Registry.IsValid()) << Registry.GetError();
	ASSERT_TRUE(RefreshAssetRegistry());
	std::string Error;
	FPackagePath MaterialPath, WrapperPath, LeafPath;
	ASSERT_TRUE(FPackagePath::TryCreate("/CookFunctionTests/Material", MaterialPath));
	ASSERT_TRUE(FPackagePath::TryCreate("/CookFunctionTests/Wrapper", WrapperPath));
	ASSERT_TRUE(FPackagePath::TryCreate("/CookFunctionTests/Leaf", LeafPath));
	DMaterial* Material = nullptr;
	DMaterialFunction* Wrapper = nullptr;
	DMaterialFunction* Leaf = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(MaterialPath, Material));
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(WrapperPath, Wrapper));
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(LeafPath, Leaf));
	ASSERT_NO_FATAL_FAILURE(AddFunctionCall(*Wrapper, *Leaf));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	const auto Output = Wrapper->GetFunctionSignature().Outputs[0];
	const FGuid CallId{63, 1, 1, 1};
	ASSERT_TRUE(PublishRootFunction(*Material, *Wrapper, CallId));
	ASSERT_TRUE(Material->CompileEdits());
	ASSERT_TRUE(SavePackage(Leaf->GetPackage()));
	ASSERT_TRUE(SavePackage(Wrapper->GetPackage()));
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	FCookRequest Request{.OutputRoot = Root / "Cooked", .TargetPlatform = ECookTargetPlatform::Win64,
		.TargetProfile = ECookTargetProfile::Game, .ExplicitRoots = {MaterialPath}};
	FCookRunResult Result;
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << Durin::CookRunCodeName(Result) << ": " << Durin::FormatCookRunError(Result);
	ASSERT_EQ(Result.Packages.size(), 1u);
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << Durin::FormatCookRunError(Result);
	ASSERT_EQ(Result.Packages.size(), 1u);
	EXPECT_EQ(Result.Packages[0].Status, ECookPackageStatus::CookHit);
	auto Edited = CaptureFunctionExpressions(*Leaf);
	Edited.Signature.Inputs[0].Default.Surface.RoughnessDefault.X = 0.27f;
	ASSERT_TRUE(Edited.Apply(*Leaf));
	ASSERT_TRUE(SavePackage(Leaf->GetPackage()));
	ASSERT_TRUE(Material->CompileEdits());
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << Durin::FormatCookRunError(Result);
	ASSERT_EQ(Result.Packages.size(), 1u);
	EXPECT_NE(Result.Packages[0].Status, ECookPackageStatus::CookHit);
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << Durin::FormatCookRunError(Result);
	EXPECT_EQ(Result.Packages[0].Status, ECookPackageStatus::CookHit);
	Edited.Expressions.clear();
	const auto ExpectedIdentity = Durin::Testing::ExpectedGameMaterialIdentity(*Material);
	ASSERT_TRUE(UnloadPackage(MaterialPath));
	ASSERT_TRUE(UnloadPackage(WrapperPath));
	ASSERT_TRUE(UnloadPackage(LeafPath));
	CollectGarbage();
	const auto LeafData = FindAssetExact(LeafPath);
	ASSERT_TRUE(LeafData);
	const std::filesystem::path LeafFile = LeafData->PhysicalPath;
	const auto HiddenLeafFile = LeafFile.string() + ".unavailable";
	std::filesystem::rename(LeafFile, HiddenLeafFile);
	const auto MissingDependency = FCookCoordinator().Run(Request, Result);
	std::filesystem::rename(HiddenLeafFile, LeafFile);
	EXPECT_FALSE(MissingDependency) << "A warm Cook hit must still admit every function source.";
	ShutdownAssetManager();
	auto Configuration = FAssetRuntimeConfiguration::Authored();
	ASSERT_TRUE(FAssetRuntimeConfiguration::Cooked(Request.OutputRoot, Configuration));
	ASSERT_TRUE(InitializeAssetManager(std::move(Configuration)));
	{
		const std::array CookMounts{FMountPoint{.VirtualRoot = "/CookFunctionTests/", .Owner = EMountOwner::Test,
			.Root = Request.OutputRoot / "CookFunctionTests", .bAutoScan = true}};
		Testing::FScopedMountRegistryFixture CookMountsFixture(CookMounts);
		ASSERT_TRUE(CookMountsFixture.IsValid()) << CookMountsFixture.GetError();
		ASSERT_TRUE(RefreshAssetRegistry(EAssetRegistryScanMode::FullValidation));
		DMaterial* Loaded = nullptr;
		const auto LoadedResult = LoadObject<DMaterial>(Testing::MakePackageLeafAssetObjectPathForTests(MaterialPath));
		Loaded = LoadedResult.value_or(nullptr);
		ASSERT_TRUE(LoadedResult) << (LoadedResult ? std::string{} : LoadedResult.error().Message);
		ASSERT_NE(Loaded, nullptr);
		ASSERT_NE(Loaded->GetAcceptedCompiledProgram(), nullptr);
		EXPECT_EQ(Loaded->GetAcceptedCompiledProgram()->Identity, ExpectedIdentity);
		EXPECT_TRUE(Loaded->GetExpressionCollection().Expressions.empty());
		EXPECT_TRUE(GetFunctionCalls(*Loaded).empty());
		EXPECT_TRUE(Loaded->GetAcceptedCompiledProgram()->GetDebugData().IR.Nodes.empty());
		EXPECT_TRUE(Loaded->GetAcceptedCompiledProgram()->GetDebugData().GeneratedSource.empty());
	}
	ShutdownAssetManager();
	CollectGarbage();
	ASSERT_TRUE(InitializeAssetManager());
}

TEST_F(FMaterialFunctionCookTests, StructuralNormalParentRoundTripsDuplicatesAndCooksWithoutGraph)
{
	Durin::Testing::FScopedRHIBackendOverride Backend("vulkan");
	using namespace Durin;
	using namespace Durin::AssetForge::Builtins;
	InitializeDObjectSystem();
	FScopedOfflinePreparation Offline;
	const auto Root = Testing::CreateTestFixtureDirectory("CookStructuralNormal");
	std::filesystem::create_directories(Root / "Content");
	const std::array Mounts{FMountPoint{.VirtualRoot = "/CookNormal/", .Owner = EMountOwner::Test,
		.Root = Root / "Content", .bAutoScan = true, .bContentWritable = true}};
	Testing::FScopedMountRegistryFixture Registry(Mounts);
	ASSERT_TRUE(Registry.IsValid()) << Registry.GetError();
	ASSERT_TRUE(RefreshAssetRegistry());
	std::string Error;
	FPackagePath MaterialPath;
	ASSERT_TRUE(FPackagePath::TryCreate("/CookNormal/Parent", MaterialPath));
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(MaterialPath, Material));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	std::array<FImportedSurfaceRole, 8> Roles;
	const FMaterialSurfaceOutputs Defaults;
	for (uint32 I = 0; I < Roles.size(); ++I)
		Roles[I].Value = GetMaterialSurfaceOutputDefault(Defaults, static_cast<EMaterialSurfaceOutput>(I));
	Roles[1].Sample = FImportedSurfaceSample{.ResourceIdentity = "normal", .Usage = ETextureUsage::Normal,
		.OutputIndex = 1};
	const auto Recipe = MakeImportedSurfaceRecipe(Roles);
	ASSERT_EQ(Recipe.Graph.Expressions.size(), 1u);
	ASSERT_TRUE(Recipe.Graph.Apply(*Material));
	ASSERT_TRUE(Material->CompileEdits());
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	auto* Duplicate = Cast<DMaterial>(DuplicateObject(Material, nullptr, "CopiedNormalParent").value());
	ASSERT_NE(Duplicate, nullptr);
	EXPECT_TRUE(Recipe.Graph.MatchesGraph(*Duplicate));
	MarkAsGarbage(Duplicate);
	ASSERT_TRUE(UnloadPackage(MaterialPath));
	CollectGarbage();
	{
		auto LoadedValue = LoadObject<DMaterial>(Testing::MakePackageLeafAssetObjectPathForTests(MaterialPath));
		Material = LoadedValue.value_or(nullptr);
		ASSERT_TRUE(LoadedValue);
	}
	EXPECT_TRUE(Recipe.Graph.MatchesGraph(*Material));
	ASSERT_TRUE(FinishMaterialCompileForTest(*Material));

	const auto ExpectedIdentity = Durin::Testing::ExpectedGameMaterialIdentity(*Material);
	FCookRequest Request{.OutputRoot = Root / "Cooked", .TargetPlatform = ECookTargetPlatform::Win64,
		.TargetProfile = ECookTargetProfile::Game, .ExplicitRoots = {MaterialPath}};
	FCookRunResult Result;
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << Durin::CookRunCodeName(Result) << ": " << Durin::FormatCookRunError(Result);
	ASSERT_EQ(Result.Packages.size(), 1u);
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << Durin::FormatCookRunError(Result);
	EXPECT_EQ(Result.Packages.front().Status, ECookPackageStatus::CookHit);
	ASSERT_TRUE(UnloadPackage(MaterialPath));
	ShutdownAssetManager();
	CollectGarbage();
	auto Configuration = FAssetRuntimeConfiguration::Authored();
	ASSERT_TRUE(FAssetRuntimeConfiguration::Cooked(Request.OutputRoot, Configuration));
	ASSERT_TRUE(InitializeAssetManager(std::move(Configuration)));
	{
		const std::array CookMounts{FMountPoint{.VirtualRoot = "/CookNormal/", .Owner = EMountOwner::Test,
			.Root = Request.OutputRoot / "CookNormal", .bAutoScan = true}};
		Testing::FScopedMountRegistryFixture CookMountsFixture(CookMounts);
		ASSERT_TRUE(CookMountsFixture.IsValid()) << CookMountsFixture.GetError();
		ASSERT_TRUE(RefreshAssetRegistry(EAssetRegistryScanMode::FullValidation));
		DMaterial* Loaded = nullptr;
		const auto LoadResult = LoadObject<DMaterial>(Testing::MakePackageLeafAssetObjectPathForTests(MaterialPath));
		Loaded = LoadResult.value_or(nullptr);
		ASSERT_TRUE(LoadResult) << (LoadResult ? std::string{} : LoadResult.error().Message);
		ASSERT_NE(Loaded->GetAcceptedCompiledProgram(), nullptr);
		EXPECT_EQ(Loaded->GetAcceptedCompiledProgram()->Identity, ExpectedIdentity);
		EXPECT_EQ(Loaded->GetAcceptedCompiledProgram()->GetShaderMap().Layout.ResourceFieldCount, 1u);
		EXPECT_TRUE(Loaded->GetExpressionCollection().Expressions.empty());
		EXPECT_TRUE(GetFunctionCalls(*Loaded).empty());
		EXPECT_TRUE(Loaded->GetAcceptedCompiledProgram()->GetDebugData().IR.Nodes.empty());
		EXPECT_TRUE(Loaded->GetAcceptedCompiledProgram()->GetDebugData().GeneratedSource.empty());
	}
	ShutdownAssetManager();
	CollectGarbage();
	ASSERT_TRUE(InitializeAssetManager());
}

TEST_F(FMaterialFunctionCookTests, StandardMaterialFixtureCooksAndLoadsWithoutAuthoredFunctionAssets)
{
	Durin::Testing::FScopedRHIBackendOverride Backend("vulkan");
	using namespace Durin;
	InitializeDObjectSystem();
	FScopedOfflinePreparation Offline;
	const auto Root = Testing::CreateTestFixtureDirectory("CookStandardMaterialFixture");
	std::filesystem::create_directories(Root / "Content/Materials/Functions");
	// Build current-schema fixtures from recipes; shipped packages have a separate rebuild gate.
	const std::array Mounts{FMountPoint{.VirtualRoot = "/Engine/", .Owner = EMountOwner::Test,
		.Root = Root / "Content", .bAutoScan = true, .bContentWritable = true}};
	Testing::FScopedMountRegistryFixture Registry(Mounts);
	ASSERT_TRUE(Registry.IsValid()) << Registry.GetError();
	ASSERT_TRUE(RefreshAssetRegistry());
	std::string Error;

	FPackagePath MaterialPath;
	ASSERT_TRUE(FPackagePath::TryCreate("/Engine/Materials/FunctionFixture", MaterialPath));
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(MaterialPath, Material));
	ASSERT_NE(Material, nullptr);
	AssetForge::Builtins::FStandardMaterialFunctions Functions;
	// This fixture references only the normal sampler; unrelated standard assets
	// belong to recipe coverage and unnecessarily expand registry/cook setup here.
	FPackagePath FunctionPath;
	ASSERT_TRUE(FPackagePath::TryCreate("/Engine/Materials/Functions/SampleNormal", FunctionPath));
	DMaterialFunction* Function = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(FunctionPath, Function));
	ASSERT_TRUE(AssetForge::Builtins::MakeStandardMaterialFunctionExpressions(
		AssetForge::Builtins::EStandardMaterialFunction::SampleNormal).Apply(*Function));
	Functions.SampleNormal = Function;
	ASSERT_TRUE(SavePackage(Function->GetPackage()));
	ASSERT_TRUE(Testing::MakeStandardMaterialExpressionsForTest(Functions).Apply(*Material));
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	ASSERT_EQ(GetFunctionCalls(*Material).size(), 1u);
	ASSERT_TRUE(FinishMaterialCompileForTest(*Material));

	const auto ExpectedIdentity = Durin::Testing::ExpectedGameMaterialIdentity(*Material);
	FCookRequest Request{.OutputRoot = Root / "Cooked", .TargetPlatform = ECookTargetPlatform::Win64,
		.TargetProfile = ECookTargetProfile::Game, .ExplicitRoots = {MaterialPath}};
	FCookRunResult Result;
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << Durin::CookRunCodeName(Result) << ": " << Durin::FormatCookRunError(Result);
	ASSERT_EQ(Result.Packages.size(), 1u);
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << Durin::FormatCookRunError(Result);
	ASSERT_EQ(Result.Packages.size(), 1u);
	EXPECT_EQ(Result.Packages.front().Status, ECookPackageStatus::CookHit);
	EXPECT_FALSE(std::filesystem::exists(Request.OutputRoot / "Engine/Materials/Functions"));
	auto FunctionRootRequest = Request;
	FunctionRootRequest.OutputRoot = Root / "OmittedFunctionRoot";
	FunctionRootRequest.ExplicitRoots = {FunctionPath};
	ASSERT_TRUE(FCookCoordinator().Run(FunctionRootRequest, Result));
	ASSERT_EQ(Result.Packages.size(), 1u);
	EXPECT_EQ(Result.Packages.front().Status, ECookPackageStatus::Omitted);
	EXPECT_TRUE(std::filesystem::exists(FunctionRootRequest.OutputRoot / "CookManifest.bin"));

	ASSERT_TRUE(UnloadPackage(MaterialPath));
	ShutdownAssetManager();
	CollectGarbage();
	auto Configuration = FAssetRuntimeConfiguration::Authored();
	ASSERT_TRUE(FAssetRuntimeConfiguration::Cooked(Request.OutputRoot, Configuration));
	ASSERT_TRUE(InitializeAssetManager(std::move(Configuration)));
	{
		const std::array CookMounts{FMountPoint{.VirtualRoot = "/Engine/", .Owner = EMountOwner::Test,
			.Root = Request.OutputRoot / "Engine", .bAutoScan = true}};
		Testing::FScopedMountRegistryFixture CookMountsFixture(CookMounts);
		ASSERT_TRUE(CookMountsFixture.IsValid()) << CookMountsFixture.GetError();
		ASSERT_TRUE(RefreshAssetRegistry(EAssetRegistryScanMode::FullValidation));
		DMaterial* Loaded = nullptr;
		{
			auto LoadedValue = LoadObject<DMaterial>(Testing::MakePackageLeafAssetObjectPathForTests(MaterialPath));
			Loaded = LoadedValue.value_or(nullptr);
			ASSERT_TRUE(LoadedValue);
		}
		ASSERT_NE(Loaded, nullptr);
		ASSERT_NE(Loaded->GetAcceptedCompiledProgram(), nullptr);
		EXPECT_EQ(Loaded->GetAcceptedCompiledProgram()->Identity, ExpectedIdentity);
		EXPECT_EQ(Loaded->GetAcceptedCompiledProgram()->GetShaderMap().Layout.ResourceFieldCount, 6u);
		EXPECT_TRUE(Loaded->GetExpressionCollection().Expressions.empty());
		EXPECT_TRUE(GetFunctionCalls(*Loaded).empty());
		EXPECT_TRUE(Loaded->GetAcceptedCompiledProgram()->GetDebugData().GeneratedSource.empty());
	}
	ShutdownAssetManager();
	CollectGarbage();
	ASSERT_TRUE(InitializeAssetManager());
}
