#pragma once

#include "CoreMinimal.h"

#include "MonaImGui.h"

namespace Durin { class DEngine; }

namespace Durin::Editor::MainFrame
{
	// Persists editor-host layout and startup behavior outside project settings.
	class FHostSettings
	{
	public:
		auto Load() -> bool;
		auto Save() const -> bool;

		auto GetWindowWidth() const -> int32 { return WindowWidth; }
		auto GetWindowHeight() const -> int32 { return WindowHeight; }
		auto GetUIScale() const -> float { return UIScale; }
		auto GetColorTheme() const -> MonaImGui::EColorTheme { return ColorTheme; }
		auto IsWindowMaximized() const -> bool { return bWindowMaximized; }
		auto GetFrameRateLimit() const -> int32 { return FrameRateLimit; }
		auto SetFrameRateLimit(int32 FPS) -> void { FrameRateLimit = std::max(0, FPS); }
		auto IsBackgroundFrameRateLimited() const -> bool { return bLimitBackgroundFrameRate; }
		auto SetLimitBackgroundFrameRate(bool bEnabled) -> void { bLimitBackgroundFrameRate = bEnabled; }
		auto GetBackgroundFrameRateLimit() const -> int32 { return BackgroundFrameRateLimit; }
		auto GetEffectiveBackgroundFrameRateLimit() const -> int32 { return bLimitBackgroundFrameRate ? BackgroundFrameRateLimit : 0; }
		auto SetBackgroundFrameRateLimit(int32 FPS) -> void { BackgroundFrameRateLimit = std::max(1, FPS); }
		auto IsVSyncEnabled() const -> bool { return bVSyncEnabled; }
		auto SetVSyncEnabled(bool bEnabled) -> void { bVSyncEnabled = bEnabled; }

		auto SetDisplaySettings(int32 Width, int32 Height, float Scale) -> void;
		auto SetColorTheme(MonaImGui::EColorTheme Theme) -> void { ColorTheme = Theme; }
		auto SetWindowMaximized(bool bMaximized) -> void { bWindowMaximized = bMaximized; }

	private:
		bool bWindowMaximized = true;
		bool bVSyncEnabled = true;
		int32 FrameRateLimit = 0;
		bool bLimitBackgroundFrameRate = true;
		int32 BackgroundFrameRateLimit = 30;
		int32 WindowWidth = 1280;
		int32 WindowHeight = 800;
		float UIScale = 1.0f;
		MonaImGui::EColorTheme ColorTheme = MonaImGui::EColorTheme::Dark;
	};

	// Applies the persisted presentation policy at startup, on edits, and on reset.
	auto ApplyDisplaySettings(const FHostSettings& Settings, DEngine* Engine) -> void;
}
