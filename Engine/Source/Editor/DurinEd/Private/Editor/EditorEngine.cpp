#include "Editor/EditorEngine.h"
#include "Editor/EditorNotification.h"
#include "Editor/EditorTransaction.h"

#include "DObject/Archive.h"
#include "DObject/ObjectLifecycle.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "Interfaces/IMainFrameModule.h"
#include "Modules/ModuleManager.h"
#include "RenderingThread.h"

namespace Durin
{
	DEditorEngine* GEditor = nullptr;

	DEditorEngine::DEditorEngine(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
		, TransactionManager(std::make_unique<FEditorTransactionManager>())
		, NotificationManager(std::make_unique<FEditorNotificationManager>())
	{
		GEditor = this;
	}

	DEditorEngine::~DEditorEngine()
	{
		if (GEditor == this) GEditor = nullptr;
	}

	auto DEditorEngine::Init() -> void
	{
		DEngine::Init();
		EditorWorld = GetWorld();
		EditorWorld->SetWorldType(EWorldType::Editor);

		IMainFrameModule& MainFrameModule = FModuleManager::LoadModuleChecked<IMainFrameModule>("MainFrame");
		MainFrameModule.CreateDefaultMainFrame();
		DURIN_DEBUG("Editor initialized successfully");
	}

	auto DEditorEngine::Tick(float DeltaSeconds, bool bIdleMode) -> void
	{
		DEngine::Tick(DeltaSeconds, bIdleMode);
	}

	auto DEditorEngine::BeginDestroy() -> void
	{
		StopPlaySession();
		EditorLevel = nullptr;
		EditorWorld = nullptr;
		DEngine::BeginDestroy();
	}

	auto DEditorEngine::StartPlaySession(DLevel* SourceLevel, std::string* OutError) -> bool
	{
		if (OutError) OutError->clear();
		if (PlayState != EEditorPlayState::Stopped)
		{
			if (OutError) *OutError = "A Play session is already active.";
			return false;
		}
		if (!EditorWorld || !SourceLevel || EditorWorld->GetCurrentLevel() != SourceLevel)
		{
			if (OutError) *OutError = "The current editor level is not available for Play.";
			return false;
		}

		PlayState = EEditorPlayState::Starting;
		DWorld* NewPlayWorld = NewObject<DWorld>(this, "PlayWorld");
		NewPlayWorld->SetWorldType(EWorldType::PlayInEditor);
		std::string DuplicateError;
		DLevel* PlayLevel = Cast<DLevel>(DuplicateObjectGraph(SourceLevel, NewPlayWorld, FName(std::format("{}_PIE", SourceLevel->GetName())), &DuplicateError));
		if (!PlayLevel)
		{
			DestroyObject(NewPlayWorld);
			PlayState = EEditorPlayState::Stopped;
			if (OutError) *OutError = DuplicateError.empty() ? "Could not duplicate the level for Play." : std::move(DuplicateError);
			return false;
		}

		EditorLevel = SourceLevel;
		PlayWorld = NewPlayWorld;
		EditorWorld->SetCurrentLevel(nullptr, false);
		SetWorld(NewPlayWorld);
		if (!NewPlayWorld->SetCurrentLevel(PlayLevel))
		{
			SetWorld(EditorWorld.Get());
			EditorWorld->SetCurrentLevel(EditorLevel.Get());
			PlayWorld = nullptr;
			EditorLevel = nullptr;
			DestroyObject(NewPlayWorld);
			PlayState = EEditorPlayState::Stopped;
			if (OutError) *OutError = "Could not activate the duplicated Play level.";
			return false;
		}

		TransactionManager->Clear();
		NewPlayWorld->BeginPlay();
		PlayState = EEditorPlayState::Playing;
		return true;
	}

	auto DEditorEngine::StopPlaySession() -> void
	{
		if (PlayState == EEditorPlayState::Stopped) return;
		PlayState = EEditorPlayState::Stopping;
		DWorld* WorldToDestroy = PlayWorld.Get();
		DLevel* LevelToDestroy = WorldToDestroy ? WorldToDestroy->GetCurrentLevel() : nullptr;
		if (WorldToDestroy)
		{
			WorldToDestroy->EndPlay();
			WorldToDestroy->SetCurrentLevel(nullptr, false);
		}
		// Scene removals may be consumed by the render thread; drain them before the
		// transient object graph that owns the scene proxies is released.
		FlushRenderingCommands();
		SetWorld(EditorWorld.Get());
		if (EditorWorld && EditorLevel) EditorWorld->SetCurrentLevel(EditorLevel.Get(), false);
		if (LevelToDestroy) DestroyObject(LevelToDestroy);
		PlayWorld = nullptr;
		EditorLevel = nullptr;
		SetGameInputEnabled(false);
		if (WorldToDestroy) DestroyObject(WorldToDestroy);
		TransactionManager->Clear();
		PlayState = EEditorPlayState::Stopped;
	}

	auto DEditorEngine::SetPlaySessionPaused(bool bPaused) -> void
	{
		if (PlayState != EEditorPlayState::Playing && PlayState != EEditorPlayState::Paused) return;
		if (PlayWorld) PlayWorld->SetPaused(bPaused);
		PlayState = bPaused ? EEditorPlayState::Paused : EEditorPlayState::Playing;
	}

	auto DEditorEngine::StepPlaySession() -> void
	{
		if (PlayState != EEditorPlayState::Paused || !PlayWorld) return;
		PlayWorld->RequestSingleStep();
	}

	auto DEditorEngine::GetTransactionManager() -> FEditorTransactionManager&
	{
		return *TransactionManager;
	}

	auto DEditorEngine::GetNotificationManager() -> FEditorNotificationManager&
	{
		return *NotificationManager;
	}
}
