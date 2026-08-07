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
	class FRenderCommandFence;
	class MWindow;

	// Tracks the lifecycle transition of a play-in-editor session.
	enum class EEditorPlayState : uint8
	{
		Stopped,
		Starting,
		Playing,
		Paused,
		Stopping
	};

	// Selects the initial player viewpoint for a play-in-editor session.
	enum class EEditorPlayStartLocation : uint8
	{
		LevelStart,
		EditorCamera
	};

	// Selects whether play renders in the editor or a separate window.
	enum class EEditorPlayDestination : uint8
	{
		EmbeddedViewport,
		NewWindow
	};

	// Describes the source world, viewpoint, and physics policy for starting play.
	struct FEditorPlayRequest
	{
		DLevel* SourceLevel = nullptr;
		EEditorPlayStartLocation StartLocation = EEditorPlayStartLocation::LevelStart;
		EEditorPlayDestination Destination = EEditorPlayDestination::EmbeddedViewport;
		FVector3 CameraLocation{0.0};
		FVector3 CameraTarget{1.0, 0.0, 0.0};
		bool bSimulatePhysics = true;
	};

	// Owns editor services and coordinates editor and play-world lifetimes.
	DCLASS(NoClassDefaultObject)
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
		auto TeardownPlaySession() -> void;
		auto ReleaseRetiredPlaySessions(bool bReleaseAll = false) -> void;

		std::unique_ptr<FEditorTransactionManager> TransactionManager;
		std::unique_ptr<FEditorNotificationManager> NotificationManager;
		// Authoritative world being edited; retained for the editor engine lifetime.
		DPROPERTY(Transient)
		TObjectPtr<DWorld> EditorWorld;

		// Duplicated runtime world while a play session is active.
		DPROPERTY(Transient)
		TObjectPtr<DWorld> PlayWorld;

		// Level currently loaded in the authoritative editor world.
		DPROPERTY(Transient)
		TObjectPtr<DLevel> EditorLevel;

		// Stopped PIE worlds remain alive until the render thread has consumed their
		// scene-removal commands. This keeps Stop responsive without weakening proxy lifetime.
		DPROPERTY(Transient)
		std::vector<TObjectPtr<DWorld>> RetiredPlayWorlds;

		// Levels are paired with retired worlds until the same render fences complete.
		DPROPERTY(Transient)
		std::vector<TObjectPtr<DLevel>> RetiredPlayLevels;

		EEditorPlayState PlayState = EEditorPlayState::Stopped;
		EEditorPlayDestination PlayDestination = EEditorPlayDestination::EmbeddedViewport;
		std::unordered_map<DObject*, DObject*> EditorToPlayObjects;
		std::unordered_map<DObject*, DObject*> PlayToEditorObjects;
		std::shared_ptr<FSceneViewport> PreviousSceneViewport;
		std::shared_ptr<FSceneViewport> PlayWindowViewport;
		std::shared_ptr<MWindow> PlayWindow;
		std::vector<std::unique_ptr<FRenderCommandFence>> RetiredPlayFences;
		std::vector<uint64> ConsoleCommandHandles;
	};

	extern DURINED_API DEditorEngine* GEditor;
}
