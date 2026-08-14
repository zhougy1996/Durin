#include "RendererModule.h"

#include "Console/ConsoleCommand.h"
#include "CoreGlobals.h"
#include "Renderers/SceneRenderer.h"
#include "Resources/DefaultTextureResources.h"
#include "Resources/RendererResourceCoordinator.h"
#include "RHI.h"
#include "RenderingThread.h"
#include "Scene.h"

namespace Durin
{
	FRendererModule::FRendererModule() = default;

	FRendererModule::~FRendererModule() = default;

	auto FRendererModule::StartupModule() -> void
	{
		check(SceneRenderer == nullptr);
		SceneRenderer = std::make_unique<FSceneRenderer>();
		SetActiveRendererResourceCoordinator(
			&SceneRenderer->GetResourceCoordinator());
		SetActiveDefaultTextureResources(
			&SceneRenderer->GetDefaultTextures());
		const bool bCommandsRegistered =
			SceneRenderer->Start(FConsoleCommandRegistry::Get());
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
			FSceneRenderer* Renderer = SceneRenderer.get();
			ENQUEUE_RENDER_COMMAND(InitializeDefaultTextures)(
				[Renderer](FRHICommandListImmediate& CommandList) {
					Renderer->InitializeStartupResources_RenderThread(
						CommandList);
				});
		}
	}

	auto FRendererModule::ShutdownModule() -> void
	{
		if (SceneRenderer == nullptr)
		{
			return;
		}
		SceneRenderer->Stop();
		FSceneRenderer* Renderer = SceneRenderer.get();
		ENQUEUE_RENDER_COMMAND(ReleaseRendererResources)(
			[Renderer](FRHICommandListImmediate&) {
				Renderer->ReleaseResources_RenderThread();
			});
		FlushRenderingCommands();
		SetActiveDefaultTextureResources(nullptr);
		SetActiveRendererResourceCoordinator(nullptr);
		SceneRenderer.reset();
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
		bool bPresentOutput,
		const FSceneViewRenderOptions& Options,
		FSceneViewStatistics* OutStatistics) -> ERenderViewResult
	{
		if (OutStatistics != nullptr) *OutStatistics = {};
		auto* RendererScene = dynamic_cast<FScene*>(Scene);
		const uint64 DrawsBefore = CommandList.GetNumRecordedDrawCommands();
		const ERenderViewResult Result = SceneRenderer->RenderView_RenderThread(
			CommandList,
			RendererScene,
			View,
			OutputTarget,
			bPresentOutput,
			Options,
			OutStatistics);
		if (OutStatistics != nullptr)
		{
			if (Result == ERenderViewResult::Success)
				OutStatistics->DrawCalls =
					CommandList.GetNumRecordedDrawCommands() - DrawsBefore;
			else
				*OutStatistics = {};
		}
		return Result;
	}

	IMPLEMENT_MODULE(FRendererModule, Renderer)
} // namespace Durin
