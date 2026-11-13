#include "CoreMinimal.h"

#if DURIN_WATER_PLUGIN_TESTS
#include "WaterSurfaceActor.h"
#include "WaterSurfaceComponent.h"
#include "WaterRenderingTestSupport.h"
#include "EngineTestSupport.h"
#include "AssetRegistry/Scan.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "LightSceneTestSupport.h"
#include "Materials/MaterialInterface.h"
#include "Misc/MountPathTestSupport.h"
#include "Misc/Paths.h"
#include "Shader/ShaderData.h"
#include "VulkanEngineTestSupport.h"

namespace
{
	[[maybe_unused]] auto GetWaterCookDirectory() -> std::filesystem::path
	{
		const char* Override = std::getenv("DURIN_WATER_COOK_DIR");
		return Override ? std::filesystem::path(Override)
			: std::filesystem::path(Durin::FPaths::RootDir()) / "Build/WaterSurfaceValidation/Cooked";
	}
	// Match project startup: plugin mounts precede all reflected defaults,
	// including defaults created by the independent rendering proof.
	class FWaterContentEnvironment final : public ::testing::Environment
	{
		auto SetUp() -> void override
		{
			using namespace Durin;
			const auto GameContent = Testing::GetTestWorkDirectory() / "GameContent";
			std::filesystem::create_directories(GameContent);
			const std::array Definitions{FMountPoint{
				.VirtualRoot = "/Plugins/Water/", .Owner = EMountOwner::Extension,
				.Root = std::filesystem::path(FPaths::RootDir()) / "Engine/Plugins/Water/Content", .bAutoScan = true},
				FMountPoint{.VirtualRoot = "/Engine/", .Owner = EMountOwner::Engine,
					.Root = std::filesystem::path(FPaths::RootDir()) / "Engine/Content"},
				FMountPoint{.VirtualRoot = "/Game/", .Owner = EMountOwner::ActiveProject, .Root = GameContent}};
			Mounts = std::make_unique<Testing::FScopedMountRegistryFixture>(Definitions);
			ASSERT_TRUE(Mounts->IsValid()) << Mounts->GetError();
#if !DURIN_WITH_EDITOR
			ASSERT_TRUE(InitializeShaderData(FShaderDataConfiguration::Cooked(GetWaterCookDirectory())));
#endif
			InitializeDObjectSystem();
			ASSERT_TRUE(InitializeAssetCompilingManager());
		}
		auto TearDown() -> void override { Durin::ShutdownAssetCompilingManager(); Mounts.reset(); }
		std::unique_ptr<Durin::Testing::FScopedMountRegistryFixture> Mounts;
	};
	[[maybe_unused]] const auto* WaterContentEnvironment = ::testing::AddGlobalTestEnvironment(new FWaterContentEnvironment);
}

TEST(FWaterSurfaceComponentVulkanTests, RegistrationEditsAndRuntimeRenderingShareParameters)
{
	using namespace Durin;
	using namespace Durin::Tests;
	using namespace Durin::Tests::Water;
	InitializeDObjectSystem();
#if DURIN_WITH_EDITOR
	ASSERT_TRUE(RefreshAssetRegistry());
#else
	Testing::FScopedAssetRuntimeForTests Assets;
	ASSERT_TRUE(Assets.RestartCooked(GetWaterCookDirectory()));
#endif
	FModuleManager::Get().LoadModule("RenderCore");
	RHIInit(GetVulkanEngineTestInitializationContext());
	ASSERT_NE(GDynamicRHI, nullptr);
	InitRenderingThread();
	FRendererModule Renderer;
	FModuleTestHarness Lifecycle("WaterSurfaceComponent"); Lifecycle.Start(Renderer);
	FGeometryRendererCleanup RenderCleanup(Lifecycle);
	FSoftObjectPath MaterialPath;
	ASSERT_TRUE(FSoftObjectPath::TryCreate("/Plugins/Water/Materials/DefaultWater.DefaultWater", MaterialPath));
	const auto LoadedMaterial = LoadObjectDetailed<DMaterialInterface>(MaterialPath);
	ASSERT_TRUE(LoadedMaterial) << LoadedMaterial.error().Message;
	TStrongObjectPtr<DMaterialInterface> WaterMaterial(*LoadedMaterial);
	WaterMaterial->CacheResourceShadersForRendering(FSceneInterface::GetDefaultFeatureLevel(), EMaterialShaderPrecompileMode::Synchronous);
	ASSERT_TRUE(WaterMaterial->GetAcceptedCompiledProgram(FSceneInterface::GetDefaultFeatureLevel()))
		<< WaterMaterial->GetMaterialCookDiagnostic(FSceneInterface::GetDefaultFeatureLevel());
	{
		FSceneTestOwner Scene;
		TStrongObjectPtr<DWorld> World(NewObject<DWorld>(nullptr, NAME_None));
		const std::shared_ptr<void> WorldCleanup(nullptr, [&](void*) {
			World->CleanupWorld(); World->SetRenderScene(nullptr); Scene.Reset(); FlushRenderingCommands();
		});
		World->InitializeSubsystems();
		auto* Level = NewObject<DLevel>(World.Get(), NAME_None);
		auto* Actor = Level->SpawnActor<AWaterSurfaceActor>();
		ASSERT_NE(Actor, nullptr);
		auto* Component = Actor->GetWaterComponent();
		auto Settings = Component->GetSettings();
		Settings.Extent = {1.5, 1.2}; Settings.Elevation = .5f;
		ASSERT_TRUE(Component->SetSettings(Settings));
		ASSERT_TRUE(World->SetCurrentLevel(Level));
		World->SetRenderScene(Scene.Get());
		ASSERT_TRUE(Component->IsRegistered());
		FDirectionalLightSceneData Light; Light.Intensity = 3;
		ASSERT_NE(PublishLightForTest<FDirectionalLightSceneProxy>(*Scene, FLightComponentId(840), Light), nullptr);
		auto Capture = [&](double Time, FByteBuffer& Pixels) {
			FSceneView View;
			View.ViewportWidth = 192; View.ViewportHeight = 108; View.MaterialTimeSeconds = Time;
			View.Settings.Mode.RenderMode = ERenderMode::Lit;
			const auto Target = FRHITextureCreateDesc::Create2D("WaterComponent", 192, 108, EPixelFormat::SRGBA8_UNORM)
				.SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource | ETextureCreateFlags::SourceCopy);
			return RenderPreparedGeometryView(Renderer, *Scene, View, Target, {},
				[&](FRHICommandListImmediate& C, FRHITexture* T) { ReadWaterPixels(C, T, Pixels); });
		};
		std::array<FByteBuffer, 6> Pixels;
		ASSERT_EQ(Capture(0, Pixels[0]), ERenderViewResult::Success) << Component->GetDiagnostic();
		ASSERT_TRUE(Component->GetDiagnostic().empty()) << Component->GetDiagnostic();
		ASSERT_EQ(Capture(.75, Pixels[1]), ERenderViewResult::Success);
		ASSERT_NE(Pixels[0], Pixels[1]);
		ASSERT_EQ(Capture(0, Pixels[2]), ERenderViewResult::Success);
		EXPECT_EQ(Pixels[0], Pixels[2]);
		Settings.Amplitude = 0; Settings.Elevation = .6f;
		ASSERT_TRUE(Component->SetSettings(Settings));
		EXPECT_EQ(Component->GetCustomPrimitiveData().Values[0], 0.f);
		const auto Query = Component->QuerySurface({0, 0, 0}, .75);
		ASSERT_TRUE(Query); EXPECT_NEAR(Query->Position.z, .6, 1.e-6);
		ASSERT_EQ(Capture(.75, Pixels[3]), ERenderViewResult::Success);
		EXPECT_NE(Pixels[1], Pixels[3]);
		Component->UnregisterComponent();
		ASSERT_EQ(Capture(.75, Pixels[4]), ERenderViewResult::Success);
		EXPECT_NE(Pixels[3], Pixels[4]);
		Component->RegisterComponent();
		ASSERT_EQ(Capture(.75, Pixels[5]), ERenderViewResult::Success) << Component->GetDiagnostic();
		EXPECT_EQ(Pixels[3], Pixels[5]);
	}
}
#endif
