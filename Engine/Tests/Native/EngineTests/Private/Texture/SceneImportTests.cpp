#include "Misc/MountPathTestSupport.h"
#include "NativeDObjectTestSupport.h"
#include "TextureTestSupport.h"

#include "Asset/PackageSerialization.h"
#include "Asset/PackageReload.h"
#include "Asset/Mutation.h"
#include "Asset/AssetCook.h"
#include "Materials/MaterialInstance.h"
#include "Modules/ModuleManager.h"
#include "RenderingThread.h"
#include "AssetForge/Builtins/SceneImport.h"
#include "AssetForge/Builtins/StandardMaterialFunctions.h"
#include "Materials/Material.h"
#include "Materials/MaterialProgramCompiler.h"
#include "EditorReimportHandler.h"
#include "StaticMesh/StaticMeshFactoryTestSupport.h"
#include "Asset/AssetCompilingManager.h"
#include "../Materials/LegacyMaterialProgramTestFixture.h"
#include "StaticMesh/StaticMesh.h"

namespace
{
	class FSceneImportTestEnvironment final : public testing::Environment
	{
	public:
		auto SetUp() -> void override
		{
			InitializeDObjectSystem();
			ASSERT_TRUE(Durin::InitializeAssetCompilingManager());
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
		Durin::FModuleManager::Get().LoadModuleChecked("StaticMeshBuild");
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
		EXPECT_NE(Durin::AssetForge::Builtins::EnsureImportedSurfaceMaterial(Error), nullptr) << Error;
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
		Durin::AssetForge::Builtins::FSceneImportResult Result;
		EXPECT_TRUE(Durin::AssetForge::Builtins::ImportSceneAssets(
			Fixture.Source, Fixture.DestinationDirectory,
			Durin::FStaticMeshImportSettings::MakeDurin(), Result)) << Result.Message;
		return Result;
	}
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
		EXPECT_TRUE(Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(Output.AssetPath), Object));
		EXPECT_NE(Object, nullptr);
		if (auto* Material = Durin::Cast<Durin::DMaterialInstance>(Object))
		{
			EXPECT_TRUE(Material->GetMaterialCompileStatus().IsCurrent());
			EXPECT_TRUE(Material->GetAcceptedCompiledProgram());
			EXPECT_TRUE(Material->GetPropertyOverrides().bOverrideBlendMode);
			EXPECT_TRUE(Material->GetPropertyOverrides().bOverrideTwoSided);
			EXPECT_FALSE(Material->GetPropertyOverrides().bOverrideShadingModel);
			EXPECT_FALSE(Material->GetPropertyOverrides().bOverrideDepthWritePolicy);
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
	// Scene publication is create-only. Exercise the supported family reimport route
	// with a standard-function material retained in its authored slot.
	const auto ReimportMesh = Durin::AssetForge::Builtins::ImportStaticMeshForTest(
		Fixture.Source, "/SceneImportTests/ReimportGeometry");
	ASSERT_TRUE(ReimportMesh) << ReimportMesh.Message;
	Durin::DMaterialInstance* Instance = nullptr;
	for (const auto& Output : Imported.Outputs)
	{
		Durin::DObject* Object = nullptr;
		ASSERT_TRUE(Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(Output.AssetPath), Object));
		if (auto* Candidate = Durin::Cast<Durin::DMaterialInstance>(Object)) Instance = Candidate;
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

TEST(FSceneImportTests, AssetForgeRejectsUnsupportedSceneFeaturesWithoutPartialPublication)
{
	FSceneFixture Fixture = InitializeFixture("Unsupported");
	std::ofstream Source(Fixture.Source, std::ios::trunc);
	ASSERT_TRUE(Source.is_open());
	Source << R"({"asset":{"version":"2.0"},"materials":[{"name":"MustNotPublish"}],"skins":[]})";
	Source.close();
	Durin::AssetForge::Builtins::FSceneImportResult Imported;
	EXPECT_FALSE(Durin::AssetForge::Builtins::ImportSceneAssets(
		Fixture.Source, Fixture.DestinationDirectory,
		Durin::FStaticMeshImportSettings::MakeDurin(), Imported));
	EXPECT_FALSE(Imported);
	EXPECT_TRUE(Imported.Outputs.empty());
	EXPECT_FALSE(Durin::FindAssetExact(MakeAssetPath(
		"/SceneImportTests/SceneImport/Unsupported/Materials/MustNotPublish")));
}

TEST(FSceneImportTests, DirectImportHonorsCancellationBeforePublication)
{
	const FSceneFixture Fixture = InitializeFixture("Scheduled");
	Durin::AssetForge::Builtins::FSceneImportResult Result;
	EXPECT_FALSE(Durin::AssetForge::Builtins::ImportSceneAssets(
		Fixture.Source, Fixture.DestinationDirectory,
		Durin::FStaticMeshImportSettings::MakeDurin(), Result, [] { return true; }));
	EXPECT_FALSE(Result);
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
	ASSERT_TRUE(EnsureStandardMaterialFunctions(Functions, Error)) << Error;
	auto* Material = EnsureImportedSurfaceMaterial(Error);
	ASSERT_NE(Material, nullptr) << Error;
	EXPECT_EQ(Material->GetMaterialProgram()->Nodes.size(), 65u);
	EXPECT_EQ(Material->GetMaterialFunctionCalls().size(), 9u);
	EXPECT_EQ(Material->GetParameterDefinitions().size(), 48u);
	EXPECT_TRUE(Material->GetMaterialProgram()->Outputs.Surface.SourceOutputId.IsValid());
	for (const auto& Node : Material->GetMaterialProgram()->Nodes)
		EXPECT_TRUE(Node.Opcode == EMaterialProgramOpcode::Parameter
			|| Node.Opcode == EMaterialProgramOpcode::TextureParameter
			|| Node.Opcode == EMaterialProgramOpcode::UVChannel
			|| Node.Opcode == EMaterialProgramOpcode::FunctionCall);
	FMaterialCompilerInput Input;
	FMaterialCompilerEnvironment Environment;
	ASSERT_TRUE(BuildDefaultMaterialCompilerEnvironment(Environment, Error)) << Error;
	ASSERT_TRUE(SnapshotMaterialCompilerInput(*Material, Environment, Input));
	const auto Normalized = NormalizeMaterialProgram(Input);
	ASSERT_TRUE(Normalized) << (Normalized.Diagnostics.empty() ? "no diagnostic" : Normalized.Diagnostics.front().Message);
	EXPECT_EQ(Normalized.Layout.ResourceFieldCount, 8u);
	EXPECT_EQ(std::ranges::count(Normalized.IR.Nodes, EMaterialProgramOpcode::TextureSample2D, &FMaterialIRNode::Opcode), 8);
	const auto NormalizedSource = GenerateMaterialProgramSlang(Normalized.IR, Normalized.Layout);
	ASSERT_TRUE(NormalizedSource);
	size_t NormalizedSamples = 0;
	for (size_t Offset = 0; (Offset = NormalizedSource.Source.find(".Sample(", Offset)) != std::string::npos; ++Offset)
		++NormalizedSamples;
	EXPECT_EQ(NormalizedSamples, 8u);

	const auto Compact = *Material->GetMaterialProgram();
	const std::vector<FMaterialFunctionCall> CompactCalls(Material->GetMaterialFunctionCalls().begin(), Material->GetMaterialFunctionCalls().end());
	auto Packed = Compact;
	auto PackedCalls = CompactCalls;
	auto& PackedCall = PackedCalls.back();
	const auto PortId = [](uint32 Slot) { return StandardMaterialPortId(EStandardMaterialFunction::StandardPBR, Slot); };
	const auto ORMTexture = std::ranges::find(PackedCall.Inputs, PortId(22), &FMaterialFunctionInputBinding::InputId)->Source;
	const auto ORMUV = std::ranges::find(PackedCall.Inputs, PortId(32), &FMaterialFunctionInputBinding::InputId)->Source;
	std::erase_if(PackedCall.Inputs, [&](const auto& Binding) {
		for (uint32 Slot : {22u, 23u, 24u, 32u, 33u, 34u}) if (Binding.InputId == PortId(Slot)) return true;
		return false;
	});
	PackedCall.Inputs.push_back({PortId(40), EMaterialProgramValueType::Texture2D, ORMTexture});
	PackedCall.Inputs.push_back({PortId(41), EMaterialProgramValueType::Float2, ORMUV});
	PackedCall.Function = Functions.StandardPBR_ORM.Get();
	PackedCall.Outputs = {{StandardMaterialPortId(EStandardMaterialFunction::StandardPBR_ORM, 100), EMaterialProgramValueType::Surface}};
	Packed.Outputs.Surface.SourceOutputId = PackedCall.Outputs.front().OutputId;
	ASSERT_TRUE(Material->SetMaterialProgramAndFunctionCalls(Packed, PackedCalls));
	ASSERT_TRUE(SnapshotMaterialCompilerInput(*Material, Environment, Input));
	const auto PackedNormalized = NormalizeMaterialProgram(Input);
	ASSERT_TRUE(PackedNormalized) << (PackedNormalized.Diagnostics.empty() ? "no diagnostic" : PackedNormalized.Diagnostics.front().Message);
	EXPECT_EQ(PackedNormalized.Layout.ResourceFieldCount, 6u);
	EXPECT_EQ(std::ranges::count(PackedNormalized.IR.Nodes, EMaterialProgramOpcode::TextureSample2D, &FMaterialIRNode::Opcode), 6);
	const auto PackedNormalizedSource = GenerateMaterialProgramSlang(PackedNormalized.IR, PackedNormalized.Layout);
	ASSERT_TRUE(PackedNormalizedSource);
	size_t PackedNormalizedSamples = 0;
	for (size_t Offset = 0; (Offset = PackedNormalizedSource.Source.find(".Sample(", Offset)) != std::string::npos; ++Offset)
		++PackedNormalizedSamples;
	EXPECT_EQ(PackedNormalizedSamples, 6u);

	ASSERT_TRUE(Material->SetMaterialProgramAndFunctionCalls(Compact, CompactCalls));
	const auto Original = Functions.StandardPBR->GetFunctionGraph();
	auto Edited = Original;
	const auto Constant = std::ranges::find(Edited.Nodes, EMaterialProgramOpcode::Constant, &FMaterialProgramNode::Opcode);
	ASSERT_NE(Constant, Edited.Nodes.end());
	Constant->Literal.X = .08f;
	ASSERT_TRUE(Functions.StandardPBR->SetFunctionGraph(Edited));
	ASSERT_TRUE(SavePackage(Functions.StandardPBR->GetPackage()));
	ASSERT_TRUE(EnsureStandardMaterialFunctions(Functions, Error)) << Error;
	EXPECT_EQ(Functions.StandardPBR->GetFunctionGraph(), Edited);
	Edited.Signature.Inputs.front().Id = FGuid::NewGuid();
	// Removing a stable declaration and replacing its terminal/default links is a valid but incompatible interface.
	const auto OldId = Original.Signature.Inputs.front().Id;
	for (auto& Node : Edited.Nodes)
		if (Node.FunctionPortId == OldId) Node.FunctionPortId = Edited.Signature.Inputs.front().Id;
	for (auto& Port : Edited.Signature.Inputs)
		if (Port.Default.InputId == OldId) Port.Default.InputId = Edited.Signature.Inputs.front().Id;
	ASSERT_TRUE(Functions.StandardPBR->SetFunctionGraph(Edited));
	EXPECT_FALSE(EnsureStandardMaterialFunctions(Functions, Error));
	EXPECT_NE(Error.find("interface"), std::string::npos);
	EXPECT_EQ(Functions.StandardPBR->GetFunctionGraph(), Edited);
	ASSERT_TRUE(Functions.StandardPBR->SetFunctionGraph(Original));
	ASSERT_TRUE(SavePackage(Functions.StandardPBR->GetPackage()));
	auto Reload = ReloadPackages({.Packages = {Functions.StandardPBR->GetPackage()}});
	const auto Reloaded = Reload.Wait();
	ASSERT_TRUE(Reloaded) << (Reloaded.Diagnostics.empty() ? "no diagnostic" : Reloaded.Diagnostics.front().Message);
	ASSERT_TRUE(EnsureStandardMaterialFunctions(Functions, Error)) << Error;
	EXPECT_EQ(Functions.StandardPBR->GetFunctionGraph(), Original);
	EXPECT_EQ(Functions.StandardPBR->GetAuthoringSourceVersion(), StandardMaterialFunctionVersion);
	EXPECT_EQ(Material->GetMaterialFunctionCalls().back().Function.Get(), Functions.StandardPBR.Get());
}

TEST(FSceneImportTests, RecognizedLegacyParentUpgradesOnceAndModifiedParentIsPreserved)
{
	using namespace Durin;
	using namespace Durin::AssetForge::Builtins;
	const auto Fixture = InitializeFixture("LegacyParent");
	std::string Error;
	auto* Material = EnsureImportedSurfaceMaterial(Error);
	ASSERT_NE(Material, nullptr) << Error;
	const auto Compact = *Material->GetMaterialProgram();
	const std::vector<FMaterialFunctionCall> CompactCalls(Material->GetMaterialFunctionCalls().begin(), Material->GetMaterialFunctionCalls().end());
	const auto Identity = Material->GetObjectPath();
	const auto Legacy = Testing::MakeLegacyPBRMaterialProgram();
	ASSERT_TRUE(Material->SetMaterialProgramAndFunctionCalls(Legacy, {}));
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	EXPECT_EQ(EnsureImportedSurfaceMaterial(Error), Material) << Error;
	EXPECT_EQ(Material->GetObjectPath(), Identity);
	EXPECT_EQ(*Material->GetMaterialProgram(), Compact);
	EXPECT_EQ(EnsureImportedSurfaceMaterial(Error), Material) << Error;
	ASSERT_TRUE(Material->SetMaterialProgramAndFunctionCalls(Testing::MakeLegacyAggregatePBRMaterialProgram(), {}));
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	EXPECT_EQ(EnsureImportedSurfaceMaterial(Error), Material) << Error;
	EXPECT_EQ(*Material->GetMaterialProgram(), Compact);
	auto Modified = Legacy;
	Modified.Nodes.front().DisplayName = "My custom graph";
	ASSERT_TRUE(Material->SetMaterialProgramAndFunctionCalls(Modified, {}));
	EXPECT_EQ(EnsureImportedSurfaceMaterial(Error), nullptr);
	EXPECT_EQ(*Material->GetMaterialProgram(), Modified);
	ASSERT_TRUE(Material->SetMaterialProgramAndFunctionCalls(Compact, CompactCalls));
}
