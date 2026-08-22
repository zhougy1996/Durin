#include <gtest/gtest.h>

#include "ApplicationCoreGlobals.h"
#include "CoreGlobals.h"
#include "DynamicRHI.h"
#include "HAL/PlatformLTS.h"
#include "Modules/ModuleManager.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "Renderers/VolumetricCloudRenderer.h"
#include "Resources/FullscreenGeometryResources.h"
#include "Resources/RendererResourceCoordinator.h"
#include "Resources/RenderTargetLayouts.h"
#include "SceneView.h"
#include "SceneViewProjection.h"
#include <vulkan/vulkan.hpp>
#include "VulkanRHIPrivate.h"

#include <array>
#include <bit>
#include <cmath>
#include <memory>
#include <vector>

namespace Durin
{
	namespace
	{
		struct FVolumetricCloudTargetLifecycle
		{
			static constexpr auto GetName() -> const char*
			{
				return "VolumetricCloudTargetLifecycle";
			}
		};

		auto BuildViewMatrix(
			const FVector3& Location,
			const FVector3& Forward
		) -> FMatrix
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

		auto DecodeHalf(const uint8* Bytes) -> float
		{
			const uint16 Bits = static_cast<uint16>(Bytes[0])
				| (static_cast<uint16>(Bytes[1]) << 8u);
			const uint32 Sign = static_cast<uint32>(Bits & 0x8000u) << 16u;
			const uint32 Exponent = (Bits >> 10u) & 0x1fu;
			uint32 Mantissa = Bits & 0x03ffu;
			uint32 FloatBits = 0;
			if (Exponent == 0)
			{
				if (Mantissa == 0)
				{
					FloatBits = Sign;
				}
				else
				{
					int Shift = 0;
					while ((Mantissa & 0x0400u) == 0)
					{
						Mantissa <<= 1u;
						++Shift;
					}
					Mantissa &= 0x03ffu;
					FloatBits = Sign
						| (static_cast<uint32>(113 - Shift) << 23u)
						| (Mantissa << 13u);
				}
			}
			else if (Exponent == 31)
			{
				FloatBits = Sign | 0x7f800000u | (Mantissa << 13u);
			}
			else
			{
				FloatBits = Sign | ((Exponent + 112u) << 23u)
					| (Mantissa << 13u);
			}
			return std::bit_cast<float>(FloatBits);
		}
	} // namespace

	TEST(FVolumetricCloudVulkanTests, RoutesRenderAndRecoverThroughPublicRHI)
	{
		if (!GIsGameThreadIdInitialized)
		{
			GGameThreadId = FPlatformLTS::GetCurrentThreadId();
			GIsGameThreadIdInitialized = true;
		}
		ASSERT_EQ(GDynamicRHI, nullptr);
		FModuleManager::Get().LoadModule("RenderCore");
		RHIInit(FRHIInitializationContext::Headless());
		ASSERT_NE(GDynamicRHI, nullptr);
		InitRenderingThread();

		FRendererResourceCoordinator Coordinator;
		FFullscreenGeometryResources FullscreenGeometry;
		FVolumetricCloudRenderer Clouds(Coordinator, FullscreenGeometry);
		auto Results = std::make_shared<std::array<bool, 22>>();
		VulkanRHI::ArmVulkanCreateFailure(
			VulkanRHI::EVulkanCreateFailurePoint::Image
		);
		EnqueueRenderCommand<FVolumetricCloudTargetLifecycle>(
			[&Coordinator, &Clouds, &FullscreenGeometry, Results](
				FRHICommandListImmediate& CommandList) {
				(*Results)[0] = Clouds.EnsureTargets_RenderThread(64, 32) == nullptr;
				(*Results)[1] = Clouds.EnsureTargets_RenderThread(64, 32) == nullptr;
				Coordinator.Apply_RenderThread(
					ERendererResourceInvalidationCause::ManualRetry,
					FRendererResourceInvalidationTargets{}
				);
				auto* FragmentTargets = Clouds.EnsureTargets_RenderThread(64, 32);
				(*Results)[2] = FragmentTargets != nullptr && FragmentTargets->Cloud
					&& FragmentTargets->Cloud->GetFormat()
						== EPixelFormat::RGBA16_FLOAT;
				auto* ComputeTargets = Clouds.EnsureComputeTargets_RenderThread(64, 32);
				(*Results)[3] = ComputeTargets != nullptr && ComputeTargets->Cloud
					&& ComputeTargets->SampledView && ComputeTargets->StorageView;
				(*Results)[4] = Clouds.GetRetainedTargetBytes_RenderThread()
					== 2ull * 64ull * 32ull * 8ull;

				GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
				auto MakeVolume = [&CommandList](const char* Name, uint8 Value) {
					constexpr uint32 Size = 4;
					std::array<uint8, Size * Size * Size> Bytes{};
					Bytes.fill(Value);
					FTextureRHIRef Texture = GDynamicRHI->RHICreateTexture(
						CommandList,
						FRHITextureCreateDesc::Create3D(Name)
							.SetExtent(Size, Size)
							.SetDepth(Size)
							.SetFormat(EPixelFormat::R8_UNORM)
							.SetFlags(ETextureCreateFlags::ShaderResource)
					);
					if (Texture)
					{
						GDynamicRHI->RHIUpdateTexture3D(
							CommandList, Texture, 0,
							FUpdateTextureRegion3D(
								0, 0, 0, 0, 0, 0, Size, Size, Size
							),
							Size, Size * Size, Bytes.data()
						);
					}
					return Texture;
				};
				FTextureRHIRef Base = MakeVolume("CloudRouteBase", 255);
				FTextureRHIRef Detail = MakeVolume("CloudRouteDetail", 0);
				FTextureRHIRef Depth = GDynamicRHI->RHICreateTexture(
					CommandList,
					FRHITextureCreateDesc::Create2D(
						"CloudRouteDepth", 64, 32, EPixelFormat::D32
					).SetFlags(ETextureCreateFlags::DepthStencilTargetable
						| ETextureCreateFlags::ShaderResource)
				);
				FSamplerRHIRef Sampler = RHICreateSampler(
					FRHISamplerDesc::LinearRepeat()
				);
				if (!Base || !Detail || !Depth || !Sampler)
				{
					GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
					return;
				}
				FRHIRenderPassInfo DepthPass{};
				DepthPass.RenderTargetLayout =
					RenderTargetLayouts::MakeDirectionalShadowDepth();
				DepthPass.DepthStencilRenderTarget = Depth;
				DepthPass.DepthStencilClearValue = FClearValueBinding(0.0f, 0u);
				CommandList.BeginRenderPass(
					DepthPass, "VolumetricCloudDepthClear"
				);
				CommandList.EndRenderPass();

				FSceneView View;
				View.ViewLocation = {0.0, 0.0, 1000.0};
				const FVector3 Forward = Math::Normalize(FVector3(1.0, 0.0, 0.15));
				View.ViewMatrix = BuildViewMatrix(View.ViewLocation, Forward);
				if (!SceneViewProjection::BuildPerspectiveProjection(
					60.0, 1.0, 0.1, 500000.0,
					ESceneDepthConvention::ReversedZ,
					View.ProjectionMatrix
				))
				{
					GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
					return;
				}
				View.ViewProjectionMatrix = View.ProjectionMatrix * View.ViewMatrix;
				View.ViewportX = 7;
				View.ViewportY = 5;
				View.ViewportWidth = 40;
				View.ViewportHeight = 20;
				View.DepthConvention = ESceneDepthConvention::ReversedZ;
				FVolumetricCloudRenderer::FRenderInput Input{
					.bRequested = true,
					.Textures = {
						.BaseDensity = Base,
						.DetailDensity = Detail,
						.SceneDepth = Depth,
						.DensitySampler = Sampler
					},
					.View = &View,
					.Width = 64,
					.Height = 32
				};

				const auto Fragment = Clouds.Render_RenderThread(
					CommandList, FragmentTargets, nullptr, Input
				);
				(*Results)[5] = Fragment.Cloud == FragmentTargets->Cloud.GetReference();
				(*Results)[6] = Fragment.Counters.Route
					== FVolumetricCloudRenderer::ERoute::Fragment;
				(*Results)[7] = Fragment.Counters.Dispatches == 0
					&& Fragment.Counters.Draws == 1
					&& Fragment.Counters.Copies == 0;
				std::vector<uint8> FragmentPixels;
				GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
				GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);

				const auto Compute = Clouds.Render_RenderThread(
					CommandList, FragmentTargets, ComputeTargets, Input
				);
				(*Results)[9] = Compute.Cloud == ComputeTargets->Cloud.GetReference();
				(*Results)[10] = Compute.Counters.Route
					== FVolumetricCloudRenderer::ERoute::Compute;
				(*Results)[11] = Compute.Counters.Dispatches == 1
					&& Compute.Counters.Draws == 0
					&& Compute.Counters.Copies == 0;
				std::vector<uint8> ComputePixels;
				FTextureRHIRef SceneColor = GDynamicRHI->RHICreateTexture(
					CommandList,
					FRHITextureCreateDesc::Create2D(
						"CloudCompositeScene", 64, 32, EPixelFormat::RGBA16_FLOAT
					).SetFlags(ETextureCreateFlags::RenderTargetable
						| ETextureCreateFlags::ShaderResource)
				);
				if (!SceneColor)
				{
					GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
					return;
				}
				FRHIRenderPassInfo SceneClear{};
				SceneClear.RenderTargetLayout =
					RenderTargetLayouts::MakeVolumetricCloudOutput();
				SceneClear.ColorRenderTargets[0] = SceneColor;
				SceneClear.ColorClearValues[0] =
					FClearValueBinding(0.25f, 0.5f, 0.75f, 1.0f);
				CommandList.BeginRenderPass(
					SceneClear, "VolumetricCloudCompositeSceneClear"
				);
				CommandList.EndRenderPass();
				FRHITexture* Composite = Clouds.Composite_RenderThread(
					CommandList, SceneColor, Compute.Cloud, View
				);
				(*Results)[15] = Composite != nullptr;
				(*Results)[19] = Clouds.GetRetainedTargetBytes_RenderThread()
					== 3ull * 64ull * 32ull * 8ull;
				CommandList.ImmediateFlush(EImmediateFlushType::FlushRHIThread);
				(*Results)[8] = GDynamicRHI->RHIReadTexture2D(
					CommandList, Fragment.Cloud, 0, 0, FragmentPixels
				) && !FragmentPixels.empty();
				(*Results)[12] = GDynamicRHI->RHIReadTexture2D(
					CommandList, Compute.Cloud, 0, 0, ComputePixels
				) && ComputePixels.size() == FragmentPixels.size();
				bool bParity = FragmentPixels.size() == ComputePixels.size();
				for (size_t Offset = 0;
					bParity && Offset + 1 < ComputePixels.size(); Offset += 2)
				{
					bParity = std::abs(DecodeHalf(ComputePixels.data() + Offset)
						- DecodeHalf(FragmentPixels.data() + Offset))
						<= 2.0f / 1024.0f;
				}
				(*Results)[14] = bParity;
				std::vector<uint8> CompositePixels;
				(*Results)[16] = GDynamicRHI->RHIReadTexture2D(
					CommandList, Composite, 0, 0, CompositePixels
				) && CompositePixels.size() == ComputePixels.size();
				const size_t Center = (15u * 64u + 32u) * 8u;
				bool bCompositeAlgebra = CompositePixels.size() >= Center + 8u;
				for (size_t Channel = 0;
					bCompositeAlgebra && Channel < 3; ++Channel)
				{
					const float Scene = std::array{0.25f, 0.5f, 0.75f}[Channel];
					const float Expected =
						DecodeHalf(ComputePixels.data() + Center + Channel * 2u)
						+ DecodeHalf(ComputePixels.data() + Center + 6u) * Scene;
					bCompositeAlgebra = std::abs(
						DecodeHalf(CompositePixels.data() + Center + Channel * 2u)
							- Expected
					) <= 2.0f / 1024.0f;
				}
				(*Results)[17] = bCompositeAlgebra;
				(*Results)[18] = CompositePixels.size() >= 8u
					&& std::abs(DecodeHalf(CompositePixels.data()) - 0.25f)
						<= 1.0f / 1024.0f
					&& std::abs(DecodeHalf(CompositePixels.data() + 2u) - 0.5f)
						<= 1.0f / 1024.0f
					&& std::abs(DecodeHalf(CompositePixels.data() + 4u) - 0.75f)
						<= 1.0f / 1024.0f;
				const auto Disabled = Clouds.Render_RenderThread(
					CommandList, FragmentTargets, ComputeTargets,
					FVolumetricCloudRenderer::FRenderInput{}
				);
				(*Results)[13] = Disabled.Cloud == nullptr
					&& Disabled.Counters.Route
						== FVolumetricCloudRenderer::ERoute::Disabled;
				GDynamicRHI->RHIEndFrame_RenderThread(CommandList);

				auto* Fragment4K = Clouds.EnsureTargets_RenderThread(3'840, 2'160);
				auto* Compute4K = Clouds.EnsureComputeTargets_RenderThread(3'840, 2'160);
				(*Results)[20] = Fragment4K != nullptr && Fragment4K->Cloud
					&& Fragment4K->Cloud->GetSizeX() == 3'840
					&& Fragment4K->Cloud->GetSizeY() == 2'160;
				(*Results)[21] = Compute4K != nullptr && Compute4K->Cloud
					&& Compute4K->SampledView && Compute4K->StorageView
					&& Compute4K->Cloud->GetSizeX() == 3'840
					&& Compute4K->Cloud->GetSizeY() == 2'160;
				Clouds.ReleaseResources_RenderThread();
				FullscreenGeometry.ReleaseResources_RenderThread();
			}
		);
		FlushRenderingCommands();
		for (size_t Index = 0; Index < Results->size(); ++Index)
		{
			EXPECT_TRUE((*Results)[Index]) << Index;
		}

		ShutdownRenderingThread();
		FRHICommandListImmediate::Get().SwitchPipeline(ERHIPipeline::None);
		RHIExit();
	}
} // namespace Durin
