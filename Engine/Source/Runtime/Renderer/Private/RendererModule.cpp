#include "RendererModule.h"

#include "Profiling/Profiling.h"

#include "Console/ConsoleCommand.h"
#include "CoreGlobals.h"
#include "Engine/PrimitiveSceneProxy.h"
#include "Renderers/EditorAssistanceRenderer.h"
#include "Renderers/PostProcessRenderer.h"
#include "Renderers/SkyBoxRenderer.h"
#include "Renderers/StaticMeshRenderer.h"
#include "Renderers/TextureCubeThumbnailRenderer.h"
#include "Resources/DefaultTextureResources.h"
#include "Resources/FullscreenGeometryResources.h"
#include "Resources/RendererResourceCoordinator.h"
#include "Resources/RenderTargetLayouts.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "Scene.h"

namespace Durin
{
	// Groups shared owners until FSceneRenderer assumes their composition.
	struct FRendererModule::FSharedResources
	{
		FSharedResources()
			: StaticMeshRenderer(Coordinator, DefaultTextures)
			, SkyBoxRenderer(Coordinator, DefaultTextures)
			, TextureCubeThumbnailRenderer(Coordinator)
			, PostProcessRenderer(Coordinator, FullscreenGeometry)
			, EditorAssistanceRenderer(Coordinator, FullscreenGeometry)
		{
		}

		FRendererResourceCoordinator Coordinator;
		FDefaultTextureResources DefaultTextures;
		FFullscreenGeometryResources FullscreenGeometry;
		FStaticMeshRenderer StaticMeshRenderer;
		FSkyBoxRenderer SkyBoxRenderer;
		FTextureCubeThumbnailRenderer TextureCubeThumbnailRenderer;
		FPostProcessRenderer PostProcessRenderer;
		FEditorAssistanceRenderer EditorAssistanceRenderer;
	};

	namespace
	{
		auto GetViewportOutput(bool bPresent)
			-> RenderTargetLayouts::EViewportOutput
		{
			return bPresent
				? RenderTargetLayouts::EViewportOutput::Present
				: RenderTargetLayouts::EViewportOutput::Offscreen;
		}

		auto ForEachStaticMeshProxy(
			IScene* Scene,
			const std::function<void(FStaticMeshSceneProxy&)>& Function)
			-> void
		{
			auto* RendererScene = dynamic_cast<FScene*>(Scene);
			if (RendererScene == nullptr)
			{
				return;
			}

			for (PrimitiveSceneProxy* Proxy :
				RendererScene->GetPrimitiveSceneProxies())
			{
				if (auto* StaticMeshProxy =
						dynamic_cast<FStaticMeshSceneProxy*>(Proxy))
				{
					Function(*StaticMeshProxy);
				}
			}
		}

		auto ForEachTextureCubeThumbnailProxy(
			IScene* Scene,
			const std::function<void(FTextureCubePreviewSceneProxy&)>&
				Function) -> void
		{
			auto* RendererScene = dynamic_cast<FScene*>(Scene);
			if (RendererScene == nullptr)
			{
				return;
			}
			for (PrimitiveSceneProxy* Proxy :
				RendererScene->GetPrimitiveSceneProxies())
			{
				if (auto* TextureCubeProxy =
						dynamic_cast<FTextureCubePreviewSceneProxy*>(Proxy))
				{
					Function(*TextureCubeProxy);
				}
			}
		}
	} // namespace

	static auto ApplyRendererResourceInvalidation_RenderThread(
		FRHICommandListImmediate& CommandList,
		ERendererResourceInvalidationCause Cause,
		FRendererResourceCoordinator& Coordinator,
		FDefaultTextureResources& DefaultTextures,
		FFullscreenGeometryResources& FullscreenGeometry,
		FStaticMeshRenderer& StaticMeshRenderer,
		FSkyBoxRenderer& SkyBoxRenderer,
		FTextureCubeThumbnailRenderer& TextureCubeThumbnailRenderer,
		FPostProcessRenderer& PostProcessRenderer,
		FEditorAssistanceRenderer& EditorAssistanceRenderer) -> void
	{
		check(IsInRenderingThread());
		Coordinator.Apply_RenderThread(
			Cause,
			{
				.InvalidateShaderResources =
					[](bool) {},
				.ReleaseDeviceResources =
					[&DefaultTextures,
					 &FullscreenGeometry,
					 &StaticMeshRenderer,
					 &SkyBoxRenderer,
					 &TextureCubeThumbnailRenderer,
					 &PostProcessRenderer,
					 &EditorAssistanceRenderer] {
						DefaultTextures.ReleaseResources_RenderThread();
						StaticMeshRenderer.ReleaseResources_RenderThread();
						TextureCubeThumbnailRenderer.
							ReleaseResources_RenderThread();
						SkyBoxRenderer.ReleaseResources_RenderThread();
						PostProcessRenderer.ReleaseResources_RenderThread();
						EditorAssistanceRenderer.
							ReleaseResources_RenderThread();
						FullscreenGeometry.ReleaseResources_RenderThread();
					},
				.RecreateStartupResources =
					[&CommandList, &DefaultTextures] {
						check(GDynamicRHI != nullptr);
						DefaultTextures.Initialize_RenderThread(CommandList);
					},
				.RetryFailedResources =
					[&FullscreenGeometry] {
						FullscreenGeometry.
							RetryFailedResources_RenderThread();
					},
			});
	}

	static auto EnqueueRendererResourceInvalidation(
		ERendererResourceInvalidationCause Cause,
		FRendererResourceCoordinator* Coordinator,
		FDefaultTextureResources* DefaultTextures,
		FFullscreenGeometryResources* FullscreenGeometry,
		FStaticMeshRenderer* StaticMeshRenderer,
		FSkyBoxRenderer* SkyBoxRenderer,
		FTextureCubeThumbnailRenderer* TextureCubeThumbnailRenderer,
		FPostProcessRenderer* PostProcessRenderer,
		FEditorAssistanceRenderer* EditorAssistanceRenderer) -> void
	{
		ENQUEUE_RENDER_COMMAND(InvalidateRendererResources)(
			[Cause,
			 Coordinator,
			 DefaultTextures,
			 FullscreenGeometry,
			 StaticMeshRenderer,
			 SkyBoxRenderer,
			 TextureCubeThumbnailRenderer,
			 PostProcessRenderer,
			 EditorAssistanceRenderer](FRHICommandListImmediate& CommandList) {
				ApplyRendererResourceInvalidation_RenderThread(
					CommandList,
					Cause,
					*Coordinator,
					*DefaultTextures,
					*FullscreenGeometry,
					*StaticMeshRenderer,
					*SkyBoxRenderer,
					*TextureCubeThumbnailRenderer,
					*PostProcessRenderer,
					*EditorAssistanceRenderer);
			});
	}

	FRendererModule::FRendererModule() = default;

	FRendererModule::~FRendererModule() = default;

	auto FRendererModule::StartupModule() -> void
	{
		check(SharedResources == nullptr);
		SharedResources = std::make_unique<FSharedResources>();
		SetActiveRendererResourceCoordinator(
			&SharedResources->Coordinator);
		SetActiveDefaultTextureResources(
			&SharedResources->DefaultTextures);
		SetActiveFullscreenGeometryResources(
			&SharedResources->FullscreenGeometry);

		const bool bCommandsRegistered =
			SharedResources->Coordinator.Start(
				FConsoleCommandRegistry::Get(),
				[Coordinator = &SharedResources->Coordinator,
				 DefaultTextures = &SharedResources->DefaultTextures,
				 FullscreenGeometry = &SharedResources->FullscreenGeometry,
				 StaticMeshRenderer =
					 &SharedResources->StaticMeshRenderer,
				 SkyBoxRenderer = &SharedResources->SkyBoxRenderer,
				 TextureCubeThumbnailRenderer =
					 &SharedResources->TextureCubeThumbnailRenderer,
				 PostProcessRenderer =
					 &SharedResources->PostProcessRenderer,
				 EditorAssistanceRenderer =
					 &SharedResources->EditorAssistanceRenderer](
					ERendererResourceInvalidationCause Cause) {
					EnqueueRendererResourceInvalidation(
						Cause,
						Coordinator,
						DefaultTextures,
						FullscreenGeometry,
						StaticMeshRenderer,
						SkyBoxRenderer,
						TextureCubeThumbnailRenderer,
						PostProcessRenderer,
						EditorAssistanceRenderer);
				});
		checkf(
			bCommandsRegistered,
			"Failed to register renderer resource invalidation commands");
		if (!bCommandsRegistered)
		{
			DURIN_ERROR(
				"Failed to register renderer resource invalidation commands");
		}
		if (GDynamicRHI != nullptr)
		{
			FDefaultTextureResources* DefaultTextures =
				&SharedResources->DefaultTextures;
			ENQUEUE_RENDER_COMMAND(InitializeDefaultTextures)(
				[DefaultTextures](FRHICommandListImmediate& CommandList) {
					DefaultTextures->Initialize_RenderThread(CommandList);
				});
		}
	}

	auto FRendererModule::ShutdownModule() -> void
	{
		if (SharedResources == nullptr)
		{
			return;
		}
		SharedResources->Coordinator.Stop();
		FSharedResources* Resources = SharedResources.get();
		ENQUEUE_RENDER_COMMAND(ReleaseRendererResources)(
			[Resources](FRHICommandListImmediate&) {
				check(IsInRenderingThread());
				Resources->DefaultTextures.ReleaseResources_RenderThread();
				Resources->StaticMeshRenderer.
					ReleaseResources_RenderThread();
				Resources->Coordinator.ReleaseResources_RenderThread();
				Resources->TextureCubeThumbnailRenderer.
					ReleaseResources_RenderThread();
				Resources->SkyBoxRenderer.ReleaseResources_RenderThread();
				Resources->EditorAssistanceRenderer.
					ReleaseResources_RenderThread();
				Resources->PostProcessRenderer.
					ReleaseResources_RenderThread();
				Resources->FullscreenGeometry.ReleaseResources_RenderThread();
			});
		FlushRenderingCommands();
		SetActiveFullscreenGeometryResources(nullptr);
		SetActiveDefaultTextureResources(nullptr);
		SetActiveRendererResourceCoordinator(nullptr);
		SharedResources.reset();
	}

	auto FRendererModule::CreateScene() -> std::unique_ptr<IScene>
	{
		check(IsInGameThread());
		return std::make_unique<FScene>();
	}

	auto FRendererModule::RenderView(
		FRHICommandListImmediate& CommandList,
		IScene* Scene,
		const FSceneView& View,
		FRHITexture* OutputTarget,
		bool bPresentOutput) -> void
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("Renderer.RenderView");
		const uint32 Width =
			OutputTarget != nullptr ? OutputTarget->GetSizeX() : 0;
		const uint32 Height =
			OutputTarget != nullptr ? OutputTarget->GetSizeY() : 0;
		if (OutputTarget == nullptr || Width == 0 || Height == 0)
		{
			return;
		}
		const RenderTargetLayouts::EViewportOutput ViewportOutput =
			GetViewportOutput(bPresentOutput);
		const RendererEditorAssistance::FRequest EditorAssistanceRequest =
			FEditorAssistanceRenderer::AnalyzeRequest(View, ViewportOutput);

		SharedResources->PostProcessRenderer.EnsureResources_RenderThread(
			CommandList);
		// Sky resources include a static index upload, so initialize them before
		// entering the Scene Color render pass.
		SharedResources->SkyBoxRenderer.EnsureResources_RenderThread();
		FPostProcessRenderer::FSceneTargets* SceneTargets =
			SharedResources->PostProcessRenderer.
				EnsureSceneTargets_RenderThread(Width, Height);
		if (SceneTargets == nullptr || SceneTargets->Color == nullptr
			|| SceneTargets->Depth == nullptr)
		{
			return;
		}
		FRHITexture* SceneColor = SceneTargets->Color;

		FSceneView RenderView = View;
		RenderView.ViewportX = 0;
		RenderView.ViewportY = 0;
		RenderView.ViewportWidth = Width;
		RenderView.ViewportHeight = Height;
		if (RenderView.AspectRatioConstraint > 0.0f)
		{
			uint32 ContentWidth = Width;
			uint32 ContentHeight = static_cast<uint32>(
				std::round(
					ContentWidth / RenderView.AspectRatioConstraint));
			if (ContentHeight > Height)
			{
				ContentHeight = Height;
				ContentWidth = static_cast<uint32>(
					std::round(
						ContentHeight
						* RenderView.AspectRatioConstraint));
			}
			RenderView.ViewportWidth = std::max(1u, ContentWidth);
			RenderView.ViewportHeight = std::max(1u, ContentHeight);
			RenderView.ViewportX =
				(Width - RenderView.ViewportWidth) / 2;
			RenderView.ViewportY =
				(Height - RenderView.ViewportHeight) / 2;
		}
		FRHIRenderPassInfo ScenePassInfo{};
		ScenePassInfo.RenderTargetLayout =
			RenderTargetLayouts::MakeSceneTargets();
		ScenePassInfo.ColorRenderTargets[0] = SceneColor;
		ScenePassInfo.DepthStencilRenderTarget = SceneTargets->Depth;
		ScenePassInfo.ColorClearValues[0] = FClearValueBinding(
			View.ClearColor.r,
			View.ClearColor.g,
			View.ClearColor.b,
			View.ClearColor.a);
		ScenePassInfo.DepthStencilClearValue = FClearValueBinding(1.0f, 0u);
		CommandList.BeginRenderPass(
			ScenePassInfo,
			"SceneColorRenderPass");
		RenderScene(CommandList, Scene, RenderView, SceneColor);
		CommandList.EndRenderPass();

		RendererEditorAssistance::FPrepared PreparedEditorAssistance;
		if (!EditorAssistanceRequest.IsEmpty())
		{
			PreparedEditorAssistance =
				SharedResources->EditorAssistanceRenderer.
					Prepare_RenderThread(
						CommandList,
						RenderView,
						EditorAssistanceRequest);
		}
		const bool bHasEditorAssistance =
			PreparedEditorAssistance.HasDrawableOperation();
		FRHIRenderPassInfo PostProcessPassInfo{};
		PostProcessPassInfo.RenderTargetLayout = bHasEditorAssistance
			? RenderTargetLayouts::MakeScenePostProcessOutput()
			: RenderTargetLayouts::MakeFinalScenePostProcessOutput(
				ViewportOutput);
		PostProcessPassInfo.ColorRenderTargets[0] = OutputTarget;
		PostProcessPassInfo.ColorClearValues[0] = FClearValueBinding(
			View.ClearColor.r,
			View.ClearColor.g,
			View.ClearColor.b,
			View.ClearColor.a);
		CommandList.BeginRenderPass(
			PostProcessPassInfo,
			bPresentOutput
				? "PostProcessPresentRenderPass"
				: "PostProcessOffscreenRenderPass");
		SharedResources->PostProcessRenderer.Draw_RenderThread(
			CommandList,
			SceneColor,
			Width,
			Height,
			bPresentOutput,
			View.Settings.bEnableFXAA,
			bHasEditorAssistance);
		CommandList.EndRenderPass();
		if (!bHasEditorAssistance)
		{
			return;
		}

		FRHIRenderPassInfo EditorAssistancePassInfo{};
		EditorAssistancePassInfo.RenderTargetLayout =
			RenderTargetLayouts::MakeEditorAssistanceOutput(ViewportOutput);
		EditorAssistancePassInfo.ColorRenderTargets[0] = OutputTarget;
		EditorAssistancePassInfo.DepthStencilRenderTarget =
			SceneTargets->Depth;
		CommandList.BeginRenderPass(
			EditorAssistancePassInfo,
			bPresentOutput
				? "EditorAssistancePresentRenderPass"
				: "EditorAssistanceOffscreenRenderPass");
		SharedResources->EditorAssistanceRenderer.Draw_RenderThread(
			CommandList,
			RenderView,
			PreparedEditorAssistance);
		CommandList.EndRenderPass();
	}

	auto FRendererModule::RenderScene(
		FRHICommandListImmediate& CommandList,
		IScene* Scene,
		const FSceneView& View,
		FRHITexture* RenderTarget) -> void
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("Renderer.RenderScene");
		const uint32 Width = View.ViewportWidth;
		const uint32 Height = View.ViewportHeight;
		if (Scene == nullptr || RenderTarget == nullptr
			|| Width == 0 || Height == 0)
		{
			return;
		}

		CommandList.SetViewport(
			static_cast<float>(View.ViewportX),
			static_cast<float>(View.ViewportY),
			0.0f,
			static_cast<float>(View.ViewportX + Width),
			static_cast<float>(View.ViewportY + Height),
			1.0f);
		CommandList.SetScissor(
			static_cast<float>(View.ViewportX),
			static_cast<float>(View.ViewportY),
			static_cast<float>(Width),
			static_cast<float>(Height));

		FSkyBoxSceneData SkyBox;
		if (Scene->GetActiveSkyBox_RenderThread(SkyBox))
		{
			SharedResources->SkyBoxRenderer.Draw_RenderThread(
				CommandList,
				View,
				SkyBox);
		}

		if (!SharedResources->StaticMeshRenderer.
				EnsureResources_RenderThread())
		{
			return;
		}

		const ERenderMode RenderMode = View.Settings.RenderMode;
		const ERasterMode RasterMode = View.Settings.RasterMode;
		FDirectionalLightSceneData Light;
		Scene->GetDirectionalLight(Light);
		ForEachStaticMeshProxy(
			Scene,
			[this,
			 &CommandList,
			 &View,
			 &Light,
			 RenderMode,
			 RasterMode](FStaticMeshSceneProxy& Proxy) {
				if (RenderMode == ERenderMode::Unlit
					|| RenderMode == ERenderMode::Lit)
				{
					SharedResources->StaticMeshRenderer.
						DrawProxy_RenderThread(
							CommandList,
							View,
							Light,
							RenderMode,
							RasterMode,
							Proxy);
				}
			});
		ForEachTextureCubeThumbnailProxy(
			Scene,
			[this, &CommandList, &View](
				FTextureCubePreviewSceneProxy& Proxy) {
				SharedResources->TextureCubeThumbnailRenderer.
					DrawProxy_RenderThread(
						CommandList,
						View,
						Proxy,
						SharedResources->SkyBoxRenderer);
			});
	}

	IMPLEMENT_MODULE(FRendererModule, Renderer)
} // namespace Durin
