#include "Renderers/SceneRenderer.h"

#include "Profiling/Profiling.h"

#include "AssetSystem.h"
#include "Console/ConsoleCommand.h"
#include "EnvironmentLighting/EnvironmentLighting.h"
#include "IScene.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "Scene.h"
#include "Resources/RenderTargetLayouts.h"
#include "SceneView.h"

namespace Durin
{
	namespace
	{
		auto GetViewportOutput(bool bPresent)
			-> RenderTargetLayouts::EViewportOutput
		{
			return bPresent
				? RenderTargetLayouts::EViewportOutput::Present
				: RenderTargetLayouts::EViewportOutput::Offscreen;
		}
	} // namespace

	FSceneRenderer::FSceneRenderer()
		: DefaultTextures(Coordinator)
		, EnvironmentLighting(Coordinator)
		, StaticMeshRenderer(Coordinator, DefaultTextures, EnvironmentLighting)
		, SkyBoxRenderer(Coordinator, DefaultTextures)
		, PostProcessRenderer(Coordinator, FullscreenGeometry)
		, EditorAssistanceRenderer(Coordinator, FullscreenGeometry)
	{
	}

	FSceneRenderer::~FSceneRenderer() = default;

	auto FSceneRenderer::Start(FConsoleCommandRegistry& Registry) -> bool
	{
		FAssetPath EnvironmentPath;
		DEnvironmentLighting* EnvironmentAsset = nullptr;
		std::string PathError;
		Asset::FAssetResult EnvironmentResult =
			FAssetPath::TryCreate(
				"/Engine/Renderer/DefaultStudioEnvironment",
				EnvironmentPath,
				&PathError)
			? Asset::LoadAsset(EnvironmentPath, EnvironmentAsset)
			: Asset::FAssetResult{Asset::EAssetError::InvalidPath, std::move(PathError)};
		if (EnvironmentResult && EnvironmentAsset != nullptr)
		{
			EnvironmentLighting.Initialize(EnvironmentAsset->GetData());
		}
		else
		{
			DURIN_ERROR(
				"Failed to load the built-in studio environment: {}",
				EnvironmentResult.Message);
		}
		return Coordinator.Start(
			Registry,
			[this](ERendererResourceInvalidationCause Cause) {
				EnqueueResourceInvalidation(Cause);
			});
	}

	auto FSceneRenderer::Stop() -> void
	{
		Coordinator.Stop();
	}

	auto FSceneRenderer::InitializeStartupResources_RenderThread(
		FRHICommandListImmediate& CommandList) -> void
	{
		check(IsInRenderingThread());
		DefaultTextures.Initialize_RenderThread(CommandList);
	}

	auto FSceneRenderer::ReleaseResources_RenderThread() -> void
	{
		check(IsInRenderingThread());
		DefaultTextures.ReleaseResources_RenderThread();
		EnvironmentLighting.ReleaseResources_RenderThread();
		StaticMeshRenderer.ReleaseResources_RenderThread();
		Coordinator.ReleaseResources_RenderThread();
		SkyBoxRenderer.ReleaseResources_RenderThread();
		EditorAssistanceRenderer.ReleaseResources_RenderThread();
		PostProcessRenderer.ReleaseResources_RenderThread();
		FullscreenGeometry.ReleaseResources_RenderThread();
	}

	auto FSceneRenderer::FitViewToOutput(
		const FSceneView& View,
		uint32 Width,
		uint32 Height) -> FSceneView
	{
		FSceneView RenderView = View;
		RenderView.ViewportX = 0;
		RenderView.ViewportY = 0;
		RenderView.ViewportWidth = Width;
		RenderView.ViewportHeight = Height;
		if (RenderView.AspectRatioConstraint <= 0.0f)
		{
			return RenderView;
		}

		uint32 ContentWidth = Width;
		uint32 ContentHeight = static_cast<uint32>(
			std::round(ContentWidth / RenderView.AspectRatioConstraint));
		if (ContentHeight > Height)
		{
			ContentHeight = Height;
			ContentWidth = static_cast<uint32>(
				std::round(
					ContentHeight * RenderView.AspectRatioConstraint));
		}
		RenderView.ViewportWidth = std::max(1u, ContentWidth);
		RenderView.ViewportHeight = std::max(1u, ContentHeight);
		RenderView.ViewportX = (Width - RenderView.ViewportWidth) / 2;
		RenderView.ViewportY = (Height - RenderView.ViewportHeight) / 2;
		return RenderView;
	}

	auto FSceneRenderer::EnqueueResourceInvalidation(
		ERendererResourceInvalidationCause Cause) -> void
	{
		ENQUEUE_RENDER_COMMAND(InvalidateRendererResources)(
			[this, Cause](FRHICommandListImmediate& CommandList) {
				ApplyResourceInvalidation_RenderThread(CommandList, Cause);
			});
	}

	auto FSceneRenderer::ApplyResourceInvalidation_RenderThread(
		FRHICommandListImmediate& CommandList,
		ERendererResourceInvalidationCause Cause) -> void
	{
		check(IsInRenderingThread());
		Coordinator.Apply_RenderThread(
			Cause,
			{
				.InvalidateShaderResources =
					[](bool) {},
				.ReleaseDeviceResources =
					[this] {
						DefaultTextures.ReleaseResources_RenderThread();
						EnvironmentLighting.ReleaseResources_RenderThread();
						StaticMeshRenderer.ReleaseResources_RenderThread();
						SkyBoxRenderer.ReleaseResources_RenderThread();
						PostProcessRenderer.ReleaseResources_RenderThread();
						EditorAssistanceRenderer.
							ReleaseResources_RenderThread();
						FullscreenGeometry.ReleaseResources_RenderThread();
					},
				.RecreateStartupResources =
					[this, &CommandList] {
						check(GDynamicRHI != nullptr);
						DefaultTextures.Initialize_RenderThread(CommandList);
					},
				.RetryFailedResources =
					[this, &CommandList] {
						DefaultTextures.Initialize_RenderThread(CommandList);
						FullscreenGeometry.
							RetryFailedResources_RenderThread();
					},
			});
	}

	auto FSceneRenderer::RenderView_RenderThread(
		FRHICommandListImmediate& CommandList,
		IScene* Scene,
		const FSceneView& View,
		FRHITexture* OutputTarget,
		bool bPresentOutput) -> void
	{
		check(IsInRenderingThread());
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

		PostProcessRenderer.EnsureResources_RenderThread(CommandList);
		// Generated IBL uploads must finish before entering the Scene Color pass.
		// Failure is non-fatal: StaticMeshRenderer binds the complete black
		// environment fallback set instead.
		EnvironmentLighting.EnsureResources_RenderThread(CommandList);
		// Sky resources include a static index upload, so initialize them before
		// entering the Scene Color render pass.
		SkyBoxRenderer.EnsureResources_RenderThread();
		FPostProcessRenderer::FSceneTargets* SceneTargets =
			PostProcessRenderer.EnsureSceneTargets_RenderThread(Width, Height);
		if (SceneTargets == nullptr || SceneTargets->Color == nullptr
			|| SceneTargets->Depth == nullptr)
		{
			return;
		}
		FRHITexture* SceneColor = SceneTargets->Color;

		const FSceneView RenderView = FitViewToOutput(View, Width, Height);

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
		RenderScene_RenderThread(CommandList, Scene, RenderView, SceneColor);
		CommandList.EndRenderPass();

		RendererEditorAssistance::FPrepared PreparedEditorAssistance;
		if (!EditorAssistanceRequest.IsEmpty())
		{
			PreparedEditorAssistance =
				EditorAssistanceRenderer.Prepare_RenderThread(
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
		PostProcessRenderer.Draw_RenderThread(
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
		EditorAssistanceRenderer.Draw_RenderThread(
			CommandList,
			RenderView,
			PreparedEditorAssistance);
		CommandList.EndRenderPass();
	}

	auto FSceneRenderer::RenderScene_RenderThread(
		FRHICommandListImmediate& CommandList,
		IScene* Scene,
		const FSceneView& View,
		FRHITexture* RenderTarget) -> void
	{
		check(IsInRenderingThread());
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
		const auto* RendererScene = dynamic_cast<FScene*>(Scene);
		const FSkyBoxSceneInfo* SkyBoxInfo = RendererScene != nullptr
			? RendererScene->GetActiveSkyBoxSceneInfo_RenderThread() : nullptr;
		if (SkyBoxInfo != nullptr)
		{
			SkyBox = SkyBoxInfo->GetProxy().GetData();
			SkyBoxRenderer.Draw_RenderThread(CommandList, View, SkyBox);
		}

		if (!StaticMeshRenderer.EnsureResources_RenderThread())
		{
			return;
		}
		const ERenderMode RenderMode = View.Settings.RenderMode;
		const ERasterMode RasterMode = View.Settings.RasterMode;
		FDirectionalLightSceneData Light;
		Scene->GetDirectionalLight(Light);
		StaticMeshRenderer.DrawScene_RenderThread(
			CommandList,
			Scene,
			View,
			Light,
			RenderMode,
			RasterMode);
		TextureCubeThumbnailRenderer.DrawScene_RenderThread(
			CommandList,
			Scene,
			View,
			SkyBoxRenderer);
	}
} // namespace Durin
