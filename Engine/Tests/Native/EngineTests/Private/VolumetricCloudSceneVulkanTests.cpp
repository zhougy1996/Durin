#include <gtest/gtest.h>

#include "Application/GenericApplication.h"
#include "ApplicationCoreGlobals.h"
#include "CoreGlobals.h"
#include "DynamicRHI.h"
#include "HAL/PlatformLTS.h"
#include "Modules/ModuleManager.h"
#include "Modules/ModuleTestSupport.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RendererModule.h"
#include "RenderingThread.h"
#include "Renderers/PreparedSceneView.h"
#include "Renderers/SceneRendererProfiling.h"
#include "Renderers/SceneVisibility.h"
#include "SceneViewProjection.h"
#include "Window/GenericWindow.h"
#include "Window/GenericWindowDefinition.h"

#include <array>
#include <memory>
#include <vector>

namespace Durin
{
	namespace
	{
		struct FSceneCloudFixture
		{
			FTextureRHIRef Base;
			FTextureRHIRef Detail;
			FTextureRHIRef Weather;
			FSamplerRHIRef Sampler;
			bool bRequested = false;
			bool bForceFragment = false;
		};

		FSceneCloudFixture* GSceneCloudFixture = nullptr;
		FViewRenderCounters GSceneCloudCounters;

		auto PrepareSceneCloud(FPreparedSceneView& PreparedView) -> void
		{
			if (GSceneCloudFixture == nullptr) return;
			PreparedView.bVolumetricCloudRequested =
				GSceneCloudFixture->bRequested;
			PreparedView.bVolumetricCloudForceFragmentForQualification =
				GSceneCloudFixture->bForceFragment;
			PreparedView.VolumetricCloudTextures = {
				.BaseDensity = GSceneCloudFixture->Base,
				.DetailDensity = GSceneCloudFixture->Detail,
				.Weather = GSceneCloudFixture->Weather,
				.DensitySampler = GSceneCloudFixture->Sampler};
		}

		auto CaptureSceneCloudCounters(const FViewRenderCounters& Counters) -> void
		{
			GSceneCloudCounters = Counters;
		}

		auto BuildViewMatrix(const FVector3& Location, const FVector3& Forward)
			-> FMatrix
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

		auto MakeSceneCloudView(uint32 Width, uint32 Height) -> FSceneView
		{
			FSceneView View;
			View.ViewLocation = {0.0, 0.0, 2500.0};
			const FVector3 Forward =
				Math::Normalize(FVector3(1.0, 0.0, 0.1));
			View.ViewMatrix = BuildViewMatrix(View.ViewLocation, Forward);
			EXPECT_TRUE(SceneViewProjection::BuildPerspectiveProjection(
				60.0, static_cast<double>(Width) / Height, 0.1, 500000.0,
				ESceneDepthConvention::ReversedZ, View.ProjectionMatrix));
			View.ViewProjectionMatrix = View.ProjectionMatrix * View.ViewMatrix;
			View.ViewportWidth = Width;
			View.ViewportHeight = Height;
			View.DepthConvention = ESceneDepthConvention::ReversedZ;
			View.ClearColor = {0.05f, 0.08f, 0.12f, 1.0f};
			return View;
		}

		auto MakeDeterministicBytes(uint32 Size, uint8 Seed)
			-> std::vector<uint8>
		{
			std::vector<uint8> Bytes(
				static_cast<size_t>(Size) * Size * Size);
			for (uint32 Z = 0; Z < Size; ++Z)
				for (uint32 Y = 0; Y < Size; ++Y)
					for (uint32 X = 0; X < Size; ++X)
					{
						Bytes[(static_cast<size_t>(Z) * Size + Y) * Size + X]
							= static_cast<uint8>(Seed + X * 3u + Y * 5u + Z * 7u);
					}
			return Bytes;
		}

		struct FSceneCloudRender
		{
			static constexpr auto GetName() -> const char*
			{
				return "SceneCloudRender";
			}
		};
	} // namespace

	TEST(FVolumetricCloudSceneVulkanTests,
		EnabledCloudTraversesOffscreenAndPresentRoutes)
	{
		class FTestApplication final : public FGenericApplication
		{
		public:
			explicit FTestApplication(std::shared_ptr<FGenericWindow> InWindow)
				: Window(std::move(InWindow))
			{
			}

			auto FindWindowByNativeWindowHandle(void* NativeWindowHandle)
				-> std::shared_ptr<FGenericWindow> override
			{
				return Window != nullptr
					&& Window->GetOSNativeWindowHandle() == NativeWindowHandle
					? Window : nullptr;
			}

			std::shared_ptr<FGenericWindow> Window;
		};

		if (!GIsGameThreadIdInitialized)
		{
			GGameThreadId = FPlatformLTS::GetCurrentThreadId();
			GIsGameThreadIdInitialized = true;
		}
		InitializeApplicationCore();
		auto Window = MakePlatformWindow();
		auto Definition = std::make_shared<FGenericWindowDefinition>();
		Definition->XDesiredPositionOnScreen = 0.0f;
		Definition->YDesiredPositionOnScreen = 0.0f;
		Definition->WidthDesiredOnScreen = 96.0f;
		Definition->HeightDesiredOnScreen = 64.0f;
		Definition->Title = "Volumetric cloud scene qualification";
		Window->Initialize(Definition);
		ASSERT_NE(Window->GetOSNativeWindowHandle(), nullptr);
		GApp = std::make_shared<FTestApplication>(Window);

		ASSERT_EQ(GDynamicRHI, nullptr);
		FModuleManager::Get().LoadModule("RenderCore");
		RHIInit(FRHIInitializationContext::Headless());
		ASSERT_NE(GDynamicRHI, nullptr);
		InitRenderingThread();
		FRendererModule Renderer;
		FModuleTestHarness RendererLifecycle("VolumetricCloudSceneVulkan");
		RendererLifecycle.Start(Renderer);

		FSceneCloudFixture Fixture;
		GSceneCloudFixture = &Fixture;
		SetVolumetricCloudPreparationSink(PrepareSceneCloud);
		SetViewRenderCounterSink(CaptureSceneCloudCounters);
		EnqueueRenderCommand<FSceneCloudRender>(
			[&Fixture](FRHICommandListImmediate& CommandList) {
				auto MakeVolume = [&CommandList](const char* Name, uint32 Size,
					uint8 Seed) {
					const std::vector<uint8> Bytes =
						MakeDeterministicBytes(Size, Seed);
					FTextureRHIRef Texture = GDynamicRHI->RHICreateTexture(
						CommandList, FRHITextureCreateDesc::Create3D(Name)
							.SetExtent(Size, Size).SetDepth(Size)
							.SetFormat(EPixelFormat::R8_UNORM)
							.SetFlags(ETextureCreateFlags::ShaderResource));
					if (Texture)
					{
						GDynamicRHI->RHIUpdateTexture3D(CommandList, Texture, 0,
							FUpdateTextureRegion3D(
								0, 0, 0, 0, 0, 0, Size, Size, Size),
							Size, Size * Size, Bytes.data());
					}
					return Texture;
				};
				Fixture.Base = MakeVolume("SceneCloudBase", 64, 11);
				Fixture.Detail = MakeVolume("SceneCloudDetail", 32, 29);
				Fixture.Weather = GDynamicRHI->RHICreateTexture(
					CommandList, FRHITextureCreateDesc::Create2D(
						"SceneCloudWeather", 64, 64, EPixelFormat::R8_UNORM)
						.SetFlags(ETextureCreateFlags::ShaderResource));
				const std::vector<uint8> Weather =
					MakeDeterministicBytes(64, 47);
				if (Fixture.Weather)
				{
					GDynamicRHI->RHIUpdateTexture2D(CommandList, Fixture.Weather,
						0, 0, FUpdateTextureRegion2D(0, 0, 0, 0, 64, 64),
						64, Weather.data());
				}
				Fixture.Sampler = RHICreateSampler(FRHISamplerDesc::LinearRepeat());
			});
		FlushRenderingCommands();
		ASSERT_TRUE(Fixture.Base && Fixture.Detail && Fixture.Weather
			&& Fixture.Sampler);

		auto RenderOffscreen = [&Renderer, &Fixture](bool bRequested,
			bool bForceFragment) {
			Fixture.bRequested = bRequested;
			Fixture.bForceFragment = bForceFragment;
			auto Pixels = std::make_shared<std::vector<uint8>>();
			auto Result = std::make_shared<ERenderViewResult>(
				ERenderViewResult::RendererResourcesUnavailable);
			EnqueueRenderCommand<FSceneCloudRender>(
				[&Renderer, Pixels, Result](
					FRHICommandListImmediate& CommandList) {
					constexpr uint32 Width = 96;
					constexpr uint32 Height = 64;
					FTextureRHIRef Output = GDynamicRHI->RHICreateTexture(
						CommandList, FRHITextureCreateDesc::Create2D(
							"SceneCloudOffscreen", Width, Height,
							EPixelFormat::SRGBA8_UNORM)
							.SetFlags(ETextureCreateFlags::RenderTargetable
								| ETextureCreateFlags::ShaderResource
								| ETextureCreateFlags::CPUReadback));
					if (!Output) return;
					++GRenderFrameCounterRenderThread;
					GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
					FSceneView View = MakeSceneCloudView(Width, Height);
					*Result = Renderer.RenderView(
						CommandList, nullptr, View, Output, false, {});
					GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
					CommandList.ImmediateFlush(EImmediateFlushType::FlushRHIThread);
					GDynamicRHI->RHIReadTexture2D(
						CommandList, Output, 0, 0, *Pixels);
				});
			FlushRenderingCommands();
			EXPECT_EQ(*Result, ERenderViewResult::Success);
			return Pixels;
		};

		const auto Disabled = RenderOffscreen(false, false);
		EXPECT_EQ(GSceneCloudCounters.VolumetricCloudDisabledViews, 1u);
		FTextureRHIRef SavedBase = Fixture.Base;
		Fixture.Base = nullptr;
		const auto InvalidRequiredInput = RenderOffscreen(true, false);
		EXPECT_EQ(GSceneCloudCounters.VolumetricCloudDisabledViews, 1u);
		EXPECT_EQ(GSceneCloudCounters.VolumetricCloudEnabledViews, 0u);
		EXPECT_EQ(*InvalidRequiredInput, *Disabled);
		Fixture.Base = SavedBase;
		SavedBase = nullptr;
		const auto Compute = RenderOffscreen(true, false);
		EXPECT_EQ(GSceneCloudCounters.VolumetricCloudEnabledViews, 1u);
		EXPECT_EQ(GSceneCloudCounters.VolumetricCloudComputeViews, 1u);
		EXPECT_EQ(GSceneCloudCounters.VolumetricCloudDispatches, 1u);
		EXPECT_EQ(GSceneCloudCounters.VolumetricCloudCompositeDraws, 1u);
		const auto Fragment = RenderOffscreen(true, true);
		EXPECT_EQ(GSceneCloudCounters.VolumetricCloudEnabledViews, 1u);
		EXPECT_EQ(GSceneCloudCounters.VolumetricCloudFragmentViews, 1u);
		EXPECT_EQ(GSceneCloudCounters.VolumetricCloudDraws, 1u);
		EXPECT_EQ(GSceneCloudCounters.VolumetricCloudCompositeDraws, 1u);
		ASSERT_EQ(Compute->size(), Fragment->size());
		ASSERT_EQ(Disabled->size(), Compute->size());
		bool bDiffersFromDisabled = false;
		for (size_t Index = 0; Index < Compute->size(); ++Index)
		{
			EXPECT_LE(std::abs(static_cast<int>((*Compute)[Index])
				- static_cast<int>((*Fragment)[Index])), 1) << Index;
			bDiffersFromDisabled |= (*Compute)[Index] != (*Disabled)[Index];
		}
		EXPECT_TRUE(bDiffersFromDisabled);

		TRefCountPtr<FRHIViewport> Viewport = GDynamicRHI->RHICreateViewport({
			.NativeWindowHandle = Window->GetOSNativeWindowHandle(),
			.SizeX = 96,
			.SizeY = 64,
			.PreferredPixelFormat = EPixelFormat::SRGBA8_UNORM,
			.PresentModePolicy = EViewportPresentModePolicy::MainWindow});
		ASSERT_NE(Viewport, nullptr);
		auto RenderPresent = [&Renderer, &Fixture, &Viewport](uint32 Width,
			uint32 Height, bool bForceFragment) {
			Fixture.bRequested = true;
			Fixture.bForceFragment = bForceFragment;
			auto Result = std::make_shared<ERenderViewResult>(
				ERenderViewResult::RendererResourcesUnavailable);
			EnqueueRenderCommand<FSceneCloudRender>(
				[&Renderer, Viewport, Width, Height, Result](
					FRHICommandListImmediate& CommandList) {
					++GRenderFrameCounterRenderThread;
					GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
					CommandList.BeginDrawingViewport(Viewport, nullptr);
					FTextureRHIRef BackBuffer =
						GDynamicRHI->RHIGetViewportBackBuffer(Viewport);
					if (BackBuffer)
					{
						FSceneView View = MakeSceneCloudView(Width, Height);
						*Result = Renderer.RenderView(
							CommandList, nullptr, View, BackBuffer, true, {});
					}
					CommandList.EndDrawingViewport(Viewport, true, false);
					GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
				});
			FlushRenderingCommands();
			return *Result;
		};

		EXPECT_EQ(RenderPresent(96, 64, false), ERenderViewResult::Success);
		EXPECT_EQ(GSceneCloudCounters.VolumetricCloudComputeViews, 1u);
		GDynamicRHI->RHIResizeViewport(Viewport, 128, 72, false);
		FlushRenderingCommands();
		EXPECT_EQ(RenderPresent(128, 72, true), ERenderViewResult::Success);
		EXPECT_EQ(GSceneCloudCounters.VolumetricCloudFragmentViews, 1u);

		SetVolumetricCloudPreparationSink(nullptr);
		SetViewRenderCounterSink(nullptr);
		GSceneCloudFixture = nullptr;
		Viewport = nullptr;
		Fixture = {};
		FlushRenderingCommands();
		RendererLifecycle.Shutdown();
		FlushRenderingCommands();
		ShutdownRenderingThread();
		FRHICommandListImmediate::Get().SwitchPipeline(ERHIPipeline::None);
		RHIExit();
		GApp = nullptr;
		Window.reset();
		ShutdownApplicationCore();
	}
} // namespace Durin
