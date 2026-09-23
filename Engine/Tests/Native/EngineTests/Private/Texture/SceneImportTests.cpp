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
#include "AssetForge/Builtins/SceneImport.h"
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
#include "Threading/TaskComposition.h"
#include <thread>

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
		Durin::FModuleManager::Get().LoadModuleChecked("TextureBuild");
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
		-> Durin::AssetForge::Builtins::FSceneImportResult
	{
		auto Result = Durin::AssetForge::Builtins::ImportSceneAssets(
			Fixture.Source, Fixture.DestinationDirectory,
			Durin::FStaticMeshImportSettings::MakeDurin());
		EXPECT_TRUE(Result) << Result.Message;
		return Result;
	}
}

namespace
{
	auto AdvanceSceneSession(Durin::AssetForge::Builtins::FSceneImportSession& Session,
		Durin::AssetForge::Builtins::ESceneImportPhase Target) -> bool
	{
		const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
		while (std::chrono::steady_clock::now() < Deadline)
		{
			Durin::PumpGameThreadDeferredWork();
			Durin::FAssetCompilingManager::Get().ProcessAsyncTasks(false);
			Session.Tick();
			if (Session.GetProgress().Phase == Target) return true;
			if (Session.GetProgress().Phase == Durin::AssetForge::Builtins::ESceneImportPhase::Completed) return false;
			std::this_thread::yield();
		}
		return false;
	}
}

TEST(FSceneImportTests, AsyncSessionPublishesAndReimports)
{
	using namespace Durin;
	using namespace Durin::AssetForge::Builtins;
	const auto Fixture = InitializeFixture("AsyncSession");
	for (int Pass = 0; Pass != 2; ++Pass)
	{
		FSceneImportSession Session(Fixture.Source, Fixture.DestinationDirectory, FStaticMeshImportSettings::MakeDurin());
		ASSERT_TRUE(AdvanceSceneSession(Session, ESceneImportPhase::Ready)) << Session.GetResult().Message;
		ASSERT_TRUE(Session.PreviewMaterials(Fixture.DestinationDirectory, {}).bSucceeded);
		ASSERT_TRUE(Session.PreviewMaterials(Fixture.DestinationDirectory, {}).bSucceeded);
		ASSERT_TRUE(Session.BeginImport(Fixture.DestinationDirectory, {}));
		ASSERT_TRUE(AdvanceSceneSession(Session, ESceneImportPhase::Saving)) << Session.GetResult().Message;
		// Normal editor frames may create unrelated objects while disk staging runs.
		TStrongObjectPtr<DStaticMeshComponent> Unrelated(NewObject<DStaticMeshComponent>(nullptr,
			FName(Pass == 0 ? "AsyncUnrelatedFirst" : "AsyncUnrelatedSecond")));
		ASSERT_TRUE(AdvanceSceneSession(Session, ESceneImportPhase::Completed));
		ASSERT_TRUE(Session.GetResult()) << Session.GetResult().Message;
		EXPECT_TRUE(Session.GetResult().bPersisted);
		EXPECT_FALSE(Session.GetResult().SavedPackages.empty());
	}
}

TEST(FSceneImportTests, BuildModuleIsRetainedOnlyDuringProductConstruction)
{
	using namespace Durin;
	using namespace Durin::AssetForge::Builtins;
	const auto Fixture = InitializeFixture("BuildModuleLifetime");
	FAssetCompilingManager::Get().FinishAllCompilation();
	struct FRestoreBuildModule
	{
		~FRestoreBuildModule() { FModuleManager::Get().LoadModuleChecked("MeshBuilder"); }
	} Restore;
	FSceneImportSession Session(Fixture.Source, Fixture.DestinationDirectory, FStaticMeshImportSettings::MakeDurin());
	ASSERT_TRUE(FModuleManager::Get().UnloadModule("MeshBuilder").Succeeded());
	ASSERT_TRUE(AdvanceSceneSession(Session, ESceneImportPhase::Ready)) << Session.GetResult().Message;
	ASSERT_TRUE(Session.PreviewMaterials(Fixture.DestinationDirectory, {}).bSucceeded);
	EXPECT_FALSE(FModuleManager::Get().IsModuleLoaded("MeshBuilder"));
	FModuleManager::Get().LoadModuleChecked("MeshBuilder");
	ASSERT_TRUE(Session.BeginImport(Fixture.DestinationDirectory, {}));
	Session.Tick();
	ASSERT_EQ(Session.GetProgress().Phase, ESceneImportPhase::Building);
	EXPECT_EQ(FModuleManager::Get().UnloadModule("MeshBuilder").Status, EModuleOperationStatus::OutstandingCodeLease);
	ASSERT_TRUE(AdvanceSceneSession(Session, ESceneImportPhase::Saving)) << Session.GetResult().Message;
	ASSERT_TRUE(FModuleManager::Get().UnloadModule("MeshBuilder").Succeeded());
	ASSERT_TRUE(AdvanceSceneSession(Session, ESceneImportPhase::Completed));
	ASSERT_TRUE(Session.GetResult()) << Session.GetResult().Message;
	EXPECT_TRUE(Session.GetResult().bPersisted);
	EXPECT_FALSE(FModuleManager::Get().IsModuleLoaded("MeshBuilder"));
}

TEST(FSceneImportTests, AsyncSessionRejectsChangedSourceAfterReusablePreview)
{
	using namespace Durin;
	using namespace Durin::AssetForge::Builtins;
	const auto Fixture = InitializeFixture("AsyncStaleSource");
	FSceneImportSession Session(Fixture.Source, Fixture.DestinationDirectory, FStaticMeshImportSettings::MakeDurin());
	ASSERT_TRUE(AdvanceSceneSession(Session, ESceneImportPhase::Ready));
	std::ofstream(Fixture.Source, std::ios::app) << "\n ";
	// Preview reads cached scene values, even after the physical source changes.
	ASSERT_TRUE(Session.PreviewMaterials(Fixture.DestinationDirectory, {}).bSucceeded);
	ASSERT_TRUE(Session.BeginImport(Fixture.DestinationDirectory, {}));
	ASSERT_TRUE(AdvanceSceneSession(Session, ESceneImportPhase::Completed));
	EXPECT_FALSE(Session.GetResult());
	EXPECT_TRUE(Session.GetResult().SavedPackages.empty());
	EXPECT_NE(Session.GetResult().Message.find("changed"), std::string::npos);
}

TEST(FSceneImportTests, AsyncSessionCancelsBeforePublication)
{
	using namespace Durin;
	using namespace Durin::AssetForge::Builtins;
	const auto Fixture = InitializeFixture("AsyncCancel");
	FSceneImportSession Session(Fixture.Source, Fixture.DestinationDirectory, FStaticMeshImportSettings::MakeDurin());
	ASSERT_TRUE(AdvanceSceneSession(Session, ESceneImportPhase::Ready));
	ASSERT_TRUE(Session.BeginImport(Fixture.DestinationDirectory, {}));
	Session.Tick();
	Session.Cancel();
	ASSERT_TRUE(AdvanceSceneSession(Session, ESceneImportPhase::Completed));
	EXPECT_FALSE(Session.GetResult());
	EXPECT_TRUE(Session.GetResult().SavedPackages.empty());
}

TEST(FSceneImportTests, AsyncSessionReportsPartialPersistence)
{
	using namespace Durin;
	using namespace Durin::AssetForge::Builtins;
	const auto Fixture = InitializeFixture("AsyncPartial");
	FSceneImportSession Session(Fixture.Source, Fixture.DestinationDirectory, FStaticMeshImportSettings::MakeDurin());
	ASSERT_TRUE(AdvanceSceneSession(Session, ESceneImportPhase::Ready));
	ASSERT_TRUE(Session.BeginImport(Fixture.DestinationDirectory, {}, {
		.ShouldFail = [](EAssetBundleSavePhase Phase, size_t Index) {
			return Index == 1 && Phase == EAssetBundleSavePhase::PublishRegistry;
		}}));
	ASSERT_TRUE(AdvanceSceneSession(Session, ESceneImportPhase::Completed));
	EXPECT_FALSE(Session.GetResult());
	EXPECT_EQ(Session.GetResult().SavedPackages.size(), 1u) << Session.GetResult().Message;
	for (const auto& Path : Session.GetResult().SavedPackages) EXPECT_TRUE(FindAssetExact(Path));
}

TEST(FSceneImportTests, AsyncSessionCancelsStagedSaveAndAllowsRetry)
{
	using namespace Durin;
	using namespace Durin::AssetForge::Builtins;
	const auto Fixture = InitializeFixture("AsyncStagedCancel");
	{
		FSceneImportSession Session(Fixture.Source, Fixture.DestinationDirectory, FStaticMeshImportSettings::MakeDurin());
		ASSERT_TRUE(AdvanceSceneSession(Session, ESceneImportPhase::Ready));
		ASSERT_TRUE(Session.BeginImport(Fixture.DestinationDirectory, {}));
		ASSERT_TRUE(AdvanceSceneSession(Session, ESceneImportPhase::Saving));
		Session.Cancel();
		ASSERT_TRUE(AdvanceSceneSession(Session, ESceneImportPhase::Completed));
		EXPECT_FALSE(Session.GetResult());
		EXPECT_TRUE(Session.GetResult().SavedPackages.empty());
	}
	EXPECT_TRUE(RunScene(Fixture));
}

TEST(FSceneImportTests, AsyncSessionDestructionDrainsPreparation)
{
	using namespace Durin;
	using namespace Durin::AssetForge::Builtins;
	const auto Fixture = InitializeFixture("AsyncDestruction");
	{
		FSceneImportSession Session(Fixture.Source, Fixture.DestinationDirectory, FStaticMeshImportSettings::MakeDurin());
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
			auto LoadedValue = Durin::LoadObject<Durin::DObject>(Durin::Testing::MakePackageLeafAssetObjectPathForTests(Output.AssetPath));
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
			auto LoadedValue = Durin::LoadObject<Durin::DObject>(Durin::Testing::MakePackageLeafAssetObjectPathForTests(Output.AssetPath));
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
	ASSERT_TRUE(Reimported) << Reimported.Message;
	EXPECT_TRUE(std::ranges::equal(ReimportMesh.Asset->GetMaterialSlots(), Slots));

}

TEST(FSceneImportTests, SceneReimportResetsEditsAndRollsBackSavedAndLiveOutputs)
{
	using namespace Durin;
	using namespace Durin::AssetForge::Builtins;
	const auto Fixture = InitializeFixture("StructuralReimport");
	const auto Initial = RunScene(Fixture);
	ASSERT_TRUE(Initial) << Initial.Message;
	for (const std::string_view Class : {"Durin::DStaticMesh", "Durin::DMaterial", "Durin::DTexture2D"})
		for (const auto& Output : Initial.Outputs)
			if (Output.AssetClassName == Class) ASSERT_TRUE(UnloadPackage(Output.AssetPath));
	CollectGarbage();
	DMaterial* Previous = nullptr;
	DStaticMesh* PreviousMesh = nullptr;
	FPackagePath MaterialPath;
	std::vector<std::pair<FPackagePath, std::string>> SavedBytes;
	const auto Read = [](const std::filesystem::path& Path) {
		std::ifstream Input(Path, std::ios::binary);
		return std::string(std::istreambuf_iterator<char>(Input), {});
	};
	for (const auto& Output : Initial.Outputs)
	{
		DObject* Object = nullptr;
		const auto Loaded = LoadObject<DObject>(Testing::MakePackageLeafAssetObjectPathForTests(Output.AssetPath));
		Object = Loaded.value_or(nullptr);
		ASSERT_TRUE(Loaded) << Output.AssetPath.ToString() << ": " << (Loaded ? std::string{} : Loaded.error().Message);
		if (auto* Material = Cast<DMaterial>(Object)) { Previous = Material; MaterialPath = Output.AssetPath; }
		if (auto* Mesh = Cast<DStaticMesh>(Object)) PreviousMesh = Mesh;
		SavedBytes.push_back({Output.AssetPath, Read(FindAssetExact(Output.AssetPath)->PhysicalPath)});
	}
	ASSERT_NE(Previous, nullptr);
	ASSERT_NE(PreviousMesh, nullptr);
	using Kind = Durin::AssetForge::Builtins::MaterialParameters::EMaterialBuiltinParameterKind;
	const auto Color = Durin::AssetForge::Builtins::GetMaterialSurfaceParameterId(EMaterialSurfaceOutput::BaseColor, Kind::Value);
	ASSERT_TRUE(Previous->SetParameterValue(Color, FMaterialParameterValue::MakeVector4({0.2, 0.3, 0.4, 0})));
	auto Properties = Previous->GetStaticProperties();
	Properties.ShadingModel = EMaterialShadingModel::Unlit;
	ASSERT_TRUE(Previous->SetStaticProperties(Properties));
	auto* Consumer = NewObject<DStaticMeshComponent>(nullptr, "ReimportConsumer");
	Consumer->SetStaticMesh(PreviousMesh);
	auto* Dependent = NewObject<DMaterialInstance>(nullptr, "ReimportDependent");
	ASSERT_TRUE(Dependent->SetParent(Previous));
	auto* Independent = NewObject<DMaterialInstance>(nullptr, "IndependentMaterial");
	ASSERT_TRUE(Independent->SetParent(Previous));
	ASSERT_TRUE(Independent->SetParameterValue(Color, FMaterialParameterValue::MakeVector4({0.1, 0.2, 0.3, 0})));
	const auto PreviousKey = Previous->GetImportProvenance().StructuralKey;
	FAssetCompilingManager::Get().FinishAllCompilation();
	const std::string OriginalSource = Read(Fixture.Source);
	std::string ChangedSource = OriginalSource;
	const std::string Factor = "\"baseColorFactor\": [0.5, 0.75, 0.25, 1.0]";
	const auto Position = ChangedSource.find(Factor);
	ASSERT_NE(Position, std::string::npos);
	ChangedSource.replace(Position, Factor.size(), "\"baseColorFactor\": [1,1,1,1]");
	const auto MaterialPosition = ChangedSource.find("\"pbrMetallicRoughness\"");
	ASSERT_NE(MaterialPosition, std::string::npos);
	ChangedSource.insert(MaterialPosition, "\"normalTexture\": {\"index\":0}, \"doubleSided\":true, ");
	std::ofstream(Fixture.Source, std::ios::trunc) << ChangedSource;
	for (const auto Failure : {EAssetBundleSavePhase::StagePackage,
		EAssetBundleSavePhase::PublishPackage, EAssetBundleSavePhase::PublishRegistry})
	{
		bool bReached = false;
		auto Failed = ImportSceneAssets(Fixture.Source, Fixture.DestinationDirectory,
			FStaticMeshImportSettings::MakeDurin(), {}, {.ShouldFail = [&](EAssetBundleSavePhase Phase, size_t) {
				EXPECT_EQ(Consumer->GetStaticMesh(), PreviousMesh);
				EXPECT_EQ(Dependent->GetParent(), Previous);
				if (Phase != Failure) return false;
				bReached = true;
				return true;
			}}, {.bRebuildExistingMaterials = true});
		EXPECT_FALSE(Failed) << Failed.Message;
		ASSERT_TRUE(bReached) << Failed.Message;
		EXPECT_EQ(Consumer->GetStaticMesh(), PreviousMesh);
		EXPECT_EQ(Dependent->GetParent(), Previous);
		for (const auto& [Path, Bytes] : SavedBytes) EXPECT_EQ(Read(FindAssetExact(Path)->PhysicalPath), Bytes);
	}
	auto Partial = ImportSceneAssets(Fixture.Source, Fixture.DestinationDirectory,
		FStaticMeshImportSettings::MakeDurin(), {}, {.ShouldFail = [](EAssetBundleSavePhase Phase, size_t) {
			return Phase == EAssetBundleSavePhase::PublishRootPackage;
		}}, {.bRebuildExistingMaterials = true});
	ASSERT_FALSE(Partial);
	ASSERT_FALSE(Partial.SavedPackages.empty());
	EXPECT_EQ(Consumer->GetStaticMesh(), PreviousMesh);
	EXPECT_NE(Dependent->GetParent(), Previous);
	EXPECT_EQ(Consumer->GetMaterial(), Dependent->GetParent());
	EXPECT_TRUE(Dependent->GetMaterialCompileStatus().IsCurrent());
	const auto Changed = ImportSceneAssets(Fixture.Source, Fixture.DestinationDirectory, FStaticMeshImportSettings::MakeDurin(), {}, {}, {.bRebuildExistingMaterials = true});
	ASSERT_TRUE(Changed) << Changed.Message;
	DMaterial* Material = nullptr;
	{
		auto LoadedValue = LoadObject<DMaterial>(Testing::MakePackageLeafAssetObjectPathForTests(MaterialPath));
		Material = LoadedValue.value_or(nullptr);
		ASSERT_TRUE(LoadedValue);
	}
	ASSERT_NE(Material, Previous);
	EXPECT_EQ(Dependent->GetParent(), Material);
	EXPECT_TRUE(Dependent->GetMaterialCompileStatus().IsCurrent());
	ASSERT_TRUE(Dependent->GetAcceptedCompiledProgram());
	EXPECT_EQ(Dependent->GetAcceptedCompiledProgram()->ActiveParameters.size(), Material->GetAcceptedCompiledProgram()->ActiveParameters.size());
	EXPECT_NE(Consumer->GetStaticMesh(), PreviousMesh);
	EXPECT_EQ(Consumer->GetMaterial(), Material);
	EXPECT_EQ(Material->FindParameterDefinition(Color), nullptr);
	EXPECT_EQ(Material->GetStaticProperties().ShadingModel, EMaterialShadingModel::Lit);
	EXPECT_TRUE(Material->GetStaticProperties().bTwoSided);
	EXPECT_NE(Material->GetImportProvenance().StructuralKey, PreviousKey);
	std::ofstream(Fixture.Source, std::ios::trunc) << OriginalSource;
	const auto Restored = ImportSceneAssets(Fixture.Source, Fixture.DestinationDirectory, FStaticMeshImportSettings::MakeDurin(), {}, {}, {.bRebuildExistingMaterials = true});
	ASSERT_TRUE(Restored) << Restored.Message;
	{
		auto LoadedValue = LoadObject<DMaterial>(Testing::MakePackageLeafAssetObjectPathForTests(MaterialPath));
		Material = LoadedValue.value_or(nullptr);
		ASSERT_TRUE(LoadedValue);
	}
	FResolvedMaterialParameter Value;
	ASSERT_TRUE(Material->ResolveParameterValue(Color, Value));
	EXPECT_EQ(FVector3(Value.Value.GetVector4()), FVector3(0.5, 0.75, 0.25));
	EXPECT_FALSE(Material->GetStaticProperties().bTwoSided);
	ASSERT_TRUE(Independent->ResolveParameterValue(Color, Value));
	EXPECT_EQ(FVector3(Value.Value.GetVector4()), FVector3(0.1f, 0.2f, 0.3f));
	EXPECT_EQ(Material->GetStaticProperties().ShadingModel, EMaterialShadingModel::Lit);
	const auto OtherSource = std::filesystem::path(Fixture.Source).parent_path() / "OtherSource" /
		std::filesystem::path(Fixture.Source).filename();
	std::filesystem::create_directories(OtherSource.parent_path());
	std::ofstream(OtherSource) << OriginalSource;
	auto Collision = ImportSceneAssets(OtherSource.generic_string(), Fixture.DestinationDirectory,
		FStaticMeshImportSettings::MakeDurin());
	EXPECT_FALSE(Collision);
	EXPECT_FALSE(Collision.bPersisted);
	EXPECT_EQ(Consumer->GetMaterial(), Material);
}

TEST(FSceneImportTests, FailedPublicationDiscardsLocalMaterialAndRetrySucceeds)
{
	using namespace Durin;
	using namespace Durin::AssetForge::Builtins;
	const auto Fixture = InitializeFixture("PublicationRollback");
	std::ifstream Input(Fixture.Source);
	std::string Source((std::istreambuf_iterator<char>(Input)), {});
	Input.close();
	const std::string Factor = "\"baseColorFactor\": [0.5, 0.75, 0.25, 1.0]";
	const auto Position = Source.find(Factor);
	ASSERT_NE(Position, std::string::npos);
	Source.replace(Position, Factor.size(), "\"baseColorFactor\":[1,1,1,1],\"metallicFactor\":0,\"roughnessFactor\":0.5");
	std::ofstream OutputFile(Fixture.Source, std::ios::trunc);
	OutputFile << Source;
	OutputFile.close();
	const auto Preview = PreviewSceneMaterials(Fixture.Source, Fixture.DestinationDirectory, FStaticMeshImportSettings::MakeDurin());
	ASSERT_TRUE(Preview.bSucceeded) << Preview.Message;
	ASSERT_EQ(Preview.Materials.size(), 1u);
	const auto ParentPath = Preview.Materials.front().AssetPath;
	ASSERT_FALSE(FindAssetExact(ParentPath));
	ASSERT_EQ(FindResidentPackage(ParentPath), nullptr);
	for (const auto Failure : {EAssetBundleSavePhase::CreateDirectories, EAssetBundleSavePhase::StagePackage,
		EAssetBundleSavePhase::PublishPackage, EAssetBundleSavePhase::PublishRegistry})
	{
		bool bInjected = false;
		auto Failed = ImportSceneAssets(Fixture.Source, Fixture.DestinationDirectory,
			FStaticMeshImportSettings::MakeDurin(), {}, {.ShouldFail = [&](EAssetBundleSavePhase Phase, size_t) {
				EXPECT_EQ(FindResidentPackage(ParentPath), nullptr);
				for (auto* Object : GDObjectArray.GetAll(EObjectQueryScope::LiveOnly))
					if (auto* Package = Cast<DPackage>(Object))
						EXPECT_FALSE(Package->GetPackagePath().starts_with(Fixture.DestinationDirectory.ToString() + "/"));
				if (Phase != Failure) return false;
				bInjected = true;
				return true;
			}});
		ASSERT_FALSE(Failed);
		EXPECT_TRUE(bInjected);
		EXPECT_FALSE(Failed.bSucceeded);
		EXPECT_FALSE(Failed.bPersisted);
		EXPECT_EQ(FindResidentPackage(ParentPath), nullptr);
		EXPECT_FALSE(FindAssetExact(ParentPath));
		for (const auto& Output : Failed.Outputs)
		{
			EXPECT_EQ(FindResidentPackage(Output.AssetPath), nullptr);
			EXPECT_FALSE(FindAssetExact(Output.AssetPath));
		}
	}
	auto Partial = ImportSceneAssets(Fixture.Source, Fixture.DestinationDirectory,
		FStaticMeshImportSettings::MakeDurin(), {}, {.ShouldFail = [](EAssetBundleSavePhase Phase, size_t) {
			return Phase == EAssetBundleSavePhase::PublishRootPackage;
		}});
	ASSERT_FALSE(Partial);
	ASSERT_FALSE(Partial.SavedPackages.empty());
	EXPECT_FALSE(Partial.bPersisted);
	EXPECT_TRUE(FindAssetExact(ParentPath));
	EXPECT_NE(FindResidentPackage(ParentPath), nullptr);
	for (const auto& Path : Partial.SavedPackages)
	{
		ASSERT_TRUE(FindAssetExact(Path));
		ASSERT_NE(FindResidentPackage(Path), nullptr);
		EXPECT_FALSE(FindResidentPackage(Path)->IsDirty());
		EXPECT_FALSE(FindResidentPackage(Path)->IsGraphPrivate());
	}
	bool bFoundUnsaved = false;
	for (const auto& Output : Partial.Outputs)
		if (std::ranges::find(Partial.SavedPackages, Output.AssetPath) == Partial.SavedPackages.end())
		{
			bFoundUnsaved = true;
			EXPECT_FALSE(FindAssetExact(Output.AssetPath));
			EXPECT_EQ(FindResidentPackage(Output.AssetPath), nullptr);
		}
	EXPECT_TRUE(bFoundUnsaved);
	const auto Retried = RunScene(Fixture);
	ASSERT_TRUE(Retried) << Retried.Message;
	EXPECT_TRUE(Retried.bPersisted);
	EXPECT_TRUE(FindAssetExact(ParentPath));
	auto* Parent = Cast<DMaterial>(FindResidentPackage(ParentPath)->FindTopLevelAsset(FName(ParentPath.GetPackageName())));
	ASSERT_NE(Parent, nullptr);
	EXPECT_EQ(Parent->GetExpressionCollection().Expressions.size(), 2u);
	EXPECT_NE(Parent->GetOutputNode(), nullptr);
	EXPECT_EQ(Parent->GetParameterDefinitions().size(), 1u);
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
				auto LoadedValue = LoadObject<DMaterial>(Testing::MakePackageLeafAssetObjectPathForTests(Output.AssetPath));
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
				auto LoadedValue = LoadObject<DMaterial>(Testing::MakePackageLeafAssetObjectPathForTests(Output.AssetPath));
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
				auto LoadedValue = LoadObject<DMaterial>(Testing::MakePackageLeafAssetObjectPathForTests(Output.AssetPath));
				Instance = LoadedValue.value_or(nullptr);
				ASSERT_TRUE(LoadedValue);
			}
	ASSERT_NE(Instance, nullptr);
	auto* Parent = Instance;
	ASSERT_NE(Parent, nullptr);
	const auto Revision = Parent->GetPackage()->GetEditRevision();
	FSceneMaterialImportOptions Options;
	Options.Default = {ESceneMaterialImportMode::CreateInstances, Parent->GetObjectPath()};
	auto Second = ImportSceneAssets(Fixture.Source, MakeAssetPath("/SceneImportTests/Second"),
		FStaticMeshImportSettings::MakeDurin(), {}, {}, Options);
	ASSERT_TRUE(Second) << Second.Message;
	for (const auto& Output : Second.Outputs)
		if (Output.Role == "MaterialInstance")
		{
			DMaterialInstance* Reused = nullptr;
			{
				auto LoadedValue = LoadObject<DMaterialInstance>(Testing::MakePackageLeafAssetObjectPathForTests(Output.AssetPath));
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
	auto Rejected = ImportSceneAssets(Fixture.Source, MakeAssetPath("/SceneImportTests/Rejected"),
		FStaticMeshImportSettings::MakeDurin(), {}, {}, Options);
	EXPECT_FALSE(Rejected);
	EXPECT_NE(Rejected.Message.find("graph"), std::string::npos);
	EXPECT_EQ(Parent->GetExpressionOutputs(), Changed);
	for (const auto& Output : Rejected.Outputs) EXPECT_FALSE(FindAssetExact(Output.AssetPath));
	ASSERT_TRUE(ApplyOutputs(Original));
}

TEST(FSceneImportTests, ReimportPreservesEditedMaterialBindingsAndRebindsTextureCache)
{
	using namespace Durin;
	using namespace Durin::AssetForge::Builtins;
	const auto Fixture = InitializeFixture("PreserveEdits");
	const auto First = RunScene(Fixture);
	DMaterial* Material = nullptr;
	DStaticMesh* Mesh = nullptr;
	for (const auto& Output : First.Outputs)
	{
		auto* Object = LoadObject<DObject>(Testing::MakePackageLeafAssetObjectPathForTests(Output.AssetPath)).value_or(nullptr);
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
	const auto Created = IAssetTools::Get().CreateAsset(CustomPath, DMaterialInstance::StaticClass());
	ASSERT_TRUE(Created) << Created.Message;
	auto* Custom = Cast<DMaterialInstance>(Created.Asset);
	ASSERT_NE(Custom, nullptr);
	ASSERT_TRUE(Custom->SetParent(Material));
	FAssetCompilingManager::Get().FinishAllCompilation();
	ASSERT_TRUE(SavePackage(Custom->GetPackage()));
	Mesh->SetMaterialSlotDefaultMaterial(0, Custom);
	DTexture2D* PreviousTexture = nullptr;
	ASSERT_TRUE(Material->GetTextureParameterValue(MaterialParameters::BaseColorTextureName(), PreviousTexture));
	const auto MaterialPath = Material->GetPackage()->GetPackagePathIdentity();
	const auto MeshPath = Mesh->GetPackage()->GetPackagePathIdentity();
	const auto Preview = PreviewSceneMaterials(Fixture.Source, Fixture.DestinationDirectory, FStaticMeshImportSettings::MakeDurin());
	ASSERT_TRUE(Preview.bSucceeded) << Preview.Message;
	ASSERT_EQ(Preview.Materials.size(), 1u);
	EXPECT_TRUE(Preview.Materials.front().bPreserved);
	const auto Reimport = RunScene(Fixture);
	ASSERT_TRUE(Reimport) << Reimport.Message;
	EXPECT_EQ(std::ranges::find(Reimport.SavedPackages, MaterialPath), Reimport.SavedPackages.end());
	EXPECT_EQ(LoadObject<DMaterial>(Testing::MakePackageLeafAssetObjectPathForTests(MaterialPath)).value_or(nullptr), Material);
	FResolvedMaterialParameter Value;
	ASSERT_TRUE(Material->ResolveParameterValue(Color, Value));
	EXPECT_EQ(FVector3(Value.Value.GetVector4()), FVector3(.2, .3, .4));
	EXPECT_TRUE(Material->GetStaticProperties().bTwoSided);
	auto* UpdatedMesh = LoadObject<DStaticMesh>(Testing::MakePackageLeafAssetObjectPathForTests(MeshPath)).value_or(nullptr);
	ASSERT_NE(UpdatedMesh, nullptr);
	EXPECT_EQ(UpdatedMesh->GetMaterialSlots()[0].DefaultMaterial.Get(), Custom);
	DTexture2D* UpdatedTexture = nullptr;
	ASSERT_TRUE(Material->GetTextureParameterValue(MaterialParameters::BaseColorTextureName(), UpdatedTexture));
	ASSERT_NE(UpdatedTexture, nullptr);
	EXPECT_NE(UpdatedTexture, PreviousTexture);
	const auto* TextureNode = std::ranges::find_if(Material->GetExpressionCollection().Expressions, [](const auto& Node) {
		return Cast<DMaterialExpressionTextureParameter>(Node.Get()) != nullptr;
	})->Get();
	EXPECT_EQ(Cast<DMaterialExpressionTextureParameter>(TextureNode)->DefaultValue.Texture.Get(), UpdatedTexture);
}

TEST(FSceneImportTests, PerMaterialOverridesMissingParentsAndTypeChangesAreExplicit)
{
	using namespace Durin;
	using namespace Durin::AssetForge::Builtins;
	const auto Fixture = InitializeFixture("MixedModes", "MultiSection.gltf");
	const auto First = RunScene(Fixture);
	DMaterial* Parent = nullptr;
	for (const auto& Output : First.Outputs)
		if (Output.Role == "Material") Parent = LoadObject<DMaterial>(Testing::MakePackageLeafAssetObjectPathForTests(Output.AssetPath)).value_or(nullptr);
	ASSERT_NE(Parent, nullptr);
	const auto Destination = MakeAssetPath("/SceneImportTests/MixedOutput");
	auto Preview = PreviewSceneMaterials(Fixture.Source, Destination, FStaticMeshImportSettings::MakeDurin());
	ASSERT_TRUE(Preview.bSucceeded) << Preview.Message;
	ASSERT_EQ(Preview.Materials.size(), 2u);
	FSceneMaterialImportOptions Options;
	Options.Default.Mode = ESceneMaterialImportMode::CreateInstances;
	auto Rejected = ImportSceneAssets(Fixture.Source, Destination, FStaticMeshImportSettings::MakeDurin(), {}, {}, Options);
	EXPECT_FALSE(Rejected);
	EXPECT_TRUE(Rejected.SavedPackages.empty());
	for (const auto& Output : Rejected.Outputs) EXPECT_FALSE(FindAssetExact(Output.AssetPath));
	Options.Default.ParentMaterialPath = Parent->GetObjectPath();
	Options.Overrides.push_back({Preview.Materials[0].StableIdentity, {ESceneMaterialImportMode::CreateMaterials}});
	Preview = PreviewSceneMaterials(Fixture.Source, Destination, FStaticMeshImportSettings::MakeDurin(), Options);
	ASSERT_TRUE(Preview.bSucceeded) << Preview.Message;
	EXPECT_TRUE(Preview.Materials[0].AssetPath.GetPackageName().starts_with("M_"));
	EXPECT_TRUE(Preview.Materials[1].AssetPath.GetPackageName().starts_with("MI_"));
	const auto Imported = ImportSceneAssets(Fixture.Source, Destination, FStaticMeshImportSettings::MakeDurin(), {}, {}, Options);
	ASSERT_TRUE(Imported) << Imported.Message;
	EXPECT_EQ(std::ranges::count(Imported.Outputs, "Material", &Durin::AssetForge::FImportOutputSummary::Role), 1);
	EXPECT_EQ(std::ranges::count(Imported.Outputs, "MaterialInstance", &Durin::AssetForge::FImportOutputSummary::Role), 1);
	// Stored class and parent are authoritative on ordinary reimport, even with default options.
	const auto Preserved = PreviewSceneMaterials(Fixture.Source, Destination, FStaticMeshImportSettings::MakeDurin());
	ASSERT_TRUE(Preserved.bSucceeded) << Preserved.Message;
	EXPECT_TRUE(Preserved.Materials[1].bPreserved);
	EXPECT_EQ(Preserved.Materials[1].Selection.ParentMaterialPath, Parent->GetObjectPath());
	EXPECT_EQ(Preserved.Materials[1].Selection.Mode, ESceneMaterialImportMode::CreateInstances);
	const auto Reimported = ImportSceneAssets(Fixture.Source, Destination, FStaticMeshImportSettings::MakeDurin());
	ASSERT_TRUE(Reimported) << Reimported.Message;
	// A parent replaced by this same operation would leave the new instance pinned to an old generation.
	FSceneMaterialImportOptions SelfParent = Options;
	SelfParent.bRebuildExistingMaterials = true;
	SelfParent.Default.ParentMaterialPath = Testing::MakePackageLeafAssetObjectPathForTests(Preview.Materials[0].AssetPath).ToString();
	const auto SelfRejected = ImportSceneAssets(Fixture.Source, Destination, FStaticMeshImportSettings::MakeDurin(), {}, {}, SelfParent);
	EXPECT_FALSE(SelfRejected);
	EXPECT_TRUE(SelfRejected.SavedPackages.empty());
	EXPECT_NE(SelfRejected.Message.find("parent is an output"), std::string::npos);
	const auto Conversion = ImportSceneAssets(Fixture.Source, Destination, FStaticMeshImportSettings::MakeDurin(), {}, {}, {.bRebuildExistingMaterials = true});
	EXPECT_FALSE(Conversion);
	EXPECT_TRUE(Conversion.SavedPackages.empty());
	EXPECT_NE(Conversion.Message.find("asset type"), std::string::npos);
	Options.Overrides.push_back(Options.Overrides.front());
	const auto Duplicate = PreviewSceneMaterials(Fixture.Source, Destination, FStaticMeshImportSettings::MakeDurin(), Options);
	EXPECT_FALSE(Duplicate.bSucceeded);
}

TEST(FSceneImportTests, DuplicateSourceNamesPreserveDistinctSlotBindings)
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
			Mesh = LoadObject<DStaticMesh>(Testing::MakePackageLeafAssetObjectPathForTests(MeshPath)).value_or(nullptr);
		}
	ASSERT_NE(Mesh, nullptr);
	ASSERT_EQ(Mesh->GetMaterialSlots().size(), 2u);
	auto* FirstMaterial = Mesh->GetMaterialSlots()[0].DefaultMaterial.Get();
	auto* SecondMaterial = Mesh->GetMaterialSlots()[1].DefaultMaterial.Get();
	ASSERT_NE(FirstMaterial, SecondMaterial);
	Mesh->SetMaterialSlotDefaultMaterial(0, SecondMaterial);
	Mesh->SetMaterialSlotDefaultMaterial(1, FirstMaterial);
	const auto Reimport = RunScene(Fixture);
	ASSERT_TRUE(Reimport) << Reimport.Message;
	Mesh = LoadObject<DStaticMesh>(Testing::MakePackageLeafAssetObjectPathForTests(MeshPath)).value_or(nullptr);
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
	const auto Created = IAssetTools::Get().CreateAsset(ParentPath, DMaterial::StaticClass());
	ASSERT_TRUE(Created) << Created.Message;
	auto* Parent = Cast<DMaterial>(Created.Asset);
	ASSERT_NE(Parent, nullptr);
	Parent->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	ASSERT_TRUE(MakePBRSurfaceMaterialMRExpressions().Apply(*Parent));
	ASSERT_TRUE(Parent->CompileEdits());
	FAssetCompilingManager::Get().FinishAllCompilation();
	ASSERT_TRUE(SavePackage(Parent->GetPackage()));
	const auto ParentRevision = Parent->GetPackage()->GetEditRevision();
	FSceneMaterialImportOptions Options;
	Options.Default = {ESceneMaterialImportMode::CreateInstances, Parent->GetObjectPath()};
	const auto Preview = PreviewSceneMaterials(Fixture.Source, Fixture.DestinationDirectory, FStaticMeshImportSettings::MakeDurin(), Options);
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
		const auto Imported = ImportSceneAssets(Fixture.Source, Destination, FStaticMeshImportSettings::MakeDurin(), {}, {}, Options);
		ASSERT_TRUE(Imported) << Imported.Message;
		DMaterialInstance* Instance = nullptr;
		for (const auto& Output : Imported.Outputs)
			if (Output.Role == "MaterialInstance") Instance = LoadObject<DMaterialInstance>(Testing::MakePackageLeafAssetObjectPathForTests(Output.AssetPath)).value_or(nullptr);
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
	auto Imported = Durin::AssetForge::Builtins::ImportSceneAssets(
		Fixture.Source, Fixture.DestinationDirectory,
		Durin::FStaticMeshImportSettings::MakeDurin());
	EXPECT_FALSE(Imported);
	EXPECT_FALSE(Imported.Diagnostics.empty());
	EXPECT_TRUE(Imported.Outputs.empty());
	EXPECT_FALSE(Durin::FindAssetExact(MakeAssetPath(
		"/SceneImportTests/SceneImport/Unsupported/Materials/MustNotPublish")));
}

TEST(FSceneImportTests, DirectImportHonorsCancellationBeforePublication)
{
	const FSceneFixture Fixture = InitializeFixture("Scheduled");
	auto Result = Durin::AssetForge::Builtins::ImportSceneAssets(
		Fixture.Source, Fixture.DestinationDirectory,
		Durin::FStaticMeshImportSettings::MakeDurin(), [] { return true; });
	EXPECT_FALSE(Result);
	ASSERT_FALSE(Result.Diagnostics.empty());
	EXPECT_EQ(Result.Diagnostics.back().Category, Durin::AssetForge::EImportDiagnosticCategory::Canceled);
	EXPECT_FALSE(Durin::FindAssetExact(
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
	EXPECT_FALSE(FindAssetExact(MakeAssetPath("/Engine/Materials/Functions/UVTransform")));
	EXPECT_EQ(FindResidentPackage(MakeAssetPath("/Engine/Materials/Functions/UVTransform")), nullptr);
	const std::array Names{"UVTransform", "SampleNormal", "SampleORM"};
	const std::array Slots{&Functions.UVTransform, &Functions.SampleNormal, &Functions.SampleORM};
	for (uint32 Index = 0; Index < Slots.size(); ++Index)
	{
		const auto Path = MakeAssetPath(std::format("/Engine/Materials/Functions/{}", Names[Index]));
		FTopLevelAssetPath AssetPath;
		ASSERT_TRUE(FTopLevelAssetPath::TryCreate(Path, Names[Index], AssetPath));
		const auto Created = IAssetTools::Get().CreateAsset(AssetPath, DMaterialFunction::StaticClass());
		ASSERT_TRUE(Created) << Created.Message;
		auto* Function = Cast<DMaterialFunction>(Created.Asset);
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
	ASSERT_TRUE(EnvironmentResult) << FormatMaterialError(EnvironmentResult.Error);
	auto InputCapture = SnapshotMaterialCompilerInput(*Material, Environment);
	ASSERT_TRUE(InputCapture);
	auto& Input = InputCapture.Snapshot->Input;
	const auto Normalized = MIR::Normalize(Input);
	ASSERT_TRUE(Normalized) << (Normalized.Diagnostics.empty() ? "no diagnostic" : Durin::FormatMaterialError(Normalized.Diagnostics.front().Error));
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
