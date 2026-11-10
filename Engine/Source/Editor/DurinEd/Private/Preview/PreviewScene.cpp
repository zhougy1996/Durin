#include "Preview/PreviewScene.h"

#include "DObject/DObjectGlobals.h"
#include "DObject/ObjectLifecycle.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "IRendererModule.h"
#include "Rendering/SceneInterface.h"
#include "RenderingThread.h"

namespace Durin::Editor
{
	FPreviewScene::FPreviewScene(FName Name, std::optional<ERHIFeatureLevel> FeatureLevel)
	{
		checkf(IsInGameThread(), "Preview scenes must be created on the game thread.");
		if (GEngine == nullptr || GEngine->GetRendererModule() == nullptr)
		{
			Error = "The renderer is not available.";
			return;
		}

		RenderScene = FeatureLevel ? GEngine->GetRendererModule()->CreateScene(*FeatureLevel) : GEngine->GetRendererModule()->CreateScene();
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
		World->SetWorldType(EWorldType::EditorPreview);
		World->SetRenderScene(RenderScene.get());
		try { World->InitializeSubsystems(); }
		catch (...) { ReleaseResources(); throw; }
		if (World->IsShutdownRequested()) Error = "Preview World retired during initialization.";
		if (Error.empty() && !World->SetCurrentLevel(Level.Get(), false))
			Error = "The preview level could not be attached to its world.";
		if (!Error.empty()) World->CleanupWorld();
	}

	FPreviewScene::~FPreviewScene()
	{
		ReleaseResources();
	}

	auto FPreviewScene::ReleaseResources() -> void
	{
		checkf(IsInGameThread(), "Preview scenes must be destroyed on the game thread.");
		if (World)
		{
			World->CleanupWorld();
		}
		Level = nullptr;

		if (RenderScene)
		{
			RenderScene->Release();
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
