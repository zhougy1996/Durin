#include <gtest/gtest.h>

#include "CoreGlobals.h"
#include "DynamicRHI.h"
#include "Engine/TerrainSceneProxy.h"
#include "HAL/PlatformLTS.h"
#include "Materials/MaterialRenderProxy.h"
#include "Modules/ModuleManager.h"
#include "Modules/ModuleTestContext.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "RendererModule.h"
#include "Scene.h"
#include "SceneViewProjection.h"
#include "Terrain/TerrainHeightmap.h"

namespace Durin
{
	namespace
	{
		struct FRenderEditorGridCapture
		{
			static constexpr auto GetName() -> const char*
			{
				return "RenderEditorGridCapture";
			}
		};

		auto BuildViewMatrix(
			const FVector3& Location,
			const FVector3& Forward) -> FMatrix
		{
			const FVector3 Right = Math::Normalize(
				Math::Cross(FVectorConstants::Up, Forward));
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

		auto MakeGridView(const FVector3& Forward) -> FSceneView
		{
			FSceneView View;
			View.ViewLocation = {-5.0, -5.0, 3.0};
			View.ViewMatrix = BuildViewMatrix(View.ViewLocation, Math::Normalize(Forward));
			EXPECT_TRUE(SceneViewProjection::BuildPerspectiveProjection(
				60.0, 1.0, 0.1, 500000.0,
				ESceneDepthConvention::ReversedZ,
				View.ProjectionMatrix));
			View.ViewProjectionMatrix = View.ProjectionMatrix * View.ViewMatrix;
			View.ViewportWidth = 129;
			View.ViewportHeight = 129;
			View.DepthConvention = ESceneDepthConvention::ReversedZ;
			View.ClearColor = {0.0f, 0.0f, 0.0f, 1.0f};
			View.Settings.RenderMode = ERenderMode::Unlit;
			View.Settings.VisibilityMode =
				EViewVisibilityMode::FrustumCullingDisabled;
			View.Settings.LODMode = EViewLODMode::ForceLOD0;
			View.EditorGrid.bVisible = true;
			View.EditorGrid.Height = 0.0;
			View.EditorGrid.FadeDistance = 1000.0f;
			View.EditorGrid.MinorColor = {1.0f, 1.0f, 1.0f, 1.0f};
			View.EditorGrid.MajorColor = {1.0f, 1.0f, 1.0f, 1.0f};
			View.EditorGrid.AxisXColor = {1.0f, 0.0f, 0.0f, 1.0f};
			View.EditorGrid.AxisYColor = {0.0f, 1.0f, 0.0f, 1.0f};
			return View;
		}

		auto CountVisiblePixels(const std::vector<uint8>& Pixels) -> size_t
		{
			size_t Result = 0;
			for (size_t Offset = 0; Offset + 3 < Pixels.size(); Offset += 4)
			{
				Result += Pixels[Offset] > 16
					|| Pixels[Offset + 1] > 16
					|| Pixels[Offset + 2] > 16 ? 1u : 0u;
			}
			return Result;
		}

		auto RenderGridCapture(
			FRendererModule& Renderer,
			FScene* Scene,
			const FVector3& Forward) -> std::vector<uint8>
		{
			auto Pixels = std::make_shared<std::vector<uint8>>();
			EnqueueRenderCommand<FRenderEditorGridCapture>(
				[&Renderer, Scene, Forward, Pixels](
					FRHICommandListImmediate& CommandList) {
					GRenderFrameCounterRenderThread++;
					GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
					const auto Desc = FRHITextureCreateDesc::Create2D(
						"EditorGridValidationColor", 129, 129,
						EPixelFormat::SRGBA8_UNORM)
						.SetFlags(ETextureCreateFlags::RenderTargetable
							| ETextureCreateFlags::ShaderResource
							| ETextureCreateFlags::CPUReadback);
					FTextureRHIRef Target =
						GDynamicRHI->RHICreateTexture(CommandList, Desc);
					ASSERT_NE(Target, nullptr);

					const FSceneView View = MakeGridView(Forward);
					EXPECT_EQ(Renderer.RenderView(
						CommandList, Scene, View, Target, false, {}),
						ERenderViewResult::Success);
					ASSERT_TRUE(GDynamicRHI->RHIReadTexture2D(
						CommandList, Target, 0, 0, *Pixels));
					GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
				});
			FlushRenderingCommands();
			return std::move(*Pixels);
		}
	}

	TEST(FEditorGridVulkanTests, ReversedZGridRemainsStableAcrossCoplanarRotatedViews)
	{
		if (!GIsGameThreadIdInitialized)
		{
			GGameThreadId = FPlatformLTS::GetCurrentThreadId();
			GIsGameThreadIdInitialized = true;
		}
		ASSERT_EQ(GDynamicRHI, nullptr);
		FModuleManager::Get().LoadModule("RenderCore");
		RHIInit();
		ASSERT_NE(GDynamicRHI, nullptr);
		InitRenderingThread();
		FRendererModule Renderer;
		auto RendererContext = Durin::FModuleTestContextFactory::CreateStartupContext("EditorGridRendererTest");
		Renderer.StartupModule(RendererContext);
		const std::array<uint16, 9> Samples{};
		std::shared_ptr<const FTerrainHeightmapPayload> Payload;
		std::string Error;
		ASSERT_TRUE(BuildTerrainHeightmapPayload(
			3, 3, Samples, Payload, Error)) << Error;
		auto Material = MakeRefCount<FMaterialRenderProxy>();
		FMaterialRenderProxyPublication Publication;
		Publication.LocalVersion = 1;
		Publication.LocalLayer.StaticProperties = FMaterialStaticProperties{
			.BlendMode = EMaterialBlendMode::Opaque,
			.ShadingModel = EMaterialShadingModel::Unlit,
			.bTwoSided = true};
		Publication.LocalLayer.Parameters.push_back({
			.Id = MaterialParameters::BaseColorId,
			.Type = EMaterialParameterType::Vector,
			.VectorValue = {0.0f, 0.0f, 0.0f}});
		ASSERT_TRUE(Material->QueuePublication_GameThread(std::move(Publication)));
		FlushRenderingCommands();
		constexpr double TerrainExtent = 4000.0;
		FTerrainPatchDescriptor Patch{
			.OriginX = 0,
			.OriginY = 0,
			.CellCountX = 2,
			.CellCountY = 2,
			.LODSteps = {1},
			.LODErrors = {0.0},
			.LocalBounds = FBox(
				{0.0, 0.0, 0.0}, {TerrainExtent, TerrainExtent, 0.0})};
		FScene Scene;
		Scene.AddOrReplacePrimitive(
			FPrimitiveSceneId(1),
			std::make_unique<FTerrainSceneProxy>(Payload, 1,
				TerrainExtent * 0.5, TerrainExtent * 0.5,
				1.0, 0.0, std::vector<FTerrainPatchDescriptor>{Patch},
				Patch.LocalBounds, Material, 1),
			Math::TranslationMatrix(FVector3{
				-TerrainExtent * 0.5, -TerrainExtent * 0.5, 0.0}));
		FScene OccluderScene;
		OccluderScene.AddOrReplacePrimitive(
			FPrimitiveSceneId(2),
			std::make_unique<FTerrainSceneProxy>(Payload, 1,
				TerrainExtent * 0.5, TerrainExtent * 0.5,
				1.0, 0.0, std::vector<FTerrainPatchDescriptor>{Patch},
				Patch.LocalBounds, Material, 1),
			Math::TranslationMatrix(FVector3{
				-TerrainExtent * 0.5, -TerrainExtent * 0.5, 0.25}));
		FlushRenderingCommands();

		const std::array<FVector3, 5> CameraDirections = {{
			{1.0, 1.0, -0.5},
			{1.0, 0.85, -0.5},
			{0.85, 1.0, -0.5},
			{1.0, 1.0, -0.42},
			{1.0, 1.0, -0.58},
		}};
		for (const FVector3& Forward : CameraDirections)
		{
			const std::vector<uint8> EmptyPixels =
				RenderGridCapture(Renderer, nullptr, Forward);
			const std::vector<uint8> TerrainPixels =
				RenderGridCapture(Renderer, &Scene, Forward);
			ASSERT_EQ(EmptyPixels.size(), 129u * 129u * 4u);
			ASSERT_EQ(TerrainPixels.size(), EmptyPixels.size());
			const size_t EmptyVisible = CountVisiblePixels(EmptyPixels);
			const size_t TerrainVisible = CountVisiblePixels(TerrainPixels);
			ASSERT_GT(EmptyVisible, 0u);
			EXPECT_GE(TerrainVisible, EmptyVisible * 99u / 100u);
		}
		const std::vector<uint8> OccludedPixels = RenderGridCapture(
			Renderer, &OccluderScene, CameraDirections.front());
		EXPECT_EQ(CountVisiblePixels(OccludedPixels), 0u);

		auto RendererShutdownContext = Durin::FModuleTestContextFactory::CreateShutdownContext(RendererContext);
		Renderer.ShutdownModule(RendererShutdownContext);
		ShutdownRenderingThread();
		FRHICommandListImmediate::Get().SwitchPipeline(ERHIPipeline::None);
		RHIExit();
	}
} // namespace Durin
