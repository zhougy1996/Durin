#include "Panels/SceneViewportPanel.h"

#include "AssetSystem.h"
#include "Components/CameraComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Assets/ContentBrowserDragDrop.h"
#include "Editor/EditorEngine.h"
#include "Editor/EditorTransaction.h"
#include "Editor/EditorWorkspaceUI.h"
#include "Engine/Engine.h"
#include "Engine/Actor.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "Actors/StaticMeshActor.h"
#include "IRendererModule.h"
#include "Workspace/LevelEditorContext.h"
#include "Workspace/LevelEditorUILayout.h"
#include "Workspace/LevelEditorWorkspace.h"
#include "Math/Vector.h"
#include "Mona/SceneViewport.h"
#include "MonaImGui.h"
#include "SceneViewProjection.h"
#include "Viewport/LevelEditorViewportClient.h"
#include "Viewport/CameraPreviewViewportClient.h"
#include "Widgets/MViewport.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshResources.h"

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
			ImVec4 ToolbarColor = Style.Colors[ImGuiCol_PopupBg];
			ToolbarColor.w = 0.94f;
			DrawList->AddRectFilled(Min, Max, ImGui::GetColorU32(ToolbarColor), MonaImGui::ScaleUI(6.0f));
			DrawList->AddRect(Min, Max, ImGui::GetColorU32(ImGuiCol_Border), MonaImGui::ScaleUI(6.0f));
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

		auto DrawToolbarSelectionIndicator(ImDrawList* DrawList, const ImVec2& Position, float Width, float Height, float ContentWidth) -> void
		{
			// Tie the active marker to the visible content instead of a fixed pixel width so text,
			// icon-only, and split buttons retain the same visual weight.
			const float MaxIndicatorWidth = FMath::Max(0.0f, Width - MonaImGui::ScaleUI(12.0f));
			const float MinIndicatorWidth = FMath::Min(MonaImGui::ScaleUI(22.0f), MaxIndicatorWidth);
			const float IndicatorWidth = FMath::Clamp(ContentWidth + MonaImGui::ScaleUI(4.0f), MinIndicatorWidth, MaxIndicatorWidth);
			const float CenterX = Position.x + Width * 0.5f;
			const float Bottom = Position.y + Height;
			DrawList->AddRectFilled(ImVec2(CenterX - IndicatorWidth * 0.5f, Bottom - MonaImGui::ScaleUI(2.0f)), ImVec2(CenterX + IndicatorWidth * 0.5f, Bottom), ImGui::GetColorU32(ImGuiCol_CheckMark), MonaImGui::ScaleUI(1.0f));
		}

		auto DrawToolbarButton(const char* Id, const ImVec2& Position, const ImVec2& Size, const char* Label, EViewportToolbarIcon Icon, bool bSelected, const char* Tooltip, bool bButtonSurface = false, bool bSuccessIcon = false) -> bool
		{
			ImGui::SetCursorScreenPos(Position);
			const bool bPressed = ImGui::InvisibleButton(Id, Size);
			const bool bHovered = ImGui::IsItemHovered();
			const bool bHeld = ImGui::IsItemActive();
			const ImGuiStyle& Style = ImGui::GetStyle();
			ImDrawList* DrawList = ImGui::GetWindowDrawList();
			const ImVec2 Max(Position.x + Size.x, Position.y + Size.y);
			if (bSelected || bButtonSurface || bHovered || bHeld)
			{
				const ImGuiCol Background = bSelected ? ImGuiCol_HeaderActive : bHeld ? ImGuiCol_ButtonActive :
															bHovered ? ImGuiCol_ButtonHovered : ImGuiCol_Button;
				DrawList->AddRectFilled(Position, Max, ImGui::GetColorU32(Style.Colors[Background]), Style.FrameRounding);
			}

			const ImU32 TextColor = ImGui::GetColorU32(bSelected || bButtonSurface || bHovered ? ImGuiCol_Text : ImGuiCol_TextDisabled);
			// Play is an action, not a persistent selection: keep the standard button surface
			// and carry its meaning with the semantic success color on the icon alone.
			const ImU32 IconColor = bSuccessIcon ? MonaImGui::GetThemeColorU32(MonaImGui::EUIThemeColor::Success) : TextColor;
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
			if (bSelected)
			{
				DrawToolbarSelectionIndicator(DrawList, Position, Size.x, Size.y, ContentWidth);
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
			const ImGuiStyle& Style = ImGui::GetStyle();
			ImDrawList* DrawList = ImGui::GetWindowDrawList();
			const ImVec2 Max(Position.x + PrimaryWidth + SecondaryWidth, Position.y + Height);
			if (bEnabled)
			{
				DrawList->AddRectFilled(Position, ImVec2(Position.x + PrimaryWidth, Max.y), ImGui::GetColorU32(ImGuiCol_HeaderActive), Style.FrameRounding, ImDrawFlags_RoundCornersLeft);
			}
			if (bPopupOpen)
			{
				DrawList->AddRectFilled(ImVec2(Position.x + PrimaryWidth, Position.y), Max, ImGui::GetColorU32(ImGuiCol_HeaderActive), Style.FrameRounding, ImDrawFlags_RoundCornersRight);
			}

			ImGui::SetCursorScreenPos(Position);
			const bool bPrimaryPressed = ImGui::InvisibleButton("##SnapToggle", ImVec2(PrimaryWidth, Height));
			const bool bPrimaryHovered = ImGui::IsItemHovered();
			const bool bPrimaryHeld = ImGui::IsItemActive();
			if (bPrimaryHovered || bPrimaryHeld)
			{
				DrawList->AddRectFilled(Position, ImVec2(Position.x + PrimaryWidth, Max.y), ImGui::GetColorU32(bPrimaryHeld ? ImGuiCol_ButtonActive : ImGuiCol_ButtonHovered), Style.FrameRounding, ImDrawFlags_RoundCornersLeft);
			}

			const ImVec2 SecondaryPosition(Position.x + PrimaryWidth, Position.y);
			ImGui::SetCursorScreenPos(SecondaryPosition);
			const bool bSecondaryPressed = ImGui::InvisibleButton("##SnapSettingsButton", ImVec2(SecondaryWidth, Height));
			const bool bSecondaryHovered = ImGui::IsItemHovered();
			const bool bSecondaryHeld = ImGui::IsItemActive();
			if (bSecondaryHovered || bSecondaryHeld)
			{
				DrawList->AddRectFilled(SecondaryPosition, Max, ImGui::GetColorU32(bSecondaryHeld ? ImGuiCol_ButtonActive : ImGuiCol_ButtonHovered), Style.FrameRounding, ImDrawFlags_RoundCornersRight);
			}

			DrawList->AddLine(ImVec2(SecondaryPosition.x, Position.y + MonaImGui::ScaleUI(5.0f)), ImVec2(SecondaryPosition.x, Max.y - MonaImGui::ScaleUI(5.0f)), ImGui::GetColorU32(ImGuiCol_Border));
			const ImU32 TextColor = ImGui::GetColorU32(bEnabled || bPopupOpen || bPrimaryHovered || bSecondaryHovered ? ImGuiCol_Text : ImGuiCol_TextDisabled);
			const float IconScale = ImGui::GetFontSize() / 15.0f;
			DrawToolbarIcon(DrawList, EViewportToolbarIcon::SnapGrid, ImVec2(Position.x + PrimaryWidth * 0.5f, Position.y + Height * 0.5f), TextColor, IconScale);
			DrawToolbarIcon(DrawList, EViewportToolbarIcon::ChevronDown, ImVec2(SecondaryPosition.x + SecondaryWidth * 0.5f, Position.y + Height * 0.5f), TextColor, IconScale);
			if (bEnabled)
			{
				DrawToolbarSelectionIndicator(DrawList, Position, PrimaryWidth, Height, 16.0f * IconScale);
			}
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

	// Retains responsive toolbar group placement for the current viewport size.
	struct FViewportToolbarLayout
	{
		IRendererModule* RendererModule = nullptr;
		ERenderMode RenderMode = ERenderMode::Lit;
		ERasterMode RasterMode = ERasterMode::Solid;
		std::string ViewModeLabel;
		ImVec2 ViewportMin;
		ImVec2 ViewportMax;
		ImVec2 BackgroundMin;
		ImVec2 BackgroundMax;
		ImVec2 PlayBackgroundMin;
		ImVec2 PlayBackgroundMax;
		ImVec2 ViewModeButtonPosition;
		ImVec2 ViewModeButtonSize;
		ImVec2 PlayButtonPosition;
		float Height = 0.0f;
		float Gap = 0.0f;
		float ToolButtonGap = 0.0f;
		float ModeButtonWidth = 0.0f;
		float SpaceButtonWidth = 0.0f;
		float SnapButtonWidth = 0.0f;
		float DropDownWidth = 0.0f;
		float PlayButtonWidth = 0.0f;
		float RuntimeButtonWidth = 0.0f;
		bool bCompact = false;
		bool bOverflow = false;
	};

	FSceneViewportPanel::FSceneViewportPanel()
		: PreferredPlayStartLocation(EEditorPlayStartLocation::LevelStart)
		, PreferredPlayDestination(EEditorPlayDestination::EmbeddedViewport)
	{
		ViewportClient = std::make_unique<FLevelEditorViewportClient>();
		ViewportWidget = std::make_shared<MViewport>();
		const std::shared_ptr<FSceneViewport> SceneViewport = std::make_shared<FSceneViewport>(ViewportClient.get(), ViewportWidget);
		ViewportWidget->SetViewportInterface(SceneViewport);
		if (GEngine != nullptr)
		{
			GEngine->SetMainSceneViewport(SceneViewport);
		}

		CameraPreviewViewportClient = std::make_unique<FCameraPreviewViewportClient>();
		CameraPreviewViewportWidget = std::make_shared<MViewport>();
		CameraPreviewSceneViewport = std::make_shared<FSceneViewport>(CameraPreviewViewportClient.get(), CameraPreviewViewportWidget);
		CameraPreviewViewportWidget->SetViewportInterface(CameraPreviewSceneViewport);
		if (GEngine != nullptr) GEngine->RegisterAuxiliarySceneViewport(CameraPreviewSceneViewport);
	}

	FSceneViewportPanel::~FSceneViewportPanel()
	{
		if (GEngine != nullptr) GEngine->UnregisterAuxiliarySceneViewport(CameraPreviewSceneViewport.get());
		CameraPreviewSceneViewport.reset();
		if (GEngine != nullptr) GEngine->SetMainSceneViewport(nullptr);
	}

	auto FSceneViewportPanel::CaptureCameraState(DLevel* Level, FLevelViewportCameraState& OutState) const -> bool
	{
		if (ViewportClient == nullptr || Level == nullptr || ViewportClient->GetCurrentLevel() != Level) return false;
		OutState = ViewportClient->GetCameraTransform().GetState();
		return true;
	}

	auto FSceneViewportPanel::RestoreCameraState(DLevel* Level, const FLevelViewportCameraState* State) -> void
	{
		if (ViewportClient != nullptr) ViewportClient->InitializeForLevel(Level, State);
	}

	auto FSceneViewportPanel::SetPreferredPlayMode(EEditorPlayStartLocation StartLocation, EEditorPlayDestination Destination) -> void
	{
		PreferredPlayStartLocation = StartLocation;
		PreferredPlayDestination = Destination;
	}

	auto FSceneViewportPanel::GetTransformGizmo() -> FTransformGizmo*
	{
		return ViewportClient ? &ViewportClient->GetTransformGizmo() : nullptr;
	}

	auto FSceneViewportPanel::GetTransformGizmo() const -> const FTransformGizmo*
	{
		return ViewportClient ? &ViewportClient->GetTransformGizmo() : nullptr;
	}

	auto FSceneViewportPanel::IsGridVisible() const -> bool
	{
		return ViewportClient != nullptr && ViewportClient->IsGridVisible();
	}

	auto FSceneViewportPanel::SetGridVisible(bool bVisible) -> void
	{
		if (ViewportClient != nullptr) ViewportClient->SetGridVisible(bVisible);
	}

	auto FSceneViewportPanel::FocusActor(const AActor* Actor) -> void
	{
		if (ViewportClient != nullptr) ViewportClient->FocusActor(Actor);
	}

	auto FSceneViewportPanel::Draw(FLevelEditorContext& Context) -> void
	{
		const bool bPlayingInNewWindow = GEditor && GEditor->IsPlayingInNewWindow();
		if (!EditorWorkspaceUI::BeginDockablePanel(
			LevelEditorWorkspace::Type,
			"Scene Viewport",
			"SceneViewport",
			GetOpenPtr(),
			ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
		))
		{
			if (GEngine && !bPlayingInNewWindow) GEngine->SetGameInputEnabled(false);
			if (ViewportClient != nullptr) ViewportClient->ResetNavigation();
			if (CameraPreviewViewportClient != nullptr) CameraPreviewViewportClient->SetCamera(nullptr);
			bViewportHovered = false;
			bViewportFocused = false;
			ImGui::End();
			return;
		}

		if (Context.Level == nullptr)
		{
			if (GEngine && !bPlayingInNewWindow) GEngine->SetGameInputEnabled(false);
			if (ViewportClient != nullptr) ViewportClient->ResetNavigation();
			if (CameraPreviewViewportClient != nullptr) CameraPreviewViewportClient->SetCamera(nullptr);
			bViewportHovered = false;
			bViewportFocused = false;
			ImGui::TextDisabled("No level is open. Open a Level asset from Content Browser.");
			ImGui::End();
			return;
		}
		UpdateViewportSize();
		if (ViewportClient != nullptr) ViewportClient->SetSelectedActors(Context.GetSelectedActors(), Context.GetPrimarySelectedActor());
		UpdateCameraPreview(Context);
		if (ViewportWidget != nullptr)
		{
			ViewportWidget->Draw();
			if (ViewportWidget->WasTextureDrawn())
			{
				const ImVec2 VpMin = ImGui::GetItemRectMin();
				const ImVec2 VpMax = ImGui::GetItemRectMax();
				if (!Context.bReadOnly && ImGui::BeginDragDropTarget())
				{
					if (const ImGuiPayload* Payload = ImGui::AcceptDragDropPayload(ContentBrowserAssetPayloadType); Payload && Payload->IsDelivery() && Payload->DataSize == sizeof(FContentBrowserAssetPayload))
					{
						const auto* AssetPayload = static_cast<const FContentBrowserAssetPayload*>(Payload->Data);
						FAssetPath AssetPath;
						DStaticMesh* Mesh = nullptr;
						if (!FAssetPath::TryCreate(AssetPayload->AssetPath.data(), AssetPath))
							Context.SetError("Dropped asset path is invalid.");
						else if (const Asset::FAssetResult Result = Asset::LoadAsset(AssetPath, Mesh); !Result)
							Context.SetError(Result.Message);
						else if (AStaticMeshActor* Actor = Context.Level->SpawnActor<AStaticMeshActor>(FName(AssetPath.GetAssetName())))
						{
							Actor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
							FSceneView View;
							ViewportClient->CalcSceneView(static_cast<uint32>(FMath::Max(1.0f, VpMax.x - VpMin.x)), static_cast<uint32>(FMath::Max(1.0f, VpMax.y - VpMin.y)), View);
							const ImVec2 Mouse = ImGui::GetMousePos();
							FVector3 Origin, Direction;
							if (SceneViewProjection::BuildViewportRay(View, {Mouse.x - VpMin.x, Mouse.y - VpMin.y}, Origin, Direction) && Actor->GetRootComponent())
								Actor->GetRootComponent()->SetWorldLocation(Origin + Direction * 5.0);
							Context.SelectActor(Actor);
						}
					}
					ImGui::EndDragDropTarget();
				}
				const FViewportToolbarLayout ToolbarLayout = CalculateToolbarLayout(VpMin, VpMax);
				bViewportHovered = ImGui::IsItemHovered();
				const bool bNavigationMousePressed = ImGui::IsMouseClicked(ImGuiMouseButton_Right) || ImGui::IsMouseClicked(ImGuiMouseButton_Middle) || (ImGui::GetIO().KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left));
				if (bViewportHovered && bNavigationMousePressed)
				{
					ImGui::SetWindowFocus();
				}
				bViewportFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
				if (GEngine && !bPlayingInNewWindow) GEngine->SetGameInputEnabled(Context.bReadOnly && bViewportFocused);
				if (Context.bReadOnly)
				{
					if (ViewportClient) ViewportClient->ResetNavigation();
					ViewportClient->SetSelectedActors({}, nullptr);
				}
				else UpdateViewportInput(Context, ToolbarLayout);
				if (GEditor && GEditor->IsPlaying())
				{
					DrawPlayStateBorder(VpMin, VpMax, GEditor->IsPlaySessionPaused());
				}
				DrawToolbar(Context, ToolbarLayout);
				DrawCameraPreview(VpMin, VpMax);
				DrawOrientationOverlay(VpMin, VpMax);
				DrawFPSOverlay(VpMin, VpMax);
				if (Context.bReadOnly)
				{
					ImDrawList* DrawList = ImGui::GetWindowDrawList();
					const char* Status = bPlayingInNewWindow ? "PLAYING IN NEW WINDOW" : GEditor && GEditor->IsPlaySessionPaused() ? "PLAY PAUSED" : "PLAYING";
					const ImVec2 TextSize = ImGui::CalcTextSize(Status);
					const ImVec2 Padding(MonaImGui::ScaleUI(8.0f), MonaImGui::ScaleUI(4.0f));
					const ImVec2 BadgeSize(TextSize.x + Padding.x * 2.0f, TextSize.y + Padding.y * 2.0f);
					const ImVec2 BadgeMin((VpMin.x + VpMax.x - BadgeSize.x) * 0.5f, VpMax.y - BadgeSize.y - MonaImGui::ScaleUI(10.0f));
					const ImVec2 BadgeMax = Add(BadgeMin, BadgeSize);
					ImVec4 BadgeColor = ImGui::GetStyleColorVec4(ImGuiCol_PopupBg);
					BadgeColor.w = 0.88f;
					DrawList->AddRectFilled(BadgeMin, BadgeMax, ImGui::GetColorU32(BadgeColor), BadgeMax.y - BadgeMin.y);
					DrawList->AddRect(BadgeMin, BadgeMax, ImGui::GetColorU32(ImGuiCol_CheckMark), BadgeMax.y - BadgeMin.y);
					DrawList->AddText(Add(BadgeMin, Padding), ImGui::GetColorU32(ImGuiCol_CheckMark), Status);
				}
			}
		}
		if (ViewportWidget == nullptr || !ViewportWidget->WasTextureDrawn())
		{
			if (ViewportClient != nullptr) ViewportClient->ResetNavigation();
			bViewportHovered = false;
			bViewportFocused = false;
			ImGui::TextDisabled("Viewport initializing...");
		}
		ImGui::End();
	}

	auto FSceneViewportPanel::CalculateToolbarLayout(const ImVec2& ViewportMin, const ImVec2& ViewportMax) const -> FViewportToolbarLayout
	{
		FViewportToolbarLayout Layout;
		Layout.ViewportMin = ViewportMin;
		Layout.ViewportMax = ViewportMax;
		if (GEngine != nullptr)
		{
			Layout.RendererModule = GEngine->GetRendererModule();
			if (Layout.RendererModule != nullptr)
			{
				Layout.RenderMode = Layout.RendererModule->GetRenderMode();
				Layout.RasterMode = Layout.RendererModule->GetRasterMode();
			}
		}

		Layout.ViewModeLabel = std::format("{} / {}", GetModeLabel(Layout.RenderMode, RenderModeOptions), GetModeLabel(Layout.RasterMode, RasterModeOptions));
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
		Layout.Gap = MonaImGui::ScaleUI(6.0f);
		Layout.ToolButtonGap = MonaImGui::ScaleUI(3.0f);
		Layout.ModeButtonWidth = Layout.Height;
		Layout.SpaceButtonWidth = FMath::Max(MonaImGui::ScaleUI(82.0f), ImGui::CalcTextSize("Parent").x + ContentGap + ChevronWidth + ContentPadding * 2.0f);
		Layout.SnapButtonWidth = Layout.Height;
		Layout.DropDownWidth = FMath::Max(MonaImGui::ScaleUI(24.0f), Layout.Height * 0.8f);
		Layout.ViewModeButtonPosition = ImVec2(ViewportMin.x + MonaImGui::ScaleUI(10.0f), ViewportMin.y + MonaImGui::ScaleUI(8.0f));
		Layout.ViewModeButtonSize = ImVec2(ImGui::CalcTextSize(Layout.ViewModeLabel.c_str()).x + ContentGap + ChevronWidth + ContentPadding * 2.0f, Layout.Height);
		const float SecondaryWidth = Layout.bOverflow ? Layout.DropDownWidth : Layout.SpaceButtonWidth + Layout.SnapButtonWidth + Layout.DropDownWidth;
		const float ToolbarWidth = Layout.ViewModeButtonSize.x + Layout.Gap + Layout.ModeButtonWidth * 3.0f + Layout.ToolButtonGap * 2.0f + Layout.Gap + SecondaryWidth + MonaImGui::ScaleUI(10.0f);
		Layout.BackgroundMin = ImVec2(Layout.ViewModeButtonPosition.x - MonaImGui::ScaleUI(4.0f), Layout.ViewModeButtonPosition.y - MonaImGui::ScaleUI(4.0f));
		Layout.BackgroundMax = ImVec2(FMath::Min(ViewportMax.x - MonaImGui::ScaleUI(6.0f), Layout.ViewModeButtonPosition.x + ToolbarWidth), Layout.ViewModeButtonPosition.y + Layout.Height + MonaImGui::ScaleUI(4.0f));
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
		Layout.PlayBackgroundMin = ImVec2(Layout.PlayButtonPosition.x - MonaImGui::ScaleUI(4.0f), Layout.PlayButtonPosition.y - MonaImGui::ScaleUI(4.0f));
		Layout.PlayBackgroundMax = ImVec2(Layout.PlayButtonPosition.x + PlayControlsWidth + MonaImGui::ScaleUI(4.0f), Layout.PlayButtonPosition.y + Layout.Height + MonaImGui::ScaleUI(4.0f));
		return Layout;
	}

	auto FSceneViewportPanel::DrawToolbar(FLevelEditorContext& Context, const FViewportToolbarLayout& Layout) -> void
	{
		const ImVec2& ViewportMin = Layout.ViewportMin;
		const ImVec2& ViewportMax = Layout.ViewportMax;

		ImDrawList* DrawList = ImGui::GetWindowDrawList();
		DrawList->PushClipRect(ViewportMin, ViewportMax, true);
		DrawToolbarSurface(Layout.BackgroundMin, Layout.BackgroundMax);
		DrawToolbarSurface(Layout.PlayBackgroundMin, Layout.PlayBackgroundMax);
		DrawList->PopClipRect();
		if (Context.bReadOnly) ImGui::BeginDisabled();

		if (DrawToolbarButton("##ViewModeButton", Layout.ViewModeButtonPosition, Layout.ViewModeButtonSize, Layout.ViewModeLabel.c_str(), EViewportToolbarIcon::ChevronDown, false, "Viewport shading and raster mode"))
		{
			ImGui::OpenPopup("ViewModePopup");
		}

		if (ImGui::BeginPopup("ViewModePopup"))
		{
			ImGui::TextDisabled("Shading");
			DrawModeOptions(Layout.RenderMode, RenderModeOptions, [RendererModule = Layout.RendererModule](ERenderMode Mode) {
				if (RendererModule != nullptr) RendererModule->SetRenderMode(Mode);
			});
			ImGui::Separator();
			ImGui::TextDisabled("Rasterization");
			DrawModeOptions(Layout.RasterMode, RasterModeOptions, [RendererModule = Layout.RendererModule](ERasterMode Mode) {
				if (RendererModule != nullptr) RendererModule->SetRasterMode(Mode);
			});
			ImGui::Separator();
			ImGui::TextDisabled("Post Processing");
			if (Layout.RendererModule != nullptr)
			{
				bool bEnableFXAA = Layout.RendererModule->IsFXAAEnabled();
				if (ImGui::Checkbox("FXAA", &bEnableFXAA)) Layout.RendererModule->SetFXAAEnabled(bEnableFXAA);
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

	auto FSceneViewportPanel::DrawOrientationOverlay(const ImVec2& ViewportMin, const ImVec2& ViewportMax) const -> void
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

	auto FSceneViewportPanel::UpdateCameraPreview(FLevelEditorContext& Context) -> void
	{
		DCameraComponent* Camera = nullptr;
		if (!Context.bReadOnly && !(GEditor && GEditor->IsPlaying()))
		{
			if (AActor* SelectedActor = Context.GetPrimarySelectedActor())
			{
				Camera = SelectedActor->FindComponentByClass<DCameraComponent>();
			}
		}
		if (CameraPreviewViewportClient != nullptr) CameraPreviewViewportClient->SetCamera(Camera);
	}

	auto FSceneViewportPanel::DrawCameraPreview(const ImVec2& ViewportMin, const ImVec2& ViewportMax) -> void
	{
		if (CameraPreviewViewportClient == nullptr || CameraPreviewViewportClient->GetCamera() == nullptr || CameraPreviewViewportWidget == nullptr) return;

		const float ViewportWidth = ViewportMax.x - ViewportMin.x;
		const float ViewportHeight = ViewportMax.y - ViewportMin.y;
		const float Padding = MonaImGui::ScaleUI(12.0f);
		const float HeaderHeight = MonaImGui::ScaleUI(24.0f);
		const DCameraComponent* Camera = CameraPreviewViewportClient->GetCamera();
		const float MainViewportAspectRatio = ViewportHeight > 0.0f ? ViewportWidth / ViewportHeight : 16.0f / 9.0f;
		const float PreviewAspectRatio = Camera->ResolveAspectRatio(MainViewportAspectRatio);
		const float PreviewWidth = FMath::Clamp(ViewportWidth * 0.28f, MonaImGui::ScaleUI(220.0f), MonaImGui::ScaleUI(360.0f));
		const float PreviewHeight = PreviewWidth / PreviewAspectRatio;
		if (PreviewWidth + Padding * 2.0f > ViewportWidth || PreviewHeight + HeaderHeight + Padding * 2.0f > ViewportHeight) return;

		const ImVec2 ImageMin(ViewportMax.x - Padding - PreviewWidth, ViewportMax.y - Padding - PreviewHeight);
		const ImVec2 ImageMax(ImageMin.x + PreviewWidth, ImageMin.y + PreviewHeight);
		const ImVec2 HeaderMin(ImageMin.x, ImageMin.y - HeaderHeight);
		const ImVec2 HeaderMax(ImageMax.x, ImageMin.y);
		const ImVec2 SavedCursor = ImGui::GetCursorScreenPos();
		CameraPreviewViewportWidget->SetDesiredSize({PreviewWidth, PreviewHeight});
		ImGui::SetCursorScreenPos(ImageMin);
		ImGui::PushClipRect(ViewportMin, ViewportMax, true);
		CameraPreviewViewportWidget->Draw();
		ImGui::PopClipRect();
		ImGui::SetCursorScreenPos(SavedCursor);

		ImDrawList* DrawList = ImGui::GetWindowDrawList();
		ImVec4 HeaderColor = ImGui::GetStyleColorVec4(ImGuiCol_PopupBg);
		HeaderColor.w = 0.94f;
		DrawList->AddRectFilled(HeaderMin, HeaderMax, ImGui::GetColorU32(HeaderColor), MonaImGui::ScaleUI(5.0f), ImDrawFlags_RoundCornersTop);
		DrawList->AddRect(HeaderMin, ImageMax, ImGui::GetColorU32(ImGuiCol_Border), MonaImGui::ScaleUI(5.0f));
		const char* Label = "Camera Preview";
		const ImVec2 LabelSize = ImGui::CalcTextSize(Label);
		DrawList->AddText(ImVec2(HeaderMin.x + MonaImGui::ScaleUI(8.0f), HeaderMin.y + (HeaderHeight - LabelSize.y) * 0.5f), ImGui::GetColorU32(ImGuiCol_Text), Label);
	}

	auto FSceneViewportPanel::DrawFPSOverlay(const ImVec2& ViewportMin, const ImVec2& ViewportMax) const -> void
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

	auto FSceneViewportPanel::UpdateViewportInput(FLevelEditorContext& Context, const FViewportToolbarLayout& ToolbarLayout) -> void
	{
		if (ViewportClient == nullptr) return;
		const ImGuiIO& IO = ImGui::GetIO();
		FLevelEditorViewportInput Input;
		Input.DeltaSeconds = IO.DeltaTime;
		Input.MouseDelta = {IO.MouseDelta.x, IO.MouseDelta.y};
		Input.MouseWheel = IO.MouseWheel;
		Input.bHovered = bViewportHovered;
		Input.bFocused = bViewportFocused;
		Input.bWantTextInput = IO.WantTextInput;
		Input.bAlt = IO.KeyAlt;
		Input.bShift = IO.KeyShift;
		Input.bCtrl = IO.KeyCtrl;
		Input.bLeftMouseDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);
		Input.bMiddleMouseDown = ImGui::IsMouseDown(ImGuiMouseButton_Middle);
		Input.bRightMouseDown = ImGui::IsMouseDown(ImGuiMouseButton_Right);
		Input.bLeftMousePressed = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
		Input.bMiddleMousePressed = ImGui::IsMouseClicked(ImGuiMouseButton_Middle);
		Input.bRightMousePressed = ImGui::IsMouseClicked(ImGuiMouseButton_Right);
		Input.bMoveForward = ImGui::IsKeyDown(ImGuiKey_W);
		Input.bMoveBackward = ImGui::IsKeyDown(ImGuiKey_S);
		Input.bMoveLeft = ImGui::IsKeyDown(ImGuiKey_A);
		Input.bMoveRight = ImGui::IsKeyDown(ImGuiKey_D);
		Input.bMoveDown = ImGui::IsKeyDown(ImGuiKey_Q);
		Input.bMoveUp = ImGui::IsKeyDown(ImGuiKey_E);
		Input.bFocusSelection = ImGui::IsKeyPressed(ImGuiKey_F, false);
		Input.bCancel = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
		const bool bGizmoShortcutAllowed = Input.bHovered && Input.bFocused && !Input.bWantTextInput && !Input.bRightMouseDown && !Input.bMiddleMouseDown && !Input.bAlt;
		Input.bModeTranslate = bGizmoShortcutAllowed && ImGui::IsKeyPressed(ImGuiKey_W, false);
		Input.bModeRotate = bGizmoShortcutAllowed && ImGui::IsKeyPressed(ImGuiKey_E, false);
		Input.bModeScale = bGizmoShortcutAllowed && ImGui::IsKeyPressed(ImGuiKey_R, false);
		const ImVec2 ViewportMin = ImGui::GetItemRectMin();
		const ImVec2 ViewportMax = ImGui::GetItemRectMax();
		const ImVec2 MousePosition = ImGui::GetMousePos();
		Input.MousePosition = {MousePosition.x - ViewportMin.x, MousePosition.y - ViewportMin.y};
		Input.ViewportSize = {ViewportMax.x - ViewportMin.x, ViewportMax.y - ViewportMin.y};
		const bool bToolbarHovered = ImGui::IsMouseHoveringRect(ToolbarLayout.BackgroundMin, ToolbarLayout.BackgroundMax)
			|| ImGui::IsMouseHoveringRect(ToolbarLayout.PlayBackgroundMin, ToolbarLayout.PlayBackgroundMax);
		const bool bPopupOpen = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
		if (bToolbarHovered || bPopupOpen) Input.bLeftMousePressed = false;
		FSceneView SceneView;
		ViewportClient->CalcSceneView(static_cast<uint32>(FMath::Max(1.0f, Input.ViewportSize.x)), static_cast<uint32>(FMath::Max(1.0f, Input.ViewportSize.y)), SceneView);
		FEditorTransactionManager* Transactions = GEditor != nullptr ? &GEditor->GetTransactionManager() : nullptr;
		if (!ViewportClient->GetTransformGizmo().IsDragging() && Input.bHovered)
			ViewportClient->UpdateHoveredVisualization(Context.Level, Input.MousePosition, Input.ViewportSize);
		else if (!Input.bHovered)
			ViewportClient->UpdateHoveredVisualization(nullptr, {}, {});
		ViewportClient->GetTransformGizmo().Update(Context, SceneView, Input, Transactions);
		const bool bGizmoConsumesMouse = ViewportClient->GetTransformGizmo().IsHovered() || ViewportClient->GetTransformGizmo().IsDragging();
		Input.bRequestSelection = Input.bHovered && Input.bLeftMousePressed && !Input.bAlt && !Input.bWantTextInput && !bToolbarHovered && !bGizmoConsumesMouse && !bPopupOpen;
		if (Input.bRequestSelection) ImGui::SetWindowFocus();
		if (ViewportClient->GetTransformGizmo().IsDragging())
		{
			Input.bLeftMousePressed = false;
			Input.bMiddleMousePressed = false;
			Input.bRightMousePressed = false;
			Input.bMiddleMouseDown = false;
			Input.bRightMouseDown = false;
			Input.MouseWheel = 0.0f;
		}
		ViewportClient->Update(Context.Level, Context.GetPrimarySelectedActor(), Input);
		if (Input.bRequestSelection)
		{
			if (AActor* HitActor = ViewportClient->PickActor(Context.Level, Input.MousePosition, Input.ViewportSize))
			{
				if (IO.KeyCtrl)
					Context.ToggleActorSelection(HitActor);
				else
					Context.SelectActor(HitActor);
			}
			else
				Context.ClearSelection();
		}
		ViewportClient->SetSelectedActors(Context.GetSelectedActors(), Context.GetPrimarySelectedActor());
	}

	auto FSceneViewportPanel::UpdateViewportSize() -> void
	{
		ImVec2 AvailableSize = ImGui::GetContentRegionAvail();
		AvailableSize.x = FMath::Max(8.0f, AvailableSize.x);
		AvailableSize.y = FMath::Max(8.0f, AvailableSize.y);
		if (ViewportWidget != nullptr)
		{
			ViewportWidget->SetDesiredSize({AvailableSize.x, AvailableSize.y});
		}
	}
} // namespace Durin
