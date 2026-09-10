#include "Asset/Cook.h"
#include "AssetRegistry/Scan.h"
#include "Asset/Load.h"
#include "Asset/PackageSerialization.h"
#include "Asset/PackageInspection.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/Object.h"
#include "DObject/Package.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/MountPathTestSupport.h"
#include "NativeTestSupport.h"
#include "NativeDObjectTestSupport.h"
#include "NativeAssetTestSupport.h"
#include "NativeAssetRuntimeTestSupport.h"
#include "Texture/Texture2D.h"
#include "Texture/TextureCube.h"
#include "Texture/VolumeTexture.h"
#include "Materials/Material.h"
#include "StaticMesh/StaticMesh.h"
#include "Threading/Task.h"

#include <gtest/gtest.h>
#if defined(__APPLE__)
#include <spawn.h>
#include <sys/wait.h>
#include <signal.h>
extern char** environ;
#endif

namespace
{
	using namespace Durin;

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

	auto RunCook(const std::filesystem::path& Project, const std::filesystem::path& Output,
		std::string_view Extra = {}) -> int32
	{
		int32 Code = -1;
		std::string Error;
		EXPECT_TRUE(FPlatformProcess::ExecuteProcess(DURIN_COOK_EXECUTABLE,
			std::format("cook --project=\"{}\" --output=\"{}\" --target=win64 --profile=game --root=/Game/Saved --json {}",
				Project.generic_string(), Output.generic_string(), Extra), Code, &Error)) << Error;
		return Code;
	}
#if defined(__APPLE__)
	auto RunCancelledCook(const std::filesystem::path& Project,
		const std::filesystem::path& Output) -> int32
	{
		std::vector<std::string> Values{DURIN_COOK_EXECUTABLE, "cook",
			"--project=" + Project.generic_string(), "--output=" + Output.generic_string(),
			"--target=win64", "--profile=game", "--root=/Game/Saved", "--no-incremental", "--json"};
		std::vector<char*> Arguments;
		for (auto& Value : Values) Arguments.push_back(Value.data());
		Arguments.push_back(nullptr);
		pid_t Child = 0;
		if (posix_spawn(&Child, Arguments.front(), nullptr, nullptr, Arguments.data(), environ) != 0)
			return -1;
		int Status = 0;
		bool Interrupted = false;
		const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
		while (waitpid(Child, &Status, WNOHANG) == 0)
		{
			// Log creation proves main has installed SIGINT handling. Use a fresh output
			// tree so an old session cannot trigger an interrupt before process startup.
			if (!Interrupted && std::filesystem::exists(Output / "Logs"))
			{
				Interrupted = kill(Child, SIGINT) == 0;
			}
			if (std::chrono::steady_clock::now() > Deadline)
			{
				kill(Child, SIGKILL);
				waitpid(Child, &Status, 0);
				return -1;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return Interrupted && WIFEXITED(Status) ? WEXITSTATUS(Status) : -1;
	}
#endif

}

TEST(FStandaloneCookProcessTests, PreservesSourcesAndExcludesParentResidentObjects)
{
	Testing::InitializeDObjectSystemForTests();
	const auto Fixture = Testing::CreateTestFixtureDirectory("StandaloneCook");
	const auto Source = Fixture / "Project";
	const auto Content = Source / "Content";
	std::filesystem::create_directories(Content);
	const auto Project = Source / "Cook.dproject";
	const std::string Descriptor = R"({"ProjectName":"Cook"})";
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(std::as_bytes(std::span(Descriptor)), Project));
	Testing::FScopedMountRegistryFixture Mounts;
	Testing::RegisterMountPointForTests("/Game/", Content.generic_string());
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/Game/Saved", Path));
	DPackage* Package = CreatePackage(Path);
	ASSERT_NE(Package, nullptr);
	ASSERT_NE(NewObject<DObject>(Package, "Saved"), nullptr);
	ASSERT_TRUE(SavePackage(Package));
	// Keep an unsaved object resident in this process throughout child execution.
	ASSERT_NE(NewObject<DObject>(Package, "Unsaved"), nullptr);
	const auto Before = Inventory(Source);
	const auto Output = Fixture / "Output";
	ASSERT_EQ(RunCook(Project, Output), 0);
	EXPECT_EQ(Inventory(Source), Before);
	FAssetPackageInspection Inspection;
	ASSERT_TRUE(InspectAssetPackage((Output / "Game/Saved.dasset").generic_string(), Path, Inspection));
	EXPECT_FALSE(std::ranges::any_of(Inspection.Objects, [](const auto& Object) {
		return Object.ObjectName == "Unsaved";
	}));
	ASSERT_EQ(RunCook(Project, Output), 0);
	EXPECT_EQ(Inventory(Source), Before);
	ASSERT_EQ(RunCook(Project, Fixture / "DryRun", "--dry-run"), 0);
	EXPECT_FALSE(std::filesystem::exists(Fixture / "DryRun/CookManifest.bin"));
	EXPECT_EQ(Inventory(Source), Before);
	EXPECT_EQ(RunCook(Project, Fixture / "Failed", "--root=/Game/Missing"), 1);
	EXPECT_EQ(Inventory(Source), Before);
#if defined(__APPLE__)
	EXPECT_EQ(RunCancelledCook(Project, Fixture / "Cancelled"), 130);
	EXPECT_FALSE(std::filesystem::exists(Fixture / "Cancelled/CookManifest.bin"));
	EXPECT_EQ(Inventory(Source), Before);
#endif
	EXPECT_EQ(RunCook(Project, Content / "Rejected"), 1);
	EXPECT_FALSE(std::filesystem::exists(Content / "Rejected"));
	EXPECT_EQ(RunCook(Project, Source), 1);
	EXPECT_EQ(Inventory(Source), Before);
	std::error_code Error;
	std::filesystem::create_directory_symlink(Content, Fixture / "Alias", Error);
	if (!Error)
	{
		EXPECT_EQ(RunCook(Project, Fixture / "Alias/Rejected"), 1);
		EXPECT_FALSE(std::filesystem::exists(Content / "Rejected"));
		EXPECT_EQ(Inventory(Source), Before);
	}
	const auto EscapedOutput = Fixture / "EscapedOutput";
	std::filesystem::create_directories(EscapedOutput);
	Error.clear();
	std::filesystem::create_directory_symlink(Content, EscapedOutput / "Game", Error);
	if (!Error)
	{
		EXPECT_EQ(RunCook(Project, EscapedOutput), 1);
		EXPECT_EQ(Inventory(Source), Before);
		EXPECT_FALSE(std::filesystem::exists(EscapedOutput / "Logs"));
	}
	FByteBuffer PriorManifest;
	ASSERT_TRUE(FFileHelper::LoadFileToArray(PriorManifest, Output / "CookManifest.bin"));
	FPackagePath CycleA, CycleB;
	ASSERT_TRUE(FPackagePath::TryCreate("/Game/CycleA", CycleA));
	ASSERT_TRUE(FPackagePath::TryCreate("/Game/CycleB", CycleB));
	DAssetRedirector* RedirectA = nullptr;
	DAssetRedirector* RedirectB = nullptr;
	ASSERT_TRUE(Testing::CreateAssetRedirectorForTests(CycleA, Path, RedirectA));
	ASSERT_TRUE(Testing::CreateAssetRedirectorForTests(CycleB, Path, RedirectB));
	auto* Destination = static_cast<FObjectProperty*>(DAssetRedirector::StaticClass()->FindPropertyByName("DestinationObject"));
	ASSERT_NE(Destination, nullptr);
	Destination->SetObjectPropertyValue(RedirectA, RedirectB);
	Destination->SetObjectPropertyValue(RedirectB, RedirectA);
	ASSERT_TRUE(SavePackage(RedirectA->GetPackage()));
	ASSERT_TRUE(SavePackage(RedirectB->GetPackage()));
	const auto WithCycle = Inventory(Source);
	EXPECT_EQ(RunCook(Project, Output, "--root=/Game/CycleA"), 1);
	EXPECT_EQ(Inventory(Source), WithCycle);
	FByteBuffer AfterFailure;
	ASSERT_TRUE(FFileHelper::LoadFileToArray(AfterFailure, Output / "CookManifest.bin"));
	EXPECT_EQ(AfterFailure, PriorManifest);
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(FByteBuffer{std::byte{0xff}}, Content / "Saved.dasset"));
	const auto CorruptSource = Inventory(Source);
	EXPECT_EQ(RunCook(Project, Output), 1);
	EXPECT_EQ(Inventory(Source), CorruptSource);
	ASSERT_TRUE(FFileHelper::LoadFileToArray(AfterFailure, Output / "CookManifest.bin"));
	EXPECT_EQ(AfterFailure, PriorManifest);
	Destination->SetObjectPropertyValue(RedirectA, nullptr);
	Destination->SetObjectPropertyValue(RedirectB, nullptr);
	EXPECT_TRUE(UnloadPackage(RedirectA->GetPackage(), EAssetPackageUnloadPolicy::DiscardUnsaved));
	EXPECT_TRUE(UnloadPackage(RedirectB->GetPackage(), EAssetPackageUnloadPolicy::DiscardUnsaved));
	EXPECT_TRUE(UnloadPackage(Package, EAssetPackageUnloadPolicy::DiscardUnsaved));
}

TEST(FStandaloneCookProcessTests, CooksSavedFamiliesAndReusesValidatedOutputs)
{
	Testing::InitializeDObjectSystemForTests();
	const auto Fixture = Testing::CreateTestFixtureDirectory("CookFamilies");
	const auto Source = Fixture / "Project";
	std::filesystem::create_directories(Source / "Content");
	const auto Project = Source / "Cook.dproject";
	const std::string Descriptor = R"({"ProjectName":"Cook"})";
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(std::as_bytes(std::span(Descriptor)), Project));
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
	FTextureCubeSourceData Faces;
	for (auto& Face : Faces.Faces) Face = PixelsImage;
	Faces.SourceChannelCounts.fill(4);
	FTextureCubeImportedData CubeInput;
	ASSERT_TRUE(CubeInput.SetSourceData(Faces));
	auto* Cube = Make.operator()<DTextureCube>("Cube");
	auto PreparedCubeSource = Durin::PrepareTextureCubeSource(CubeInput);
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
    std::vector<float> HdrPixels(8*4*4,4.0f);
    const auto HdrBytes=std::as_bytes(std::span(HdrPixels));
    ASSERT_TRUE(Image::FImage::TryCreate({.Width=8,.Height=4,.Format=Image::ERawImageFormat::RGBA32F,
        .GammaSpace=Image::EImageGammaSpace::Linear},FByteBuffer(HdrBytes.begin(),HdrBytes.end()),HdrImage));
    auto HdrSource=PrepareTextureCubePanoramaSource(HdrImage.GetView(),4,0);
    ASSERT_TRUE(HdrSource);
    Environment->SetSource(std::move(*HdrSource));
    Environment->SetBuildSettings(ETextureCubeSourceLayout::EquirectangularPanorama,4,0,8,4,false,ETextureCubeOutput::HDR);
    ASSERT_TRUE(SavePackage(Environment->GetPackage()));
	FPackagePath AliasPath;
	ASSERT_TRUE(FPackagePath::TryCreate("/Game/Alias", AliasPath));
	DAssetRedirector* Alias = nullptr;
	ASSERT_TRUE(Testing::CreateAssetRedirectorForTests(AliasPath, Paths[1], Alias));
	ASSERT_TRUE(SavePackage(Alias->GetPackage()));
	const auto Before = Inventory(Source);
	const auto Output = Fixture / "Output";
	const std::string Roots = "--root=/Game/Alias --root=/Game/Cube --root=/Game/Volume --root=/Game/Material --root=/Game/Mesh --root=/Game/Environment";
	ASSERT_EQ(RunCook(Project, Output, Roots), 0);
	EXPECT_EQ(Inventory(Source), Before);
	const auto First = Inventory(Output / "Game");
	ASSERT_EQ(RunCook(Project, Output, Roots), 0);
	EXPECT_EQ(Inventory(Source), Before);
	EXPECT_EQ(Inventory(Output / "Game"), First);
	FByteBuffer StateBytes;
	ASSERT_TRUE(FFileHelper::LoadFileToArray(StateBytes, Output / "CookState.bin"));
	FCookState State;
	ASSERT_TRUE(DecodeCookState(StateBytes, State));
	ASSERT_EQ(State.Entries.size(), Paths.size());
	for (const auto& Entry : State.Entries)
		if (Entry.Contributor != "generic-package") EXPECT_FALSE(Entry.BuildDependencies.empty());
	ASSERT_EQ(RunCook(Project, Output, Roots + " --no-incremental"), 0);
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
		if (Asset->IsA(DTexture::StaticClass()))
			EXPECT_TRUE(static_cast<DTexture*>(Asset)->EnsurePlatformDataLoadedBlocking());
		if (Asset->IsA(DStaticMesh::StaticClass()))
			EXPECT_TRUE(static_cast<DStaticMesh*>(Asset)->EnsureRenderDataLoadedBlocking());
        if (Path.GetView()=="/Game/Environment")
        {
            auto* Hdr=Cast<DTextureCube>(Asset);
            ASSERT_NE(Hdr,nullptr);
            EXPECT_EQ(Hdr->GetBuiltPixelFormat(),EPixelFormat::RGBA32_FLOAT);
            float Value=0;
            std::memcpy(&Value,Hdr->GetPlatformData()->Faces[0].Mips[0].Pixels.data(),sizeof(float));
            EXPECT_FLOAT_EQ(Value,4.0f);
        }
	}
	EXPECT_EQ(Inventory(Source), Before);
}
