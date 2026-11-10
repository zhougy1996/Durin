#pragma once

#include "CoreMinimal.h"

#include "Engine/Engine.h"

#include "GameEngine.gen.h"

namespace Durin
{
	// Constructs game services, then starts project content through the ordinary frame loop.
	DCLASS(NoClassDefaultObject)
	class DGameEngine : public DEngine
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DGameEngine(const FObjectInitializer& ObjectInitializer);
		ENGINE_API auto Init(const FEngineInitContext& Context)
			-> bool override;
		ENGINE_API auto Tick(float DeltaSeconds, bool bIdleMode) -> void override;
		ENGINE_API auto AdvanceStartup() -> void override;
		ENGINE_API auto ShouldDrawStartupScene() const -> bool override;
		auto GetStartupState() const -> EHostStartupState override
		{
			return StartupPhase == EStartupPhase::Failed ? EHostStartupState::Failed
				: StartupPhase == EStartupPhase::Ready ? EHostStartupState::Ready : EHostStartupState::Pending;
		}
		ENGINE_API auto BeginDestroy() -> void override;
		ENGINE_API auto RequestGameMouseCapture(const std::shared_ptr<FGenericWindow>& Window) -> bool;
		ENGINE_API auto ReleaseGameMouseCapture() -> void;
		auto GetStartupError() const -> const std::string& override { return StartupError; }
		auto IsGameMouseCaptured() const -> bool { return !CapturedMouseWindow.expired(); }

	protected:
		ENGINE_API auto RequestSubsystemDeinitialization() -> void override;
		auto HandleGameInputWindowFocus(const std::shared_ptr<FGenericWindow>& Window, bool bFocused) -> void override;
		auto HandleGameInputWindowClose(const std::shared_ptr<FGenericWindow>& Window) -> void override;
		auto HandleGameInputKeyDown(const std::shared_ptr<FGenericWindow>& Window, EKey Key, bool bRepeat) -> bool override;
		auto HandleGameInputMouseDown(const std::shared_ptr<FGenericWindow>& Window, EMouseButton Button) -> bool override;

	private:
		enum class EStartupPhase : uint8 { Ready, StartProject, AwaitingScene, Failed };
		auto StartGame() -> std::expected<void, std::string>;
		auto FailStartup(std::string Error) -> void;
		auto CompleteStartup() -> void;
		// Service-only hosts remain usable; successful Init queues production startup.
		EStartupPhase StartupPhase = EStartupPhase::Ready;
		std::string StartupError;
		std::weak_ptr<FGenericWindow> CapturedMouseWindow;

		friend struct FGameEngineTestAccess;
	};
}
