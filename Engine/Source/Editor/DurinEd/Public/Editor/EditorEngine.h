#pragma once

#include "DurinEdAPI.h"
#include "Engine/Engine.h"

#include "EditorEngine.gen.h"

namespace Durin
{
	class FEditorTransactionManager;
	class FEditorNotificationManager;
	class DLevel;
	class DWorld;
	class AActor;
	class DObject;
	class FSceneViewport;
	class MWindow;

	enum class EEditorPlayState : uint8
	{
		Stopped,
		Starting,
		Playing,
		Paused,
		Stopping
	};

	enum class EEditorPlayStartLocation : uint8
	{
		LevelStart,
		EditorCamera
	};

	enum class EEditorPlayDestination : uint8
	{
		EmbeddedViewport,
		NewWindow
	};

	struct FEditorPlayRequest
	{
		DLevel* SourceLevel = nullptr;
		EEditorPlayStartLocation StartLocation = EEditorPlayStartLocation::LevelStart;
		EEditorPlayDestination Destination = EEditorPlayDestination::EmbeddedViewport;
		FVector3 CameraLocation{0.0};
		FVector3 CameraTarget{1.0, 0.0, 0.0};
		bool bSimulatePhysics = true;
	};

	DCLASS()
	class DEditorEngine : public DEngine
	{
		GENERATED_BODY()
	public:
		DURINED_API explicit DEditorEngine(const FObjectInitializer& ObjectInitializer);
		DURINED_API ~DEditorEngine() override;
		DURINED_API auto Init() -> void override;
		DURINED_API auto Tick(float DeltaSeconds, bool bIdleMode) -> void override;
		DURINED_API auto BeginDestroy() -> void override;
		DURINED_API auto GetTransactionManager() -> FEditorTransactionManager&;
		DURINED_API auto GetNotificationManager() -> FEditorNotificationManager&;
		DURINED_API auto StartPlaySession(DLevel* SourceLevel, std::string* OutError = nullptr) -> bool;
		DURINED_API auto StartPlaySession(const FEditorPlayRequest& Request, std::string* OutError = nullptr) -> bool;
		DURINED_API auto StopPlaySession() -> void;
		DURINED_API auto SetPlaySessionPaused(bool bPaused) -> void;
		DURINED_API auto StepPlaySession() -> void;
		DURINED_API auto ApplyPlaySessionChanges(const std::vector<AActor*>& PlayActors, uint32* OutAppliedActorCount = nullptr, std::string* OutError = nullptr) -> bool;
		DURINED_API auto GetEditorObjectForPlayObject(const DObject* PlayObject) const -> DObject*;
		auto GetPlayState() const -> EEditorPlayState { return PlayState; }
		auto IsPlaying() const -> bool { return PlayState != EEditorPlayState::Stopped; }
		auto IsPlaySessionPaused() const -> bool { return PlayState == EEditorPlayState::Paused; }
		auto GetEditorWorld() const -> DWorld* { return EditorWorld.Get(); }
		auto GetPlayWorld() const -> DWorld* { return PlayWorld.Get(); }
		auto IsPlayingInNewWindow() const -> bool { return IsPlaying() && PlayDestination == EEditorPlayDestination::NewWindow; }

	private:
		std::unique_ptr<FEditorTransactionManager> TransactionManager;
		std::unique_ptr<FEditorNotificationManager> NotificationManager;

		DPROPERTY(Transient)
		TObjectPtr<DWorld> EditorWorld;

		DPROPERTY(Transient)
		TObjectPtr<DWorld> PlayWorld;

		DPROPERTY(Transient)
		TObjectPtr<DLevel> EditorLevel;

		EEditorPlayState PlayState = EEditorPlayState::Stopped;
		EEditorPlayDestination PlayDestination = EEditorPlayDestination::EmbeddedViewport;
		std::unordered_map<DObject*, DObject*> EditorToPlayObjects;
		std::unordered_map<DObject*, DObject*> PlayToEditorObjects;
		std::shared_ptr<FSceneViewport> PreviousSceneViewport;
		std::shared_ptr<FSceneViewport> PlayWindowViewport;
		std::shared_ptr<MWindow> PlayWindow;
		std::vector<uint64> ConsoleCommandHandles;
	};

	extern DURINED_API DEditorEngine* GEditor;
}
