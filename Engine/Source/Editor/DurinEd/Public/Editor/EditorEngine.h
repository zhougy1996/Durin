#pragma once

#include "DurinEdAPI.h"
#include "Engine/Engine.h"

#include "EditorEngine.gen.h"

namespace Durin
{
	class DLevel;
}

namespace Durin::Editor
{
	class FNotificationManager;
	class FTransactionManager;

	// Tracks the lifecycle transition of a play-in-editor session.
	enum class EPlayState : uint8
	{
		Stopped,
		Starting,
		Playing,
		Paused,
		Stopping
	};

	// Selects the initial player viewpoint for a play-in-editor session.
	enum class EPlayStartLocation : uint8
	{
		LevelStart,
		EditorCamera
	};

	// Selects whether play renders in the editor or a separate window.
	enum class EPlayDestination : uint8
	{
		EmbeddedViewport,
		NewWindow
	};

	// Tracks editor Play ownership of the native mouse independently of Play state.
	enum class EMouseCaptureState : uint8
	{
		Released,
		Captured,
		Suspended
	};

	// Describes the source world, viewpoint, and physics policy for starting play.
	struct FPlayRequest
	{
		DLevel* SourceLevel = nullptr;
		EPlayStartLocation StartLocation = EPlayStartLocation::LevelStart;
		EPlayDestination Destination = EPlayDestination::EmbeddedViewport;
		FVector3 CameraLocation{0.0};
		FVector3 CameraTarget{1.0, 0.0, 0.0};
		bool bSimulatePhysics = true;
	};
}

namespace Durin
{
	class DWorld;
	class AActor;
	class DObject;
	class FSceneViewport;
	class FRenderCommandFence;
	class IMainFrameModule;
	class MWindow;
	class FGenericWindow;
	struct FEditorEngineTestAccess;

	// Owns editor services and coordinates editor and play-world lifetimes.
	DCLASS(NoClassDefaultObject)
	class DEditorEngine : public DEngine
	{
		GENERATED_BODY()
	public:
		DURINED_API explicit DEditorEngine(const FObjectInitializer& ObjectInitializer);
		DURINED_API ~DEditorEngine() override;
		DURINED_API auto Init(const FEngineInitContext& Context)
			-> FEngineInitializationResult override;
		DURINED_API auto Tick(float DeltaSeconds, bool bIdleMode) -> void override;
		DURINED_API auto BeginDestroy() -> void override;
		DURINED_API auto GetTransactionManager() -> Editor::FTransactionManager&;
		DURINED_API auto GetNotificationManager() -> Editor::FNotificationManager&;
		DURINED_API auto StartPlaySession(DLevel* SourceLevel, std::string* OutError = nullptr) -> bool;
		DURINED_API auto StartPlaySession(const Editor::FPlayRequest& Request, std::string* OutError = nullptr) -> bool;
		DURINED_API auto StopPlaySession() -> void;
		DURINED_API auto SetPlaySessionPaused(bool bPaused) -> void;
		DURINED_API auto StepPlaySession() -> void;
		DURINED_API auto UpdateEmbeddedPlayMouseTarget(
			const std::shared_ptr<FGenericWindow>& Window,
			bool bFocused,
			bool bCaptureClicked) -> void;
		DURINED_API auto RequestPlayMouseCapture(const std::shared_ptr<FGenericWindow>& Window) -> bool;
		DURINED_API auto ReleasePlayMouseCapture() -> void;
		DURINED_API auto ApplyPlaySessionChanges(const std::vector<AActor*>& PlayActors, uint32* OutAppliedActorCount = nullptr, std::string* OutError = nullptr) -> bool;
		DURINED_API auto GetEditorObjectForPlayObject(const DObject* PlayObject) const -> DObject*;
		auto GetPlayState() const -> Editor::EPlayState { return PlayState; }
		auto IsPlaying() const -> bool { return PlayState != Editor::EPlayState::Stopped; }
		auto IsPlaySessionPaused() const -> bool { return PlayState == Editor::EPlayState::Paused; }
		auto GetEditorWorld() const -> DWorld* { return EditorWorld.Get(); }
		auto GetPlayWorld() const -> DWorld* { return PlayWorld.Get(); }
		auto IsPlayingInNewWindow() const -> bool { return IsPlaying() && PlayDestination == Editor::EPlayDestination::NewWindow; }
		auto GetMouseCaptureState() const -> Editor::EMouseCaptureState { return MouseCaptureState; }
		auto IsPlayMouseCaptured() const -> bool { return MouseCaptureState == Editor::EMouseCaptureState::Captured; }

	protected:
		auto HandleGameInputWindowFocus(const std::shared_ptr<FGenericWindow>& Window, bool bFocused) -> void override;
		auto HandleGameInputWindowClose(const std::shared_ptr<FGenericWindow>& Window) -> void override;
		auto HandleGameInputKeyDown(const std::shared_ptr<FGenericWindow>& Window, EKey Key, bool bRepeat) -> bool override;
		auto HandleGameInputMouseDown(const std::shared_ptr<FGenericWindow>& Window, EMouseButton Button) -> bool override;

	private:
		DURINED_API auto StartPlaySessionInternal(
			const Editor::FPlayRequest& Request,
			std::optional<DClass*> GameModeOverride,
			std::string* OutError) -> bool;
		auto TeardownPlaySession() -> void;
		auto ReleaseRetiredPlaySessions(bool bReleaseAll = false) -> void;
		auto SuspendPlayMouseCapture() -> void;

		std::unique_ptr<Editor::FTransactionManager> TransactionManager;
		std::unique_ptr<Editor::FNotificationManager> NotificationManager;
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

		Editor::EPlayState PlayState = Editor::EPlayState::Stopped;
		Editor::EPlayDestination PlayDestination = Editor::EPlayDestination::EmbeddedViewport;
		std::unordered_map<DObject*, DObject*> EditorToPlayObjects;
		std::unordered_map<DObject*, DObject*> PlayToEditorObjects;
		std::shared_ptr<FSceneViewport> PreviousSceneViewport;
		std::shared_ptr<FSceneViewport> PlayWindowViewport;
		std::shared_ptr<MWindow> PlayWindow;
		std::weak_ptr<FGenericWindow> CapturedMouseWindow;
		Editor::EMouseCaptureState MouseCaptureState = Editor::EMouseCaptureState::Released;
		std::vector<std::unique_ptr<FRenderCommandFence>> RetiredPlayFences;
		std::vector<uint64> ConsoleCommandHandles;
		IMainFrameModule* MainFrameModule = nullptr;

		friend struct FEditorEngineTestAccess;
	};

	extern DURINED_API DEditorEngine* GEditor;
}
