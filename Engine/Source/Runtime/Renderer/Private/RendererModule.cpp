#include "RendererModule.h"

#include "Console/ConsoleCommand.h"
#include "CoreGlobals.h"
#include "Renderers/SceneRenderer.h"
#include "Resources/DefaultTextureResources.h"
#include "Resources/FullscreenGeometryResources.h"
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
		SetActiveFullscreenGeometryResources(
			&SceneRenderer->GetFullscreenGeometry());

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
		SetActiveFullscreenGeometryResources(nullptr);
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
		bool bPresentOutput) -> void
	{
		SceneRenderer->RenderView_RenderThread(
			CommandList,
			Scene,
			View,
			OutputTarget,
			bPresentOutput);
	}

	IMPLEMENT_MODULE(FRendererModule, Renderer)
} // namespace Durin
