#include "Asset/Testing/PackageSaveTesting.h"
#include "NativeAssetBuildTestSupport.h"
#include "Asset/PackageUnloading.h"
#include "StaticMesh/StaticMeshImportSettings.h"
#include "AssetForge/Builtins/PBRMaterialParameters.h"
#include "../Materials/FunctionPortTestFixture.h"
#include "Misc/MountPathTestSupport.h"
#include "NativeDObjectTestSupport.h"
#include "TextureTestSupport.h"

#include "Asset/PackageSerialization.h"
#include "Asset/PackageReload.h"
#include "Asset/Mutation.h"
#include "Asset/AssetCook.h"
#include "AssetTools/IAssetTools.h"
#include "Materials/MaterialInstance.h"
#include "Modules/ModuleManager.h"
#include "RenderingThread.h"
#include "AssetForge/Builtins/AssetImport.h"
#include "AssetForge/Builtins/SceneImportData.h"
#include "AssetForge/Builtins/StaticMeshFactory.h"
#include "AssetForge/Builtins/Texture2DFactory.h"
#include "AssetForge/Builtins/Texture2DImport.h"
#include "ContentBrowser/ContentBrowserContracts.h"
#include "AssetForge/Builtins/StandardMaterialFunctions.h"
#include "AssetForge/Builtins/ImportedSurfaceRecipe.h"
#include "AssetForge/Builtins/PBRSurfaceMaterial.h"
#include "Hash/XxHash.h"
#include "Materials/Material.h"
#include "DObject/StrongObjectPtr.h"
#include "DObject/DObjectArray.h"
#include "Materials/MaterialProgramCompiler.h"
#include "Import/EditorReimportHandler.h"
#include "StaticMesh/StaticMeshFactoryTestSupport.h"
#include "Asset/AssetCompilingManager.h"
#include "../Materials/ExplicitMaterialProgramTestFixture.h"
#include "../Materials/StandardMaterialFunctionTestFixture.h"
#include "StaticMesh/StaticMesh.h"
#include "Components/StaticMeshComponent.h"
#include "Threading/Task.h"

namespace
{
	class FSceneImportTestEnvironment final : public testing::Environment
	{
	public:
		auto SetUp() -> void override
		{
			InitializeDObjectSystem();
			ASSERT_TRUE(Durin::InitializeAssetCompilingManager());
			ASSERT_TRUE(Durin::InitializeGameThreadDeferredExecutor());
		}
		auto TearDown() -> void override
		{
			Durin::FAssetCompilingManager::Get().FinishAllCompilation();
			Durin::ShutdownAssetCompilingManager();
		}
	};
	[[maybe_unused]] auto* GSceneImportEnvironment = testing::AddGlobalTestEnvironment(new FSceneImportTestEnvironment);

	auto MakeAssetPath(std::string_view Value) -> Durin::FPackagePath
	{
		Durin::FPackagePath Result;
		EXPECT_TRUE(Durin::FPackagePath::TryCreate(Value, Result));
		return Result;
	}

	struct FSceneFixture
	{
		struct FRenderingThreadScope
		{
			FRenderingThreadScope() { Durin::InitRenderingThread(); }
			~FRenderingThreadScope() { Durin::ShutdownRenderingThread(); }
		};

		std::unique_ptr<FRenderingThreadScope> RenderingThread;
		std::unique_ptr<Durin::Testing::FScopedMountRegistryFixture> Mounts;
		std::string Source;
		Durin::FPackagePath DestinationDirectory;

		FSceneFixture() = default;
		~FSceneFixture() { Durin::FAssetCompilingManager::Get().FinishAllCompilation(); }
		FSceneFixture(FSceneFixture&& Other) noexcept
			: RenderingThread(std::move(Other.RenderingThread)),
			Mounts(std::move(Other.Mounts)), Source(std::move(Other.Source)),
			DestinationDirectory(std::move(Other.DestinationDirectory)) {}
	};

	auto InitializeFixture(std::string_view Name,
		std::string_view FixturePath = "StaticModelMaterials/RenderedOpaqueDataUri.gltf")
		-> FSceneFixture
	{
		InitializeDObjectSystem();
		Durin::FModuleManager::Get().LoadModuleChecked("TextureCompressor");
		Durin::FModuleManager::Get().LoadModuleChecked("ShaderBuild");
		Durin::FModuleManager::Get().LoadModuleChecked("MeshBuilder");
		auto RenderingThread =
			std::make_unique<FSceneFixture::FRenderingThreadScope>();
		std::string Error;
		const std::filesystem::path Root = Durin::Testing::GetTestWorkDirectory()
			/ "SceneAssetForge" / std::string(Name);
		Durin::Testing::RemoveTestWorkDirectory(Root);
		std::filesystem::create_directories(Root / "Engine/Content");
		std::filesystem::create_directories(Root / "Project/Content/Scenes");
		auto Mounts = std::make_unique<Durin::Testing::FScopedMountRegistryFixture>(
			std::vector<Durin::FMountPoint>{
				{.VirtualRoot = "/Engine/", .Owner = Durin::EMountOwner::Test,
					.Root = Root / "Engine/Content", .bAutoScan = true,
					.bContentWritable = true},
				{.VirtualRoot = "/SceneImportTests/", .Owner = Durin::EMountOwner::Test,
					.Root = Root / "Project/Content", .bAutoScan = true,
					.bContentWritable = true, .Dependencies = {"/Engine/"}}});
		EXPECT_TRUE(Mounts->IsValid()) << Mounts->GetError();
		EXPECT_TRUE(Durin::RefreshAssetRegistry());
		const std::filesystem::path Input = std::filesystem::path(DURIN_TEST_DATA_DIR)
			/ FixturePath;
		const std::string Extension = Input.extension().generic_string();
		std::filesystem::copy_file(Input,
			Root / "Project/Content/Scenes" / (std::string(Name) + Extension),
			std::filesystem::copy_options::overwrite_existing);
		FSceneFixture Fixture;
		Fixture.RenderingThread = std::move(RenderingThread);
		Fixture.Mounts = std::move(Mounts);
		Fixture.Source = (Root / "Project/Content/Scenes"
			/ (std::string(Name) + Extension)).generic_string();
		Fixture.DestinationDirectory = MakeAssetPath(std::format(
			"/SceneImportTests/SceneImport/{}", Name));
		return Fixture;
	}

	auto RunScene(const FSceneFixture& Fixture)
		-> Durin::AssetForge::Builtins::FAssetImportResult
	{
		auto Result = Durin::AssetForge::Builtins::ImportAssetOutputs(Fixture.Source, Fixture.DestinationDirectory,
			Durin::FStaticMeshImportSettings::MakeDurin(),
			{.MeshMode = Durin::AssetForge::Builtins::EAssetImportMeshMode::Combined}, {}, {}, Durin::FAssetPackageSaveArgs{});
		EXPECT_TRUE(Result) << Result.Message;
		return Result;
	}
}

namespace
{
	auto AdvanceSceneSession(Durin::AssetForge::Builtins::FAssetImportSession& Session,
		Durin::AssetForge::Builtins::EAssetImportPhase Target) -> bool
	{
		const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
		while (std::chrono::steady_clock::now() < Deadline)
		{
			Durin::PumpGameThreadDeferredWork();
			Durin::FAssetCompilingManager::Get().ProcessAsyncTasks(false);
			Session.Tick();
			if (Session.GetProgress().Phase == Target) return true;
			if (Session.GetProgress().Phase == Durin::AssetForge::Builtins::EAssetImportPhase::Completed) return false;
			std::this_thread::yield();
		}
		return false;
	}
}

TEST(FSceneImportTests, GltfImportExtensionDoesNotRequireLevelEditorWorkspace)
{
	Durin::FModuleManager::Get().LoadModuleChecked("AssetForgeBuiltins");
	const auto Extensions = Durin::Editor::ContentBrowser::CaptureExtensions(Durin::Editor::ContentBrowser::EExtensionCategory::Import);
	const auto Extension = std::ranges::find(Extensions, std::string("gltf.import-assets"),
		&Durin::Editor::ContentBrowser::FExtensionDescriptor::Id);
	ASSERT_NE(Extension, Extensions.end());
	EXPECT_TRUE(Extension->Invoke);
	EXPECT_TRUE(Extension->DrawHostPresentation);
	EXPECT_EQ(Extension->Mutation, Durin::Editor::ContentBrowser::EContentMutation::MutatesContent);
}

TEST(FSceneImportTests, OutputSelectionReplansCapturedDocumentWithoutReopeningSource)
{
	using namespace Durin;
	using namespace Durin::AssetForge::Builtins;
	const auto Fixture = InitializeFixture("Replan", "StaticModelMaterials/PrimitiveProjection.gltf");
	std::filesystem::copy_file(std::filesystem::path(DURIN_TEST_DATA_DIR) / "StaticModelMaterials/Triangle.bin",
		std::filesystem::path(Fixture.Source).parent_path() / "Triangle.bin");
	FAssetImportSession Session(Fixture.Source, Fixture.DestinationDirectory, FStaticMeshImportSettings::MakeDurin(), {});
	ASSERT_TRUE(AdvanceSceneSession(Session, EAssetImportPhase::Ready));
	ASSERT_EQ(Session.GetSourceMeshes().size(), 2u);
	ASSERT_EQ(Session.GetSourceScenes().size(), 1u);
	const auto OriginalPath = std::filesystem::path(Fixture.Source);
	const auto BackupPath = OriginalPath.string() + ".backup";
	std::filesystem::rename(OriginalPath, BackupPath);
	ASSERT_TRUE(Session.SetOptions({.SelectedMeshes = {1}, .bCreateMaterials = false}));
	EXPECT_TRUE(AdvanceSceneSession(Session, EAssetImportPhase::Ready));
	const auto Preview = Session.PreviewOutputs(Fixture.DestinationDirectory);
	std::filesystem::rename(BackupPath, OriginalPath);
	ASSERT_TRUE(Preview.bSucceeded) << Preview.Message;
	ASSERT_EQ(Preview.Outputs.size(), 1u);
	ASSERT_TRUE(Session.BeginImport(Fixture.DestinationDirectory, {}));
	ASSERT_TRUE(AdvanceSceneSession(Session, EAssetImportPhase::Completed));
	ASSERT_TRUE(Session.GetResult()) << Session.GetResult().Message;
	EXPECT_EQ(Session.GetResult().Outputs[0].AssetPath, Preview.Outputs[0].AssetPath);
}

TEST(FSceneImportTests, SplitOutputsReuseInstancesAndBindOnlyTheirOwnMaterials)
{
	using namespace Durin;
	using namespace Durin::AssetForge;
	using namespace Durin::AssetForge::Builtins;
	const auto Fixture = InitializeFixture("Split", "StaticModelMaterials/ResourceInstances.gltf");
	std::filesystem::copy_file(std::filesystem::path(DURIN_TEST_DATA_DIR) / "StaticModelMaterials/Triangle.bin",
		std::filesystem::path(Fixture.Source).parent_path() / "Triangle.bin");
	const auto Result = ImportAssetOutputs(Fixture.Source, Fixture.DestinationDirectory);
	ASSERT_TRUE(Result) << Result.Message;
	EXPECT_EQ(Result.Outputs.size(), 3u); // One shared source mesh and its two materials.
	EXPECT_EQ(Result.ImportedPackages.size(), 3u);
	EXPECT_FALSE(Result.SaveResult);
	for (const auto& Path : Result.ImportedPackages)
	{
		auto* Package = FindResidentPackage(Path);
		ASSERT_NE(Package, nullptr);
		EXPECT_TRUE(Package->IsDirty());
		EXPECT_TRUE(Package->IsNewlyCreated());
		EXPECT_FALSE(FindPackageExact(Path));
		const auto Physical = FMountPaths::ResolveAssetPath(Path.GetView());
		ASSERT_TRUE(Physical);
		EXPECT_FALSE(std::filesystem::exists(Physical->PhysicalPath.generic_string() + ".dasset"));
	}
	const auto MeshRow = std::ranges::find(Result.Outputs, std::string("StaticMesh"), &FImportOutputSummary::Role);
	ASSERT_NE(MeshRow, Result.Outputs.end());
	auto* Mesh = LoadObjectDetailed<DStaticMesh>(Testing::MakePackageLeafAssetObjectPathForTests(MeshRow->AssetPath)).value_or(nullptr);
	ASSERT_NE(Mesh, nullptr);
	ASSERT_EQ(Mesh->GetNumMaterialSlots(), 2u);
	EXPECT_EQ(Mesh->GetMaterialSlots()[0].SourceMaterialIndex, 0u);
	EXPECT_EQ(Mesh->GetMaterialSlots()[1].SourceMaterialIndex, 2u);
	for (const auto& Slot : Mesh->GetMaterialSlots()) EXPECT_NE(Slot.DefaultMaterial.Get(), nullptr);
	EXPECT_TRUE(std::ranges::all_of(Result.Outputs, [](const auto& Row) { return Row.State == EImportOutputState::Imported; }));
	const auto All = ImportAssetOutputs(Fixture.Source, MakeAssetPath("/SceneImportTests/AllResources"),
		FStaticMeshImportSettings::MakeYUpNegativeZForward(), {.bIncludeUnusedResources = true});
	ASSERT_TRUE(All) << All.Message;
	EXPECT_EQ(std::ranges::count(All.Outputs, std::string("StaticMesh"), &FImportOutputSummary::Role), 2);
	EXPECT_EQ(All.Outputs.size(), 6u);
	EXPECT_EQ(Mesh->GetSource().GetMeshCount(), 2u);
	const auto Combined = ImportAssetOutputs(Fixture.Source, MakeAssetPath("/SceneImportTests/CombinedInstances"),
		FStaticMeshImportSettings::MakeDurin(), {.MeshMode = EAssetImportMeshMode::Combined});
	ASSERT_TRUE(Combined) << Combined.Message;
	const auto CombinedRow = std::ranges::find(Combined.Outputs, std::string("StaticMesh"), &FImportOutputSummary::Role);
	ASSERT_NE(CombinedRow, Combined.Outputs.end());
	auto* CombinedMesh = LoadObjectDetailed<DStaticMesh>(Testing::MakePackageLeafAssetObjectPathForTests(CombinedRow->AssetPath)).value_or(nullptr);
	ASSERT_NE(CombinedMesh, nullptr);
	EXPECT_EQ(CombinedMesh->GetSource().GetMeshCount(), 4u);

}

TEST(FSceneImportTests, SessionDefaultsToSplitAndCombinesOnlyWithExplicitOptions)
{
	using namespace Durin;
	using namespace Durin::AssetForge;
	using namespace Durin::AssetForge::Builtins;
	const auto Fixture = InitializeFixture("SessionMeshModes", "StaticModelMaterials/ResourceInstances.gltf");
	std::filesystem::copy_file(std::filesystem::path(DURIN_TEST_DATA_DIR) / "StaticModelMaterials/Triangle.bin",
		std::filesystem::path(Fixture.Source).parent_path() / "Triangle.bin");
	const auto Settings = FStaticMeshImportSettings::MakeDurin();
	FAssetImportSession Session(Fixture.Source, Fixture.DestinationDirectory, Settings);
	ASSERT_TRUE(AdvanceSceneSession(Session, EAssetImportPhase::Ready));
	const auto Split = Session.PreviewOutputs(Fixture.DestinationDirectory);
	ASSERT_TRUE(Split.bSucceeded) << Split.Message;
	ASSERT_EQ(Split.Outputs.size(), 3u);
	ASSERT_TRUE(Session.BeginImport(Fixture.DestinationDirectory, {}));
	ASSERT_TRUE(AdvanceSceneSession(Session, EAssetImportPhase::Completed));
	ASSERT_TRUE(Session.GetResult()) << Session.GetResult().Message;
	EXPECT_FALSE(Session.GetResult().SaveResult);
	const auto MeshRow = std::ranges::find(Session.GetResult().Outputs, std::string("StaticMesh"), &FImportOutputSummary::Role);
	ASSERT_NE(MeshRow, Session.GetResult().Outputs.end());
	auto* Mesh = LoadObjectDetailed<DStaticMesh>(Testing::MakePackageLeafAssetObjectPathForTests(MeshRow->AssetPath)).value_or(nullptr);
	ASSERT_NE(Mesh, nullptr);
	EXPECT_EQ(Mesh->GetSource().GetMeshCount(), 2u);

	const auto Destination = MakeAssetPath("/SceneImportTests/SessionCombined");
	FAssetImportSession Combined(Fixture.Source, Destination, Settings, {.MeshMode = EAssetImportMeshMode::Combined});
	ASSERT_TRUE(AdvanceSceneSession(Combined, EAssetImportPhase::Ready));
	const auto Preview = Combined.PreviewOutputs(Destination);
	ASSERT_TRUE(Preview.bSucceeded) << Preview.Message;
	ASSERT_TRUE(Combined.BeginImport(Destination, {}));
	ASSERT_TRUE(AdvanceSceneSession(Combined, EAssetImportPhase::Completed));
	ASSERT_TRUE(Combined.GetResult()) << Combined.GetResult().Message;
	const auto CombinedRow = std::ranges::find(Combined.GetResult().Outputs, std::string("StaticMesh"), &FImportOutputSummary::Role);
	ASSERT_NE(CombinedRow, Combined.GetResult().Outputs.end());
	EXPECT_EQ(CombinedRow->StableIdentity, "scene:mesh:combined");
	auto* CombinedMesh = LoadObjectDetailed<DStaticMesh>(Testing::MakePackageLeafAssetObjectPathForTests(CombinedRow->AssetPath)).value_or(nullptr);
	ASSERT_NE(CombinedMesh, nullptr);
	EXPECT_EQ(CombinedMesh->GetSource().GetMeshCount(), 4u);
}

TEST(FSceneImportTests, ExplicitSelectionAndGeometryOnlyHaveClosedDependencies)
{
	using namespace Durin;
	using namespace Durin::AssetForge;
	using namespace Durin::AssetForge::Builtins;
	const auto Fixture = InitializeFixture("Selection", "StaticModelMaterials/PrimitiveProjection.gltf");
	std::filesystem::copy_file(std::filesystem::path(DURIN_TEST_DATA_DIR) / "StaticModelMaterials/Triangle.bin",
		std::filesystem::path(Fixture.Source).parent_path() / "Triangle.bin");
	const auto Selected = ImportAssetOutputs(Fixture.Source, Fixture.DestinationDirectory,
		FStaticMeshImportSettings::MakeDurin(), {.SelectedMeshes = {1}});
	ASSERT_TRUE(Selected) << Selected.Message;
	EXPECT_EQ(Selected.Outputs.size(), 2u);
	const auto Geometry = ImportAssetOutputs(Fixture.Source, MakeAssetPath("/SceneImportTests/GeometryOnly"),
		FStaticMeshImportSettings::MakeDurin(), {.bCreateMaterials = false, .bCreateTextures = false});
	ASSERT_TRUE(Geometry) << Geometry.Message;
	EXPECT_EQ(Geometry.Outputs.size(), 2u);
	for (const auto& Row : Geometry.Outputs)
	{
		auto* Mesh = LoadObjectDetailed<DStaticMesh>(Testing::MakePackageLeafAssetObjectPathForTests(Row.AssetPath)).value_or(nullptr);
		ASSERT_NE(Mesh, nullptr);
		for (const auto& Slot : Mesh->GetMaterialSlots()) EXPECT_EQ(Slot.DefaultMaterial.Get(), nullptr);
	}
}

TEST(FSceneImportTests, SharedImageUsageAndDisabledTexturesProduceExplicitProducts)
{
	using namespace Durin;
	using namespace Durin::AssetForge;
	using namespace Durin::AssetForge::Builtins;
	const auto Fixture = InitializeFixture("SharedUsage", "StaticModelMaterials/SharedImageUsage.gltf");
	for (const auto File : {"Triangle.bin", "Red.png"})
		std::filesystem::copy_file(std::filesystem::path(DURIN_TEST_DATA_DIR) / "StaticModelMaterials" / File,
			std::filesystem::path(Fixture.Source).parent_path() / File);
	const auto Result = ImportAssetOutputs(Fixture.Source, Fixture.DestinationDirectory);
	ASSERT_TRUE(Result) << Result.Message;
	EXPECT_EQ(std::ranges::count_if(Result.Outputs, [](const auto& Output) { return Output.Role.starts_with("Texture2D."); }), 2);
	const auto Fallback = ImportAssetOutputs(Fixture.Source, MakeAssetPath("/SceneImportTests/WithoutTextures"),
		FStaticMeshImportSettings::MakeDurin(), {.bCreateTextures = false});
	ASSERT_TRUE(Fallback) << Fallback.Message;
	EXPECT_TRUE(std::ranges::none_of(Fallback.Outputs, [](const auto& Output) { return Output.Role.starts_with("Texture2D."); }));
	EXPECT_TRUE(std::ranges::any_of(Fallback.Diagnostics, [](const auto& Diagnostic) { return Diagnostic.Severity == EImportDiagnosticSeverity::Warning; }));
}

TEST(FSceneImportTests, ExistingMaterialMappingPublishesOnlyMeshAndPreservesMappedAsset)
{
	using namespace Durin;
	using namespace Durin::AssetForge;
	using namespace Durin::AssetForge::Builtins;
	const auto Fixture = InitializeFixture("MaterialMapping");
	const auto Original = ImportAssetOutputs(Fixture.Source, Fixture.DestinationDirectory,
		FStaticMeshImportSettings::MakeDurin(), {.bCreateMeshes = false});
	ASSERT_TRUE(Original) << Original.Message;
	const auto Row = std::ranges::find(Original.Outputs, std::string("Material"), &FImportOutputSummary::Role);
	ASSERT_NE(Row, Original.Outputs.end());
	auto* Material = LoadObjectDetailed<DMaterial>(Testing::MakePackageLeafAssetObjectPathForTests(Row->AssetPath)).value_or(nullptr);
	ASSERT_NE(Material, nullptr);
	const auto Revision = Material->GetPackage()->GetEditRevision();
	const auto Result = ImportAssetOutputs(Fixture.Source, MakeAssetPath("/SceneImportTests/MappedMesh"),
		FStaticMeshImportSettings::MakeDurin(), {}, {.Default = {.Mode = ESceneMaterialImportMode::UseExisting,
			.ExistingMaterialPath = Material->GetObjectPath()}});
	ASSERT_TRUE(Result) << Result.Message;
	EXPECT_EQ(Result.ImportedPackages.size(), 1u);
	EXPECT_EQ(std::ranges::count(Result.Outputs, EImportOutputState::Preserved, &FImportOutputSummary::State), 1);
	EXPECT_EQ(Material->GetPackage()->GetEditRevision(), Revision);
	const auto MeshRow = std::ranges::find(Result.Outputs, std::string("StaticMesh"), &FImportOutputSummary::Role);
	ASSERT_NE(MeshRow, Result.Outputs.end());
	auto* Mesh = LoadObjectDetailed<DStaticMesh>(Testing::MakePackageLeafAssetObjectPathForTests(MeshRow->AssetPath)).value_or(nullptr);
	ASSERT_NE(Mesh, nullptr);
	EXPECT_EQ(Mesh->GetMaterialSlots()[0].DefaultMaterial.Get(), Material);
	const auto PreservedOnly = ImportAssetOutputs(Fixture.Source, MakeAssetPath("/SceneImportTests/PreservedOnly"),
		FStaticMeshImportSettings::MakeDurin(), {.bCreateMeshes = false, .bCreateTextures = false},
		{.Default = {.Mode = ESceneMaterialImportMode::UseExisting, .ExistingMaterialPath = Material->GetObjectPath()}},
		{}, FAssetPackageSaveArgs{});
	ASSERT_TRUE(PreservedOnly) << PreservedOnly.Message;
	EXPECT_TRUE(PreservedOnly.ImportedPackages.empty());
	ASSERT_TRUE(PreservedOnly.SaveResult);
	EXPECT_TRUE(*PreservedOnly.SaveResult);
	EXPECT_TRUE(PreservedOnly.SaveResult->SavedPackages.empty());
	EXPECT_TRUE(std::ranges::all_of(PreservedOnly.Outputs, [](const auto& Row) { return Row.State == EImportOutputState::Preserved; }));

}

TEST(FSceneImportTests, PreviewAndExecutionRejectDestinationChanges)
{
	using namespace Durin;
	using namespace Durin::AssetForge;
	using namespace Durin::AssetForge::Builtins;
	const auto Fixture = InitializeFixture("PreviewRace");
	FAssetImportSession Session(Fixture.Source, Fixture.DestinationDirectory,
		FStaticMeshImportSettings::MakeYUpNegativeZForward(), {});
	ASSERT_TRUE(AdvanceSceneSession(Session, EAssetImportPhase::Ready));
	const auto Preview = Session.PreviewOutputs(Fixture.DestinationDirectory);
	ASSERT_TRUE(Preview.bSucceeded) << Preview.Message;
	ASSERT_FALSE(Preview.Outputs.empty());
	// Another completed import occupies the previewed outputs before this session executes.
	const auto Other = ImportAssetOutputs(Fixture.Source, Fixture.DestinationDirectory);
	ASSERT_TRUE(Other) << Other.Message;
	ASSERT_TRUE(Session.BeginImport(Fixture.DestinationDirectory, {}));
	ASSERT_TRUE(AdvanceSceneSession(Session, EAssetImportPhase::Completed));
	EXPECT_FALSE(Session.GetResult());
	EXPECT_TRUE(Session.GetResult().ImportedPackages.empty());
	EXPECT_TRUE(std::ranges::any_of(Session.GetResult().Diagnostics, [](const auto& Diagnostic) {
		return Diagnostic.Category == EImportDiagnosticCategory::Collision;
	}));
}

TEST(FSceneImportTests, CombinedPreviewRejectsOccupiedMeshOutputAndRebasesFromCapturedSource)
{
	using namespace Durin;
	using namespace Durin::AssetForge;
	using namespace Durin::AssetForge::Builtins;
	const auto Fixture = InitializeFixture("CombinedPreviewCollision");
	FAssetImportSession Session(Fixture.Source, Fixture.DestinationDirectory, FStaticMeshImportSettings::MakeDurin(),
		{.MeshMode = EAssetImportMeshMode::Combined});
	ASSERT_TRUE(AdvanceSceneSession(Session, EAssetImportPhase::Ready));
	const auto Preview = Session.PreviewOutputs(Fixture.DestinationDirectory);
	ASSERT_TRUE(Preview.bSucceeded) << Preview.Message;
	const auto Mesh = std::ranges::find(Preview.Outputs, std::string("StaticMesh"), &FImportOutputSummary::Role);
	ASSERT_NE(Mesh, Preview.Outputs.end());
	const auto Existing = ImportAssetOutputs(Fixture.Source, Fixture.DestinationDirectory, FStaticMeshImportSettings::MakeDurin(),
		{.MeshMode = EAssetImportMeshMode::Combined, .bCreateMaterials = false, .bCreateTextures = false});
	ASSERT_TRUE(Existing) << Existing.Message;
	ASSERT_EQ(Existing.Outputs.size(), 1u);
	EXPECT_EQ(Existing.Outputs[0].AssetPath, Mesh->AssetPath);
	const auto Conflict = Session.PreviewOutputs(Fixture.DestinationDirectory);
	EXPECT_FALSE(Conflict.bSucceeded);
	EXPECT_TRUE(std::ranges::any_of(Conflict.Outputs, [&](const auto& Output) {
		return Output.AssetPath == Mesh->AssetPath && Output.Disposition == EImportOutputDisposition::Conflict;
	}));
	EXPECT_TRUE(std::ranges::any_of(Conflict.Diagnostics, [](const auto& Diagnostic) {
		return Diagnostic.Category == EImportDiagnosticCategory::Collision;
	}));
	EXPECT_FALSE(Session.BeginImport(Fixture.DestinationDirectory, {}));
	std::filesystem::remove(Fixture.Source);
	const auto Rebased = Session.PreviewOutputs(MakeAssetPath("/SceneImportTests/UnoccupiedCombinedPreview"));
	ASSERT_TRUE(Rebased.bSucceeded) << Rebased.Message;
	EXPECT_EQ(Rebased.Outputs.size(), Preview.Outputs.size());
	EXPECT_EQ(Rebased.Materials.size(), Preview.Materials.size());
}

TEST(FSceneImportTests, OptionalSaveFailureRetainsEveryImportedOutput)
{
	using namespace Durin;
	using namespace Durin::AssetForge;
	using namespace Durin::AssetForge::Builtins;
	const auto Fixture = InitializeFixture("SplitPartial", "StaticModelMaterials/PrimitiveProjection.gltf");
	std::filesystem::copy_file(std::filesystem::path(DURIN_TEST_DATA_DIR) / "StaticModelMaterials/Triangle.bin",
		std::filesystem::path(Fixture.Source).parent_path() / "Triangle.bin");
	FAssetImportResult Result;
	{
		Testing::FScopedPackageSaveFault Fault([](Testing::EPackageSaveFaultPoint Phase, size_t Index) {
			return Phase == Testing::EPackageSaveFaultPoint::StagePackage && Index == 1;
		});
		Result = ImportAssetOutputs(Fixture.Source, Fixture.DestinationDirectory,
			FStaticMeshImportSettings::MakeDurin(), {}, {}, {}, FAssetPackageSaveArgs{});
	}
	ASSERT_TRUE(Result) << Result.Message;
	ASSERT_TRUE(Result.SaveResult);
	EXPECT_FALSE(*Result.SaveResult);
	EXPECT_EQ(Result.SaveResult->SavedPackages.size(), 1u);
	EXPECT_EQ(Result.ImportedPackages.size(), 5u);
	EXPECT_EQ(std::ranges::count(Result.Outputs, EImportOutputState::Imported, &FImportOutputSummary::State), 5);
	std::vector<DPackage*> Packages;
	for (const auto& Path : Result.ImportedPackages)
	{
		auto* Package = FindResidentPackage(Path);
		ASSERT_NE(Package, nullptr);
		EXPECT_FALSE(Package->IsGraphPrivate());
		EXPECT_EQ(Package->IsDirty(), Path != Result.SaveResult->SavedPackages.front());
		Packages.push_back(Package);
	}
	EXPECT_TRUE(SavePackages(Packages));
}

TEST(FSceneImportTests, AsyncSessionPublishesIndependentImports)
{
	using namespace Durin;
	using namespace Durin::AssetForge::Builtins;
	const auto Fixture = InitializeFixture("AsyncSession");
	for (int Pass = 0; Pass != 2; ++Pass)
	{
		const auto Destination = MakeAssetPath(Fixture.DestinationDirectory.ToString() + std::format("/Pass{}", Pass));
		FAssetImportSession Session(Fixture.Source, Destination, FStaticMeshImportSettings::MakeDurin(), {.MeshMode = EAssetImportMeshMode::Combined});
		ASSERT_TRUE(AdvanceSceneSession(Session, EAssetImportPhase::Ready)) << Session.GetResult().Message;
		ASSERT_TRUE(Session.PreviewOutputs(Destination, {}).bSucceeded);
		ASSERT_TRUE(Session.PreviewOutputs(Destination, {}).bSucceeded);
		ASSERT_TRUE(Session.BeginImport(Destination, {}, FAssetPackageSaveArgs{}));
		ASSERT_TRUE(AdvanceSceneSession(Session, EAssetImportPhase::Saving)) << Session.GetResult().Message;
		// Normal editor frames may create unrelated objects while disk staging runs.
		TStrongObjectPtr<DStaticMeshComponent> Unrelated(NewObject<DStaticMeshComponent>(nullptr,
			FName(Pass == 0 ? "AsyncUnrelatedFirst" : "AsyncUnrelatedSecond")));
		ASSERT_TRUE(AdvanceSceneSession(Session, EAssetImportPhase::Completed));
		ASSERT_TRUE(Session.GetResult()) << Session.GetResult().Message;
		ASSERT_TRUE(Session.GetResult().SaveResult);
		EXPECT_TRUE(*Session.GetResult().SaveResult);
		EXPECT_FALSE(Session.GetResult().ImportedPackages.empty());
	}
}

TEST(FSceneImportTests, AsyncSessionRejectsChangedSourceAfterReusablePreview)
{
	using namespace Durin;
	using namespace Durin::AssetForge::Builtins;
	const auto Fixture = InitializeFixture("AsyncStaleSource");
	FAssetImportSession Session(Fixture.Source, Fixture.DestinationDirectory,
		FStaticMeshImportSettings::MakeDurin(),
		{.MeshMode = EAssetImportMeshMode::Combined});
	ASSERT_TRUE(AdvanceSceneSession(Session, EAssetImportPhase::Ready));
	std::ofstream(Fixture.Source, std::ios::app) << "\n ";
	// Preview reads cached scene values, even after the physical source changes.
	ASSERT_TRUE(Session.PreviewOutputs(Fixture.DestinationDirectory, {}).bSucceeded);
	ASSERT_TRUE(Session.BeginImport(Fixture.DestinationDirectory, {}));
	ASSERT_TRUE(AdvanceSceneSession(Session, EAssetImportPhase::Completed));
	EXPECT_FALSE(Session.GetResult());
	EXPECT_TRUE(Session.GetResult().ImportedPackages.empty());
	EXPECT_NE(Session.GetResult().Message.find("changed"), std::string::npos);
}

TEST(FSceneImportTests, AsyncSessionCancelsBeforePublication)
{
	using namespace Durin;
	using namespace Durin::AssetForge::Builtins;
	const auto Fixture = InitializeFixture("AsyncCancel");
	FAssetImportSession Session(Fixture.Source, Fixture.DestinationDirectory,
		FStaticMeshImportSettings::MakeDurin(),
		{.MeshMode = EAssetImportMeshMode::Combined});
	ASSERT_TRUE(AdvanceSceneSession(Session, EAssetImportPhase::Ready));
	ASSERT_TRUE(Session.BeginImport(Fixture.DestinationDirectory, {}));
	Session.Tick();
	Session.Cancel();
	ASSERT_TRUE(AdvanceSceneSession(Session, EAssetImportPhase::Completed));
	EXPECT_FALSE(Session.GetResult());
	EXPECT_TRUE(Session.GetResult().ImportedPackages.empty());
}

TEST(FSceneImportTests, AsyncSessionReportsImportAndSaveResultsSeparately)
{
	using namespace Durin;
	using namespace Durin::AssetForge::Builtins;
	const auto Fixture = InitializeFixture("AsyncPartial");
	FAssetImportSession Session(Fixture.Source, Fixture.DestinationDirectory,
		FStaticMeshImportSettings::MakeDurin(),
		{.MeshMode = EAssetImportMeshMode::Combined});
	ASSERT_TRUE(AdvanceSceneSession(Session, EAssetImportPhase::Ready));
	{
		Testing::FScopedPackageSaveFault Fault([](Testing::EPackageSaveFaultPoint Phase, size_t Index) {
			return Index == 1 && Phase == Testing::EPackageSaveFaultPoint::PublishRegistry;
		});
		ASSERT_TRUE(Session.BeginImport(Fixture.DestinationDirectory, {}, FAssetPackageSaveArgs{}));
	}
	ASSERT_TRUE(AdvanceSceneSession(Session, EAssetImportPhase::Completed));
	const auto& Result = Session.GetResult();
	ASSERT_TRUE(Result);
	ASSERT_TRUE(Result.SaveResult);
	EXPECT_FALSE(*Result.SaveResult);
	EXPECT_EQ(Result.SaveResult->SavedPackages.size(), 1u) << Result.Message;
	EXPECT_EQ(Result.SaveResult->Result.Effect, EAssetWriteEffect::ContentCommittedProjectionPending);
	for (const auto& Path : Result.ImportedPackages) EXPECT_NE(FindResidentPackage(Path), nullptr);
	ASSERT_TRUE(RefreshAssetRegistry());
	std::vector<DPackage*> Packages;
	for (const auto& Path : Result.ImportedPackages) Packages.push_back(FindResidentPackage(Path));
	EXPECT_TRUE(SavePackages(Packages));
}

TEST(FSceneImportTests, AsyncSaveCancellationKeepsImportAndAllowsOrdinarySaveRetry)
{
	using namespace Durin;
	using namespace Durin::AssetForge::Builtins;
	const auto Fixture = InitializeFixture("AsyncStagedCancel");
	std::vector<FPackagePath> Imported;
	{
		FAssetImportSession Session(Fixture.Source, Fixture.DestinationDirectory,
			FStaticMeshImportSettings::MakeDurin(),
			{.MeshMode = EAssetImportMeshMode::Combined});
		ASSERT_TRUE(AdvanceSceneSession(Session, EAssetImportPhase::Ready));
		ASSERT_TRUE(Session.BeginImport(Fixture.DestinationDirectory, {}, FAssetPackageSaveArgs{}));
		ASSERT_TRUE(AdvanceSceneSession(Session, EAssetImportPhase::Saving));
		Session.Cancel();
		ASSERT_TRUE(AdvanceSceneSession(Session, EAssetImportPhase::Completed));
		const auto& Result = Session.GetResult();
		ASSERT_TRUE(Result);
		ASSERT_TRUE(Result.SaveResult);
		EXPECT_EQ(Result.SaveResult->Result.Error, EAssetWriteError::Cancelled);
		EXPECT_TRUE(Result.SaveResult->SavedPackages.empty());
		Imported = Result.ImportedPackages;
	}
	ASSERT_FALSE(Imported.empty());
	std::vector<DPackage*> Packages;
	for (const auto& Path : Imported)
	{
		auto* Package = FindResidentPackage(Path);
		ASSERT_NE(Package, nullptr);
		EXPECT_TRUE(Package->IsDirty());
		EXPECT_FALSE(FindPackageExact(Path));
		Packages.push_back(Package);
	}
	EXPECT_TRUE(SavePackages(Packages));
}

TEST(FSceneImportTests, AsyncSessionDestructionDrainsPreparation)
{
	using namespace Durin;
	using namespace Durin::AssetForge::Builtins;
	const auto Fixture = InitializeFixture("AsyncDestruction");
	{
		FAssetImportSession Session(Fixture.Source, Fixture.DestinationDirectory,
			FStaticMeshImportSettings::MakeDurin(),
			{.MeshMode = EAssetImportMeshMode::Combined});
		Session.Tick();
	}
	EXPECT_TRUE(RunScene(Fixture));
}

TEST(FSceneImportTests, AssetForgePublishesHeterogeneousGraph)
{
	const FSceneFixture Fixture = InitializeFixture("Heterogeneous");
	const auto Imported = RunScene(Fixture);
	ASSERT_TRUE(Imported) << Imported.Message;
	ASSERT_EQ(Imported.Outputs.size(), 3u);
	bool bSawTexture = false;
	for (const auto& Output : Imported.Outputs)
	{
		Durin::DObject* Object = nullptr;
		{
			auto LoadedValue = Durin::LoadObjectDetailed<Durin::DObject>(Durin::Testing::MakePackageLeafAssetObjectPathForTests(Output.AssetPath));
			Object = LoadedValue.value_or(nullptr);
			EXPECT_TRUE(LoadedValue);
		}
		EXPECT_NE(Object, nullptr);
		if (auto* Material = Durin::Cast<Durin::DMaterial>(Object))
		{
			auto* Parent = Material;
			ASSERT_NE(Parent, nullptr);
			EXPECT_TRUE(Parent->GetPackage()->GetPackagePath().starts_with(Fixture.DestinationDirectory.ToString() + "/Materials/M_"));
			EXPECT_EQ(Parent->GetImportProvenance().StructuralKey, Material->GetImportProvenance().StructuralKey);
			EXPECT_EQ(Material->GetImportProvenance().OutputIdentity, Output.StableIdentity);
			EXPECT_LT(Parent->GetParameterDefinitions().size(), 48u);
			EXPECT_EQ(std::ranges::count_if(Parent->GetExpressionCollection().Expressions,
				[](const auto& Expression) { return Durin::Cast<Durin::DMaterialExpressionTextureSampleParameter2D>(Expression.Get()) != nullptr; }), 1);
			EXPECT_TRUE(Material->GetMaterialCompileStatus().IsCurrent());
			EXPECT_TRUE(Material->GetAcceptedCompiledProgram());

		}
		if (const auto* Texture = Durin::Cast<Durin::DTexture2D>(Object))
		{
			bSawTexture = true;
			ASSERT_NE(Texture->GetAssetImportData(), nullptr);
			const Durin::FSourceFile* Source = Texture->GetAssetImportData()
				->GetSourceData().FindByRole("source");
			ASSERT_NE(Source, nullptr);
			EXPECT_FALSE(Source->GetContentHash().IsZero());
			EXPECT_NE(Source->GetContentHash(), Texture->GetSource().GetIdentity());
		}
	}
	EXPECT_TRUE(bSawTexture);
	// The family-only geometry route must also retain authored material slots.
	const auto ReimportMesh = Durin::AssetForge::Builtins::ImportStaticMeshForTest(
		Fixture.Source, "/SceneImportTests/ReimportGeometry");
	ASSERT_TRUE(ReimportMesh) << ReimportMesh.Message;
	Durin::DMaterial* Instance = nullptr;
	for (const auto& Output : Imported.Outputs)
	{
		Durin::DObject* Object = nullptr;
		{
			auto LoadedValue = Durin::LoadObjectDetailed<Durin::DObject>(Durin::Testing::MakePackageLeafAssetObjectPathForTests(Output.AssetPath));
			Object = LoadedValue.value_or(nullptr);
			ASSERT_TRUE(LoadedValue);
		}
		if (auto* Candidate = Durin::Cast<Durin::DMaterial>(Object)) Instance = Candidate;
	}
	ASSERT_NE(Instance, nullptr);
	ASSERT_GT(ReimportMesh.Asset->GetNumMaterialSlots(), 0u);
	ReimportMesh.Asset->SetMaterialSlotDefaultMaterial(0, Instance);
	const auto Slots = std::vector(ReimportMesh.Asset->GetMaterialSlots().begin(), ReimportMesh.Asset->GetMaterialSlots().end());
	Durin::FReimportResult Reimported;
	Durin::FReimportManager::Reimport(*ReimportMesh.Asset, {}, [&](Durin::FReimportResult Result) { Reimported = std::move(Result); });
	Durin::FAssetCompilingManager::Get().FinishCompilationForObject(*ReimportMesh.Asset);
	EXPECT_EQ(Reimported.Status, Durin::EReimportResult::Failed);
	EXPECT_TRUE(std::ranges::equal(ReimportMesh.Asset->GetMaterialSlots(), Slots));

}

TEST(FSceneImportTests, SavedGltfOutputsReloadWithoutSourcesAndRejectEveryReimportRoute)
{
	using namespace Durin;
	using namespace Durin::AssetForge::Builtins;
	const auto Fixture = InitializeFixture("OneTimeReload");
	const auto Initial = ImportAssetOutputs(Fixture.Source, Fixture.DestinationDirectory,
		FStaticMeshImportSettings::MakeDurin(), {}, {}, {}, FAssetPackageSaveArgs{});
	ASSERT_TRUE(Initial) << Initial.Message;
	for (const std::string_view Class : {"Durin::DStaticMesh", "Durin::DMaterial", "Durin::DTexture2D"})
		for (const auto& Output : Initial.Outputs)
			if (Output.AssetClassName == Class) ASSERT_TRUE(UnloadPackage(Output.AssetPath));
	CollectGarbage();
	ASSERT_TRUE(std::filesystem::remove(Fixture.Source));
	auto* MeshFactory = Cast<DStaticMeshFactory>(DStaticMeshFactory::StaticClass()->GetDefaultObject());
	auto* TextureFactory = Cast<DTexture2DFactory>(DTexture2DFactory::StaticClass()->GetDefaultObject());
	for (const auto& Output : Initial.Outputs)
	{
		auto Loaded = LoadObjectDetailed<DObject>(Testing::MakePackageLeafAssetObjectPathForTests(Output.AssetPath));
		ASSERT_TRUE(Loaded) << Loaded.error().Message;
		auto* Object = *Loaded;
		EXPECT_TRUE(IsOneTimeModelImportedAsset(*Object));
		const auto Capabilities = FReimportManager::GetCapabilities(*Object);
		EXPECT_FALSE(Capabilities.bCanReimport);
		EXPECT_FALSE(Capabilities.bCanReimportFromFile);
		FReimportResult Reimported;
		FReimportManager::Reimport(*Object, {}, [&](auto Result) { Reimported = std::move(Result); });
		EXPECT_EQ(Reimported.Status, EReimportResult::Failed);
		const std::array Files{Fixture.Source};
		FReimportManager::ReimportFromFiles(*Object, Files, {}, [&](auto Result) { Reimported = std::move(Result); });
		EXPECT_EQ(Reimported.Status, EReimportResult::Failed);
		if (auto* Mesh = Cast<DStaticMesh>(Object))
		{
			FAssetCompilingManager::Get().FinishCompilationForObject(*Mesh);
			EXPECT_NE(Mesh->GetRenderData(), nullptr);
			EXPECT_FALSE(MeshFactory->GetReimportCapabilities(*Mesh).bCanReimportFromFile);
			MeshFactory->ReimportFromFiles(*Mesh, Files, [&](auto Result) { Reimported = std::move(Result); });
			EXPECT_EQ(Reimported.Status, EReimportResult::Failed);
			EXPECT_FALSE(ReimportStaticMesh(*Mesh).Message.empty());
			EXPECT_FALSE(ReimportStaticMeshFromFile(*Mesh, "Triangle.obj").Message.empty());
		}
		if (auto* Texture = Cast<DTexture2D>(Object))
		{
			EXPECT_TRUE(Texture->GetSource().IsValid());
			EXPECT_FALSE(TextureFactory->GetReimportCapabilities(*Texture).bCanReimportFromFile);
			TextureFactory->Reimport(*Texture, [&](auto Result) { Reimported = std::move(Result); });
			EXPECT_EQ(Reimported.Status, EReimportResult::Failed);
			TextureFactory->ReimportFromFiles(*Texture, Files, [&](auto Result) { Reimported = std::move(Result); });
			EXPECT_EQ(Reimported.Status, EReimportResult::Failed);
			EXPECT_FALSE(ReimportTexture2D(*Texture).Message.empty());
			EXPECT_FALSE(ReimportTexture2DFromFile(*Texture, "Red.png").Message.empty());
		}
		if (auto* Material = Cast<DMaterial>(Object))
		{
			const auto Revision = Material->GetPackage()->GetEditRevision();
			auto Properties = Material->GetStaticProperties();
			Properties.bTwoSided = !Properties.bTwoSided;
			ASSERT_TRUE(Material->SetStaticProperties(Properties));
			EXPECT_GT(Material->GetPackage()->GetEditRevision(), Revision);
		}
	}
}

TEST(FSceneImportTests, FbxSceneImportRejectsImplicitUpdates)
{
	using namespace Durin;
	using namespace Durin::AssetForge::Builtins;
	const auto Fixture = InitializeFixture("OneTimeFbx", "StaticModelMaterials/PhongMaterial.fbx");
	const auto Imported = ImportAssetOutputs(Fixture.Source, Fixture.DestinationDirectory,
		FStaticMeshImportSettings::MakeDurin(),
		{.MeshMode = EAssetImportMeshMode::Combined});
	ASSERT_TRUE(Imported) << Imported.Message;
	EXPECT_FALSE(Imported.ImportedPackages.empty());
	const auto Repeated = ImportAssetOutputs(Fixture.Source, Fixture.DestinationDirectory,
		FStaticMeshImportSettings::MakeDurin(),
		{.MeshMode = EAssetImportMeshMode::Combined});
	EXPECT_FALSE(Repeated);
	EXPECT_TRUE(Repeated.ImportedPackages.empty());
	for (const auto& Output : Imported.Outputs)
	{
		auto Loaded = LoadObjectDetailed<DObject>(Testing::MakePackageLeafAssetObjectPathForTests(Output.AssetPath));
		ASSERT_TRUE(Loaded);
		EXPECT_TRUE(IsOneTimeModelImportedAsset(**Loaded));
		EXPECT_FALSE(FReimportManager::GetCapabilities(**Loaded).bCanReimport);
	}
}

TEST(FSceneImportTests, LegacyStandaloneModelReceiptsRemainReadableWithoutReimport)
{
	using namespace Durin;
	using namespace Durin::AssetForge::Builtins;
	for (const bool bFbx : {false, true})
	{
		const auto Fixture = InitializeFixture(bFbx ? "LegacyFbx" : "LegacyGltf", bFbx
				? "StaticModelMaterials/PhongMaterial.fbx" : "StaticModelMaterials/RenderedOpaqueDataUri.gltf");
		const auto Imported = ImportStaticMeshForTest(Fixture.Source, bFbx ? "/SceneImportTests/LegacyFbxGeometry" : "/SceneImportTests/LegacyGltfGeometry");
		ASSERT_TRUE(Imported) << Imported.Message;
		EXPECT_TRUE(IsOneTimeModelImportedAsset(*Imported.Asset));
		auto* Factory = Cast<DStaticMeshFactory>(DStaticMeshFactory::StaticClass()->GetDefaultObject());
		EXPECT_FALSE(Factory->GetReimportCapabilities(*Imported.Asset).bCanReimport);
		const std::array Files{std::string("Triangle.obj")};
		FReimportResult Result;
		Factory->ReimportFromFiles(*Imported.Asset, Files, [&](auto Value) { Result = std::move(Value); });
		EXPECT_EQ(Result.Status, EReimportResult::Failed);
		EXPECT_FALSE(ReimportStaticMesh(*Imported.Asset).Message.empty());
		EXPECT_TRUE(Imported.Asset->GetAssetImportData()->Validate());
		const auto Path = Imported.Asset->GetPackage()->GetPackagePathIdentity();
		ASSERT_TRUE(UnloadPackage(Path));
		CollectGarbage();
		ASSERT_TRUE(std::filesystem::remove(Fixture.Source));
		auto Reloaded = LoadObjectDetailed<DStaticMesh>(Testing::MakePackageLeafAssetObjectPathForTests(Path));
		ASSERT_TRUE(Reloaded);
		FAssetCompilingManager::Get().FinishCompilationForObject(**Reloaded);
		EXPECT_NE((*Reloaded)->GetRenderData(), nullptr);
		EXPECT_TRUE(IsOneTimeModelImportedAsset(**Reloaded));

	}
}

TEST(FSceneImportTests, StandaloneMeshReplacementCannotIntroduceGltfOrFbxSources)
{
	using namespace Durin;
	using namespace Durin::AssetForge::Builtins;
	const auto Fixture = InitializeFixture("ReplacementFormats");
	const auto Imported = ImportStaticMeshForTest((std::filesystem::path(DURIN_TEST_DATA_DIR) / "Triangle.obj").generic_string(), "/SceneImportTests/ObjReplacement");
	ASSERT_TRUE(Imported) << Imported.Message;
	auto* Factory = Cast<DStaticMeshFactory>(DStaticMeshFactory::StaticClass()->GetDefaultObject());
	EXPECT_TRUE(Factory->GetReimportCapabilities(*Imported.Asset).bCanReimport);
	for (const auto* Source : {"replacement.GLTF", "replacement.GLB", "replacement.FBX"})
	{
		const std::array Files{std::string(Source)};
		FReimportResult Result;
		Factory->ReimportFromFiles(*Imported.Asset, Files, [&](auto Value) { Result = std::move(Value); });
		EXPECT_EQ(Result.Status, EReimportResult::Failed);
		EXPECT_FALSE(ReimportStaticMeshFromFile(*Imported.Asset, Source).Message.empty());
	}
	const auto Dialogs = Factory->GetSourceFileDialogs(*Imported.Asset);
	ASSERT_EQ(Dialogs.size(), 1u);
	EXPECT_EQ(Dialogs.front().Pattern.find("*.fbx"), std::string::npos);
	EXPECT_EQ(Dialogs.front().Pattern.find("*.gltf"), std::string::npos);
}

TEST(FSceneImportTests, SaveFaultsKeepImportedGraphsAndUseOrdinaryRetry)
{
	using namespace Durin;
	using namespace Durin::AssetForge;
	using namespace Durin::AssetForge::Builtins;
	const auto Fixture = InitializeFixture("SaveRetry");
	for (const auto Failure : {Testing::EPackageSaveFaultPoint::CreateDirectories,
		Testing::EPackageSaveFaultPoint::StagePackage, Testing::EPackageSaveFaultPoint::PublishPackage,
		Testing::EPackageSaveFaultPoint::PublishRegistry})
	{
		const auto Destination = MakeAssetPath(Fixture.DestinationDirectory.ToString() + std::format("/Fault{}", static_cast<int>(Failure)));
		FAssetImportResult Result;
		bool bInjected = false;
		{
			Testing::FScopedPackageSaveFault Fault([&](Testing::EPackageSaveFaultPoint Phase, size_t) {
				if (Phase != Failure) return false;
				bInjected = true;
				return true;
			});
			Result = ImportAssetOutputs(Fixture.Source, Destination,
				FStaticMeshImportSettings::MakeDurin(),
				{.MeshMode = EAssetImportMeshMode::Combined}, {}, {}, FAssetPackageSaveArgs{});
		}
		ASSERT_TRUE(Result) << Result.Message;
		EXPECT_TRUE(bInjected);
		ASSERT_TRUE(Result.SaveResult);
		EXPECT_FALSE(*Result.SaveResult);
		ASSERT_FALSE(Result.ImportedPackages.empty());
		std::vector<DPackage*> Packages;
		for (const auto& Path : Result.ImportedPackages)
		{
			auto* Package = FindResidentPackage(Path);
			ASSERT_NE(Package, nullptr);
			EXPECT_FALSE(Package->IsGraphPrivate());
			Packages.push_back(Package);
		}
		// Registry publication failure keeps valid committed content; reconcile it before retry.
		if (Failure == Testing::EPackageSaveFaultPoint::PublishRegistry) ASSERT_TRUE(RefreshAssetRegistry());
		ASSERT_TRUE(SavePackages(Packages));
		for (auto* Package : Packages)
		{
			EXPECT_FALSE(Package->IsDirty());
			EXPECT_TRUE(FindPackageExact(Package->GetPackagePathIdentity()));
		}
	}
}

TEST(FSceneImportTests, SourceTransformsMaskFactorAndTexturelessEmissiveArePublished)
{
	using namespace Durin;
	const auto Fixture = InitializeFixture("SourceValues");
	std::ifstream Input(Fixture.Source);
	std::string Source((std::istreambuf_iterator<char>(Input)), {});
	Input.close();
	const auto Replace = [&](std::string_view Old, std::string_view New) {
		const auto Position = Source.find(Old);
		if (Position == std::string::npos) return false;
		Source.replace(Position, Old.size(), New);
		return true;
	};
	ASSERT_TRUE(Replace("\"baseColorTexture\": {\"index\": 0}",
		R"("baseColorTexture": {"index":0,"texCoord":1,"extensions":{"KHR_texture_transform":{"offset":[0.25,0.5],"scale":[2,3],"rotation":0.4}}})"));
	ASSERT_TRUE(Replace("[0.5, 0.75, 0.25, 1.0]", "[0.5, 0.75, 0.25, 0.4]"));
	ASSERT_TRUE(Replace("\"textures\": [{\"source\": 0}]",
		R"("samplers":[{"minFilter":9728,"magFilter":9728,"wrapS":33071,"wrapT":33648}],"textures":[{"source":0,"sampler":0}])"));
	ASSERT_TRUE(Replace("\"pbrMetallicRoughness\":", "\"alphaMode\":\"MASK\",\"emissiveFactor\":[2,3,4],\"pbrMetallicRoughness\":"));
	std::ofstream OutputFile(Fixture.Source, std::ios::trunc);
	OutputFile << Source;
	OutputFile.close();
	const auto Imported = RunScene(Fixture);
	ASSERT_TRUE(Imported) << Imported.Message;
	DMaterial* Instance = nullptr;
	for (const auto& Output : Imported.Outputs)
		if (Output.Role == "Material")
			{
				auto LoadedValue = LoadObjectDetailed<DMaterial>(Testing::MakePackageLeafAssetObjectPathForTests(Output.AssetPath));
				Instance = LoadedValue.value_or(nullptr);
				ASSERT_TRUE(LoadedValue);
			}
	ASSERT_NE(Instance, nullptr);
	using Kind = Durin::AssetForge::Builtins::MaterialParameters::EMaterialBuiltinParameterKind;
	FResolvedMaterialParameter Parameter;
	ASSERT_TRUE(Instance->ResolveParameterValue(Durin::AssetForge::Builtins::GetMaterialSurfaceParameterId(EMaterialSurfaceOutput::BaseColor, Kind::Texture), Parameter));
	EXPECT_EQ(Parameter.Value.GetTexture().SamplerState.MinFilter, EMaterialSamplerMinFilter::Nearest);
	EXPECT_EQ(Parameter.Value.GetTexture().SamplerState.MagFilter, EMaterialSamplerMagFilter::Nearest);
	EXPECT_EQ(Parameter.Value.GetTexture().SamplerState.AddressU, EMaterialSamplerAddressMode::ClampToEdge);
	EXPECT_EQ(Parameter.Value.GetTexture().SamplerState.AddressV, EMaterialSamplerAddressMode::MirroredRepeat);
	ASSERT_TRUE(Instance->ResolveParameterValue(Durin::AssetForge::Builtins::GetMaterialSurfaceParameterId(EMaterialSurfaceOutput::BaseColor, Kind::UVChannel), Parameter));
	EXPECT_EQ(Parameter.Value.GetScalar(), 1);
	ASSERT_TRUE(Instance->ResolveParameterValue(Durin::AssetForge::Builtins::GetMaterialSurfaceParameterId(EMaterialSurfaceOutput::BaseColor, Kind::UVScale), Parameter));
	EXPECT_EQ(FVector2(Parameter.Value.GetVector4()), FVector2(2, 3));
	ASSERT_TRUE(Instance->ResolveParameterValue(Durin::AssetForge::Builtins::GetMaterialSurfaceParameterId(EMaterialSurfaceOutput::BaseColor, Kind::UVOffset), Parameter));
	EXPECT_EQ(FVector2(Parameter.Value.GetVector4()), FVector2(.25, .5));
	ASSERT_TRUE(Instance->ResolveParameterValue(Durin::AssetForge::Builtins::GetMaterialSurfaceParameterId(EMaterialSurfaceOutput::BaseColor, Kind::UVRotation), Parameter));
	EXPECT_FLOAT_EQ(Parameter.Value.GetScalar(), .4f);
	ASSERT_TRUE(Instance->ResolveParameterValue(Durin::AssetForge::Builtins::GetMaterialSurfaceParameterId(EMaterialSurfaceOutput::OpacityMask, Kind::Value), Parameter));
	EXPECT_FLOAT_EQ(Parameter.Value.GetScalar(), .4f);
	ASSERT_TRUE(Instance->ResolveParameterValue(Durin::AssetForge::Builtins::GetMaterialSurfaceParameterId(EMaterialSurfaceOutput::Emissive, Kind::Value), Parameter));
	EXPECT_EQ(FVector3(Parameter.Value.GetVector4()), FVector3(2, 3, 4));
	EXPECT_EQ(Instance->FindParameterDefinition(Durin::AssetForge::Builtins::GetMaterialSurfaceParameterId(EMaterialSurfaceOutput::Emissive, Kind::Texture)), nullptr);
	EXPECT_EQ(Instance->FindParameterDefinition(Durin::AssetForge::Builtins::GetMaterialSurfaceParameterId(EMaterialSurfaceOutput::Opacity, Kind::Value)), nullptr);
}

TEST(FSceneImportTests, PackedSourceChannelsPublishOneLinearSampleOwner)
{
	using namespace Durin;
	const auto Fixture = InitializeFixture("PackedChannels");
	std::ifstream Input(Fixture.Source);
	std::string Source((std::istreambuf_iterator<char>(Input)), {});
	Input.close();
	const std::string TextureField = "\"baseColorTexture\": {\"index\": 0}";
	const auto TexturePosition = Source.find(TextureField);
	ASSERT_NE(TexturePosition, std::string::npos);
	Source.replace(TexturePosition, TextureField.size(), "\"metallicRoughnessTexture\": {\"index\": 0}");
	const auto MaterialPosition = Source.find("\"pbrMetallicRoughness\":");
	ASSERT_NE(MaterialPosition, std::string::npos);
	Source.insert(MaterialPosition, "\"occlusionTexture\": {\"index\": 0}, ");
	std::ofstream OutputFile(Fixture.Source, std::ios::trunc);
	OutputFile << Source;
	OutputFile.close();
	const auto Imported = RunScene(Fixture);
	ASSERT_TRUE(Imported) << Imported.Message;
	DMaterial* Instance = nullptr;
	uint32 TextureCount = 0;
	for (const auto& Output : Imported.Outputs)
	{
		if (Output.AssetClassName == "Durin::DTexture2D") ++TextureCount;
		if (Output.Role == "Material")
			{
				auto LoadedValue = LoadObjectDetailed<DMaterial>(Testing::MakePackageLeafAssetObjectPathForTests(Output.AssetPath));
				Instance = LoadedValue.value_or(nullptr);
				ASSERT_TRUE(LoadedValue);
			}
	}
	ASSERT_NE(Instance, nullptr);
	EXPECT_EQ(TextureCount, 1u);
	const auto* Parent = Instance;
	ASSERT_NE(Parent, nullptr);
	EXPECT_EQ(std::ranges::count_if(Parent->GetExpressionCollection().Expressions,
		[](const auto& Expression) { return Cast<DMaterialExpressionTextureSampleParameter2D>(Expression.Get()) != nullptr; }), 1);
	const auto& Outputs = Parent->GetExpressionOutputs();
	EXPECT_EQ(Outputs.Metallic.Connection.ExpressionId, Outputs.Roughness.Connection.ExpressionId);
	EXPECT_EQ(Outputs.Metallic.Connection.ExpressionId, Outputs.AmbientOcclusion.Connection.ExpressionId);
	EXPECT_EQ(Outputs.Metallic.Connection.OutputIndex, 4);
	EXPECT_EQ(Outputs.Roughness.Connection.OutputIndex, 3);
	EXPECT_EQ(Outputs.AmbientOcclusion.Connection.OutputIndex, 2);
	EXPECT_EQ(Instance->GetParameterDefinitions().size(), 2u);
	EXPECT_FALSE(Instance->GetImportProvenance().OutputIdentity.empty());
	DTexture2D* Texture = nullptr;
	ASSERT_TRUE(Instance->GetTextureParameterValue(Durin::AssetForge::Builtins::MaterialParameters::MetallicTextureName(), Texture));
	ASSERT_NE(Texture, nullptr);
	EXPECT_EQ(Texture->GetUsage(), ETextureUsage::DataMask);
}

TEST(FSceneImportTests, ExplicitParentsReuseAcrossDestinationsAndRejectIncompatibleGraphs)
{
	using namespace Durin;
	using namespace Durin::AssetForge::Builtins;
	const auto Fixture = InitializeFixture("ParentReuse");
	const auto First = RunScene(Fixture);
	ASSERT_TRUE(First) << First.Message;
	DMaterial* Instance = nullptr;
	for (const auto& Output : First.Outputs)
		if (Output.Role == "Material")
			{
				auto LoadedValue = LoadObjectDetailed<DMaterial>(Testing::MakePackageLeafAssetObjectPathForTests(Output.AssetPath));
				Instance = LoadedValue.value_or(nullptr);
				ASSERT_TRUE(LoadedValue);
			}
	ASSERT_NE(Instance, nullptr);
	auto* Parent = Instance;
	ASSERT_NE(Parent, nullptr);
	const auto Revision = Parent->GetPackage()->GetEditRevision();
	FSceneMaterialImportOptions Options;
	Options.Default = {ESceneMaterialImportMode::CreateInstances, Parent->GetObjectPath()};
	auto Second = ImportAssetOutputs(Fixture.Source, MakeAssetPath("/SceneImportTests/Second"),
		FStaticMeshImportSettings::MakeDurin(),
		{.MeshMode = EAssetImportMeshMode::Combined}, Options, {}, {});
	ASSERT_TRUE(Second) << Second.Message;
	for (const auto& Output : Second.Outputs)
		if (Output.Role == "MaterialInstance")
		{
			DMaterialInstance* Reused = nullptr;
			{
				auto LoadedValue = LoadObjectDetailed<DMaterialInstance>(Testing::MakePackageLeafAssetObjectPathForTests(Output.AssetPath));
				Reused = LoadedValue.value_or(nullptr);
				ASSERT_TRUE(LoadedValue);
			}
			EXPECT_EQ(Reused->GetParent(), Parent);
		}
	EXPECT_EQ(Parent->GetPackage()->GetEditRevision(), Revision);
	EXPECT_FALSE(Parent->GetPackage()->IsDirty());
	const auto Original = Parent->GetExpressionOutputs();
	const auto ApplyOutputs = [&](const FMaterialExpressionSurfaceOutputs& Outputs) {
		std::vector<DMaterialExpression*> Expressions;
		for (const auto& Expression : Parent->GetExpressionCollection().Expressions) Expressions.push_back(Expression.Get());
		return Parent->SetMaterialExpressions(Expressions, Outputs);
	};
	auto Changed = Original;
	Changed.AmbientOcclusion.SetConstant({.3f});
	ASSERT_TRUE(ApplyOutputs(Changed));
	auto Rejected = ImportAssetOutputs(Fixture.Source, MakeAssetPath("/SceneImportTests/Rejected"),
		FStaticMeshImportSettings::MakeDurin(),
		{.MeshMode = EAssetImportMeshMode::Combined}, Options, {}, {});
	EXPECT_FALSE(Rejected);
	EXPECT_NE(Rejected.Message.find("graph"), std::string::npos);
	EXPECT_EQ(Parent->GetExpressionOutputs(), Changed);
	for (const auto& Output : Rejected.Outputs) EXPECT_FALSE(FindPackageExact(Output.AssetPath));
	ASSERT_TRUE(ApplyOutputs(Original));
}

TEST(FSceneImportTests, RepeatedGltfImportRejectsOccupiedOutputsAndPreservesEdits)
{
	using namespace Durin;
	using namespace Durin::AssetForge::Builtins;
	const auto Fixture = InitializeFixture("PreserveEdits");
	const auto First = RunScene(Fixture);
	DMaterial* Material = nullptr;
	DStaticMesh* Mesh = nullptr;
	for (const auto& Output : First.Outputs)
	{
		auto* Object = LoadObjectDetailed<DObject>(Testing::MakePackageLeafAssetObjectPathForTests(Output.AssetPath)).value_or(nullptr);
		if (auto* Value = Cast<DMaterial>(Object)) Material = Value;
		if (auto* Value = Cast<DStaticMesh>(Object)) Mesh = Value;
	}
	ASSERT_NE(Material, nullptr);
	ASSERT_NE(Mesh, nullptr);
	using Kind = MaterialParameters::EMaterialBuiltinParameterKind;
	const auto Color = GetMaterialSurfaceParameterId(EMaterialSurfaceOutput::BaseColor, Kind::Value);
	ASSERT_TRUE(Material->SetParameterValue(Color, FMaterialParameterValue::MakeVector4({.2, .3, .4, 0})));
	auto Properties = Material->GetStaticProperties();
	Properties.bTwoSided = true;
	ASSERT_TRUE(Material->SetStaticProperties(Properties));
	FTopLevelAssetPath CustomPath;
	ASSERT_TRUE(FTopLevelAssetPath::TryCreate(MakeAssetPath("/SceneImportTests/Custom/PreservedBinding"), "PreservedBinding", CustomPath));
	std::string CreatedError;
	Durin::FFeedbackContext CreatedFeedback({}, [&](Durin::ELogLevel Level, std::string_view Message) {
		if (Level >= Durin::ELogLevel::Error) { if (!CreatedError.empty()) CreatedError += "\n"; CreatedError += Message; }
	});
	const auto Created = IAssetTools::Get().CreateAsset(CustomPath, DMaterialInstance::StaticClass(), nullptr, nullptr, Durin::EObjectFlags::Public | Durin::EObjectFlags::Transactional, &CreatedFeedback);
	ASSERT_TRUE(Created) << CreatedError;
	auto* Custom = Cast<DMaterialInstance>(Created);
	ASSERT_NE(Custom, nullptr);
	ASSERT_TRUE(Custom->SetParent(Material));
	FAssetCompilingManager::Get().FinishAllCompilation();
	ASSERT_TRUE(SavePackage(Custom->GetPackage()));
	Mesh->SetMaterialSlotDefaultMaterial(0, Custom);
	DTexture2D* PreviousTexture = nullptr;
	ASSERT_TRUE(Material->GetTextureParameterValue(MaterialParameters::BaseColorTextureName(), PreviousTexture));
	const auto MaterialPath = Material->GetPackage()->GetPackagePathIdentity();
	const auto MeshPath = Mesh->GetPackage()->GetPackagePathIdentity();
	const auto Revision = Material->GetPackage()->GetEditRevision();
	const auto Reimport = ImportAssetOutputs(Fixture.Source, Fixture.DestinationDirectory,
		FStaticMeshImportSettings::MakeDurin(),
		{.MeshMode = EAssetImportMeshMode::Combined});
	EXPECT_FALSE(Reimport);
	EXPECT_TRUE(Reimport.ImportedPackages.empty());
	EXPECT_EQ(Material->GetPackage()->GetEditRevision(), Revision);
	EXPECT_EQ(LoadObjectDetailed<DMaterial>(Testing::MakePackageLeafAssetObjectPathForTests(MaterialPath)).value_or(nullptr), Material);
	EXPECT_EQ(LoadObjectDetailed<DStaticMesh>(Testing::MakePackageLeafAssetObjectPathForTests(MeshPath)).value_or(nullptr), Mesh);
	EXPECT_EQ(Mesh->GetMaterialSlots()[0].DefaultMaterial.Get(), Custom);
	DTexture2D* CurrentTexture = nullptr;
	ASSERT_TRUE(Material->GetTextureParameterValue(MaterialParameters::BaseColorTextureName(), CurrentTexture));
	EXPECT_EQ(CurrentTexture, PreviousTexture);
	EXPECT_TRUE(Material->GetStaticProperties().bTwoSided);
}

TEST(FSceneImportTests, PerMaterialOverridesMissingParentsAndTypeChangesAreExplicit)
{
	using namespace Durin;
	using namespace Durin::AssetForge::Builtins;
	const auto Fixture = InitializeFixture("MixedModes", "MultiSection.gltf");
	const auto First = RunScene(Fixture);
	DMaterial* Parent = nullptr;
	for (const auto& Output : First.Outputs)
		if (Output.Role == "Material") Parent = LoadObjectDetailed<DMaterial>(Testing::MakePackageLeafAssetObjectPathForTests(Output.AssetPath)).value_or(nullptr);
	ASSERT_NE(Parent, nullptr);
	const auto Destination = MakeAssetPath("/SceneImportTests/MixedOutput");
	FAssetImportSession Session(Fixture.Source, Destination, FStaticMeshImportSettings::MakeDurin(),
		{.MeshMode = EAssetImportMeshMode::Combined});
	ASSERT_TRUE(AdvanceSceneSession(Session, EAssetImportPhase::Ready));
	auto Preview = Session.PreviewOutputs(Destination);
	ASSERT_TRUE(Preview.bSucceeded) << Preview.Message;
	ASSERT_EQ(Preview.Materials.size(), 2u);
	FSceneMaterialImportOptions Options;
	Options.Default.Mode = ESceneMaterialImportMode::CreateInstances;
	auto Rejected = ImportAssetOutputs(Fixture.Source, Destination,
		FStaticMeshImportSettings::MakeDurin(),
		{.MeshMode = EAssetImportMeshMode::Combined}, Options, {}, {});
	EXPECT_FALSE(Rejected);
	EXPECT_TRUE(Rejected.ImportedPackages.empty());
	for (const auto& Output : Rejected.Outputs) EXPECT_FALSE(FindPackageExact(Output.AssetPath));
	Options.Default.ParentMaterialPath = Parent->GetObjectPath();
	Options.Overrides.push_back({Preview.Materials[0].StableIdentity, {ESceneMaterialImportMode::CreateMaterials}});
	Preview = Session.PreviewOutputs(Destination, Options);
	ASSERT_TRUE(Preview.bSucceeded) << Preview.Message;
	EXPECT_TRUE(Preview.Materials[0].AssetPath.GetPackageName().starts_with("M_"));
	EXPECT_TRUE(Preview.Materials[1].AssetPath.GetPackageName().starts_with("MI_"));
	auto DuplicateOptions = Options;
	DuplicateOptions.Overrides.push_back(Options.Overrides.front());
	const auto Duplicate = Session.PreviewOutputs(Destination, DuplicateOptions);
	EXPECT_FALSE(Duplicate.bSucceeded);
	EXPECT_NE(Duplicate.Message.find("duplicate"), std::string::npos);
	ASSERT_TRUE(Session.BeginImport(Destination, Options));
	ASSERT_TRUE(AdvanceSceneSession(Session, EAssetImportPhase::Completed));
	const auto& Imported = Session.GetResult();
	ASSERT_TRUE(Imported) << Imported.Message;
	ASSERT_EQ(Imported.Outputs.size(), Preview.Outputs.size());
	for (size_t Index = 0; Index < Preview.Outputs.size(); ++Index)
	{
		EXPECT_EQ(Imported.Outputs[Index].StableIdentity, Preview.Outputs[Index].StableIdentity);
		EXPECT_EQ(Imported.Outputs[Index].AssetPath, Preview.Outputs[Index].AssetPath);
	}
	EXPECT_EQ(std::ranges::count(Imported.Outputs, "Material", &Durin::AssetForge::FImportOutputSummary::Role), 1);
	EXPECT_EQ(std::ranges::count(Imported.Outputs, "MaterialInstance", &Durin::AssetForge::FImportOutputSummary::Role), 1);
	const auto Repeated = ImportAssetOutputs(Fixture.Source, Destination,
		FStaticMeshImportSettings::MakeDurin(),
		{.MeshMode = EAssetImportMeshMode::Combined});
	EXPECT_FALSE(Repeated);
	EXPECT_TRUE(Repeated.ImportedPackages.empty());
}

TEST(FSceneImportTests, DuplicateSourceNamesDoNotPermitImplicitGltfUpdates)
{
	using namespace Durin;
	const auto Fixture = InitializeFixture("DuplicateSlotNames", "MultiSection.gltf");
	std::ifstream Input(Fixture.Source);
	std::string Source((std::istreambuf_iterator<char>(Input)), {});
	Input.close();
	const auto Name = Source.find("\"Blue\"");
	ASSERT_NE(Name, std::string::npos);
	Source.replace(Name, 6, "\"Red\"");
	std::ofstream(Fixture.Source, std::ios::trunc) << Source;
	const auto First = RunScene(Fixture);
	DStaticMesh* Mesh = nullptr;
	FPackagePath MeshPath;
	for (const auto& Output : First.Outputs)
		if (Output.Role == "StaticMesh")
		{
			MeshPath = Output.AssetPath;
			Mesh = LoadObjectDetailed<DStaticMesh>(Testing::MakePackageLeafAssetObjectPathForTests(MeshPath)).value_or(nullptr);
		}
	ASSERT_NE(Mesh, nullptr);
	ASSERT_EQ(Mesh->GetMaterialSlots().size(), 2u);
	auto* FirstMaterial = Mesh->GetMaterialSlots()[0].DefaultMaterial.Get();
	auto* SecondMaterial = Mesh->GetMaterialSlots()[1].DefaultMaterial.Get();
	ASSERT_NE(FirstMaterial, SecondMaterial);
	Mesh->SetMaterialSlotDefaultMaterial(0, SecondMaterial);
	Mesh->SetMaterialSlotDefaultMaterial(1, FirstMaterial);
	const auto Reimport = Durin::AssetForge::Builtins::ImportAssetOutputs(Fixture.Source, Fixture.DestinationDirectory,
		FStaticMeshImportSettings::MakeDurin(),
		{.MeshMode = Durin::AssetForge::Builtins::EAssetImportMeshMode::Combined});
	EXPECT_FALSE(Reimport);
	EXPECT_TRUE(Reimport.ImportedPackages.empty());
	Mesh = LoadObjectDetailed<DStaticMesh>(Testing::MakePackageLeafAssetObjectPathForTests(MeshPath)).value_or(nullptr);
	ASSERT_NE(Mesh, nullptr);
	EXPECT_EQ(Mesh->GetMaterialSlots()[0].DefaultMaterial.Get(), SecondMaterial);
	EXPECT_EQ(Mesh->GetMaterialSlots()[1].DefaultMaterial.Get(), FirstMaterial);
}

TEST(FSceneImportTests, StandardPBRParentMapsPackedChannelsUVsEmissiveAndOpacity)
{
	using namespace Durin;
	using namespace Durin::AssetForge::Builtins;
	const auto Fixture = InitializeFixture("StandardPBR", "StaticModelMaterials/ImportedPbrContract.gltf");
	for (const auto* Dependency : {"Triangle.bin", "Red.png"})
		std::filesystem::copy_file(std::filesystem::path(DURIN_TEST_DATA_DIR) / "StaticModelMaterials" / Dependency,
			std::filesystem::path(Fixture.Source).parent_path() / Dependency, std::filesystem::copy_options::overwrite_existing);
	FTopLevelAssetPath ParentPath;
	ASSERT_TRUE(FTopLevelAssetPath::TryCreate(MakeAssetPath("/SceneImportTests/Shared/StandardPBR"), "StandardPBR", ParentPath));
	std::string CreatedError;
	Durin::FFeedbackContext CreatedFeedback({}, [&](Durin::ELogLevel Level, std::string_view Message) {
		if (Level >= Durin::ELogLevel::Error) { if (!CreatedError.empty()) CreatedError += "\n"; CreatedError += Message; }
	});
	const auto Created = IAssetTools::Get().CreateAsset(ParentPath, DMaterial::StaticClass(), nullptr, nullptr, Durin::EObjectFlags::Public | Durin::EObjectFlags::Transactional, &CreatedFeedback);
	ASSERT_TRUE(Created) << CreatedError;
	auto* Parent = Cast<DMaterial>(Created);
	ASSERT_NE(Parent, nullptr);
	Parent->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	ASSERT_TRUE(MakePBRSurfaceMaterialMRExpressions().Apply(*Parent));
	ASSERT_TRUE(Parent->CompileEdits());
	FAssetCompilingManager::Get().FinishAllCompilation();
	ASSERT_TRUE(SavePackage(Parent->GetPackage()));
	const auto ParentRevision = Parent->GetPackage()->GetEditRevision();
	FSceneMaterialImportOptions Options;
	Options.Default = {ESceneMaterialImportMode::CreateInstances, Parent->GetObjectPath()};
	FAssetImportSession PreviewSession(Fixture.Source, Fixture.DestinationDirectory, FStaticMeshImportSettings::MakeDurin(),
		{.MeshMode = EAssetImportMeshMode::Combined});
	ASSERT_TRUE(AdvanceSceneSession(PreviewSession, EAssetImportPhase::Ready));
	const auto Preview = PreviewSession.PreviewOutputs(Fixture.DestinationDirectory, Options);
	ASSERT_TRUE(Preview.bSucceeded) << Preview.Message;
	EXPECT_EQ(Preview.Materials[0].Message, "Compatible standard PBR mapping");
	using Kind = MaterialParameters::EMaterialBuiltinParameterKind;
	for (const bool bTranslucent : {false, true})
	{
		if (bTranslucent)
		{
			std::ifstream Input(Fixture.Source);
			std::string Source((std::istreambuf_iterator<char>(Input)), {});
			Input.close();
			const auto Mode = Source.find("\"MASK\"");
			ASSERT_NE(Mode, std::string::npos);
			Source.replace(Mode, 6, "\"BLEND\"");
			std::ofstream(Fixture.Source, std::ios::trunc) << Source;
		}
		const auto Destination = bTranslucent ? MakeAssetPath("/SceneImportTests/StandardTranslucent") : Fixture.DestinationDirectory;
		const auto Imported = ImportAssetOutputs(Fixture.Source, Destination,
			FStaticMeshImportSettings::MakeDurin(),
			{.MeshMode = EAssetImportMeshMode::Combined}, Options, {}, {});
		ASSERT_TRUE(Imported) << Imported.Message;
		DMaterialInstance* Instance = nullptr;
		for (const auto& Output : Imported.Outputs)
			if (Output.Role == "MaterialInstance") Instance = LoadObjectDetailed<DMaterialInstance>(Testing::MakePackageLeafAssetObjectPathForTests(Output.AssetPath)).value_or(nullptr);
		ASSERT_NE(Instance, nullptr);
		EXPECT_EQ(Instance->GetParent(), Parent);
		EXPECT_EQ(Instance->GetLocalParameterValueCount(), 48u);
		EXPECT_TRUE(Instance->GetMaterialCompileStatus().IsCurrent());
		EXPECT_EQ(Instance->GetStaticProperties().BlendMode, bTranslucent ? EMaterialBlendMode::Translucent : EMaterialBlendMode::Masked);
		FResolvedMaterialParameter Value;
		ASSERT_TRUE(Instance->ResolveParameterValue(GetMaterialSurfaceParameterId(EMaterialSurfaceOutput::BaseColor, Kind::UVScale), Value));
		EXPECT_EQ(FVector2(Value.Value.GetVector4()), FVector2(2, 3));
		ASSERT_TRUE(Instance->ResolveParameterValue(GetMaterialSurfaceParameterId(EMaterialSurfaceOutput::Emissive, Kind::Value), Value));
		EXPECT_EQ(FVector3(Value.Value.GetVector4()), FVector3(0));
		DTexture2D* Metallic = nullptr;
		DTexture2D* Roughness = nullptr;
		ASSERT_TRUE(Instance->GetTextureParameterValue(MaterialParameters::MetallicTextureName(), Metallic));
		ASSERT_TRUE(Instance->GetTextureParameterValue(MaterialParameters::RoughnessTextureName(), Roughness));
		ASSERT_NE(Metallic, nullptr);
		EXPECT_EQ(Metallic, Roughness);
		DTexture2D* BaseColor = nullptr;
		DTexture2D* Opacity = nullptr;
		DTexture2D* Mask = nullptr;
		ASSERT_TRUE(Instance->GetTextureParameterValue(MaterialParameters::BaseColorTextureName(), BaseColor));
		ASSERT_TRUE(Instance->GetTextureParameterValue(MaterialParameters::OpacityTextureName(), Opacity));
		ASSERT_TRUE(Instance->GetTextureParameterValue(MaterialParameters::OpacityMaskTextureName(), Mask));
		if (bTranslucent) { EXPECT_EQ(Opacity, BaseColor); EXPECT_EQ(Mask, nullptr); }
		else { EXPECT_EQ(Opacity, nullptr); EXPECT_NE(Mask, nullptr); }
	}
	EXPECT_EQ(Parent->GetPackage()->GetEditRevision(), ParentRevision);
	EXPECT_FALSE(Parent->GetPackage()->IsDirty());
}

TEST(FSceneImportTests, AssetForgeRejectsUnsupportedSceneFeaturesWithoutPartialPublication)
{
	FSceneFixture Fixture = InitializeFixture("Unsupported");
	std::ofstream Source(Fixture.Source, std::ios::trunc);
	ASSERT_TRUE(Source.is_open());
	Source << R"({"asset":{"version":"2.0"},"materials":[{"name":"MustNotPublish"}],"skins":[]})";
	Source.close();
	auto Imported = Durin::AssetForge::Builtins::ImportAssetOutputs(Fixture.Source, Fixture.DestinationDirectory,
		Durin::FStaticMeshImportSettings::MakeDurin(),
		{.MeshMode = Durin::AssetForge::Builtins::EAssetImportMeshMode::Combined});
	EXPECT_FALSE(Imported);
	EXPECT_FALSE(Imported.Diagnostics.empty());
	EXPECT_TRUE(Imported.Outputs.empty());
	EXPECT_FALSE(Durin::FindPackageExact(MakeAssetPath(
		"/SceneImportTests/SceneImport/Unsupported/Materials/MustNotPublish")));
}

TEST(FSceneImportTests, DirectImportHonorsCancellationBeforePublication)
{
	const FSceneFixture Fixture = InitializeFixture("Scheduled");
	uint32 CancellationChecks = 0;
	auto Result = Durin::AssetForge::Builtins::ImportAssetOutputs(Fixture.Source, Fixture.DestinationDirectory,
		Durin::FStaticMeshImportSettings::MakeDurin(),
		{.MeshMode = Durin::AssetForge::Builtins::EAssetImportMeshMode::Combined}, {}, [&] { return ++CancellationChecks == 1; });
	EXPECT_FALSE(Result);
	EXPECT_EQ(CancellationChecks, 1u);
	ASSERT_FALSE(Result.Diagnostics.empty());
	EXPECT_EQ(Result.Diagnostics.back().Category, Durin::AssetForge::EImportDiagnosticCategory::Canceled);
	EXPECT_FALSE(Durin::FindPackageExact(
		MakeAssetPath("/SceneImportTests/SceneImport/Scheduled/StaticMeshes/Scheduled")));
}

TEST(FSceneImportTests, RuntimeOutputsDoNotReflectSceneOwnershipState)
{
	InitializeDObjectSystem();
	for (Durin::DClass* Class : {Durin::DStaticMesh::StaticClass(),
		Durin::DMaterialInstance::StaticClass(), Durin::DTexture2D::StaticClass()})
		EXPECT_EQ(Class->FindPropertyByName("ImportOwnership"), nullptr);
}

TEST(FSceneImportTests, StandardFunctionLibraryPreservesEditsAndRejectsIncompatibleInterfaces)
{
	using namespace Durin;
	using namespace Durin::AssetForge::Builtins;
	const auto Fixture = InitializeFixture("StandardLibrary");
	std::string Error;
	FStandardMaterialFunctions Functions;
	// An empty Engine mount must fail without synthesizing any packages.
	ASSERT_FALSE(LoadStandardMaterialFunctions(Functions, Error));
	EXPECT_NE(Error.find("Missing standard function"), std::string::npos);
	EXPECT_FALSE(FindPackageExact(MakeAssetPath("/Engine/Materials/Functions/UVTransform")));
	EXPECT_EQ(FindResidentPackage(MakeAssetPath("/Engine/Materials/Functions/UVTransform")), nullptr);
	const std::array Names{"UVTransform", "SampleNormal", "SampleORM"};
	const std::array Slots{&Functions.UVTransform, &Functions.SampleNormal, &Functions.SampleORM};
	for (uint32 Index = 0; Index < Slots.size(); ++Index)
	{
		const auto Path = MakeAssetPath(std::format("/Engine/Materials/Functions/{}", Names[Index]));
		FTopLevelAssetPath AssetPath;
		ASSERT_TRUE(FTopLevelAssetPath::TryCreate(Path, Names[Index], AssetPath));
		std::string CreatedError;
		Durin::FFeedbackContext CreatedFeedback({}, [&](Durin::ELogLevel Level, std::string_view Message) {
			if (Level >= Durin::ELogLevel::Error) { if (!CreatedError.empty()) CreatedError += "\n"; CreatedError += Message; }
		});
		const auto Created = IAssetTools::Get().CreateAsset(AssetPath, DMaterialFunction::StaticClass(), nullptr, nullptr, Durin::EObjectFlags::Public | Durin::EObjectFlags::Transactional, &CreatedFeedback);
		ASSERT_TRUE(Created) << CreatedError;
		auto* Function = Cast<DMaterialFunction>(Created);
		ASSERT_NE(Function, nullptr);
		ASSERT_TRUE(MakeStandardMaterialFunctionExpressions(
			static_cast<EStandardMaterialFunction>(Index + 1)).Apply(*Function));
		ASSERT_TRUE(SavePackage(Function->GetPackage()));
		*Slots[Index] = Function;
	}
	const auto ObjectRevision = GDObjectArray.GetRevision();
	ASSERT_TRUE(LoadStandardMaterialFunctions(Functions, Error)) << Error;
	// Resident asset admission must not construct and discard a reference graph.
	EXPECT_EQ(GDObjectArray.GetRevision(), ObjectRevision);
	TStrongObjectPtr<DMaterial> MaterialOwner(NewObject<DMaterial>(nullptr, "StandardLibraryFixture"));
	auto* Material = MaterialOwner.Get();
	ASSERT_NE(Material, nullptr);
	auto Compact = Testing::MakeStandardMaterialExpressionsForTest(Functions);
	ASSERT_TRUE(Compact.Apply(*Material));
	EXPECT_EQ(std::ranges::count_if(Material->GetExpressionCollection().Expressions, [](const auto& E) { return Cast<DMaterialExpressionFunctionCall>(E.Get()) != nullptr; }), 1u);
	EXPECT_EQ(Material->GetParameterDefinitions().size(), 48u);
	EXPECT_FALSE(Material->GetExpressionOutputs().Surface.ExpressionId.IsValid());
	const auto& Outputs = Material->GetExpressionOutputs();
	for (const auto& Link : {Outputs.BaseColor, Outputs.Normal, Outputs.Metallic, Outputs.Roughness,
		Outputs.AmbientOcclusion, Outputs.Emissive, Outputs.Opacity, Outputs.OpacityMask}) EXPECT_TRUE(Link.Connection.ExpressionId.IsValid());
	FMaterialCompilerEnvironment Environment;
	const auto EnvironmentResult = BuildDefaultMaterialCompilerEnvironment(Environment);
	ASSERT_TRUE(EnvironmentResult) << EnvironmentResult.error();
	auto InputCapture = SnapshotMaterialCompilerInput(*Material, Environment);
	ASSERT_TRUE(InputCapture);
	auto& Input = InputCapture.Snapshot->Input;
	const auto Normalized = MIR::Normalize(Input);
	ASSERT_TRUE(Normalized) << (Normalized.Diagnostics.empty() ? "no diagnostic" : Normalized.Diagnostics.front().Message);
	EXPECT_EQ(Normalized.Layout.ResourceFieldCount, 6u);
	EXPECT_EQ(std::ranges::count(Normalized.IR.Nodes, EMaterialProgramOpcode::TextureSample2D, &MIR::FNode::Opcode), 6);
	const auto NormalizedSource = GenerateMaterialProgramSlang(Normalized.IR, Normalized.Layout);
	ASSERT_TRUE(NormalizedSource);
	size_t NormalizedSamples = 0;
	for (size_t Offset = 0; (Offset = NormalizedSource.Source.find(".Sample(", Offset)) != std::string::npos; ++Offset)
		++NormalizedSamples;
	EXPECT_EQ(NormalizedSamples, 6u);

	ASSERT_TRUE(Compact.Apply(*Material));
	const auto Clone = [](const auto& Expressions) {
		std::vector<TStrongObjectPtr<DMaterialExpression>> Result;
		for (const auto& Expression : Expressions) Result.emplace_back(DuplicateObject(Expression.Get(), nullptr, NAME_None).value());
		return Result;
	};
	const auto Original = Clone(Functions.SampleNormal->GetExpressionCollection().Expressions);
	const auto OriginalSignature = Functions.SampleNormal->GetFunctionSignature();
	auto Edited = Clone(Original);
	auto Signature = OriginalSignature;
	const auto Apply = [&](const auto& Expressions, const FMaterialFunctionSignature& CandidateSignature) {
		std::vector<DMaterialExpression*> Values;
		for (const auto& Expression : Expressions) Values.push_back(Expression.Get());
		return Functions.SampleNormal->SetFunctionExpressions(Durin::Testing::WithFunctionPorts(CandidateSignature, Values));
	};
	const auto Matches = [&](const auto& Expressions, const FMaterialFunctionSignature& ExpectedSignature) {
		const auto& Actual = Functions.SampleNormal->GetExpressionCollection().Expressions;
		if (Functions.SampleNormal->GetFunctionSignature() != ExpectedSignature || Actual.size() != Expressions.size()) return false;
		bool Equal = true;
		for (size_t Index = 0; Index < Actual.size(); ++Index)
		{
			if (Actual[Index]->GetClass() != Expressions[Index]->GetClass()) return false;
			Actual[Index]->GetClass()->ForEachProperty([&](FProperty* Property) {
				Equal &= ArePropertyValuesIdentical(Property, Actual[Index].Get(), 0, Expressions[Index].Get(), 0);
			});
		}
		return Equal;
	};
	DMaterialExpressionLerp* NormalBlend = nullptr;
	for (const auto& Expression : Edited)
		if (auto* Lerp = Cast<DMaterialExpressionLerp>(Expression.Get())) { NormalBlend = Lerp; break; }
	ASSERT_NE(NormalBlend, nullptr);
	NormalBlend->A.SetConstant({0, .1f, 1});
	ASSERT_TRUE(Apply(Edited, Signature));
	ASSERT_TRUE(SavePackage(Functions.SampleNormal->GetPackage()));
	ASSERT_TRUE(LoadStandardMaterialFunctions(Functions, Error)) << Error;
	EXPECT_TRUE(Matches(Edited, Signature));
	Signature.Inputs.front().Id = FGuid::NewGuid();
	const auto OldId = OriginalSignature.Inputs.front().Id;
	for (const auto& Expression : Edited)
		if (auto* Input = Cast<DMaterialExpressionFunctionInput>(Expression.Get()); Input && Input->Port.Id == OldId)
			Input->Port.Id = Signature.Inputs.front().Id;
	for (auto& Port : Signature.Inputs)
		if (Port.Default.InputId == OldId) Port.Default.InputId = Signature.Inputs.front().Id;
	ASSERT_TRUE(Apply(Edited, Signature));
	EXPECT_FALSE(LoadStandardMaterialFunctions(Functions, Error));
	EXPECT_NE(Error.find("interface"), std::string::npos);
	EXPECT_TRUE(Matches(Edited, Signature));
	ASSERT_TRUE(Apply(Original, OriginalSignature));
	ASSERT_TRUE(SavePackage(Functions.SampleNormal->GetPackage()));
	auto Reload = ReloadPackages({.Packages = {Functions.SampleNormal->GetPackage()}});
	const auto Reloaded = Reload.Wait();
	ASSERT_TRUE(Reloaded) << (Reloaded.Diagnostics.empty() ? "no diagnostic" : FormatPackageReloadDiagnostic(Reloaded.Diagnostics.front()));
	ASSERT_TRUE(LoadStandardMaterialFunctions(Functions, Error)) << Error;
	EXPECT_TRUE(Matches(Original, OriginalSignature));
	EXPECT_EQ(DMaterialFunction::StaticClass()->FindPropertyByName("AuthoringSource"), nullptr);
	EXPECT_EQ(DMaterialFunction::StaticClass()->FindPropertyByName("AuthoringSourceVersion"), nullptr);
	EXPECT_TRUE(std::ranges::any_of(Material->GetExpressionCollection().Expressions, [&](const auto& Expression) {
		const auto* Call = Cast<DMaterialExpressionFunctionCall>(Expression.Get());
		return Call && Call->Function.Get() == Functions.SampleNormal.Get();
	}));
}

TEST(FSceneImportTests, CancellationRemainsVisibleAfterCleanupReplacesProgress)
{
	using namespace Durin;
	using namespace Durin::AssetForge::Builtins;
	const auto Fixture = InitializeFixture("FeedbackCancel", "StaticModelMaterials/PrimitiveProjection.gltf");
	std::filesystem::copy_file(std::filesystem::path(DURIN_TEST_DATA_DIR) / "StaticModelMaterials/Triangle.bin",
		std::filesystem::path(Fixture.Source).parent_path() / "Triangle.bin");
	FAssetImportSession Session(Fixture.Source, Fixture.DestinationDirectory, FStaticMeshImportSettings::MakeDurin(), {});
	Session.Cancel();
	ASSERT_TRUE(AdvanceSceneSession(Session, EAssetImportPhase::Completed));
	EXPECT_TRUE(Session.GetProgress().bCancellationRequested);
	EXPECT_FALSE(Session.GetResult());
	EXPECT_TRUE(Session.GetResult().ImportedPackages.empty());
}
