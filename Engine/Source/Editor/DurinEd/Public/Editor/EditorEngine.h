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

	enum class EEditorPlayState : uint8
	{
		Stopped,
		Starting,
		Playing,
		Paused,
		Stopping
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
		DURINED_API auto StopPlaySession() -> void;
		DURINED_API auto SetPlaySessionPaused(bool bPaused) -> void;
		DURINED_API auto StepPlaySession() -> void;
		auto GetPlayState() const -> EEditorPlayState { return PlayState; }
		auto IsPlaying() const -> bool { return PlayState != EEditorPlayState::Stopped; }
		auto IsPlaySessionPaused() const -> bool { return PlayState == EEditorPlayState::Paused; }
		auto GetEditorWorld() const -> DWorld* { return EditorWorld.Get(); }
		auto GetPlayWorld() const -> DWorld* { return PlayWorld.Get(); }

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
	};

	extern DURINED_API DEditorEngine* GEditor;
}
