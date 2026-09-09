#include "EngineTestSupport.h"
#include "VulkanEngineTestSupport.h"
#include "Asset/AssetCompilingManager.h"
#include "Asset/PackageReload.h"
#include "Actors/VolumetricCloudActor.h"
#include "Actors/DirectionalLightActor.h"
#include "Components/VolumetricCloudComponent.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/StrongObjectPtr.h"
#include "DynamicRHI.h"
#include "Editor/WorkspaceRootWindow.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "Image/ImageEncoder.h"
#include "Misc/MountPathTestSupport.h"
#include "Misc/Paths.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RendererModule.h"
#include "RenderingThread.h"
#include "SceneViewProjection.h"
#include "SceneTestAccess.h"
#include "Texture/Texture2D.h"
#include "Texture/VolumeTexture.h"

#include <gtest/gtest.h>

namespace Durin
{
namespace
{
// Owns the production scene while the fixture drives offscreen frames.
class FReloadSceneEngine final : public DEngine
{
public:
    FReloadSceneEngine() : DEngine(FObjectInitializer::Get()) {}
    auto InstallScene(FScenePtr Scene) -> FScene*
    {
        MainScene = std::move(Scene);
        return static_cast<FScene*>(MainScene.get());
    }
    auto ResetScene() -> void { FSceneInterfaceTestAccess::ReleaseScene(MainScene); }
};
		auto BuildViewMatrix(const FVector3& Location, const FVector3& Forward)
			-> FMatrix
		{
			const FVector3 Right = Math::Normalize(
				Math::Cross(FVectorConstants::Up, Forward)
			);
			const FVector3 Up = Math::Normalize(Math::Cross(Forward, Right));
			FMatrix View(1.0);
			View[0][0] = Forward.x;
			View[1][0] = Forward.y;
			View[2][0] = Forward.z;
			View[3][0] = -Math::Dot(Forward, Location);
			View[0][1] = Right.x;
			View[1][1] = Right.y;
			View[2][1] = Right.z;
			View[3][1] = -Math::Dot(Right, Location);
			View[0][2] = Up.x;
			View[1][2] = Up.y;
			View[2][2] = Up.z;
			View[3][2] = -Math::Dot(Up, Location);
			return View;
		}

		auto MakeSceneCloudView(uint32 Width, uint32 Height) -> FSceneView
		{
			FSceneView View;
			View.ViewLocation = {0.0, 0.0, 2500.0};
			const FVector3 Forward =
				Math::Normalize(FVector3(1.0, 0.0, 0.1));
			View.ViewMatrix = BuildViewMatrix(View.ViewLocation, Forward);
			EXPECT_TRUE(SceneViewProjection::BuildPerspectiveProjection(
				60.0, static_cast<double>(Width) / Height, 0.1, 500000.0,
				ESceneDepthConvention::ReversedZ, View.ProjectionMatrix
			));
			View.ViewProjectionMatrix = View.ProjectionMatrix * View.ViewMatrix;
			View.ViewportWidth = Width;
			View.ViewportHeight = Height;
			View.DepthConvention = ESceneDepthConvention::ReversedZ;
			View.ClearColor = {0.05f, 0.08f, 0.12f, 1.0f};
			return View;
		}


auto SetDensity(DVolumeTexture& Texture, uint8 Value) -> void
{
	FVolumeTextureSourceData Source{.Width = 2, .Height = 2, .Depth = 2,
		.Format = EVolumeTextureFormat::R8_UNORM};
	ASSERT_TRUE(Source.SetVoxelBytes(FByteBuffer(8, static_cast<std::byte>(Value))));
	auto Prepared = PrepareVolumeTextureSource(Source);
	ASSERT_TRUE(Prepared);
	Texture.SetSource(std::move(*Prepared));
	Texture.PostLoad();
	ASSERT_TRUE(Texture.FinishReloadResourcePreparation());
}

auto SetWeather(DTexture2D& Texture, uint8 Value) -> void
{
	Image::FImage Pixels;
	ASSERT_TRUE(Image::FImage::TryCreate({.Width = 4, .Height = 4,
		.Format = Image::ERawImageFormat::RGBA8},
		FByteBuffer(64, static_cast<std::byte>(Value)), Pixels));
	FTextureSource Source;
	ASSERT_TRUE(Source.Init2D(Pixels.GetView(), 4));
	Texture.SetSource(std::move(Source));
	Texture.PostLoad();
	ASSERT_TRUE(Texture.FinishReloadResourcePreparation());
}

auto SaveEvidence(std::string_view Name, const FByteBuffer& Pixels) -> void
{
	FByteBuffer Png;
	ASSERT_TRUE(Image::EncodeRgba8Png(Pixels, 96, 64, Png));
	std::ofstream File(Testing::GetTestWorkDirectory() / (std::string(Name) + ".png"),
		std::ios::binary);
	File.write(reinterpret_cast<const char*>(Png.data()), static_cast<std::streamsize>(Png.size()));
	ASSERT_TRUE(File.good());
}
}

TEST(FAssetPackageReloadVulkanTests, DiscardRestoresRenderedWeatherAndVolumeFromDisk)
{
	InitializeDObjectSystem();
	ASSERT_TRUE(InitializeAssetCompilingManager());
	FModuleManager::Get().LoadModuleChecked("TextureBuild");
	Testing::FScopedMountRegistryFixture Mounts;
	Testing::RegisterMountPointForTests("/Engine/", FPaths::EngineContentDir());
	const auto Root = Testing::CreateTestFixtureDirectory("ReloadSceneAssets");
	Testing::RegisterMountPointForTests("/ReloadScene/", Root.generic_string() + "/");
	ASSERT_TRUE(RefreshAssetRegistry());
	FModuleManager::Get().LoadModuleChecked("RenderCore");
	ASSERT_TRUE(InitializeGameThreadDeferredExecutor());
	RHIInit(Tests::GetVulkanEngineTestInitializationContext());
	ASSERT_NE(GDynamicRHI, nullptr);
	InitRenderingThread();
	FRendererModule Renderer;
	FModuleTestHarness RendererLifecycle("AssetPackageReloadVulkan");
	RendererLifecycle.Start(Renderer);
	FReloadSceneEngine Engine;
	FScene* Scene = Engine.InstallScene(Renderer.CreateScene());
	GEngine = &Engine;
	auto* World = NewObject<DWorld>(&Engine, "ReloadWorld");
	TStrongObjectPtr<DWorld> WorldRoot(World);
	ASSERT_TRUE(World->InitializeSubsystems());
	ASSERT_TRUE(World->SetCurrentLevel(NewObject<DLevel>(World, "ReloadLevel")));
	Engine.SetWorld(World);
	ASSERT_NE(World->SpawnActor<ADirectionalLightActor>("Sun"), nullptr);
	auto* Actor = World->SpawnActor<AVolumetricCloudActor>("Cloud");
	ASSERT_NE(Actor, nullptr);
	auto* Cloud = Actor->GetVolumetricCloudComponent();
	FPackagePath WeatherPath, VolumePath;
	ASSERT_TRUE(FPackagePath::TryCreate("/ReloadScene/Weather", WeatherPath));
	ASSERT_TRUE(FPackagePath::TryCreate("/ReloadScene/Volume", VolumePath));
	DTexture2D* Weather = nullptr;
	DVolumeTexture* Volume = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(WeatherPath, Weather));
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(VolumePath, Volume));
	SetWeather(*Weather, 255);
	SetDensity(*Volume, 220);
	auto* Detail = NewObject<DVolumeTexture>(nullptr, "Detail");
	SetDensity(*Detail, 0);
	ASSERT_TRUE(SavePackage(Weather->GetPackage()));
	ASSERT_TRUE(SavePackage(Volume->GetPackage()));
	const auto WeatherIdentity = Weather->GetSource().GetIdentity();
	const auto VolumeIdentity = Volume->GetSource().GetIdentity();
	Cloud->SetWeatherTexture(Weather);
	Cloud->SetBaseDensityTexture(Volume);
	Cloud->SetDetailDensityTexture(Detail);
	Cloud->RegisterComponent();
	FlushRenderingCommands();

	auto Render = [&]() {
		FByteBuffer Pixels;
		ERenderViewResult Result = ERenderViewResult::RendererResourcesUnavailable;
		TryEnqueueRenderCommand("RenderPackageReloadScene", [&](FRHICommandListImmediate& Commands) {
			auto Output = GDynamicRHI->RHICreateTexture(Commands,
				FRHITextureCreateDesc::Create2D("ReloadSceneOutput", 96, 64, EPixelFormat::SRGBA8_UNORM)
				.SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource
					| ETextureCreateFlags::CPUReadback));
			if (!Output) return;
			++GRenderFrameCounterRenderThread;
			GDynamicRHI->RHIBeginFrame_RenderThread(Commands);
			auto View = MakeSceneCloudView(96, 64);
			Result = Renderer.RenderView(Commands, Scene, View, Output, false, {});
			GDynamicRHI->RHIEndFrame_RenderThread(Commands);
			Commands.ImmediateFlush(EImmediateFlushType::FlushRHIThread);
			EXPECT_TRUE(GDynamicRHI->RHIReadTexture2D(Commands, Output, 0, 0, Pixels));
		});
		FlushRenderingCommands();
		EXPECT_EQ(Result, ERenderViewResult::Success);
		EXPECT_EQ(Pixels.size(), 96u * 64u * 4u);
		return Pixels;
	};
	const auto Saved = Render();
	SaveEvidence("saved", Saved);
	Editor::FEditableAssetDocumentModel Documents;
	ASSERT_TRUE(Documents.Activate({.Id = {1}, .ResourceId = WeatherPath.ToString()}, Weather));
	SetWeather(*Weather, 0);
	Weather->GetPackage()->MarkDirty();
	const auto EditedWeather = Render();
	EXPECT_NE(EditedWeather, Saved);
	SaveEvidence("weather-edited", EditedWeather);
	for (auto Fault : {EPackageReloadFaultPoint::PrepareRuntimeProduct,
		EPackageReloadFaultPoint::ReserveRenderPublish})
	{
		FPackageReloadRequest Request{.Packages = {Weather->GetPackage()}};
		Request.ShouldFail = [Fault](EPackageReloadFaultPoint Point, uint64, uint64) {
			return Point == Fault;
		};
		EXPECT_EQ(ReloadPackages(Request).GetResult().Status, EPackageReloadStatus::Failed);
		CollectGarbage();
		EXPECT_EQ(Cloud->GetWeatherTexture(), Weather);
		EXPECT_TRUE(Weather->GetPackage()->IsDirty());
		EXPECT_EQ(Render(), EditedWeather);
	}
	ASSERT_TRUE(Documents.Discard(Weather));
	Weather = Cloud->GetWeatherTexture();
	ASSERT_NE(Weather, nullptr);
	EXPECT_EQ(Weather->GetSource().GetIdentity(), WeatherIdentity);
	const auto RestoredWeather = Render();
	EXPECT_EQ(RestoredWeather, Saved);
	SaveEvidence("weather-discarded", RestoredWeather);

	ASSERT_TRUE(Documents.Activate({.Id = {2}, .ResourceId = VolumePath.ToString()}, Volume));
	SetDensity(*Volume, 0);
	Volume->GetPackage()->MarkDirty();
	const auto EditedVolume = Render();
	EXPECT_NE(EditedVolume, Saved);
	SaveEvidence("volume-edited", EditedVolume);
	ASSERT_TRUE(Documents.Discard(Volume));
	Volume = Cloud->GetBaseDensityTexture();
	ASSERT_NE(Volume, nullptr);
	EXPECT_EQ(Volume->GetSource().GetIdentity(), VolumeIdentity);
	const auto RestoredVolume = Render();
	EXPECT_EQ(RestoredVolume, Saved);
	SaveEvidence("volume-discarded", RestoredVolume);
	ASSERT_TRUE(SavePackage(Weather->GetPackage()));
	ASSERT_TRUE(SavePackage(Volume->GetPackage()));

	Cloud->UnregisterComponent();
	Engine.SetWorld(nullptr);
	Engine.ResetScene();
	GEngine = nullptr;
	WorldRoot.Reset();
	MarkObjectHierarchyAsGarbage(World);
	MarkAsGarbage(Detail);
	CollectGarbage();
	ASSERT_TRUE(UnloadPackage(WeatherPath));
	ASSERT_TRUE(UnloadPackage(VolumePath));
	Weather = nullptr;
	Volume = nullptr;
	ASSERT_TRUE(LoadObject(Testing::MakePackageLeafAssetObjectPathForTests(WeatherPath), Weather));
	ASSERT_TRUE(LoadObject(Testing::MakePackageLeafAssetObjectPathForTests(VolumePath), Volume));
	EXPECT_EQ(Weather->GetSource().GetIdentity(), WeatherIdentity);
	EXPECT_EQ(Volume->GetSource().GetIdentity(), VolumeIdentity);
	ASSERT_TRUE(UnloadPackage(WeatherPath));
	ASSERT_TRUE(UnloadPackage(VolumePath));
	CollectGarbage();
	FlushRenderingCommands();
	RendererLifecycle.Shutdown();
	FlushRenderingCommands();
	ShutdownRenderingThread();
	FRHICommandListImmediate::Get().SwitchPipeline(ERHIPipeline::None);
	RHIExit();
	ShutdownAssetCompilingManager();
	ShutdownTaskScheduler();
}
}
