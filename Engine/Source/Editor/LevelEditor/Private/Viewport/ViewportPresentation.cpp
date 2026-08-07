#include "Viewport/ViewportPresentation.h"

#include "Editor/EditorEngine.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Math/Vector.h"
#include "MonaImGui.h"
#include "Viewport/LevelEditorViewportClient.h"
#include "LevelEditorViewportEditing.h"
#include "Workspace/LevelEditorContext.h"
#include "Workspace/LevelEditorUILayout.h"

namespace Durin
{
	namespace
	{
		template<typename T>
		// Couples a viewport-mode value with its label, tooltip, and toolbar icon.
		struct TViewportModeOption
		{
			T Value;
			const char* Label;
		};

		constexpr std::array RenderModeOptions = {
			TViewportModeOption{ERenderMode::Lit, "Lit"},
			TViewportModeOption{ERenderMode::Unlit, "Unlit"}
		};

		constexpr std::array RasterModeOptions = {
			TViewportModeOption{ERasterMode::Solid, "Solid"},
			TViewportModeOption{ERasterMode::Wireframe, "Wireframe"}
		};

		template<typename T, size_t Size>
		auto GetModeLabel(T CurrentMode, const std::array<TViewportModeOption<T>, Size>& Options) -> const char*
		{
			for (const TViewportModeOption<T>& Option : Options)
			{
				if (Option.Value == CurrentMode) return Option.Label;
			}
			return "Unknown";
		}

		template<typename T, size_t Size, typename FSetMode>
		auto DrawModeOptions(T CurrentMode, const std::array<TViewportModeOption<T>, Size>& Options, FSetMode&& SetMode) -> void
		{
			for (const TViewportModeOption<T>& Option : Options)
			{
				if (ImGui::RadioButton(Option.Label, Option.Value == CurrentMode)) SetMode(Option.Value);
			}
		}

		auto Add(const ImVec2& A, const ImVec2& B) -> ImVec2 { return ImVec2(A.x + B.x, A.y + B.y); }
		auto Mul(const ImVec2& Value, float Scale) -> ImVec2 { return ImVec2(Value.x * Scale, Value.y * Scale); }

		// Identifies semantic toolbar icons independent of the active font glyphs.
		enum class EViewportToolbarIcon : uint8
		{
			None,
			Translate,
			Rotate,
			Scale,
			SnapGrid,
			Play,
			Pause,
			Step,
			Stop,
			ChevronDown
		};

		auto DrawToolbarIcon(ImDrawList* DrawList, EViewportToolbarIcon Icon, const ImVec2& Center, ImU32 Color, float Scale) -> void
		{
			const float Thickness = FMath::Max(1.0f, 1.6f * Scale);
			switch (Icon)
			{
			case EViewportToolbarIcon::Translate:
				{
					const float Radius = 6.0f * Scale;
					DrawList->AddLine(ImVec2(Center.x - Radius, Center.y), ImVec2(Center.x + Radius, Center.y), Color, Thickness);
					DrawList->AddLine(ImVec2(Center.x, Center.y - Radius), ImVec2(Center.x, Center.y + Radius), Color, Thickness);
					DrawList->AddTriangleFilled(ImVec2(Center.x + Radius + 2.0f * Scale, Center.y), ImVec2(Center.x + Radius - 2.0f * Scale, Center.y - 2.5f * Scale), ImVec2(Center.x + Radius - 2.0f * Scale, Center.y + 2.5f * Scale), Color);
					DrawList->AddTriangleFilled(ImVec2(Center.x, Center.y - Radius - 2.0f * Scale), ImVec2(Center.x - 2.5f * Scale, Center.y - Radius + 2.0f * Scale), ImVec2(Center.x + 2.5f * Scale, Center.y - Radius + 2.0f * Scale), Color);
					break;
				}
			case EViewportToolbarIcon::Rotate:
				{
					const float Radius = 6.5f * Scale;
					DrawList->PathArcTo(Center, Radius, -2.65f, 2.15f, 18);
					DrawList->PathStroke(Color, 0, Thickness);
					const ImVec2 Tip(Center.x - 5.5f * Scale, Center.y - 4.3f * Scale);
					DrawList->AddTriangleFilled(Tip, ImVec2(Tip.x + 4.8f * Scale, Tip.y - 0.3f * Scale), ImVec2(Tip.x + 1.2f * Scale, Tip.y + 4.2f * Scale), Color);
					break;
				}
			case EViewportToolbarIcon::Scale:
				{
					const ImVec2 Start(Center.x - 5.0f * Scale, Center.y + 5.0f * Scale);
					const ImVec2 End(Center.x + 5.0f * Scale, Center.y - 5.0f * Scale);
					DrawList->AddLine(Start, End, Color, Thickness);
					DrawList->AddRectFilled(ImVec2(Start.x - 2.5f * Scale, Start.y - 2.5f * Scale), ImVec2(Start.x + 2.5f * Scale, Start.y + 2.5f * Scale), Color, 1.0f * Scale);
					DrawList->AddRectFilled(ImVec2(End.x - 2.5f * Scale, End.y - 2.5f * Scale), ImVec2(End.x + 2.5f * Scale, End.y + 2.5f * Scale), Color, 1.0f * Scale);
					break;
				}
			case EViewportToolbarIcon::SnapGrid:
				{
					const float Radius = 6.0f * Scale;
					for (const float Offset : {-Radius, 0.0f, Radius})
					{
						DrawList->AddLine(ImVec2(Center.x - Radius, Center.y + Offset), ImVec2(Center.x + Radius, Center.y + Offset), Color, Thickness);
						DrawList->AddLine(ImVec2(Center.x + Offset, Center.y - Radius), ImVec2(Center.x + Offset, Center.y + Radius), Color, Thickness);
					}
					break;
				}
			case EViewportToolbarIcon::Play:
				{
					const float Radius = 6.0f * Scale;
					DrawList->AddTriangleFilled(ImVec2(Center.x - Radius * 0.65f, Center.y - Radius), ImVec2(Center.x - Radius * 0.65f, Center.y + Radius), ImVec2(Center.x + Radius, Center.y), Color);
					break;
				}
			case EViewportToolbarIcon::Pause:
				{
					const float HalfHeight = 6.0f * Scale;
					const float HalfWidth = 2.0f * Scale;
					const float Gap = 2.2f * Scale;
					DrawList->AddRectFilled(ImVec2(Center.x - Gap - HalfWidth, Center.y - HalfHeight), ImVec2(Center.x - Gap + HalfWidth, Center.y + HalfHeight), Color, Scale);
					DrawList->AddRectFilled(ImVec2(Center.x + Gap - HalfWidth, Center.y - HalfHeight), ImVec2(Center.x + Gap + HalfWidth, Center.y + HalfHeight), Color, Scale);
					break;
				}
			case EViewportToolbarIcon::Step:
				{
					const float Radius = 5.5f * Scale;
					DrawList->AddTriangleFilled(ImVec2(Center.x - Radius, Center.y - Radius), ImVec2(Center.x - Radius, Center.y + Radius), ImVec2(Center.x + Radius * 0.45f, Center.y), Color);
					DrawList->AddRectFilled(ImVec2(Center.x + Radius * 0.55f, Center.y - Radius), ImVec2(Center.x + Radius * 0.9f, Center.y + Radius), Color, Scale);
					break;
				}
			case EViewportToolbarIcon::Stop:
				{
					const float Radius = 5.0f * Scale;
					DrawList->AddRectFilled(ImVec2(Center.x - Radius, Center.y - Radius), ImVec2(Center.x + Radius, Center.y + Radius), Color, 1.5f * Scale);
					break;
				}
			case EViewportToolbarIcon::ChevronDown:
				DrawList->AddTriangleFilled(ImVec2(Center.x - 3.5f * Scale, Center.y - 1.5f * Scale), ImVec2(Center.x + 3.5f * Scale, Center.y - 1.5f * Scale), ImVec2(Center.x, Center.y + 2.5f * Scale), Color);
				break;
			default:
				break;
			}
		}

		auto DrawToolbarSurface(const ImVec2& Min, const ImVec2& Max) -> void
		{
			ImDrawList* DrawList = ImGui::GetWindowDrawList();
			const ImGuiStyle& Style = ImGui::GetStyle();
			const float Rounding = MonaImGui::ScaleUI(4.0f);
			ImVec4 ToolbarColor = Style.Colors[ImGuiCol_WindowBg];
			ToolbarColor.w = 0.92f;
			ImVec4 BorderColor = Style.Colors[ImGuiCol_Border];
			BorderColor.w *= 0.40f;
			DrawList->AddRectFilled(Min, Max, ImGui::GetColorU32(ToolbarColor), Rounding);
			DrawList->AddRect(Min, Max, ImGui::GetColorU32(BorderColor), Rounding);
		}

		auto DrawToolbarSeparator(ImDrawList* DrawList, float X, float Y, float Height) -> void
		{
			ImVec4 Color = ImGui::GetStyleColorVec4(ImGuiCol_Border);
			Color.w *= 0.45f;
			const float Inset = MonaImGui::ScaleUI(7.0f);
			DrawList->AddLine(ImVec2(X, Y + Inset), ImVec2(X, Y + Height - Inset), ImGui::GetColorU32(Color));
		}

		auto DrawPlayStateBorder(const ImVec2& ViewportMin, const ImVec2& ViewportMax, bool bPaused) -> void
		{
			const ImVec2 Inset(MonaImGui::ScaleUI(1.0f), MonaImGui::ScaleUI(1.0f));
			const ImU32 Color = MonaImGui::GetThemeColorU32(
				bPaused ? MonaImGui::EUIThemeColor::Warning : MonaImGui::EUIThemeColor::Success);
			ImDrawList* DrawList = ImGui::GetWindowDrawList();
			DrawList->PushClipRect(ViewportMin, ViewportMax, true);
			DrawList->AddRect(Add(ViewportMin, Inset), Add(ViewportMax, Mul(Inset, -1.0f)), Color, 0.0f, 0, MonaImGui::ScaleUI(2.0f));
			DrawList->PopClipRect();
		}

		auto DrawToolbarButtonBackground(
			ImDrawList* DrawList,
			const ImVec2& Min,
			const ImVec2& Max,
			bool bSelected,
			bool bButtonSurface,
			bool bHovered,
			bool bHeld,
			ImDrawFlags Rounding = ImDrawFlags_RoundCornersAll
		) -> void
		{
			ImGuiCol Background;
			if (bSelected)
			{
				Background = bHeld ? ImGuiCol_HeaderActive : bHovered ? ImGuiCol_HeaderHovered : ImGuiCol_Header;
			}
			else if (bHeld)
			{
				Background = ImGuiCol_ButtonActive;
			}
			else if (bHovered)
			{
				Background = ImGuiCol_ButtonHovered;
			}
			else if (bButtonSurface)
			{
				Background = ImGuiCol_Button;
			}
			else
			{
				return;
			}
			DrawList->AddRectFilled(Min, Max, ImGui::GetColorU32(Background), ImGui::GetStyle().FrameRounding, Rounding);
		}

		auto DrawToolbarButton(const char* Id, const ImVec2& Position, const ImVec2& Size, const char* Label, EViewportToolbarIcon Icon, bool bSelected, const char* Tooltip, bool bButtonSurface = false, bool bSuccessIcon = false) -> bool
		{
			ImGui::SetCursorScreenPos(Position);
			const bool bPressed = ImGui::InvisibleButton(Id, Size);
			const bool bHovered = ImGui::IsItemHovered();
			const bool bHeld = ImGui::IsItemActive();
			ImDrawList* DrawList = ImGui::GetWindowDrawList();
			const ImVec2 Max(Position.x + Size.x, Position.y + Size.y);
			DrawToolbarButtonBackground(DrawList, Position, Max, bSelected, bButtonSurface, bHovered, bHeld);

			const ImU32 TextColor = ImGui::GetColorU32(bSelected || bButtonSurface || bHovered ? ImGuiCol_Text : ImGuiCol_TextDisabled);
			// Play is an action, not a persistent selection: keep the standard button surface
			// and carry its meaning with the semantic success color on the icon alone.
			const ImU32 IconColor = bSuccessIcon ? MonaImGui::GetThemeColorU32(MonaImGui::EUIThemeColor::Success)
				: bSelected ? ImGui::GetColorU32(ImGuiCol_CheckMark) : TextColor;
			const float IconScale = ImGui::GetFontSize() / 15.0f;
			float ContentWidth = 0.0f;
			const ImVec2 TextSize = Label != nullptr ? ImGui::CalcTextSize(Label) : ImVec2(0.0f, 0.0f);
			const float IconWidth = Icon == EViewportToolbarIcon::None ? 0.0f : (Icon == EViewportToolbarIcon::ChevronDown ? 10.0f : 16.0f) * IconScale;
			const float Gap = Icon != EViewportToolbarIcon::None && Label != nullptr ? MonaImGui::ScaleUI(6.0f) : 0.0f;
			ContentWidth = IconWidth + Gap + TextSize.x;
			float ContentX = Position.x + (Size.x - ContentWidth) * 0.5f;
			const bool bTrailingIcon = Icon == EViewportToolbarIcon::ChevronDown && Label != nullptr;
			if (Icon != EViewportToolbarIcon::None && !bTrailingIcon)
			{
				DrawToolbarIcon(DrawList, Icon, ImVec2(ContentX + IconWidth * 0.5f, Position.y + Size.y * 0.5f), IconColor, IconScale);
				ContentX += IconWidth + Gap;
			}
			if (Label != nullptr)
			{
				DrawList->AddText(ImVec2(ContentX, Position.y + (Size.y - TextSize.y) * 0.5f), TextColor, Label);
			}
			if (bTrailingIcon)
			{
				DrawToolbarIcon(DrawList, Icon, ImVec2(ContentX + TextSize.x + Gap + IconWidth * 0.5f, Position.y + Size.y * 0.5f), IconColor, IconScale);
			}
			if (bHovered && Tooltip != nullptr)
			{
				ImGui::BeginTooltip();
				ImGui::TextUnformatted(Tooltip);
				ImGui::EndTooltip();
			}
			return bPressed;
		}

		// Reports which half of a split toolbar button was activated.
		struct FSplitButtonResult
		{
			bool bPrimaryPressed = false;
			bool bSecondaryPressed = false;
		};

		// Reports play-control requests gathered during one toolbar draw.
		struct FRuntimeControlResult
		{
			bool bStopPressed = false;
			bool bPausePressed = false;
			bool bStepPressed = false;
			bool bOptionsPressed = false;
		};

		auto DrawPlaySplitButton(const ImVec2& Position, float PrimaryWidth, float SecondaryWidth, float Height, const char* Label, const char* PlayTooltip) -> FSplitButtonResult
		{
			const ImGuiStyle& Style = ImGui::GetStyle();
			ImDrawList* DrawList = ImGui::GetWindowDrawList();
			const ImVec2 Max(Position.x + PrimaryWidth + SecondaryWidth, Position.y + Height);

			ImGui::SetCursorScreenPos(Position);
			const bool bPrimaryPressed = ImGui::InvisibleButton("##ViewportPlay", ImVec2(PrimaryWidth, Height));
			const bool bPrimaryHovered = ImGui::IsItemHovered();
			const bool bPrimaryHeld = ImGui::IsItemActive();
			if (bPrimaryHovered || bPrimaryHeld)
			{
				DrawList->AddRectFilled(Position, ImVec2(Position.x + PrimaryWidth, Max.y), ImGui::GetColorU32(bPrimaryHeld ? ImGuiCol_ButtonActive : ImGuiCol_ButtonHovered), Style.FrameRounding, ImDrawFlags_RoundCornersLeft);
			}

			const ImVec2 SecondaryPosition(Position.x + PrimaryWidth, Position.y);
			ImGui::SetCursorScreenPos(SecondaryPosition);
			const bool bSecondaryPressed = ImGui::InvisibleButton("##ViewportPlayOptions", ImVec2(SecondaryWidth, Height));
			const bool bSecondaryHovered = ImGui::IsItemHovered();
			const bool bSecondaryHeld = ImGui::IsItemActive();
			if (bSecondaryHovered || bSecondaryHeld)
			{
				DrawList->AddRectFilled(SecondaryPosition, Max, ImGui::GetColorU32(bSecondaryHeld ? ImGuiCol_ButtonActive : ImGuiCol_ButtonHovered), Style.FrameRounding, ImDrawFlags_RoundCornersRight);
			}

			DrawList->AddLine(ImVec2(SecondaryPosition.x, Position.y + MonaImGui::ScaleUI(5.0f)), ImVec2(SecondaryPosition.x, Max.y - MonaImGui::ScaleUI(5.0f)), ImGui::GetColorU32(ImGuiCol_Border));
			const float IconScale = ImGui::GetFontSize() / 15.0f;
			const float IconWidth = 16.0f * IconScale;
			const ImVec2 TextSize = Label != nullptr ? ImGui::CalcTextSize(Label) : ImVec2(0.0f, 0.0f);
			const float Gap = Label != nullptr ? MonaImGui::ScaleUI(6.0f) : 0.0f;
			const float ContentWidth = IconWidth + Gap + TextSize.x;
			const float ContentX = Position.x + (PrimaryWidth - ContentWidth) * 0.5f;
			DrawToolbarIcon(DrawList, EViewportToolbarIcon::Play, ImVec2(ContentX + IconWidth * 0.5f, Position.y + Height * 0.5f), MonaImGui::GetThemeColorU32(MonaImGui::EUIThemeColor::Success), IconScale);
			if (Label != nullptr)
			{
				DrawList->AddText(ImVec2(ContentX + IconWidth + Gap, Position.y + (Height - TextSize.y) * 0.5f), ImGui::GetColorU32(ImGuiCol_Text), Label);
			}
			DrawToolbarIcon(DrawList, EViewportToolbarIcon::ChevronDown, ImVec2(SecondaryPosition.x + SecondaryWidth * 0.5f, Position.y + Height * 0.5f), ImGui::GetColorU32(ImGuiCol_Text), IconScale);

			if (bPrimaryHovered)
			{
				ImGui::BeginTooltip();
				ImGui::TextUnformatted(PlayTooltip);
				ImGui::EndTooltip();
			}
			else if (bSecondaryHovered)
			{
				ImGui::BeginTooltip();
				ImGui::TextUnformatted("Play settings");
				ImGui::EndTooltip();
			}
			return {bPrimaryPressed, bSecondaryPressed};
		}

		auto DrawRuntimeControlGroup(const ImVec2& Position, float ControlWidth, float OptionsWidth, float Height, bool bPaused) -> FRuntimeControlResult
		{
			const ImGuiStyle& Style = ImGui::GetStyle();
			ImDrawList* DrawList = ImGui::GetWindowDrawList();
			const float GroupWidth = ControlWidth * 3.0f + OptionsWidth;
			const ImVec2 Max(Position.x + GroupWidth, Position.y + Height);

			auto DrawSegment = [&](const char* Id, float Offset, float Width, EViewportToolbarIcon Icon, ImU32 EnabledIconColor, const char* Tooltip, bool bEnabled, ImDrawFlags Rounding) {
				const ImVec2 SegmentMin(Position.x + Offset, Position.y);
				const ImVec2 SegmentMax(SegmentMin.x + Width, Max.y);
				if (!bEnabled) ImGui::BeginDisabled();
				ImGui::SetCursorScreenPos(SegmentMin);
				const bool bPressed = ImGui::InvisibleButton(Id, ImVec2(Width, Height));
				const bool bHovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
				const bool bHeld = ImGui::IsItemActive();
				if (!bEnabled) ImGui::EndDisabled();
				if (bEnabled && (bHovered || bHeld))
				{
					DrawList->AddRectFilled(SegmentMin, SegmentMax, ImGui::GetColorU32(bHeld ? ImGuiCol_ButtonActive : ImGuiCol_ButtonHovered), Style.FrameRounding, Rounding);
				}
				const ImU32 IconColor = bEnabled ? EnabledIconColor : ImGui::GetColorU32(ImGuiCol_TextDisabled);
				DrawToolbarIcon(DrawList, Icon, ImVec2((SegmentMin.x + SegmentMax.x) * 0.5f, Position.y + Height * 0.5f), IconColor, ImGui::GetFontSize() / 15.0f);
				if (bHovered)
				{
					ImGui::BeginTooltip();
					ImGui::TextUnformatted(Tooltip);
					ImGui::EndTooltip();
				}
				return bPressed;
			};

			FRuntimeControlResult Result;
			Result.bStopPressed = DrawSegment("##ViewportStop", 0.0f, ControlWidth, EViewportToolbarIcon::Stop,
				MonaImGui::GetThemeColorU32(MonaImGui::EUIThemeColor::Error), "Stop play (F5)", true, ImDrawFlags_RoundCornersLeft);
			Result.bPausePressed = DrawSegment("##ViewportPause", ControlWidth, ControlWidth, bPaused ? EViewportToolbarIcon::Play : EViewportToolbarIcon::Pause,
				MonaImGui::GetThemeColorU32(bPaused ? MonaImGui::EUIThemeColor::Success : MonaImGui::EUIThemeColor::Warning),
				bPaused ? "Resume play (F6)" : "Pause play (F6)", true, ImDrawFlags_RoundCornersNone);
			Result.bStepPressed = DrawSegment("##ViewportStep", ControlWidth * 2.0f, ControlWidth, EViewportToolbarIcon::Step,
				MonaImGui::GetThemeColorU32(MonaImGui::EUIThemeColor::Info), "Advance one frame while paused (F7)", bPaused, ImDrawFlags_RoundCornersNone);
			Result.bOptionsPressed = DrawSegment("##ViewportRuntimeOptions", ControlWidth * 3.0f, OptionsWidth, EViewportToolbarIcon::ChevronDown,
				ImGui::GetColorU32(ImGuiCol_Text), "Runtime options", true, ImDrawFlags_RoundCornersRight);

			const float SeparatorInset = MonaImGui::ScaleUI(5.0f);
			for (const float Offset : {ControlWidth, ControlWidth * 2.0f, ControlWidth * 3.0f})
			{
				const float SeparatorX = Position.x + Offset;
				DrawList->AddLine(ImVec2(SeparatorX, Position.y + SeparatorInset), ImVec2(SeparatorX, Max.y - SeparatorInset), ImGui::GetColorU32(ImGuiCol_Border));
			}
			return Result;
		}

		auto DrawSnapSplitButton(const ImVec2& Position, float PrimaryWidth, float SecondaryWidth, float Height, bool bEnabled, bool bPopupOpen) -> FSplitButtonResult
		{
			ImDrawList* DrawList = ImGui::GetWindowDrawList();
			const ImVec2 Max(Position.x + PrimaryWidth + SecondaryWidth, Position.y + Height);

			ImGui::SetCursorScreenPos(Position);
			const bool bPrimaryPressed = ImGui::InvisibleButton("##SnapToggle", ImVec2(PrimaryWidth, Height));
			const bool bPrimaryHovered = ImGui::IsItemHovered();
			const bool bPrimaryHeld = ImGui::IsItemActive();
			const ImVec2 PrimaryMax(Position.x + PrimaryWidth, Max.y);

			const ImVec2 SecondaryPosition(Position.x + PrimaryWidth, Position.y);
			ImGui::SetCursorScreenPos(SecondaryPosition);
			const bool bSecondaryPressed = ImGui::InvisibleButton("##SnapSettingsButton", ImVec2(SecondaryWidth, Height));
			const bool bSecondaryHovered = ImGui::IsItemHovered();
			const bool bSecondaryHeld = ImGui::IsItemActive();
			DrawToolbarButtonBackground(DrawList, Position, PrimaryMax, bEnabled, false, bPrimaryHovered, bPrimaryHeld, ImDrawFlags_RoundCornersLeft);
			DrawToolbarButtonBackground(DrawList, SecondaryPosition, Max, bPopupOpen, false, bSecondaryHovered, bSecondaryHeld, ImDrawFlags_RoundCornersRight);

			DrawList->AddLine(ImVec2(SecondaryPosition.x, Position.y + MonaImGui::ScaleUI(5.0f)), ImVec2(SecondaryPosition.x, Max.y - MonaImGui::ScaleUI(5.0f)), ImGui::GetColorU32(ImGuiCol_Border));
			const ImU32 TextColor = ImGui::GetColorU32(bEnabled || bPopupOpen || bPrimaryHovered || bSecondaryHovered ? ImGuiCol_Text : ImGuiCol_TextDisabled);
			const float IconScale = ImGui::GetFontSize() / 15.0f;
			const ImU32 SnapIconColor = bEnabled ? ImGui::GetColorU32(ImGuiCol_CheckMark) : TextColor;
			DrawToolbarIcon(DrawList, EViewportToolbarIcon::SnapGrid, ImVec2(Position.x + PrimaryWidth * 0.5f, Position.y + Height * 0.5f), SnapIconColor, IconScale);
			DrawToolbarIcon(DrawList, EViewportToolbarIcon::ChevronDown, ImVec2(SecondaryPosition.x + SecondaryWidth * 0.5f, Position.y + Height * 0.5f), TextColor, IconScale);
			if (bPrimaryHovered)
			{
				ImGui::BeginTooltip();
				ImGui::TextUnformatted("Toggle transform snapping");
				ImGui::EndTooltip();
			}
			else if (bSecondaryHovered)
			{
				ImGui::BeginTooltip();
				ImGui::TextUnformatted("Snapping settings");
				ImGui::EndTooltip();
			}
			return {bPrimaryPressed, bSecondaryPressed};
		}

		auto GetScreenAxisDirection(const FMatrix& ViewMatrix, const FVector3& WorldAxis, const ImVec2& FallbackDirection) -> ImVec2
		{
			const FVector4 AxisInView = ViewMatrix * FVector4(WorldAxis, 0.0);
			ImVec2 Direction(static_cast<float>(AxisInView.y), static_cast<float>(-AxisInView.z));
			const float LengthSquared = Direction.x * Direction.x + Direction.y * Direction.y;
			if (LengthSquared <= 0.0001f)
			{
				return FallbackDirection;
			}
			return Mul(Direction, 1.0f / std::sqrt(LengthSquared));
		}

		auto DrawAxisText(ImDrawList* DrawList, const ImVec2& Position, ImU32 Color, const char* Text) -> void
		{
			DrawList->AddText(Add(Position, ImVec2(MonaImGui::ScaleUI(1.0f), MonaImGui::ScaleUI(1.0f))), MonaImGui::GetThemeColorU32(MonaImGui::EUIThemeColor::ViewportShadow), Text);
			DrawList->AddText(Position, Color, Text);
		}
	} // namespace

	auto DrawViewportPlayStateBorder(const ImVec2& ViewportMin, const ImVec2& ViewportMax, bool bPaused) -> void
	{
		DrawPlayStateBorder(ViewportMin, ViewportMax, bPaused);
	}

	auto FViewportToolbar::CalculateLayout(
		const FLevelEditorViewportClient* ViewportClient,
		const FLevelViewportEditModeManager* EditModeManager,
		const ImVec2& ViewportMin,
		const ImVec2& ViewportMax
	) const -> FViewportToolbarLayout
	{
		FViewportToolbarLayout Layout;
		Layout.ViewportMin = ViewportMin;
		Layout.ViewportMax = ViewportMax;
		if (ViewportClient != nullptr)
		{
			const FSceneViewSettings& Settings = ViewportClient->GetViewSettings();
			Layout.bEnableFXAA = Settings.bEnableFXAA;
			Layout.RenderMode = Settings.RenderMode;
			Layout.RasterMode = Settings.RasterMode;
		}

		Layout.ViewModeLabel = Layout.RasterMode == ERasterMode::Wireframe
			? GetModeLabel(Layout.RasterMode, RasterModeOptions)
			: GetModeLabel(Layout.RenderMode, RenderModeOptions);
		Layout.EditModeLabel = EditModeManager && !EditModeManager->GetActiveModeId().empty() ? std::string(EditModeManager->GetActiveModeId()) : "Select";
		const float AvailableWidth = ViewportMax.x - ViewportMin.x;
		const EEditorUILayoutMode LayoutMode = ResolveEditorUILayout(AvailableWidth, MonaImGui::ScaleUI(560.0f), MonaImGui::ScaleUI(980.0f));
		Layout.bCompact = LayoutMode != EEditorUILayoutMode::Full;
		Layout.bOverflow = LayoutMode == EEditorUILayoutMode::Narrow;
		const float FontSize = ImGui::GetFontSize();
		const float ContentPadding = MonaImGui::ScaleUI(10.0f);
		const float ContentGap = MonaImGui::ScaleUI(6.0f);
		const float TransformIconWidth = 16.0f * FontSize / 15.0f;
		const float ChevronWidth = 10.0f * FontSize / 15.0f;
		Layout.Height = FMath::Max(MonaImGui::ScaleUI(30.0f), FontSize + MonaImGui::ScaleUI(12.0f));
		Layout.Gap = MonaImGui::ScaleUI(10.0f);
		Layout.ToolButtonGap = MonaImGui::ScaleUI(3.0f);
		Layout.ModeButtonWidth = Layout.Height;
		Layout.EditModeButtonWidth = FMath::Max(MonaImGui::ScaleUI(78.0f), ImGui::CalcTextSize(Layout.EditModeLabel.c_str()).x + ContentGap + ChevronWidth + ContentPadding * 2.0f);
		Layout.SpaceButtonWidth = FMath::Max(MonaImGui::ScaleUI(82.0f), ImGui::CalcTextSize("Parent").x + ContentGap + ChevronWidth + ContentPadding * 2.0f);
		Layout.SnapButtonWidth = Layout.Height;
		Layout.DropDownWidth = FMath::Max(MonaImGui::ScaleUI(24.0f), Layout.Height * 0.8f);
		Layout.ViewModeButtonPosition = ImVec2(ViewportMin.x + MonaImGui::ScaleUI(10.0f), ViewportMin.y + MonaImGui::ScaleUI(8.0f));
		Layout.ViewModeButtonSize = ImVec2(ImGui::CalcTextSize(Layout.ViewModeLabel.c_str()).x + ContentGap + ChevronWidth + ContentPadding * 2.0f, Layout.Height);
		const float SecondaryWidth = Layout.bOverflow ? Layout.DropDownWidth : Layout.SpaceButtonWidth + Layout.SnapButtonWidth + Layout.DropDownWidth;
		const float ToolbarWidth = Layout.ViewModeButtonSize.x + Layout.Gap + Layout.EditModeButtonWidth + Layout.Gap + Layout.ModeButtonWidth * 3.0f + Layout.ToolButtonGap * 2.0f + Layout.Gap + SecondaryWidth + MonaImGui::ScaleUI(3.0f);
		Layout.BackgroundMin = ImVec2(Layout.ViewModeButtonPosition.x - MonaImGui::ScaleUI(3.0f), Layout.ViewModeButtonPosition.y - MonaImGui::ScaleUI(3.0f));
		Layout.BackgroundMax = ImVec2(FMath::Min(ViewportMax.x - MonaImGui::ScaleUI(6.0f), Layout.ViewModeButtonPosition.x + ToolbarWidth), Layout.ViewModeButtonPosition.y + Layout.Height + MonaImGui::ScaleUI(3.0f));
		const float PlayLabelWidth = ImGui::CalcTextSize("Play").x;
		Layout.PlayButtonWidth = FMath::Max(MonaImGui::ScaleUI(68.0f), TransformIconWidth + ContentGap + PlayLabelWidth + ContentPadding * 2.0f);
		Layout.RuntimeButtonWidth = Layout.Height;
		const bool bPlaying = GEditor && GEditor->IsPlaying();
		const float PlayControlsWidth = bPlaying ? Layout.RuntimeButtonWidth * 3.0f + Layout.DropDownWidth : Layout.PlayButtonWidth + Layout.DropDownWidth;
		// Keep Play visually independent from both the editing tools and the viewport edge.
		// The center anchor is preserved until responsive pressure requires it to clear the left group.
		const float CenteredPlayX = (ViewportMin.x + ViewportMax.x - PlayControlsWidth) * 0.5f;
		const float MinimumPlayX = Layout.BackgroundMax.x + MonaImGui::ScaleUI(10.0f);
		const float MaximumPlayX = ViewportMax.x - MonaImGui::ScaleUI(10.0f) - PlayControlsWidth;
		const float ResolvedPlayX = MinimumPlayX <= MaximumPlayX ? FMath::Clamp(CenteredPlayX, MinimumPlayX, MaximumPlayX) : MaximumPlayX;
		Layout.PlayButtonPosition = ImVec2(ResolvedPlayX, Layout.ViewModeButtonPosition.y);
		Layout.PlayBackgroundMin = ImVec2(Layout.PlayButtonPosition.x - MonaImGui::ScaleUI(3.0f), Layout.PlayButtonPosition.y - MonaImGui::ScaleUI(3.0f));
		Layout.PlayBackgroundMax = ImVec2(Layout.PlayButtonPosition.x + PlayControlsWidth + MonaImGui::ScaleUI(3.0f), Layout.PlayButtonPosition.y + Layout.Height + MonaImGui::ScaleUI(3.0f));
		return Layout;
	}

	auto FViewportToolbar::Draw(
		FLevelEditorContext& Context,
		FLevelEditorViewportClient* ViewportClient,
		FLevelViewportEditModeManager* EditModeManager,
		EEditorPlayStartLocation& PreferredPlayStartLocation,
		EEditorPlayDestination& PreferredPlayDestination,
		const FViewportToolbarLayout& Layout
	) const -> void
	{
		const ImVec2& ViewportMin = Layout.ViewportMin;
		const ImVec2& ViewportMax = Layout.ViewportMax;

		ImDrawList* DrawList = ImGui::GetWindowDrawList();
		DrawList->PushClipRect(ViewportMin, ViewportMax, true);
		DrawToolbarSurface(Layout.BackgroundMin, Layout.BackgroundMax);
		DrawToolbarSurface(Layout.PlayBackgroundMin, Layout.PlayBackgroundMax);
		const float ViewSeparatorX = Layout.ViewModeButtonPosition.x + Layout.ViewModeButtonSize.x + Layout.Gap * 0.5f;
		const float TransformSeparatorX = Layout.ViewModeButtonPosition.x + Layout.ViewModeButtonSize.x + Layout.Gap
			+ Layout.EditModeButtonWidth + Layout.Gap + Layout.ModeButtonWidth * 3.0f + Layout.ToolButtonGap * 2.0f + Layout.Gap * 0.5f;
		DrawToolbarSeparator(DrawList, ViewSeparatorX, Layout.ViewModeButtonPosition.y, Layout.Height);
		DrawToolbarSeparator(DrawList, TransformSeparatorX, Layout.ViewModeButtonPosition.y, Layout.Height);
		DrawList->PopClipRect();
		if (Context.bReadOnly) ImGui::BeginDisabled();

		if (DrawToolbarButton("##ViewModeButton", Layout.ViewModeButtonPosition, Layout.ViewModeButtonSize, Layout.ViewModeLabel.c_str(), EViewportToolbarIcon::ChevronDown, false, "Viewport shading and raster mode"))
		{
			ImGui::OpenPopup("ViewModePopup");
		}

		if (ImGui::BeginPopup("ViewModePopup"))
		{
			ImGui::TextDisabled("Shading");
			DrawModeOptions(Layout.RenderMode, RenderModeOptions, [ViewportClient](ERenderMode Mode) {
				if (ViewportClient == nullptr) return;
				FSceneViewSettings Settings = ViewportClient->GetViewSettings();
				Settings.RenderMode = Mode;
				ViewportClient->SetViewSettings(Settings);
			});
			ImGui::Separator();
			ImGui::TextDisabled("Rasterization");
			DrawModeOptions(Layout.RasterMode, RasterModeOptions, [ViewportClient](ERasterMode Mode) {
				if (ViewportClient == nullptr) return;
				FSceneViewSettings Settings = ViewportClient->GetViewSettings();
				Settings.RasterMode = Mode;
				ViewportClient->SetViewSettings(Settings);
			});
			ImGui::Separator();
			ImGui::TextDisabled("Post Processing");
			if (ViewportClient != nullptr)
			{
				bool bEnableFXAA = Layout.bEnableFXAA;
				if (ImGui::Checkbox("FXAA", &bEnableFXAA))
				{
					FSceneViewSettings Settings = ViewportClient->GetViewSettings();
					Settings.bEnableFXAA = bEnableFXAA;
					ViewportClient->SetViewSettings(Settings);
				}
			}
			ImGui::Separator();
			ImGui::TextDisabled("Overlays");
			if (ViewportClient != nullptr)
			{
				bool bShowGrid = ViewportClient->IsGridVisible();
				if (ImGui::Checkbox("World Grid", &bShowGrid)) ViewportClient->SetGridVisible(bShowGrid);
			}
			ImGui::EndPopup();
		}

		if (ViewportClient == nullptr)
		{
			if (Context.bReadOnly) ImGui::EndDisabled();
			return;
		}
		FTransformGizmo& Gizmo = ViewportClient->GetTransformGizmo();
		bool bOpenSnapSettings = false;
		float X = Layout.ViewModeButtonPosition.x + Layout.ViewModeButtonSize.x + Layout.Gap;
		const float Y = Layout.ViewModeButtonPosition.y;
		auto ToolbarButton = [&](const char* Id, const char* Text, EViewportToolbarIcon Icon, float Width, bool bSelected, const char* Tooltip, auto&& Action) {
			if (DrawToolbarButton(Id, ImVec2(X, Y), ImVec2(Width, Layout.Height), Text, Icon, bSelected, Tooltip)) Action();
			X += Width;
		};
		if (DrawToolbarButton("##EditModeButton", ImVec2(X, Y), ImVec2(Layout.EditModeButtonWidth, Layout.Height), Layout.EditModeLabel.c_str(), EViewportToolbarIcon::ChevronDown, true, "Viewport editing mode"))
			ImGui::OpenPopup("ViewportEditModePopup");
		if (ImGui::BeginPopup("ViewportEditModePopup"))
		{
			if (EditModeManager)
			{
				for (const FLevelViewportEditModeDescriptor* Descriptor : FLevelViewportEditModeRegistry::Get().GetAvailable(Context))
					if (ImGui::MenuItem(Descriptor->DisplayName.c_str(), nullptr, EditModeManager->GetActiveModeId() == Descriptor->Id)) EditModeManager->Activate(Descriptor->Id, Context);
			}
			ImGui::EndPopup();
		}
		X += Layout.EditModeButtonWidth + Layout.Gap;
		auto SpaceLabel = [](ETransformGizmoSpace Space) {
			switch (Space)
			{
			case ETransformGizmoSpace::Local: return "Local";
			case ETransformGizmoSpace::Parent: return "Parent";
			default: return "World";
			}
		};
		auto DrawTransformSpaceOptions = [&] {
			for (const ETransformGizmoSpace Space : {ETransformGizmoSpace::World, ETransformGizmoSpace::Local, ETransformGizmoSpace::Parent})
			{
				if (ImGui::MenuItem(SpaceLabel(Space), nullptr, Gizmo.GetSpace() == Space)) Gizmo.SetSpace(Space);
			}
			ImGui::Separator();
			ImGui::TextDisabled("Scale always uses Local space");
		};
		ToolbarButton("##MoveMode", nullptr, EViewportToolbarIcon::Translate, Layout.ModeButtonWidth, Gizmo.GetMode() == ETransformGizmoMode::Translate, "Move tool (W)", [&] { Gizmo.SetMode(ETransformGizmoMode::Translate); });
		X += Layout.ToolButtonGap;
		ToolbarButton("##RotateMode", nullptr, EViewportToolbarIcon::Rotate, Layout.ModeButtonWidth, Gizmo.GetMode() == ETransformGizmoMode::Rotate, "Rotate tool (E)", [&] { Gizmo.SetMode(ETransformGizmoMode::Rotate); });
		X += Layout.ToolButtonGap;
		ToolbarButton("##ScaleMode", nullptr, EViewportToolbarIcon::Scale, Layout.ModeButtonWidth, Gizmo.GetMode() == ETransformGizmoMode::Scale, "Scale tool (R)", [&] { Gizmo.SetMode(ETransformGizmoMode::Scale); });
		X += Layout.Gap;
		if (Layout.bOverflow)
		{
			ToolbarButton("##ViewportOverflow", "...", EViewportToolbarIcon::None, Layout.DropDownWidth, false, "More viewport tools", [&] { ImGui::OpenPopup("ViewportToolsOverflow"); });
			if (ImGui::BeginPopup("ViewportToolsOverflow"))
			{
				if (ImGui::BeginMenu("Transform Space"))
				{
					DrawTransformSpaceOptions();
					ImGui::EndMenu();
				}
				if (ImGui::MenuItem("Enable Snapping", nullptr, Gizmo.GetSnapSettings().bEnabled)) Gizmo.GetSnapSettings().bEnabled = !Gizmo.GetSnapSettings().bEnabled;
				if (ImGui::MenuItem("Snap Settings...")) bOpenSnapSettings = true;
				ImGui::EndPopup();
			}
		}
		else
		{
			ToolbarButton("##TransformSpace", SpaceLabel(Gizmo.GetSpace()), EViewportToolbarIcon::ChevronDown, Layout.SpaceButtonWidth, false, "Choose transform space; scale always uses Local", [&] { ImGui::OpenPopup("TransformSpacePopup"); });
			if (ImGui::BeginPopup("TransformSpacePopup"))
			{
				DrawTransformSpaceOptions();
				ImGui::EndPopup();
			}
			const FSplitButtonResult SnapResult = DrawSnapSplitButton(ImVec2(X, Y), Layout.SnapButtonWidth, Layout.DropDownWidth, Layout.Height, Gizmo.GetSnapSettings().bEnabled, ImGui::IsPopupOpen("GizmoSnapSettings"));
			if (SnapResult.bPrimaryPressed) Gizmo.GetSnapSettings().bEnabled = !Gizmo.GetSnapSettings().bEnabled;
			if (SnapResult.bSecondaryPressed) bOpenSnapSettings = true;
		}
		if (bOpenSnapSettings) ImGui::OpenPopup("GizmoSnapSettings");
		ImGui::SetNextWindowSize(ImVec2(MonaImGui::ScaleUI(270.0f), 0.0f), ImGuiCond_Appearing);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(MonaImGui::ScaleUI(12.0f), MonaImGui::ScaleUI(12.0f)));
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(MonaImGui::ScaleUI(8.0f), MonaImGui::ScaleUI(7.0f)));
		if (ImGui::BeginPopup("GizmoSnapSettings"))
		{
			FTransformGizmoSnapSettings& Settings = Gizmo.GetSnapSettings();
			ImGui::TextUnformatted("Snapping");
			ImGui::SameLine();
			ImGui::TextColored(Settings.bEnabled ? ImGui::GetStyleColorVec4(ImGuiCol_CheckMark) : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), Settings.bEnabled ? "Enabled" : "Disabled");
			ImGui::Separator();
			ImGui::Checkbox("Enable snapping", &Settings.bEnabled);
			ImGui::Spacing();
			ImGui::TextDisabled("Step size");
			ImGui::TextUnformatted("Move");
			ImGui::SetNextItemWidth(-FLT_MIN);
			ImGui::DragFloat("##TranslationSnap", &Settings.Translation, 0.05f, 0.001f, 10000.0f, "%.3f units");
			ImGui::TextUnformatted("Rotate");
			ImGui::SetNextItemWidth(-FLT_MIN);
			ImGui::DragFloat("##RotationSnap", &Settings.RotationDegrees, 1.0f, 0.1f, 180.0f, "%.1f deg");
			ImGui::TextUnformatted("Scale");
			ImGui::SetNextItemWidth(-FLT_MIN);
			ImGui::DragFloat("##ScaleSnap", &Settings.Scale, 0.01f, 0.001f, 10.0f, "%.3f");
			ImGui::Separator();
			ImGui::TextDisabled("Hold Ctrl for temporary snapping.");
			ImGui::EndPopup();
		}
		ImGui::PopStyleVar(2);
		if (Context.bReadOnly) ImGui::EndDisabled();

		const bool bPlaying = GEditor && GEditor->IsPlaying();
		const bool bPaused = bPlaying && GEditor->IsPlaySessionPaused();
		float PlayX = Layout.PlayButtonPosition.x;
		const float PlayY = Layout.PlayButtonPosition.y;
		if (!bPlaying)
		{
			const char* StartLabel = PreferredPlayStartLocation == EEditorPlayStartLocation::EditorCamera ? "Editor Camera" : "Level Start";
			const char* DestinationLabel = PreferredPlayDestination == EEditorPlayDestination::NewWindow ? "New Window" : "Viewport";
			const std::string PlayTooltip = std::format("Play from {} in {} (F5)", StartLabel, DestinationLabel);
			const FSplitButtonResult PlayResult = DrawPlaySplitButton(ImVec2(PlayX, PlayY), Layout.PlayButtonWidth, Layout.DropDownWidth, Layout.Height, "Play", PlayTooltip.c_str());
			if (PlayResult.bPrimaryPressed && Context.StartPlay)
				Context.StartPlay(PreferredPlayStartLocation, PreferredPlayDestination);
			if (PlayResult.bSecondaryPressed)
				ImGui::OpenPopup("ViewportPlayOptionsPopup");
			if (ImGui::BeginPopup("ViewportPlayOptionsPopup"))
			{
				ImGui::TextDisabled("Start Location");
				if (ImGui::RadioButton("Level Start", PreferredPlayStartLocation == EEditorPlayStartLocation::LevelStart)) PreferredPlayStartLocation = EEditorPlayStartLocation::LevelStart;
				if (ImGui::RadioButton("Editor Camera", PreferredPlayStartLocation == EEditorPlayStartLocation::EditorCamera)) PreferredPlayStartLocation = EEditorPlayStartLocation::EditorCamera;
				ImGui::Separator();
				ImGui::TextDisabled("Destination");
				if (ImGui::RadioButton("Embedded Viewport", PreferredPlayDestination == EEditorPlayDestination::EmbeddedViewport)) PreferredPlayDestination = EEditorPlayDestination::EmbeddedViewport;
				if (ImGui::RadioButton("New Window", PreferredPlayDestination == EEditorPlayDestination::NewWindow)) PreferredPlayDestination = EEditorPlayDestination::NewWindow;
				ImGui::Separator();
				ImGui::Checkbox("Simulate Physics", &Context.bSimulatePhysics);
				ImGui::Spacing();
				if (ImGui::Button("Play Now", ImVec2(ImGui::GetContentRegionAvail().x, 0.0f)))
				{
					ImGui::CloseCurrentPopup();
					if (Context.StartPlay) Context.StartPlay(PreferredPlayStartLocation, PreferredPlayDestination);
				}
				ImGui::EndPopup();
			}
		}
		else
		{
			const FRuntimeControlResult RuntimeResult = DrawRuntimeControlGroup(
				ImVec2(PlayX, PlayY), Layout.RuntimeButtonWidth, Layout.DropDownWidth, Layout.Height, bPaused);
			if (RuntimeResult.bStopPressed) GEditor->StopPlaySession();
			else
			{
				if (RuntimeResult.bPausePressed) GEditor->SetPlaySessionPaused(!bPaused);
				if (RuntimeResult.bStepPressed) GEditor->StepPlaySession();
				if (RuntimeResult.bOptionsPressed) ImGui::OpenPopup("ViewportRuntimeOptionsPopup");
			}
			if (ImGui::BeginPopup("ViewportRuntimeOptionsPopup"))
			{
				const bool bHasSelection = !Context.GetSelectedActors().empty();
				if (ImGui::MenuItem("Apply Selected Runtime Changes", nullptr, false, bHasSelection) && Context.ApplyPlayChanges) Context.ApplyPlayChanges(true);
				if (ImGui::MenuItem("Apply All Runtime Changes") && Context.ApplyPlayChanges) Context.ApplyPlayChanges(false);
				ImGui::Separator();
				bool bPhysicsEnabled = GEditor->GetPlayWorld() && GEditor->GetPlayWorld()->IsPhysicsSimulationEnabled();
				if (ImGui::Checkbox("Simulate Physics", &bPhysicsEnabled) && GEditor->GetPlayWorld()) GEditor->GetPlayWorld()->SetPhysicsSimulationEnabled(bPhysicsEnabled);
				ImGui::EndPopup();
			}
		}
	}

	auto DrawViewportOrientationOverlay(const FLevelEditorViewportClient* ViewportClient, const ImVec2& ViewportMin, const ImVec2& ViewportMax) -> void
	{
		const ImVec2 ViewportSize(ViewportMax.x - ViewportMin.x, ViewportMax.y - ViewportMin.y);
		if (ViewportSize.x <= 8.0f || ViewportSize.y <= 8.0f)
		{
			return;
		}

		std::array<ImVec2, 3> AxisDirections = {ImVec2(1.0f, 0.0f), ImVec2(-0.62f, 0.38f), ImVec2(0.0f, -1.0f)};
		if (ViewportClient != nullptr)
		{
			const FMatrix ViewMatrix = ViewportClient->GetViewMatrix();
			AxisDirections[0] = GetScreenAxisDirection(ViewMatrix, FVectorConstants::Forward, AxisDirections[0]);
			AxisDirections[1] = GetScreenAxisDirection(ViewMatrix, FVectorConstants::Right, AxisDirections[1]);
			AxisDirections[2] = GetScreenAxisDirection(ViewMatrix, FVectorConstants::Up, AxisDirections[2]);
		}

		const float AxisLength = FMath::Max(MonaImGui::ScaleUI(22.0f), FMath::Min(MonaImGui::ScaleUI(34.0f), FMath::Min(ViewportSize.x, ViewportSize.y) * 0.08f));
		const ImVec2 Origin(ViewportMin.x + AxisLength + MonaImGui::ScaleUI(18.0f), ViewportMax.y - AxisLength - MonaImGui::ScaleUI(18.0f));
		ImDrawList* DrawList = ImGui::GetWindowDrawList();
		DrawList->PushClipRect(ViewportMin, ViewportMax, true);
		DrawList->AddCircleFilled(Origin, MonaImGui::ScaleUI(3.0f), MonaImGui::GetThemeColorU32(MonaImGui::EUIThemeColor::ViewportText));

		const std::array<ImU32, 3> AxisColors = {
			MonaImGui::GetThemeColorU32(MonaImGui::EUIThemeColor::AxisX),
			MonaImGui::GetThemeColorU32(MonaImGui::EUIThemeColor::AxisY),
			MonaImGui::GetThemeColorU32(MonaImGui::EUIThemeColor::AxisZ)
		};
		const std::array<const char*, 3> AxisLabels = {"X", "Y", "Z"};
		for (uint32 AxisIndex = 0; AxisIndex < 3; ++AxisIndex)
		{
			const ImVec2 End = Add(Origin, Mul(AxisDirections[AxisIndex], AxisLength));
			DrawList->AddLine(Origin, End, MonaImGui::GetThemeColorU32(MonaImGui::EUIThemeColor::ViewportShadow), MonaImGui::ScaleUI(4.0f));
			DrawList->AddLine(Origin, End, AxisColors[AxisIndex], MonaImGui::ScaleUI(2.0f));
			const ImVec2 TextSize = ImGui::CalcTextSize(AxisLabels[AxisIndex]);
			DrawAxisText(DrawList, Add(End, Add(Mul(AxisDirections[AxisIndex], MonaImGui::ScaleUI(5.0f)), ImVec2(-TextSize.x * 0.5f, -TextSize.y * 0.5f))), AxisColors[AxisIndex], AxisLabels[AxisIndex]);
		}
		DrawList->PopClipRect();
	}

	auto DrawViewportFPSOverlay(const ImVec2& ViewportMin, const ImVec2& ViewportMax) -> void
	{
		char FpsText[32];
		snprintf(FpsText, sizeof(FpsText), "%.0f FPS", ImGui::GetIO().Framerate);
		const ImVec2 TextSize = ImGui::CalcTextSize(FpsText);
		const ImVec2 Padding(MonaImGui::ScaleUI(7.0f), MonaImGui::ScaleUI(3.0f));
		const ImVec2 BadgeMax(ViewportMax.x - MonaImGui::ScaleUI(10.0f), ViewportMin.y + MonaImGui::ScaleUI(8.0f) + TextSize.y + Padding.y * 2.0f);
		const ImVec2 BadgeMin(BadgeMax.x - TextSize.x - Padding.x * 2.0f, ViewportMin.y + MonaImGui::ScaleUI(8.0f));
		ImDrawList* DrawList = ImGui::GetWindowDrawList();
		DrawList->PushClipRect(ViewportMin, ViewportMax, true);
		ImVec4 BadgeColor = ImGui::GetStyleColorVec4(ImGuiCol_PopupBg);
		BadgeColor.w = 0.72f;
		DrawList->AddRectFilled(BadgeMin, BadgeMax, ImGui::GetColorU32(BadgeColor), MonaImGui::ScaleUI(5.0f));
		DrawList->AddRect(BadgeMin, BadgeMax, ImGui::GetColorU32(ImGuiCol_Border), MonaImGui::ScaleUI(5.0f));
		DrawList->AddText(Add(BadgeMin, Padding), MonaImGui::GetThemeColorU32(MonaImGui::EUIThemeColor::ViewportText), FpsText);
		DrawList->PopClipRect();
	}
} // namespace Durin
