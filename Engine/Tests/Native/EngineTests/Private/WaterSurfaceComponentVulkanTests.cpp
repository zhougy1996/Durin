#include "CoreMinimal.h"
#include "Misc/UnifiedError.h"

#if DURIN_WATER_PLUGIN_TESTS
#include "WaterSurfaceActor.h"
#include "WaterSurfaceComponent.h"
#include "WaterWaves.h"
#include "WaterRenderingTestSupport.h"
#include "Renderers/StaticMeshRenderPreparation.h"
#include "EngineTestSupport.h"
#include "AssetRegistry/Scan.h"
#include "DObject/Class.h"
#include "DObject/Property.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "LightSceneTestSupport.h"
#include "Math/Operations.h"
#include "Materials/MaterialInterface.h"
#include "Misc/FileHelper.h"
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
			InitializeAssetCompilingManager();
		}
		auto TearDown() -> void override { Durin::ShutdownAssetCompilingManager(); Mounts.reset(); }
		std::unique_ptr<Durin::Testing::FScopedMountRegistryFixture> Mounts;
	};
	[[maybe_unused]] const auto* WaterContentEnvironment = ::testing::AddGlobalTestEnvironment(new FWaterContentEnvironment);
}

TEST(FWaterSurfaceComponentVulkanTests, RegistrationEditsAndRuntimeRenderingShareParameters)
{
	std::optional<Durin::UnifiedError::FError> LoadedMaterialError;
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
	const auto LoadedMaterial = LoadObject<DMaterialInterface>(MaterialPath, nullptr, &LoadedMaterialError);
	ASSERT_TRUE(LoadedMaterial) << (*LoadedMaterialError);
	TStrongObjectPtr<DMaterialInterface> WaterMaterial(LoadedMaterial);
	ASSERT_EQ(WaterMaterial->GetStaticProperties().ShadingModel, EMaterialShadingModel::SingleLayerWater);
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
		EnqueueRenderCommand("CheckWaterStaticElements", [&](FRHICommandListImmediate& Commands) {
			const auto View = CollectStaticMeshView_RenderThread(Commands, Scene->GetPrimitiveSceneInfos(),
				FSceneView{}, ERasterMode::Solid, ERenderPreparationMode::Full);
			ASSERT_EQ(View->Primitives.size(), 1u);
			EXPECT_TRUE(View->Primitives.front().bRetainedStaticMesh);
		});
		FlushRenderingCommands();
		FDirectionalLightSceneData Light; Light.Intensity = 3;
		ASSERT_NE(PublishLightForTest<FDirectionalLightSceneProxy>(*Scene, FLightComponentId(840), Light), nullptr);
		static FRDGCapture* GraphSink = nullptr;
		auto Capture = [&](double Time, FByteBuffer& Pixels, FRDGCapture* Graph = nullptr) {
			FSceneView View;
			View.ViewportWidth = 192; View.ViewportHeight = 108; View.MaterialTimeSeconds = Time;
			View.Settings.Mode.RenderMode = ERenderMode::Lit;
			const auto Target = FRHITextureCreateDesc::Create2D("WaterComponent", 192, 108, EPixelFormat::SRGBA8_UNORM)
				.SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource | ETextureCreateFlags::SourceCopy);
			const auto Result = RenderPreparedGeometryView(Renderer, *Scene, View, Target, [Graph] {
				GraphSink = Graph;
				if (GraphSink) SetSceneRenderGraphCaptureSink(+[](const FRDGCapture& Capture) { *GraphSink = Capture; });
			},
				[&](FRHICommandListImmediate& C, FRHITexture* T) { ReadWaterPixels(C, T, Pixels); });
			SetSceneRenderGraphCaptureSink(nullptr);
			GraphSink = nullptr;
			return Result;
		};
		std::array<FByteBuffer, 6> Pixels;
		FRDGCapture WaterGraph;
		ASSERT_EQ(Capture(0, Pixels[0], &WaterGraph), ERenderViewResult::Success) << Component->GetDiagnostic();
		ASSERT_TRUE(Component->GetDiagnostic().empty()) << Component->GetDiagnostic();
		EXPECT_EQ(std::ranges::count(WaterGraph.Passes, "Water.NearestDepth", &FRDGPassCapture::Name), 1);
		EXPECT_EQ(std::ranges::count(WaterGraph.Passes, "Water.Shading", &FRDGPassCapture::Name), 1);
		ASSERT_EQ(Capture(.75, Pixels[1]), ERenderViewResult::Success);
		ASSERT_NE(Pixels[0], Pixels[1]);
		ASSERT_EQ(Capture(0, Pixels[2]), ERenderViewResult::Success);
		EXPECT_EQ(Pixels[0], Pixels[2]);
		Settings.Waves = {{{0, 2}, .9f, 1.8f}};
		ASSERT_TRUE(Component->SetSettings(Settings));
		ASSERT_EQ(Capture(.75, Pixels[2]), ERenderViewResult::Success);
		EXPECT_NE(Pixels[1], Pixels[2]);
		Settings.Waves.clear(); Settings.Elevation = .6f;
		ASSERT_TRUE(Component->SetSettings(Settings));
		EXPECT_TRUE(std::ranges::equal(std::span(Component->GetCustomPrimitiveData().Values).first(FWaterWaveData{}.size()),
			PackWaterWaveData(Settings)));
		const auto Query = Component->QuerySurface({0, 0, 0}, .75);
		ASSERT_TRUE(Query); EXPECT_NEAR(Query->Position.z, .6, 1.e-6);
		ASSERT_EQ(Capture(.75, Pixels[3]), ERenderViewResult::Success);
		EXPECT_NE(Pixels[2], Pixels[3]);
		Component->UnregisterComponent();
		ASSERT_EQ(Capture(.75, Pixels[4]), ERenderViewResult::Success);
		EXPECT_NE(Pixels[3], Pixels[4]);
		Component->RegisterComponent();
		ASSERT_EQ(Capture(.75, Pixels[5]), ERenderViewResult::Success) << Component->GetDiagnostic();
		EXPECT_EQ(Pixels[3], Pixels[5]);

		auto* SharedWaves = NewObject<DWaterWaves>(nullptr, NAME_None);
		auto* SecondActor = Level->SpawnActor<AWaterSurfaceActor>();
		ASSERT_NE(SecondActor, nullptr);
		ASSERT_TRUE(SecondActor->SetActorLocation({100, 0, 0}));
		auto* Second = SecondActor->GetWaterComponent();
		ASSERT_TRUE(Second->SetSettings(Settings));
		ASSERT_TRUE(Second->IsRegistered());
		ASSERT_TRUE(Component->SetWaveAsset(SharedWaves));
		ASSERT_TRUE(Second->SetWaveAsset(SharedWaves));

		const auto OriginalData = Component->GetCustomPrimitiveData();
		EXPECT_EQ(OriginalData, Second->GetCustomPrimitiveData());
		const auto OriginalQuery = Component->QuerySurface({0, 0, 0}, .75);
		ASSERT_TRUE(OriginalQuery);
		FByteBuffer OriginalWavePixels, ChangedWavePixels, RestoredWavePixels;
		ASSERT_EQ(Capture(.75, OriginalWavePixels), ERenderViewResult::Success);
		const auto OriginalSettings = SharedWaves->GetGeneratorSettings();
		auto ChangedSettings = OriginalSettings;
		++ChangedSettings.Seed;
		ChangedSettings.MinAmplitude = ChangedSettings.MaxAmplitude = .075f;
		ASSERT_TRUE(SharedWaves->SetGeneratorSettings(ChangedSettings));

		EXPECT_NE(OriginalData, Component->GetCustomPrimitiveData());
		EXPECT_EQ(Component->GetCustomPrimitiveData(), Second->GetCustomPrimitiveData());
		const auto FirstChanged = Component->QuerySurface({0, 0, 0}, .75);
		const auto SecondChanged = Second->QuerySurface({100, 0, 0}, .75);
		ASSERT_TRUE(FirstChanged && SecondChanged);
		EXPECT_NE(FirstChanged->Position.z, OriginalQuery->Position.z);
		EXPECT_NEAR(FirstChanged->Position.z, SecondChanged->Position.z, 1.e-8);
		EXPECT_NEAR(Math::Dot(FirstChanged->Normal, SecondChanged->Normal), 1., 1.e-8);
		ASSERT_EQ(Capture(.75, ChangedWavePixels), ERenderViewResult::Success);
		EXPECT_NE(OriginalWavePixels, ChangedWavePixels);

		// Transaction replay restores reflected data before dispatching PostEditUndo.
		const auto* GeneratorProperty = SharedWaves->GetClass()->FindPropertyByName("Settings");
		ASSERT_NE(GeneratorProperty, nullptr);
		*GeneratorProperty->ContainerPtrToValuePtr<FWaterWavesSettings>(SharedWaves) = OriginalSettings;
		SharedWaves->PostEditUndo();
		EXPECT_EQ(OriginalData, Component->GetCustomPrimitiveData());
		EXPECT_EQ(OriginalData, Second->GetCustomPrimitiveData());
		ASSERT_EQ(Capture(.75, RestoredWavePixels), ERenderViewResult::Success);
		EXPECT_EQ(OriginalWavePixels, RestoredWavePixels);

		ASSERT_TRUE(Component->SetWaveAsset(nullptr));
		const auto ManualData = Component->GetCustomPrimitiveData();
		ASSERT_TRUE(SharedWaves->SetGeneratorSettings(ChangedSettings));
		EXPECT_EQ(ManualData, Component->GetCustomPrimitiveData());
		EXPECT_NE(OriginalData, Second->GetCustomPrimitiveData());
		if (const char* Evidence = std::getenv("DURIN_WATER_PROOF_DIR"))
		{
			const auto Directory = std::filesystem::path(Evidence) / (DURIN_WITH_EDITOR ? "Editor" : "Cooked");
			std::filesystem::create_directories(Directory);
			for (const size_t Index : {0u, 1u, 3u, 4u})
				ASSERT_TRUE(FFileHelper::SaveArrayToFile(Pixels[Index], Directory / std::format("component-{}.rgba8", Index)));
			ASSERT_TRUE(FFileHelper::SaveArrayToFile(std::as_bytes(std::span(WaterGraph.Dump)), Directory / "component-rdg.txt"));
		}
	}
}
#endif
