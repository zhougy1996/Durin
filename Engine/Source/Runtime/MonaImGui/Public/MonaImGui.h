#pragma once

#include "MonaImGuiAPI.h"
#include "MonaImGuiPropertyTable.h"
#include "MonaImGuiWidgets.h"
#include "Math/MathFwd.h"
#include "RHIResources.h"
// ReSharper disable once CppUnusedIncludeDirective
#include "ThirdParty/ImGui/ImGuiCommon.h"

namespace Durin
{
	class MWindow;

	namespace MonaImGui
	{
		struct FUIStyleMetrics
		{
			float SpacingXS = 2.0f;
			float SpacingS = 4.0f;
			float SpacingM = 8.0f;
			float SpacingL = 12.0f;
			float SplitterThickness = 6.0f;
			float StandardPopupWidth = 320.0f;
			float WidePopupWidth = 640.0f;
			float CompactButtonWidth = 80.0f;
			float StandardButtonWidth = 96.0f;
		};

		enum class EColorTheme : uint8
		{
			Dark,
			Light,
		};

		enum class EUIThemeColor : uint8
		{
			Warning,
			Error,
			Info,
			Success,
			Folder,
			Asset,
			SourceFile,
			AxisX,
			AxisY,
			AxisZ,
			ViewportText,
			ViewportShadow,
			ConsoleTimestamp,
			ConsoleModule,
			SelectionPrimary,
			SelectionSecondary,
			Count,
		};

		MONAIMGUI_API auto DrawTexture(const FRHITexture* Texture, const FVector2f& Size) -> void;
		MONAIMGUI_API auto BindMainViewportToWindow(const std::shared_ptr<MWindow>& Window) -> void;
		MONAIMGUI_API auto SetGlobalUIScale(float Scale) -> void;
		MONAIMGUI_API auto GetGlobalUIScale() -> float;
		MONAIMGUI_API auto ScaleUI(float BaseValue) -> float;
		MONAIMGUI_API auto GetUIStyleMetrics() -> FUIStyleMetrics;
		MONAIMGUI_API auto SetColorTheme(EColorTheme Theme) -> void;
		MONAIMGUI_API auto GetColorTheme() -> EColorTheme;
		MONAIMGUI_API auto GetThemeColor(EUIThemeColor Color) -> const ImVec4&;
		MONAIMGUI_API auto GetThemeColorU32(EUIThemeColor Color) -> ImU32;
	} // namespace MonaImGui
} // namespace Durin
