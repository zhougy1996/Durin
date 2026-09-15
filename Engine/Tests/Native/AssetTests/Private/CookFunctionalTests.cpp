#include "Asset/Cook.h"
#include "AssetRegistry/Scan.h"
#include "Asset/Load.h"
#include "Asset/PackageSerialization.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/Object.h"
#include "DObject/Package.h"
#include "Asset/OfflinePreparation.h"
#include "Misc/FileHelper.h"
#include "Misc/MountPathTestSupport.h"
#include "Modules/ModuleManager.h"
#include "Shader/GlobalShader.h"
#include "ShaderBuild/ShaderPaths.h"
#include "NativeTestSupport.h"
#include "NativeDObjectTestSupport.h"
#include "NativeAssetTestSupport.h"
#include "NativeAssetRuntimeTestSupport.h"
#include "Texture/Texture2D.h"
#include "Texture/TextureCube.h"
#include "Texture/VolumeTexture.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "StaticMesh/StaticMesh.h"
#include "Threading/Task.h"

#include <gtest/gtest.h>

namespace
{
	using namespace Durin;

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
			EXPECT_TRUE(FFileHelper::LoadFileToArray(Bytes, Entry.path()));
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
			FModuleManager::Get().LoadModuleChecked("TextureBuild");
			FModuleManager::Get().LoadModuleChecked("StaticMeshBuild");
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
		auto SetUp() -> void override
		{
			std::string Error;
			ASSERT_TRUE(RegisterEngineCookContributors(Handles, Error)) << Error;
			const auto Generic = RegisterCookContributor(DObject::StaticClass(), {
				.Name = "generic-package",
				.Contribute = [](DObject& Object, std::string_view Path, FCookContext& Context) -> FAssetResult {
					std::string Error;
					if (!Context.AddPackage(std::string(Path), Object.GetPackage(), &Error))
						return {EAssetError::InvalidPackageType, std::move(Error)};
					return {};
				},
				.DeclareDependencies = [](const FCookDependencyRequest&, std::vector<FCookDependencyDeclaration>&) -> FAssetResult { return {}; }});
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
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << Result.Diagnostic;
	const auto Published = Inventory(Request.OutputRoot);
	EXPECT_EQ(Inventory(Content), Before);
	ASSERT_TRUE(LoadPackage(Path, Package));
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
	EXPECT_EQ(Inventory(Request.OutputRoot), Published);
	Request.ExplicitRoots = {Missing};
	EXPECT_FALSE(FCookCoordinator().Run(Request, Result));
	EXPECT_EQ(Inventory(Request.OutputRoot), Published);
	Request.ExplicitRoots = {Path};
	Request.IsCancelled = [] { return true; };
	EXPECT_FALSE(FCookCoordinator().Run(Request, Result));
	EXPECT_EQ(Result.Status, ECookRunStatus::Cancelled);
	EXPECT_EQ(Inventory(Request.OutputRoot), Published);
	Request.IsCancelled = {};
	Request.OutputRoot = Fixture / "DryRun";
	Request.bDryRun = true;
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << Result.Diagnostic;
	EXPECT_FALSE(std::filesystem::exists(Request.OutputRoot / "CookManifest.bin"));
	EXPECT_EQ(Inventory(Content), Before);
}

TEST_F(FCookFunctionalTests, CooksSavedFamiliesAndReusesValidatedOutputs)
{
	const auto Fixture = Testing::CreateTestFixtureDirectory("CookFamilies");
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
	Image::FImage PixelsImage;
	ASSERT_TRUE(Image::FImage::TryCreate({.Width = 4, .Height = 4,
		.Format = Image::ERawImageFormat::RGBA8}, FByteBuffer(4 * 4 * 4, std::byte{0xff}), PixelsImage));
	FTextureSource Pixels;
	ASSERT_TRUE(Pixels.Init2D(PixelsImage.GetView(), 4));
	auto* Texture = Make.operator()<DTexture2D>("Texture");
	Texture->SetSource(std::move(Pixels));
	ASSERT_TRUE(SavePackage(Texture->GetPackage()));
	FTextureCubeDecodedFaces Faces;
	for (auto& Face : Faces.Faces) Face = PixelsImage;
	Faces.SourceChannelCounts.fill(4);
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
	FStaticMeshDecodedGeometry Geometry;
	Geometry.MaterialSlots.push_back({"Material", 0, "Material"});
	auto& Triangle = Geometry.Meshes.emplace_back();
	Triangle.Name = "Triangle";
	Triangle.Positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
	Triangle.Indices = {0, 1, 2};
	FStaticMeshSource MeshInput;
	ASSERT_TRUE(MeshInput.Initialize(std::move(Geometry), Error)) << Error;
	*DStaticMesh::StaticClass()->FindPropertyByName("Source")
		->ContainerPtrToValuePtr<FStaticMeshSource>(Mesh) = std::move(MeshInput);
	*DStaticMesh::StaticClass()->FindPropertyByName("MaterialSlots")
		->ContainerPtrToValuePtr<std::vector<FMeshMaterialSlotDefinition>>(Mesh) = {{.Name = "Material", .DefaultMaterial = Material}};
	ASSERT_TRUE(SavePackage(Mesh->GetPackage()));
	auto* Environment = Make.operator()<DTextureCube>("Environment");
	Image::FImage HdrImage;
	std::vector<float> HdrPixels(8 * 4 * 4, 4.0f);
	const auto HdrBytes = std::as_bytes(std::span(HdrPixels));
	ASSERT_TRUE(Image::FImage::TryCreate({.Width = 8, .Height = 4, .Format = Image::ERawImageFormat::RGBA32F,
		.GammaSpace = Image::EImageGammaSpace::Linear}, FByteBuffer(HdrBytes.begin(), HdrBytes.end()), HdrImage));
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
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << Result.Diagnostic;
	EXPECT_EQ(Inventory(Source), Before);
	const auto First = Inventory(Output / "Game");
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << Result.Diagnostic;
	ASSERT_EQ(Result.Packages.size(), Paths.size());
	for (const auto& Package : Result.Packages) EXPECT_EQ(Package.Status, ECookPackageStatus::CookHit);
	EXPECT_EQ(Inventory(Source), Before);
	EXPECT_EQ(Inventory(Output / "Game"), First);
	FByteBuffer StateBytes;
	ASSERT_TRUE(FFileHelper::LoadFileToArray(StateBytes, Output / "CookState.bin"));
	FCookState State;
	ASSERT_TRUE(DecodeCookState(StateBytes, State));
	ASSERT_EQ(State.Entries.size(), Paths.size());
	for (const auto& Path : Paths)
		EXPECT_TRUE(std::ranges::any_of(State.Entries, [&](const auto& Entry) { return Entry.VirtualPackagePath == Path.GetView(); })) << Path.GetView();
	for (const auto& Entry : State.Entries)
		if (Entry.Contributor != "generic-package") EXPECT_FALSE(Entry.BuildDependencies.empty());
	Request.IncrementalPolicy = ECookIncrementalPolicy::Disabled;
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << Result.Diagnostic;
	EXPECT_EQ(Inventory(Output / "Game"), First);
	EXPECT_EQ(Inventory(Source), Before);
	ASSERT_TRUE(InitializeTaskScheduler(2));
	struct FTaskCleanup { ~FTaskCleanup() { ShutdownTaskSystem(ETaskShutdownMode::Drain); } } TaskCleanup;
	Testing::FScopedAssetRuntimeForTests Runtime;
	ASSERT_TRUE(Runtime.RestartCooked(Output));
	Testing::RegisterMountPointForTests("/Game/", (Output / "Game").generic_string(), true, false);
	ASSERT_TRUE(RefreshAssetRegistry(EAssetRegistryScanMode::FullValidation));
	for (const auto& Path : Paths)
	{
		DPackage* Loaded = nullptr;
		const auto Result = LoadPackage(Path, Loaded);
		ASSERT_TRUE(Result) << Path.GetView() << ": " << Result.Message;
		EXPECT_NE(Loaded, nullptr);
		auto* Asset = Loaded->FindTopLevelAsset(Path.GetPackageName());
		ASSERT_NE(Asset, nullptr);
		if (auto* LoadedMaterial = Cast<DMaterialInterface>(Asset))
		{
			const auto Program = LoadedMaterial->GetAcceptedCompiledProgram();
			ASSERT_TRUE(Program) << LoadedMaterial->GetMaterialCookDiagnostic();
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
