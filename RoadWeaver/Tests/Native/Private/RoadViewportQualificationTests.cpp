#include "RoadNet/RoadNetActor.h"
#include "RoadNet/RoadNetBuilder.h"
#include "Asset/AssetCompilingManager.h"
#include "Components/SplineMeshComponent.h"
#include "CoreGlobals.h"
#include "DObject/ObjectLifecycle.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "HAL/PlatformLTS.h"
#include "LevelEditorViewportEditing.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressions.h"
#include "Math/Operations.h"
#include "Modules/ModuleManager.h"
#include "Modules/ModuleTestSupport.h"
#include "NativeDObjectTestSupport.h"
#include "NativeTestSupport.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RendererModule.h"
#include "Rendering/SceneInterface.h"
#include "RenderingThread.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshCompilation.h"
#include "StaticMesh/StaticMeshSource.h"
#include "Threading/TaskRuntime.h"
#include "Viewport/LevelEditorViewportClient.h"
#include "Workspace/LevelEditorContext.h"

#include <gtest/gtest.h>

namespace
{
	using namespace Durin;
	using namespace Durin::RoadNet;
	using namespace Durin::Editor::Level;

	class FRoadViewportEngine final : public DEngine
	{
	public:
		FRoadViewportEngine() : DEngine(FObjectInitializer::Get()) {}
		auto Install(IRendererModule& Renderer) -> void
		{
			RendererModule = &Renderer;
			MainScene = Renderer.CreateScene();
		}
		auto Reset() -> void
		{
			SetWorld(nullptr);
			if (MainScene) MainScene->Release();
			FlushRenderingCommands();
			MainScene.reset();
			RendererModule = nullptr;
		}
	};

	class FRoadViewportQualificationTests : public testing::TestWithParam<bool>
	{
	protected:
		auto SetUp() -> void override
		{
			Testing::InitializeDObjectSystemForTests();
			InitializeTaskScheduler(2);
			ASSERT_TRUE(InitializeGameThreadDeferredExecutor());
			if (!FAssetCompilingManager::Get().IsAcceptingRequests()) ASSERT_TRUE(InitializeAssetCompilingManager());
			ASSERT_TRUE(FModuleManager::Get().LoadModule("MeshBuilder"));
			ASSERT_EQ(GDynamicRHI, nullptr);
			ASSERT_TRUE(RHIInit(FRHIInitializationContext::Headless()));
			InitRenderingThread();
			bRenderingStarted = true;
			Renderer = std::make_unique<FRendererModule>();
			Lifecycle = std::make_unique<FModuleTestHarness>("RoadViewport");
			Lifecycle->Start(*Renderer);
			Engine = std::make_unique<FRoadViewportEngine>();
			Engine->Install(*Renderer);
			ASSERT_EQ(GEngine, nullptr);
			GEngine = Engine.get();
			World = NewObject<DWorld>(nullptr, "RoadViewportWorld");
			AddToRoot(World);
			ASSERT_TRUE(World->InitializeSubsystems());
			Level = NewObject<DLevel>(World, "RoadViewportLevel");
			ASSERT_TRUE(World->SetCurrentLevel(Level));
			Engine->SetWorld(World);
			EvidenceRoot = Testing::CreateTestFixtureDirectory(GetParam() ? "Sphere" : "Plane");
		}

		auto TearDown() -> void override
		{
			if (Engine) Engine->Reset();
			GEngine = nullptr;
			if (World)
			{
				RemoveFromRoot(World);
				MarkObjectHierarchyAsGarbage(World);
				CollectGarbage();
			}
			Engine.reset();
			if (bRenderingStarted) FlushRenderingCommands();
			if (Lifecycle) Lifecycle->Shutdown();
			Lifecycle.reset();
			Renderer.reset();
			if (bRenderingStarted)
			{
				FlushRenderingCommands();
				ShutdownRenderingThread();
				FRHICommandListImmediate::Get().SwitchPipeline(ERHIPipeline::None);
			}
			if (GDynamicRHI) RHIExit();
		}

		auto MakeMaterial(FVector3 Color, const char* Name) -> DMaterial*
		{
			auto* Material = NewObject<DMaterial>(World, Name);
			auto* Constant = NewObject<DMaterialExpressionVector3Constant>(Material, "Color");
			Constant->Id = FGuid::NewGuid();
			Constant->Value = Color;
			FMaterialExpressionSurfaceOutputs Outputs;
			Outputs.BaseColor = {Constant->Id};
			Outputs.Emissive = {Constant->Id};
			const std::array<DMaterialExpression*, 1> Expressions{Constant};
			EXPECT_TRUE(Material->SetMaterialExpressions(Expressions, Outputs));
			FMaterialStaticProperties Properties;
			Properties.ShadingModel = EMaterialShadingModel::Unlit;
			Properties.bTwoSided = true;
			EXPECT_TRUE(Material->SetStaticProperties(Properties));
			Material->GetRenderProxy();
			FAssetCompilingManager::Get().FinishCompilationForObject(*Material);
			return Material;
		}

		auto BuildRoad(DMaterial* Material) -> void
		{
			FMeshDescription Geometry;
			Geometry.PolygonGroups.push_back({.Name = "Road", .SourceMaterialIndex = 0, .SourceName = "Road"});
			auto& Strip = Geometry.Sections.emplace_back();
			Strip.Name = "Road";
			Strip.SourceMaterialIndex = 0;
			for (uint32 Index = 0; Index <= 16; ++Index)
			{
				const float X = float(Index) / 16;
				Strip.Positions.insert(Strip.Positions.end(), {{X, -.5f, 0}, {X, .5f, 0}});
				if (!Index) continue;
				const uint32 Base = (Index - 1) * 2;
				Strip.Indices.insert(Strip.Indices.end(), {Base, Base+2, Base+1, Base+1, Base+2, Base+3});
			}
			FStaticMeshSource Source;
			ASSERT_TRUE(Source.Initialize(std::move(Geometry)));
			auto* Mesh = NewObject<DStaticMesh>(World, "RoadStrip");
			ASSERT_TRUE(Mesh->BuildFromSource(EStaticMeshBuildMode::Synchronous, {.Source = std::move(Source),
				.PreparedMaterialSlots = std::vector<FMeshMaterialSlotDefinition>{{.Name = FName("Road"), .SourceName = "Road", .SourceMaterialIndex = 0}},
				.bPersistDerivedData = false, .bMarkPackageDirty = false}));
			Mesh->SetMaterialSlotDefaultMaterial(0, Material);

			FRoadNetBuilder Builder;
			FRoad Road;
			Road.StartNodeId = Builder.AddNode("Start", {0, 0, 0});
			Road.EndNodeId = Builder.AddNode("End", {40, 0, 0});
			Road.ReferenceLine.SetPoints({FSplinePoint({0, 0, 0}), FSplinePoint({40, 0, 0})});
			Road.LaneSections = {{.EndDistanceMeters = 40,
				.Lanes = {{.Index = 1}, {.Index = -1, .Direction = ELaneDirection::AgainstReferenceLine}}}};
			Builder.AddRoad(Road);
			auto Definition = Builder.TakeDefinition();
			Definition.Planet.Id = FGuid::NewGuid();
			Definition.Planet.Center = {0, 0, -100};
			Definition.Planet.RadiusMeters = 100;
			FRoadSurface Surface;
			Surface.Mode = GetParam() ? ERoadSurfaceMode::Sphere : ERoadSurfaceMode::Plane;
			Surface.Origin = GetParam() ? Definition.Planet.Center : FVector3(0);
			Surface.RadiusMeters = 100;
			std::string Error;
			ASSERT_TRUE(FitRoadDefinition(Definition, Surface, Error)) << Error;
			auto* Asset = NewObject<DRoadNet>(World, "Network");
			ASSERT_TRUE(Asset->SetDefinition(Definition, Error)) << Error;
			Actor = Level->SpawnActor<ARoadNetActor>("RoadPreview");
			ASSERT_NE(Actor, nullptr);
			Actor->SetPreviewMesh(Mesh);
			Actor->SetRoadNet(Asset);
			ASSERT_EQ(Actor->GetGenerationState(), "Ready") << Actor->GetDiagnostic();
			FRoadSample Middle;
			const auto& Alignment = Actor->GetAlignments().front();
			ASSERT_TRUE(Alignment->Sample(Alignment->GetLengthMeters() * .5, Middle, Error)) << Error;
			Midpoint = Middle.Position;
			FlushRenderingCommands();
		}

		auto Render(const FSceneView& View) -> FByteBuffer
		{
			FByteBuffer Pixels;
			ERenderViewResult Result = ERenderViewResult::RendererResourcesPending;
			const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(45);
			do
			{
				PumpGameThreadDeferredWork();
				ENQUEUE_RENDER_COMMAND(RenderRoadAcceptance)([&](FRHICommandListImmediate& Commands) {
					auto Target = GDynamicRHI->RHICreateTexture(Commands,
						FRHITextureCreateDesc::Create2D("RoadAcceptance", 256, 192, EPixelFormat::SRGBA8_UNORM)
						.SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource | ETextureCreateFlags::CPUReadback));
					ASSERT_TRUE(Target);
					++GRenderFrameCounterRenderThread;
					GDynamicRHI->RHIBeginFrame_RenderThread(Commands);
					Result = Renderer->RenderView(Commands, {Engine->GetMainScene(), View, Target, false, {}});
					GDynamicRHI->RHIEndFrame_RenderThread(Commands);
					if (Result == ERenderViewResult::Success)
						GDynamicRHI->RHIReadTexture2D(Commands, Target, 0, 0, Pixels);
				});
				FlushRenderingCommands();
				if (Result != ERenderViewResult::RendererResourcesPending) break;
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			} while (std::chrono::steady_clock::now() < Deadline);
			EXPECT_EQ(Result, ERenderViewResult::Success);
			EXPECT_EQ(Pixels.size(), 256u * 192u * 4u);
			return Pixels;
		}

		auto SaveImage(const FByteBuffer& Pixels, const char* Name) -> void
		{
			std::ofstream File(EvidenceRoot / (std::string(Name) + ".ppm"), std::ios::binary);
			File << "P6\n256 192\n255\n";
			for (size_t Offset = 0; Offset + 3 < Pixels.size(); Offset += 4)
				File.write(reinterpret_cast<const char*>(Pixels.data() + Offset), 3);
			EXPECT_TRUE(File.good());
		}

		bool bRenderingStarted = false;
		std::unique_ptr<FRendererModule> Renderer;
		std::unique_ptr<FModuleTestHarness> Lifecycle;
		std::unique_ptr<FRoadViewportEngine> Engine;
		DWorld* World = nullptr;
		DLevel* Level = nullptr;
		ARoadNetActor* Actor = nullptr;
		FVector3 Midpoint{0};
		std::filesystem::path EvidenceRoot;
	};

	TEST_P(FRoadViewportQualificationTests, ProductionClickSelectionAndMaterialPixelsMatchGeneratedSurface)
	{
		auto* Red = MakeMaterial({1, 0, 0}, "RedRoad");
		BuildRoad(Red);
		ASSERT_FALSE(HasFatalFailure());
		FLevelEditorViewportClient Client;
		FLevelViewportCameraState Camera;
		Camera.Location = Midpoint + FVector3(0, -20, 35);
		Camera.Yaw = 90;
		Camera.Pitch = -60;
		Client.InitializeForLevel(Level, &Camera);
		Client.SetGridVisible(false);
		FSceneView View;
		ASSERT_TRUE(Client.BuildViewMatrices(256, 192, View));
		View.Settings.Mode.RenderMode = ERenderMode::Unlit;
		View.Settings.AmbientOcclusion.bEnabled = false;
		const auto RedPixels = Render(View);
		ASSERT_EQ(RedPixels.size(), 256u * 192u * 4u);
		SaveImage(RedPixels, "Red");

		FVector2f Position;
		ASSERT_TRUE(Client.ProjectWorldToViewport(Midpoint, {256, 192}, Position));
		FLevelEditorContext Context;
		Context.Synchronize(World);
		FLevelViewportEditModeManager Modes;
		const auto ClickAndWait = [&](FVector2f Pixel, bool Ctrl, bool Selected) {
			// The real editor encloses input-triggered hit-proxy work in its RHI frame.
			ENQUEUE_RENDER_COMMAND(BeginRoadPickFrame)([](FRHICommandListImmediate& Commands) {
				++GRenderFrameCounterRenderThread;
				GDynamicRHI->RHIBeginFrame_RenderThread(Commands);
			});
			FLevelEditorViewportInput Click;
			Click.bRequestSelection = true;
			Click.bCtrl = Ctrl;
			Click.MousePosition = Pixel;
			Modes.Tick(Context, Client, View, Click, nullptr);
			ENQUEUE_RENDER_COMMAND(EndRoadPickFrame)([](FRHICommandListImmediate& Commands) {
				GDynamicRHI->RHIEndFrame_RenderThread(Commands);
			});
			const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
			do
			{
				ENQUEUE_RENDER_COMMAND(PollRoadPick)([](FRHICommandListImmediate& Commands) { Commands.PollTextureReadbacks(); });
				FlushRenderingCommands();
				FLevelEditorViewportInput Poll;
				Modes.Tick(Context, Client, View, Poll, nullptr);
				if (Context.IsActorSelected(Actor) == Selected) break;
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			} while (std::chrono::steady_clock::now() < Deadline);
			EXPECT_EQ(Context.GetPrimarySelectedActor(), Selected ? Actor : nullptr);
		};
		ClickAndWait(Position, false, true);
		ClickAndWait(Position, true, false);
		ClickAndWait(Position, false, true);
		ClickAndWait({0, 0}, false, false);
		Modes.Shutdown(&Context);

		auto* Green = MakeMaterial({0, 1, 0}, "GreenRoad");
		for (auto* Component : Actor->FindComponentsByClass<DSplineMeshComponent>()) Component->SetMaterial(0, Green);
		FlushRenderingCommands();
		const auto GreenPixels = Render(View);
		ASSERT_EQ(GreenPixels.size(), RedPixels.size());
		SaveImage(GreenPixels, "Green");
		size_t RedCount = 0, GreenCount = 0;
		for (size_t Offset = 0; Offset < RedPixels.size(); Offset += 4)
		{
			const auto R = std::to_integer<int>(RedPixels[Offset]);
			const auto G = std::to_integer<int>(RedPixels[Offset + 1]);
			const auto NewR = std::to_integer<int>(GreenPixels[Offset]);
			const auto NewG = std::to_integer<int>(GreenPixels[Offset + 1]);
			if (R > 64 && R > G * 2) ++RedCount;
			if (NewG > 64 && NewG > NewR * 2) ++GreenCount;
		}
		EXPECT_GT(RedCount, 100u);
		EXPECT_GT(GreenCount, 100u);
		RecordProperty("red_material_pixels", RedCount);
		RecordProperty("green_material_pixels", GreenCount);
	}

	INSTANTIATE_TEST_SUITE_P(Surfaces, FRoadViewportQualificationTests, testing::Bool(),
		[](const testing::TestParamInfo<bool>& Info) { return Info.param ? "Sphere" : "Plane"; });
}
