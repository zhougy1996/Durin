#include "CoreMinimal.h"

#if DURIN_WATER_PLUGIN_TESTS
#include "WaterSurfaceActor.h"
#include "WaterSurfaceComponent.h"
#include "WaterRenderingTestSupport.h"
#include "EngineTestSupport.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "LightSceneTestSupport.h"
#include "Misc/MountPathTestSupport.h"
#include "Misc/Paths.h"
#include "Shader/ShaderData.h"
#include "VulkanEngineTestSupport.h"

TEST(FWaterSurfaceComponentVulkanTests, RegistrationEditsAndRuntimeRenderingShareParameters)
{
	using namespace Durin;
	using namespace Durin::Tests;
	using namespace Durin::Tests::Water;
#if !DURIN_WITH_EDITOR
	const char* ProofOverride = std::getenv("DURIN_WATER_PROOF_DIR");
	const std::filesystem::path ProofDirectory = ProofOverride ? std::filesystem::path(ProofOverride)
		: std::filesystem::path(FPaths::RootDir()) / "Build/WaterSurfaceValidation/Proof";
	if (!GetConfiguredShaderTarget()) ASSERT_TRUE(InitializeShaderData(FShaderDataConfiguration::Cooked(ProofDirectory)));
#endif
	InitializeDObjectSystem();
	const std::array MountDefinitions{FMountPoint{
		.VirtualRoot = "/Plugins/Water/", .Owner = EMountOwner::Extension,
		.Root = std::filesystem::path(FPaths::RootDir()) / "Engine/Plugins/Water/Content"}};
	Testing::FScopedMountRegistryFixture Mounts(MountDefinitions);
	ASSERT_TRUE(Mounts.IsValid()) << Mounts.GetError();
	FModuleManager::Get().LoadModule("RenderCore");
	RHIInit(GetVulkanEngineTestInitializationContext());
	ASSERT_NE(GDynamicRHI, nullptr);
	InitRenderingThread();
	FRendererModule Renderer;
	FModuleTestHarness Lifecycle("WaterSurfaceComponent"); Lifecycle.Start(Renderer);
	FGeometryRendererCleanup RenderCleanup(Lifecycle);
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
