#pragma once

#include "CoreMinimal.h"
#include "CoreGlobals.h"
#include "Asset/AssetCompilingManager.h"
#include "DynamicRHI.h"
#include "Modules/ModuleTestSupport.h"
#include "RendererModule.h"
#include "RenderResourcePreparation.h"
#include "RenderingThread.h"
#include "RHICommandList.h"
#include <gtest/gtest.h>

namespace Durin::Tests::Water
{
	inline auto ReadWaterPixels(Durin::FRHICommandListImmediate& Commands, Durin::FRHITexture* Source, Durin::FByteBuffer& Pixels) -> void
	{
		using namespace Durin;
		const auto Readback = GDynamicRHI->RHICreateTexture(Commands,
			FRHITextureCreateDesc::Create2D("WaterReadback", Source->GetSizeX(), Source->GetSizeY(), Source->GetFormat())
				.SetFlags(ETextureCreateFlags::DestinationCopy | ETextureCreateFlags::CPUReadback | ETextureCreateFlags::ShaderResource));
		const FRHITextureSubresourceRange Whole{ERHITextureAspect::Color, 0, 1, 0, 1};
		Commands.TransitionTextures(std::array{
			FRHITextureTransition{Source, Whole, ERHIAccess::GraphicsShaderRead, ERHIAccess::TransferRead},
			FRHITextureTransition{Readback, Whole, ERHIAccess::Discard, ERHIAccess::TransferWrite}});
		FRHITextureCopyRegion Region;
		Region.Extent = {Source->GetSizeX(), Source->GetSizeY(), 1};
		Commands.CopyTexture(Source, Readback, std::array{Region});
		Commands.TransitionTextures(std::array{
			FRHITextureTransition{Source, Whole, ERHIAccess::TransferRead, ERHIAccess::GraphicsShaderRead},
			FRHITextureTransition{Readback, Whole, ERHIAccess::TransferWrite, ERHIAccess::GraphicsShaderRead}});
		ASSERT_TRUE(GDynamicRHI->RHIReadTexture2D(Commands, Readback, 0, 0, Pixels));
	}

	class FGeometryQualificationFrame final
	{
	public:
		explicit FGeometryQualificationFrame(Durin::FRHICommandListImmediate& InCommands) : Commands(InCommands)
		{
			++Durin::GRenderFrameCounterRenderThread;
			Durin::GDynamicRHI->RHIBeginFrame_RenderThread(Commands);
		}
		~FGeometryQualificationFrame()
		{
			Durin::GDynamicRHI->RHIEndFrame_RenderThread(Commands);
		}
	private:
		Durin::FRHICommandListImmediate& Commands;
	};

	class FGeometryRendererCleanup final
	{
	public:
		explicit FGeometryRendererCleanup(Durin::FModuleTestHarness& InLifecycle) : Lifecycle(InLifecycle) {}
		~FGeometryRendererCleanup()
		{
			Durin::FlushRenderingCommands();
			Lifecycle.Shutdown();
			Durin::FlushRenderingCommands();
			Durin::ShutdownRenderingThread();
			Durin::RHIExit();
		}
	private:
		Durin::FModuleTestHarness& Lifecycle;
	};

	inline auto RenderPreparedGeometryView(Durin::FRendererModule& Renderer, Durin::FSceneInterface& Scene,
		const Durin::FSceneView& View, const Durin::FRHITextureCreateDesc& TargetDesc,
		const std::function<void()>& BeforeDraw = {},
		const std::function<void(Durin::FRHICommandListImmediate&, Durin::FRHITexture*)>& AfterDraw = {},
		const Durin::FSceneViewRenderOptions& Options = {}) -> Durin::ERenderViewResult
	{
		using namespace Durin;
		const auto Preparation = FRenderResourcePreparation::Create();
		FTextureRHIRef Target;
		auto Result = ERenderViewResult::RendererResourcesPending;
		const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
		do
		{
			// Ordinary assets publish completed material preparations on the host
			// tick; rendering alone cannot advance their async Cooked payloads.
			FAssetCompilingManager::Get().ProcessAsyncTasks();
			EnqueueRenderCommand("WaterCapture", [&](FRHICommandListImmediate& Commands) {
				FGeometryQualificationFrame Frame(Commands);
				if (!Target) Target = GDynamicRHI->RHICreateTexture(Commands, TargetDesc);
				if (!Target) { Result = ERenderViewResult::RendererResourcesUnavailable; return; }
				const auto DrawsBefore = Commands.GetNumRecordedDrawCommands();
				Renderer.PrepareViewResources(Commands, {&Scene, View, Target, false, Options}, Preparation);
				EXPECT_EQ(Commands.GetNumRecordedDrawCommands(), DrawsBefore);
				const auto State = Preparation.GetProgress().State;
				if (State == ERenderResourcePreparationState::Pending) return;
				if (State != ERenderResourcePreparationState::Ready)
				{ Result = ERenderViewResult::RendererResourcesUnavailable; return; }
				if (BeforeDraw) BeforeDraw();
				if (testing::Test::HasFatalFailure())
				{ Result = ERenderViewResult::RendererResourcesUnavailable; return; }
				Result = Renderer.RenderView(Commands,
					{&Scene, View, Target, false, Options});
				ASSERT_NE(Result, ERenderViewResult::RendererResourcesPending)
					<< "A ready view must finish recording even when capture exceeds the preparation time budget.";
				if (Result == ERenderViewResult::Success)
					EXPECT_EQ(Preparation.GetProgress().State, ERenderResourcePreparationState::Ready);
				if (Result == ERenderViewResult::Success && AfterDraw) AfterDraw(Commands, Target);
			});
			FlushRenderingCommands();
			if (Result != ERenderViewResult::RendererResourcesPending || testing::Test::HasFatalFailure()) break;
			// Only the host yields between frames; the render thread never joins Shader work.
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		} while (std::chrono::steady_clock::now() < Deadline);
		if (Result == ERenderViewResult::RendererResourcesPending) Preparation.RequestCancel();
		EnqueueRenderCommand("WaterCapture", [&](FRHICommandListImmediate&) {
			if (Result == ERenderViewResult::RendererResourcesPending)
				Preparation.Retire_RenderThread(ERenderResourcePreparationState::Canceled);
			Target = {};
		});
		FlushRenderingCommands();
		return Result;
	}
}
