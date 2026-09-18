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
#include "Hash/XxHash.h"
#include "Materials/Material.h"
#include "DObject/StrongObjectPtr.h"
#include "DObject/DObjectArray.h"
#include "Materials/MaterialProgramCompiler.h"
#include "EditorReimportHandler.h"
#include "StaticMesh/StaticMeshFactoryTestSupport.h"
#include "Asset/AssetCompilingManager.h"
#include "../Materials/ExplicitMaterialProgramTestFixture.h"
#include "../Materials/StandardMaterialFunctionTestFixture.h"
#include "StaticMesh/StaticMesh.h"
#include "Components/StaticMeshComponent.h"

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
			auto* Parent = Durin::Cast<Durin::DMaterial>(Material->GetParent());
			ASSERT_NE(Parent, nullptr);
			EXPECT_TRUE(Parent->GetPackage()->GetPackagePath().starts_with("/SceneImportTests/Materials/ImportedParents/Surface_v1_"));
			EXPECT_EQ(Parent->GetImportProvenance().StructuralKey, Material->GetImportProvenance().StructuralKey);
			EXPECT_EQ(Material->GetImportProvenance().OutputIdentity, Output.StableIdentity);
			EXPECT_LT(Parent->GetParameterDefinitions().size(), 48u);
			EXPECT_EQ(Material->GetLocalParameterValueCount(), Parent->GetParameterDefinitions().size());
			EXPECT_EQ(std::ranges::count_if(Parent->GetExpressionCollection().Expressions,
				[](const auto& Expression) { return Durin::Cast<Durin::DMaterialExpressionTextureSampleParameter2D>(Expression.Get()) != nullptr; }), 1);
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
	// The family-only geometry route must also retain authored material slots.
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

TEST(FSceneImportTests, SceneReimportResetsEditsAndRollsBackSavedAndLiveOutputs)
{
	using namespace Durin;
	using namespace Durin::AssetForge::Builtins;
	const auto Fixture = InitializeFixture("StructuralReimport");
	const auto Initial = RunScene(Fixture);
	ASSERT_TRUE(Initial) << Initial.Message;
	for (const std::string_view Class : {"Durin::DStaticMesh", "Durin::DMaterialInstance", "Durin::DTexture2D"})
		for (const auto& Output : Initial.Outputs)
			if (Output.AssetClassName == Class) ASSERT_TRUE(UnloadPackage(Output.AssetPath));
	CollectGarbage();
	DMaterialInstance* Previous = nullptr;
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
		const auto Loaded = LoadObject(Testing::MakePackageLeafAssetObjectPathForTests(Output.AssetPath), Object);
		ASSERT_TRUE(Loaded) << Output.AssetPath.ToString() << ": " << Loaded.Message;
		if (auto* Material = Cast<DMaterialInstance>(Object)) { Previous = Material; MaterialPath = Output.AssetPath; }
		if (auto* Mesh = Cast<DStaticMesh>(Object)) PreviousMesh = Mesh;
		SavedBytes.push_back({Output.AssetPath, Read(FindAssetExact(Output.AssetPath)->PhysicalPath)});
	}
	ASSERT_NE(Previous, nullptr);
	ASSERT_NE(PreviousMesh, nullptr);
	using Kind = Durin::AssetForge::Builtins::MaterialParameters::EMaterialBuiltinParameterKind;
	const auto Color = Durin::AssetForge::Builtins::GetMaterialSurfaceParameterId(EMaterialSurfaceOutput::BaseColor, Kind::Value);
	ASSERT_TRUE(Previous->SetParameterValue(Color, FMaterialParameterValue::MakeVector4({0.2, 0.3, 0.4, 0})));
	auto Properties = Previous->GetPropertyOverrides();
	Properties.bOverrideShadingModel = true;
	Properties.Values.ShadingModel = EMaterialShadingModel::Unlit;
	ASSERT_TRUE(Previous->SetPropertyOverrides(Properties));
	auto* Consumer = NewObject<DStaticMeshComponent>(nullptr, "ReimportConsumer");
	Consumer->SetStaticMesh(PreviousMesh);
	auto* Dependent = NewObject<DMaterialInstance>(nullptr, "ReimportDependent");
	ASSERT_TRUE(Dependent->SetParent(Previous));
	auto* Independent = NewObject<DMaterialInstance>(nullptr, "IndependentMaterial");
	ASSERT_TRUE(Independent->SetParent(Previous->GetParent()));
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
		FSceneImportResult Failed;
		EXPECT_FALSE(ImportSceneAssets(Fixture.Source, Fixture.DestinationDirectory,
			FStaticMeshImportSettings::MakeDurin(), Failed, {}, {.ShouldFail = [&](EAssetBundleSavePhase Phase, size_t) {
				EXPECT_EQ(Consumer->GetStaticMesh(), PreviousMesh);
				EXPECT_EQ(Dependent->GetParent(), Previous);
				if (Phase != Failure) return false;
				bReached = true;
				return true;
			}})) << Failed.Message;
		ASSERT_TRUE(bReached) << Failed.Message;
		EXPECT_EQ(Consumer->GetStaticMesh(), PreviousMesh);
		EXPECT_EQ(Dependent->GetParent(), Previous);
		for (const auto& [Path, Bytes] : SavedBytes) EXPECT_EQ(Read(FindAssetExact(Path)->PhysicalPath), Bytes);
	}
	const auto Changed = RunScene(Fixture);
	ASSERT_TRUE(Changed) << Changed.Message;
	DMaterialInstance* Material = nullptr;
	ASSERT_TRUE(LoadObject(Testing::MakePackageLeafAssetObjectPathForTests(MaterialPath), Material));
	ASSERT_NE(Material, Previous);
	EXPECT_EQ(Dependent->GetParent(), Material);
	EXPECT_TRUE(Dependent->GetMaterialCompileStatus().IsCurrent());
	ASSERT_TRUE(Dependent->GetAcceptedCompiledProgram());
	EXPECT_EQ(Dependent->GetAcceptedCompiledProgram()->ActiveParameters.size(), Material->GetAcceptedCompiledProgram()->ActiveParameters.size());
	EXPECT_NE(Consumer->GetStaticMesh(), PreviousMesh);
	EXPECT_EQ(Consumer->GetMaterial(), Material);
	EXPECT_FALSE(Material->HasLocalParameterValue(Color));
	EXPECT_EQ(Material->GetStaticProperties().ShadingModel, EMaterialShadingModel::Lit);
	EXPECT_TRUE(Material->GetStaticProperties().bTwoSided);
	EXPECT_NE(Material->GetImportProvenance().StructuralKey, PreviousKey);
	std::ofstream(Fixture.Source, std::ios::trunc) << OriginalSource;
	const auto Restored = RunScene(Fixture);
	ASSERT_TRUE(Restored) << Restored.Message;
	ASSERT_TRUE(LoadObject(Testing::MakePackageLeafAssetObjectPathForTests(MaterialPath), Material));
	FResolvedMaterialParameter Value;
	ASSERT_TRUE(Material->ResolveParameterValue(Color, Value));
	EXPECT_EQ(FVector3(Value.Value.GetVector4()), FVector3(0.5, 0.75, 0.25));
	EXPECT_FALSE(Material->IsParameterValueOrphan(Color));
	EXPECT_FALSE(Material->GetStaticProperties().bTwoSided);
	ASSERT_TRUE(Independent->ResolveParameterValue(Color, Value));
	EXPECT_EQ(FVector3(Value.Value.GetVector4()), FVector3(0.1f, 0.2f, 0.3f));
	EXPECT_EQ(Material->GetStaticProperties().ShadingModel, EMaterialShadingModel::Lit);
	const auto OtherSource = std::filesystem::path(Fixture.Source).parent_path() / "OtherSource" /
		std::filesystem::path(Fixture.Source).filename();
	std::filesystem::create_directories(OtherSource.parent_path());
	std::ofstream(OtherSource) << OriginalSource;
	FSceneImportResult Collision;
	EXPECT_FALSE(ImportSceneAssets(OtherSource.generic_string(), Fixture.DestinationDirectory,
		FStaticMeshImportSettings::MakeDurin(), Collision));
	EXPECT_FALSE(Collision.bPersisted);
	EXPECT_EQ(Consumer->GetMaterial(), Material);
}

TEST(FSceneImportTests, FailedPublicationDiscardsGeneratedParentAndRetrySucceeds)
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
	std::array<FImportedSurfaceRole, 8> Roles;
	const FMaterialSurfaceOutputs Defaults;
	for (uint32 I = 0; I < 8; ++I) Roles[I].Value = GetMaterialSurfaceOutputDefault(Defaults, static_cast<EMaterialSurfaceOutput>(I));
	Roles[0].Value = {1, 1, 1};
	Roles[0].Sample = FImportedSurfaceSample{.ResourceIdentity = "irrelevant"};
	const auto Recipe = MakeImportedSurfaceRecipe(Roles);
	const auto ParentPath = MakeAssetPath("/SceneImportTests/Materials/ImportedParents/Surface_v1_" +
		FXxHash128::HashBuffer(std::as_bytes(std::span(Recipe.CanonicalKey))).ToString());
	ASSERT_FALSE(FindAssetExact(ParentPath));
	ASSERT_EQ(FindResidentPackage(ParentPath), nullptr);
	for (const auto Failure : {EAssetBundleSavePhase::CreateDirectories, EAssetBundleSavePhase::StagePackage,
		EAssetBundleSavePhase::PublishPackage, EAssetBundleSavePhase::PublishRootPackage, EAssetBundleSavePhase::PublishRegistry})
	{
		FSceneImportResult Failed;
		bool bInjected = false;
		ASSERT_FALSE(ImportSceneAssets(Fixture.Source, Fixture.DestinationDirectory,
			FStaticMeshImportSettings::MakeDurin(), Failed, {}, {.ShouldFail = [&](EAssetBundleSavePhase Phase, size_t) {
				EXPECT_EQ(FindResidentPackage(ParentPath), nullptr);
				for (const auto& Output : Failed.Outputs) EXPECT_EQ(FindResidentPackage(Output.AssetPath), nullptr);
				if (Phase != Failure) return false;
				bInjected = true;
				return true;
			}}));
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
	DMaterialInstance* Instance = nullptr;
	for (const auto& Output : Imported.Outputs)
		if (Output.Role == "MaterialInstance")
			ASSERT_TRUE(LoadObject(Testing::MakePackageLeafAssetObjectPathForTests(Output.AssetPath), Instance));
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
	DMaterialInstance* Instance = nullptr;
	uint32 TextureCount = 0;
	for (const auto& Output : Imported.Outputs)
	{
		if (Output.AssetClassName == "Durin::DTexture2D") ++TextureCount;
		if (Output.Role == "MaterialInstance")
			ASSERT_TRUE(LoadObject(Testing::MakePackageLeafAssetObjectPathForTests(Output.AssetPath), Instance));
	}
	ASSERT_NE(Instance, nullptr);
	EXPECT_EQ(TextureCount, 1u);
	const auto* Parent = Cast<DMaterial>(Instance->GetParent());
	ASSERT_NE(Parent, nullptr);
	EXPECT_EQ(std::ranges::count_if(Parent->GetExpressionCollection().Expressions,
		[](const auto& Expression) { return Cast<DMaterialExpressionTextureSampleParameter2D>(Expression.Get()) != nullptr; }), 1);
	const auto& Outputs = Parent->GetExpressionOutputs();
	EXPECT_EQ(Outputs.Metallic.ExpressionId, Outputs.Roughness.ExpressionId);
	EXPECT_EQ(Outputs.Metallic.ExpressionId, Outputs.AmbientOcclusion.ExpressionId);
	EXPECT_EQ(Outputs.Metallic.OutputIndex, 4);
	EXPECT_EQ(Outputs.Roughness.OutputIndex, 3);
	EXPECT_EQ(Outputs.AmbientOcclusion.OutputIndex, 2);
	EXPECT_EQ(Instance->GetParameterDefinitions().size(), 2u);
	EXPECT_EQ(Instance->GetLocalParameterValueCount(), 2u);
	EXPECT_FALSE(Instance->GetImportProvenance().OutputIdentity.empty());
	DTexture2D* Texture = nullptr;
	ASSERT_TRUE(Instance->GetTextureParameterValue(Durin::AssetForge::Builtins::MaterialParameters::MetallicTextureName(), Texture));
	ASSERT_NE(Texture, nullptr);
	EXPECT_EQ(Texture->GetUsage(), ETextureUsage::DataMask);
}

TEST(FSceneImportTests, StructuralParentsReuseAcrossDestinationsAndRejectAuthoredChanges)
{
	using namespace Durin;
	using namespace Durin::AssetForge::Builtins;
	const auto Fixture = InitializeFixture("ParentReuse");
	const auto First = RunScene(Fixture);
	ASSERT_TRUE(First) << First.Message;
	DMaterialInstance* Instance = nullptr;
	for (const auto& Output : First.Outputs)
		if (Output.Role == "MaterialInstance")
			ASSERT_TRUE(LoadObject(Testing::MakePackageLeafAssetObjectPathForTests(Output.AssetPath), Instance));
	ASSERT_NE(Instance, nullptr);
	auto* Parent = Cast<DMaterial>(Instance->GetParent());
	ASSERT_NE(Parent, nullptr);
	const auto Revision = Parent->GetPackage()->GetEditRevision();
	FSceneImportResult Second;
	ASSERT_TRUE(ImportSceneAssets(Fixture.Source, MakeAssetPath("/SceneImportTests/Second"),
		FStaticMeshImportSettings::MakeDurin(), Second)) << Second.Message;
	for (const auto& Output : Second.Outputs)
		if (Output.Role == "MaterialInstance")
		{
			DMaterialInstance* Reused = nullptr;
			ASSERT_TRUE(LoadObject(Testing::MakePackageLeafAssetObjectPathForTests(Output.AssetPath), Reused));
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
	Changed.AmbientOcclusionDefault = .3f;
	ASSERT_TRUE(ApplyOutputs(Changed));
	FSceneImportResult Rejected;
	EXPECT_FALSE(ImportSceneAssets(Fixture.Source, MakeAssetPath("/SceneImportTests/Rejected"),
		FStaticMeshImportSettings::MakeDurin(), Rejected));
	EXPECT_NE(Rejected.Message.find("modified"), std::string::npos);
	EXPECT_EQ(Parent->GetExpressionOutputs(), Changed);
	for (const auto& Output : Rejected.Outputs) EXPECT_FALSE(FindAssetExact(Output.AssetPath));
	ASSERT_TRUE(ApplyOutputs(Original));
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
	// An empty Engine mount must fail without synthesizing any packages.
	ASSERT_FALSE(LoadStandardMaterialFunctions(Functions, Error));
	EXPECT_NE(Error.find("Missing standard function"), std::string::npos);
	EXPECT_FALSE(FindAssetExact(MakeAssetPath("/Engine/Materials/Functions/UVTransform")));
	EXPECT_EQ(FindResidentPackage(MakeAssetPath("/Engine/Materials/Functions/UVTransform")), nullptr);
	const std::array Names{"UVTransform", "SampleNormal", "SampleORM", "StandardPBR", "StandardPBR_ORM", "ImportedSurfaceValues"};
	const std::array Slots{&Functions.UVTransform, &Functions.SampleNormal, &Functions.SampleORM,
		&Functions.StandardPBR, &Functions.StandardPBR_ORM, &Functions.ImportedSurfaceValues};
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
			static_cast<EStandardMaterialFunction>(Index + 1), Functions).Apply(*Function));
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
		Outputs.AmbientOcclusion, Outputs.Emissive, Outputs.Opacity, Outputs.OpacityMask}) EXPECT_TRUE(Link.ExpressionId.IsValid());
	MIR::FCompilerInput Input;
	FMaterialCompilerEnvironment Environment;
	const auto EnvironmentResult = BuildDefaultMaterialCompilerEnvironment(Environment);
	ASSERT_TRUE(EnvironmentResult) << FormatMaterialError(EnvironmentResult.Error);
	ASSERT_TRUE(SnapshotMaterialCompilerInput(*Material, Environment, Input));
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

	auto Packed = Testing::MakeStandardMaterialExpressionsForTest(Functions);
	TStrongObjectPtr<DMaterialExpressionFunctionCall> PackedCall(NewObject<DMaterialExpressionFunctionCall>(nullptr, NAME_None));
	PackedCall->Id = FGuid::NewGuid(); PackedCall->Function = Functions.StandardPBR_ORM.Get();
	const auto PortId = [](uint32 Slot) { return StandardMaterialPortId(EStandardMaterialFunction::StandardPBR, Slot); };
	for (uint32 Role = 0; Role < 8; ++Role)
	{
		const auto Owner = [&](Durin::AssetForge::Builtins::MaterialParameters::EMaterialBuiltinParameterKind Kind) -> DMaterialExpressionParameter* {
			const auto Id = Durin::AssetForge::Builtins::GetMaterialSurfaceParameterId(static_cast<EMaterialSurfaceOutput>(Role), Kind);
			for (const auto& E : Packed.Expressions)
				if (auto* P = Cast<DMaterialExpressionParameter>(E.Get()); P && P->Metadata.Id == Id) return P;
			return nullptr;
		};
		const auto* Factor = Owner(Durin::AssetForge::Builtins::MaterialParameters::EMaterialBuiltinParameterKind::Value);
		ASSERT_NE(Factor, nullptr);
		const auto Type = Cast<DMaterialExpressionScalarParameter>(Factor) ? EMaterialProgramValueType::Float : EMaterialProgramValueType::Float3;
		FMaterialExpressionInput FactorInput{Factor->Id};
		if (Type == EMaterialProgramValueType::Float3)
		{
			TStrongObjectPtr<DMaterialExpressionSwizzle> Mask(NewObject<DMaterialExpressionSwizzle>(nullptr, NAME_None));
			Mask->Id = FGuid::NewGuid(); Mask->Input = FactorInput; Mask->Components = {0, 1, 2};
			FactorInput = {Mask->Id}; Packed.Expressions.emplace_back(Mask.Get());
		}
		PackedCall->Inputs.push_back({PortId(10 + Role), Type, FactorInput});
		if (Role == 3 || Role == 4) continue;
		const auto* Sample = Cast<DMaterialExpressionTextureSampleParameter2D>(Owner(Durin::AssetForge::Builtins::MaterialParameters::EMaterialBuiltinParameterKind::Texture));
		ASSERT_NE(Sample, nullptr);
		PackedCall->Inputs.push_back({PortId(Role == 2 ? 40 : 20 + Role), EMaterialProgramValueType::Texture2D, {Sample->Id, 7}});
		PackedCall->Inputs.push_back({PortId(Role == 2 ? 41 : 30 + Role), EMaterialProgramValueType::Float2, Sample->UV});
	}
	PackedCall->Outputs = {{StandardMaterialPortId(EStandardMaterialFunction::StandardPBR_ORM, 100), EMaterialProgramValueType::Surface}};
	Packed.Outputs = {}; Packed.Outputs.Surface = {.ExpressionId = PackedCall->Id, .OutputId = PackedCall->Outputs[0].OutputId}; Packed.Outputs.bUseMaterialAttributes = true;
	Packed.Expressions.emplace_back(PackedCall.Get());
	ASSERT_TRUE(Packed.Apply(*Material));
	ASSERT_TRUE(SnapshotMaterialCompilerInput(*Material, Environment, Input));
	const auto PackedNormalized = MIR::Normalize(Input);
	ASSERT_TRUE(PackedNormalized) << (PackedNormalized.Diagnostics.empty() ? "no diagnostic" : Durin::FormatMaterialError(PackedNormalized.Diagnostics.front().Error));
	EXPECT_EQ(PackedNormalized.Layout.ResourceFieldCount, 4u);
	EXPECT_EQ(std::ranges::count(PackedNormalized.IR.Nodes, EMaterialProgramOpcode::TextureSample2D, &MIR::FNode::Opcode), 4);
	const auto PackedNormalizedSource = GenerateMaterialProgramSlang(PackedNormalized.IR, PackedNormalized.Layout);
	ASSERT_TRUE(PackedNormalizedSource);
	size_t PackedNormalizedSamples = 0;
	for (size_t Offset = 0; (Offset = PackedNormalizedSource.Source.find(".Sample(", Offset)) != std::string::npos; ++Offset)
		++PackedNormalizedSamples;
	EXPECT_EQ(PackedNormalizedSamples, 4u);

	ASSERT_TRUE(Compact.Apply(*Material));
	const auto Clone = [](const auto& Expressions) {
		std::vector<TStrongObjectPtr<DMaterialExpression>> Result;
		for (const auto& Expression : Expressions) Result.emplace_back(DuplicateObject(Expression.Get(), nullptr, NAME_None).Object);
		return Result;
	};
	const auto Original = Clone(Functions.StandardPBR->GetExpressionCollection().Expressions);
	const auto OriginalSignature = Functions.StandardPBR->GetFunctionSignature();
	auto Edited = Clone(Original);
	auto Signature = OriginalSignature;
	const auto Apply = [&](const auto& Expressions, const FMaterialFunctionSignature& CandidateSignature) {
		std::vector<DMaterialExpression*> Values;
		for (const auto& Expression : Expressions) Values.push_back(Expression.Get());
		return Functions.StandardPBR->SetFunctionExpressions(Durin::Testing::WithFunctionPorts(CandidateSignature, Values));
	};
	const auto Matches = [&](const auto& Expressions, const FMaterialFunctionSignature& ExpectedSignature) {
		const auto& Actual = Functions.StandardPBR->GetExpressionCollection().Expressions;
		if (Functions.StandardPBR->GetFunctionSignature() != ExpectedSignature || Actual.size() != Expressions.size()) return false;
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
	DMaterialExpressionScalarConstant* Constant = nullptr;
	for (const auto& Expression : Edited)
		if (auto* Scalar = Cast<DMaterialExpressionScalarConstant>(Expression.Get())) { Constant = Scalar; break; }
	ASSERT_NE(Constant, nullptr);
	Constant->Value = .08f;
	ASSERT_TRUE(Apply(Edited, Signature));
	ASSERT_TRUE(SavePackage(Functions.StandardPBR->GetPackage()));
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
	ASSERT_TRUE(SavePackage(Functions.StandardPBR->GetPackage()));
	auto Reload = ReloadPackages({.Packages = {Functions.StandardPBR->GetPackage()}});
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
