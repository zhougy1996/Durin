#include "Preview/PreviewScene.h"

#include "DObject/DObjectGlobals.h"
#include "DObject/ObjectLifecycle.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "IRendererModule.h"
#include "IScene.h"
#include "RenderingThread.h"

namespace Durin::Editor
{
	FPreviewScene::FPreviewScene(FName Name)
	{
		checkf(IsInGameThread(), "Preview scenes must be created on the game thread.");
		if (GEngine == nullptr || GEngine->GetRendererModule() == nullptr)
		{
			Error = "The renderer is not available.";
			return;
		}

		RenderScene = GEngine->GetRendererModule()->CreateScene();
		if (RenderScene == nullptr)
		{
			Error = "The renderer could not create a preview scene.";
			return;
		}

		const std::string BaseName = Name.IsNone() ? "PreviewScene" : Name.ToString();
		World = NewObject<DWorld>(GEngine, std::format("{}World", BaseName));
		Level = NewObject<DLevel>(World.Get(), std::format("{}Level", BaseName));
		if (World == nullptr || Level == nullptr)
		{
			Error = "The preview world could not be created.";
			return;
		}

		AddToRoot(World.Get());
		World->SetWorldType(EWorldType::Preview);
		World->SetRenderScene(RenderScene.get());
		if (!World->SetCurrentLevel(Level.Get(), false))
			Error = "The preview level could not be attached to its world.";
	}

	FPreviewScene::~FPreviewScene()
	{
		checkf(IsInGameThread(), "Preview scenes must be destroyed on the game thread.");
		if (World)
		{
			World->EndPlay();
			World->SetCurrentLevel(nullptr);
			World->SetRenderScene(nullptr);
		}
		Level = nullptr;

		if (RenderScene)
		{
			RenderScene->Release();
			if (GRenderingThread) FlushRenderingCommands();
			RenderScene.reset();
		}

		if (World)
		{
			RemoveFromRoot(World.Get());
			MarkObjectHierarchyAsGarbage(World.Get());
			World = nullptr;
		}
	}

	auto FPreviewScene::BeginPlay() -> void
	{
		if (World) World->BeginPlay({});
	}

	auto FPreviewScene::Tick(float DeltaSeconds) -> void
	{
		if (World) World->Tick({.DeltaSeconds = DeltaSeconds});
	}

	auto FPreviewScene::EndPlay() -> void
	{
		if (World) World->EndPlay();
	}
} // namespace Durin::Editor
