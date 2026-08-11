#pragma once

#include "Engine/Engine.h"

#include "GameEngine.gen.h"

namespace Durin
{
	// Initializes the runtime engine with the active game world and main viewport.
	DCLASS(NoClassDefaultObject)
	class DGameEngine : public DEngine
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DGameEngine(const FObjectInitializer& ObjectInitializer);
		ENGINE_API auto Init(const FEngineInitContext& Context)
			-> FEngineInitializationResult override;
		ENGINE_API auto BeginDestroy() -> void override;
		ENGINE_API auto RequestGameMouseCapture(const std::shared_ptr<FGenericWindow>& Window) -> bool;
		ENGINE_API auto ReleaseGameMouseCapture() -> void;
		auto GetStartupError() const -> const std::string& { return StartupError; }
		auto IsGameMouseCaptured() const -> bool { return !CapturedMouseWindow.expired(); }

	protected:
		auto HandleGameInputWindowFocus(const std::shared_ptr<FGenericWindow>& Window, bool bFocused) -> void override;
		auto HandleGameInputWindowClose(const std::shared_ptr<FGenericWindow>& Window) -> void override;
		auto HandleGameInputKeyDown(const std::shared_ptr<FGenericWindow>& Window, EKey Key, bool bRepeat) -> bool override;
		auto HandleGameInputMouseDown(const std::shared_ptr<FGenericWindow>& Window, EMouseButton Button) -> bool override;

	private:
		std::string StartupError;
		std::weak_ptr<FGenericWindow> CapturedMouseWindow;

		friend struct FGameEngineTestAccess;
	};
}
