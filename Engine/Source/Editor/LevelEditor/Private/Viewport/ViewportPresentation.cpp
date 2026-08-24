#include "Viewport/ViewportPresentation.h"

#include "Editor/EditorEngine.h"
#include "Client/SceneViewport.h"
#include "Client/ViewportClient.h"
#include "Engine/Engine.h"
#include "EngineGlobals.h"
#include "Engine/World.h"
#include "Math/Vector.h"
#include "MonaImGui.h"
#include "MonaImGuiPropertyTable.h"
#include "SceneViewProjection.h"
#include "Viewport/LevelEditorViewportClient.h"
#include "LevelEditorViewportEditing.h"
#include "Workspace/LevelEditorContext.h"
#include "Workspace/LevelEditorUILayout.h"

namespace Durin::Editor::Level
{
	namespace
	{
		struct FStableEditorFrameTimingState final
		{
			ImGuiContext* Context = nullptr;
			int32 LastFrame = -1;
			float PresentationAccumulatorSeconds = 0.0f;
			FEngineFrameTiming Presented;
			bool bInitialized = false;
		};

		FStableEditorFrameTimingState StableEditorFrameTiming;

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

		constexpr std::array DirectionalShadowQualityOptions = {
			TViewportModeOption{EDirectionalShadowFilterQuality::Low, "Low"},
			TViewportModeOption{EDirectionalShadowFilterQuality::Medium, "Medium (Default)"},
			TViewportModeOption{EDirectionalShadowFilterQuality::High, "High"}
		};

		constexpr std::array ContactShadowRouteOptions = {
			TViewportModeOption{EContactShadowRoutePreference::Auto, "Auto (Compute Preferred)"},
			TViewportModeOption{EContactShadowRoutePreference::Compute, "Compute Only"},
			TViewportModeOption{EContactShadowRoutePreference::Fragment, "Fragment Only"}
		};

		constexpr std::array GroundTruthAmbientOcclusionQualityOptions = {
			TViewportModeOption{EGroundTruthAmbientOcclusionQuality::HalfResolution, "Half Resolution"},
			TViewportModeOption{EGroundTruthAmbientOcclusionQuality::FullResolution, "Full Resolution"}
		};

		constexpr std::array VolumetricCloudQualityOptions = {
			TViewportModeOption{EVolumetricCloudQuality::Performance, "Performance"},
			TViewportModeOption{EVolumetricCloudQuality::High, "High (Default)"},
			TViewportModeOption{EVolumetricCloudQuality::Epic, "Epic"},
			TViewportModeOption{EVolumetricCloudQuality::Reference, "Reference"}
		};

		constexpr std::array VolumetricCloudDebugOptions = {
			TViewportModeOption{EVolumetricCloudDebugMode::Radiance, "Cloud Radiance"},
			TViewportModeOption{EVolumetricCloudDebugMode::Transmittance, "Transmittance"},
			TViewportModeOption{EVolumetricCloudDebugMode::TemporalStatus, "Temporal Status"},
			TViewportModeOption{EVolumetricCloudDebugMode::ShadowVisibility, "Shadow Visibility"}
		};

		constexpr std::array DirectionalShadowBiasDiagnosticOptions = {
			TViewportModeOption{EDirectionalShadowDiagnosticMode::ShadowDepthCoverage, "Shadow Depth Coverage"},
			TViewportModeOption{EDirectionalShadowDiagnosticMode::ReceiverUnbiased, "Receiver Unbiased"},
			TViewportModeOption{EDirectionalShadowDiagnosticMode::ReceiverBiased, "Receiver Biased"},
			TViewportModeOption{EDirectionalShadowDiagnosticMode::ReceiverNormalOffset, "Receiver Normal Offset"},
			TViewportModeOption{EDirectionalShadowDiagnosticMode::TexelGrid, "Texel Grid"},
			TViewportModeOption{EDirectionalShadowDiagnosticMode::BiasContributions, "Bias Contributions"},
			TViewportModeOption{EDirectionalShadowDiagnosticMode::Classification, "Classification"}
		};

		constexpr std::array DirectionalShadowFilterDiagnosticOptions = {
			TViewportModeOption{EDirectionalShadowDiagnosticMode::FilterFootprint, "Filter Footprint"},
			TViewportModeOption{EDirectionalShadowDiagnosticMode::FilterTapValidity, "Filter Tap Validity"},
			TViewportModeOption{EDirectionalShadowDiagnosticMode::FilterDifference, "Filter Difference"}
		};

		constexpr std::array DirectionalShadowCascadeDiagnosticOptions = {
			TViewportModeOption{EDirectionalShadowDiagnosticMode::CascadeIndex, "Cascade Index"},
			TViewportModeOption{EDirectionalShadowDiagnosticMode::CascadeTransition, "Cascade Transition"},
			TViewportModeOption{EDirectionalShadowDiagnosticMode::CascadeCoverage, "Cascade Coverage"},
			TViewportModeOption{EDirectionalShadowDiagnosticMode::CascadeDifference, "Cascade Difference"}
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

		auto FormatByteCount(uint64 Bytes) -> std::string
		{
			if (Bytes >= 1024ull * 1024ull)
				return std::format("{:.2f} MiB", static_cast<double>(Bytes) / (1024.0 * 1024.0));
			if (Bytes >= 1024ull)
				return std::format("{:.2f} KiB", static_cast<double>(Bytes) / 1024.0);
			return std::format("{} B", Bytes);
		}

		template<typename T, size_t Size, typename FSetMode>
		auto DrawModeOptions(T CurrentMode, const std::array<TViewportModeOption<T>, Size>& Options, FSetMode&& SetMode) -> void
		{
			for (const TViewportModeOption<T>& Option : Options)
			{
				if (ImGui::RadioButton(Option.Label, Option.Value == CurrentMode)) SetMode(Option.Value);
			}
		}

		auto DrawDirectionalShadowDiagnosticOptions(FViewportClient* ViewportClient) -> void
		{
			if (ViewportClient == nullptr) return;
			const FSceneViewSettings CurrentSettings = ViewportClient->GetViewSettings();
			const EDirectionalShadowDiagnosticMode CurrentMode =
				CurrentSettings.DirectionalShadow.DiagnosticMode;
			const bool bDebugViewsActive = CurrentMode != EDirectionalShadowDiagnosticMode::Lit
				|| CurrentSettings.DirectionalShadow.bShowContactDebug;
			auto SetMode = [ViewportClient](EDirectionalShadowDiagnosticMode Mode) {
				FSceneViewSettings Settings = ViewportClient->GetViewSettings();
				Settings.DirectionalShadow.DiagnosticMode = Mode;
				Settings.DirectionalShadow.bShowContactDebug = false;
				ViewportClient->SetViewSettings(Settings);
			};

			if (ImGui::MenuItem("Reset Debug Views", nullptr, false, bDebugViewsActive))
			{
				SetMode(EDirectionalShadowDiagnosticMode::Lit);
			}
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
				ImGui::SetTooltip("Disable all shadow diagnostics without changing production rendering settings.");
			ImGui::Separator();
			if (ImGui::RadioButton("Contact Shadow Contribution",
				CurrentSettings.DirectionalShadow.bShowContactDebug))
			{
				FSceneViewSettings Settings = ViewportClient->GetViewSettings();
				Settings.DirectionalShadow.DiagnosticMode = EDirectionalShadowDiagnosticMode::Lit;
				Settings.DirectionalShadow.bEnableContactShadows = true;
				Settings.DirectionalShadow.bShowContactDebug = true;
				ViewportClient->SetViewSettings(Settings);
			}
			ImGui::Separator();
			if (ImGui::BeginMenu("Coverage and Bias"))
			{
				DrawModeOptions(CurrentMode, DirectionalShadowBiasDiagnosticOptions, SetMode);
				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu("Filtering"))
			{
				DrawModeOptions(CurrentMode, DirectionalShadowFilterDiagnosticOptions, SetMode);
				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu("Cascades"))
			{
				DrawModeOptions(CurrentMode, DirectionalShadowCascadeDiagnosticOptions, SetMode);
				ImGui::EndMenu();
			}
		}

		auto DrawDirectionalShadowQualityOptions(FViewportClient* ViewportClient) -> void
		{
			if (ViewportClient == nullptr) return;
			const EDirectionalShadowFilterQuality CurrentQuality =
				ViewportClient->GetViewSettings().DirectionalShadow.FilterQuality;
			DrawModeOptions(CurrentQuality, DirectionalShadowQualityOptions,
				[ViewportClient](EDirectionalShadowFilterQuality Quality) {
					FSceneViewSettings Settings = ViewportClient->GetViewSettings();
					Settings.DirectionalShadow.FilterQuality = Quality;
					ViewportClient->SetViewSettings(Settings);
				});
		}

		auto Add(const ImVec2& A, const ImVec2& B) -> ImVec2 { return ImVec2(A.x + B.x, A.y + B.y); }
		auto Mul(const ImVec2& Value, float Scale) -> ImVec2 { return ImVec2(Value.x * Scale, Value.y * Scale); }

		auto SetNextToolbarPopupPosition(const ImVec2& ButtonPosition, const ImVec2& ButtonSize) -> void
		{
			ImGui::SetNextWindowPos(
				ImVec2(ButtonPosition.x, ButtonPosition.y + ButtonSize.y + MonaImGui::ScaleUI(4.0f)),
				ImGuiCond_Appearing);
		}

		// Identifies semantic toolbar icons independent of the active font glyphs.
		enum class EViewportToolbarIcon : uint8
		{
			None,
			ShadingSphere,
			Translate,
			Rotate,
			Scale,
			SnapGrid,
			Play,
			Pause,
			Step,
			Stop,
			Camera,
			ChevronDown
		};

		auto DrawToolbarIcon(ImDrawList* DrawList, EViewportToolbarIcon Icon, const ImVec2& Center, ImU32 Color, float Scale) -> void
		{
			const float Thickness = FMath::Max(1.0f, 1.6f * Scale);
			switch (Icon)
			{
			case EViewportToolbarIcon::ShadingSphere:
				{
					const float Radius = 6.5f * Scale;
					DrawList->PathClear();
					DrawList->PathLineTo(ImVec2(Center.x, Center.y - Radius));
					DrawList->PathArcTo(Center, Radius, -1.5708f, 1.5708f, 12);
					DrawList->PathFillConvex(Color);
					DrawList->AddCircle(Center, Radius, Color, 24, Thickness);
					break;
				}
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
			case EViewportToolbarIcon::Camera:
				{
					const ImVec2 OpticalCenter(Center.x, Center.y + 1.0f * Scale);
					const float HalfWidth = 7.0f * Scale;
					const float HalfHeight = 4.5f * Scale;
					DrawList->AddRect(
						ImVec2(OpticalCenter.x - HalfWidth, OpticalCenter.y - HalfHeight),
						ImVec2(OpticalCenter.x + HalfWidth, OpticalCenter.y + HalfHeight),
						Color, 1.5f * Scale, 0, Thickness);
					DrawList->AddCircle(OpticalCenter, 2.6f * Scale, Color, 12, Thickness);
					DrawList->AddRectFilled(
						ImVec2(OpticalCenter.x - 4.5f * Scale, OpticalCenter.y - HalfHeight - 2.0f * Scale),
						ImVec2(OpticalCenter.x - 0.5f * Scale, OpticalCenter.y - HalfHeight),
						Color, Scale);
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

		auto DrawViewportTopScrim(ImDrawList* DrawList, const ImVec2& ViewportMin, const ImVec2& ViewportMax) -> void
		{
			const float ScrimHeight = MonaImGui::ScaleUI(48.0f);
			const ImU32 TopColor = ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 0.22f));
			const ImU32 BottomColor = ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
			DrawList->AddRectFilledMultiColor(
				ViewportMin,
				ImVec2(ViewportMax.x, FMath::Min(ViewportMax.y, ViewportMin.y + ScrimHeight)),
				TopColor, TopColor, BottomColor, BottomColor);
		}

		auto DrawViewportHudSurface(const ImVec2& Min, const ImVec2& Max) -> void
		{
			ImVec4 SurfaceColor = ImGui::GetStyleColorVec4(ImGuiCol_PopupBg);
			SurfaceColor.w = 0.46f;
			ImVec4 BorderColor = ImGui::GetStyleColorVec4(ImGuiCol_Border);
			BorderColor.w *= 0.18f;
			ImDrawList* DrawList = ImGui::GetWindowDrawList();
			const float Rounding = MonaImGui::ScaleUI(5.0f);
			DrawList->AddRectFilled(Min, Max, ImGui::GetColorU32(SurfaceColor), Rounding);
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

		auto DrawToolbarButton(const char* Id, const ImVec2& Position, const ImVec2& Size, const char* Label, EViewportToolbarIcon Icon, bool bSelected, const char* Tooltip, bool bButtonSurface = false, bool bSuccessIcon = false, bool bStrongContent = false) -> bool
		{
			ImGui::SetCursorScreenPos(Position);
			const bool bPressed = ImGui::InvisibleButton(Id, Size);
			const bool bHovered = ImGui::IsItemHovered();
			const bool bHeld = ImGui::IsItemActive();
			ImDrawList* DrawList = ImGui::GetWindowDrawList();
			const ImVec2 Max(Position.x + Size.x, Position.y + Size.y);
			DrawToolbarButtonBackground(DrawList, Position, Max, bSelected, bButtonSurface, bHovered, bHeld);

			const ImU32 TextColor = ImGui::GetColorU32(
				bStrongContent || bSelected || bButtonSurface || bHovered ? ImGuiCol_Text : ImGuiCol_TextDisabled);
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

		auto DrawViewportDisplayButton(
			const ImVec2& Position,
			const ImVec2& Size,
			const char* Tooltip) -> bool
		{
			ImGui::SetCursorScreenPos(Position);
			const bool bPressed = ImGui::InvisibleButton("##ViewModeButton", Size);
			const bool bHovered = ImGui::IsItemHovered();
			const bool bHeld = ImGui::IsItemActive();
			ImDrawList* DrawList = ImGui::GetWindowDrawList();
			DrawToolbarButtonBackground(
				DrawList,
				Position,
				ImVec2(Position.x + Size.x, Position.y + Size.y),
				false,
				false,
				bHovered,
				bHeld);

			const float IconScale = ImGui::GetFontSize() / 15.0f;
			const float SphereWidth = 16.0f * IconScale;
			const float ChevronWidth = 10.0f * IconScale;
			const float Gap = MonaImGui::ScaleUI(6.0f);
			const float ContentX = Position.x
				+ (Size.x - SphereWidth - Gap - ChevronWidth) * 0.5f;
			const float CenterY = Position.y + Size.y * 0.5f;
			const ImU32 Color = ImGui::GetColorU32(
				bHovered ? ImGuiCol_Text : ImGuiCol_TextDisabled);
			DrawToolbarIcon(
				DrawList,
				EViewportToolbarIcon::ShadingSphere,
				ImVec2(ContentX + SphereWidth * 0.5f, CenterY),
				Color,
				IconScale);
			DrawToolbarIcon(
				DrawList,
				EViewportToolbarIcon::ChevronDown,
				ImVec2(ContentX + SphereWidth + Gap + ChevronWidth * 0.5f, CenterY),
				Color,
				IconScale);

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

		auto DrawSnapButton(const ImVec2& Position, float Width, float Height, bool bEnabled, bool bPopupOpen) -> FSplitButtonResult
		{
			ImDrawList* DrawList = ImGui::GetWindowDrawList();
			const ImVec2 Max(Position.x + Width, Position.y + Height);

			ImGui::SetCursorScreenPos(Position);
			const bool bPrimaryPressed = ImGui::InvisibleButton("##SnapToggle", ImVec2(Width, Height));
			const bool bSecondaryPressed = ImGui::IsItemClicked(ImGuiMouseButton_Right);
			const bool bHovered = ImGui::IsItemHovered();
			const bool bHeld = ImGui::IsItemActive();
			DrawToolbarButtonBackground(DrawList, Position, Max, bEnabled || bPopupOpen, false, bHovered, bHeld);

			const ImU32 TextColor = ImGui::GetColorU32(bEnabled || bPopupOpen || bHovered ? ImGuiCol_Text : ImGuiCol_TextDisabled);
			const float IconScale = ImGui::GetFontSize() / 15.0f;
			const ImU32 SnapIconColor = bEnabled ? ImGui::GetColorU32(ImGuiCol_CheckMark) : TextColor;
			DrawToolbarIcon(DrawList, EViewportToolbarIcon::SnapGrid, ImVec2(Position.x + Width * 0.5f, Position.y + Height * 0.5f), SnapIconColor, IconScale);
			if (bHovered)
			{
				ImGui::BeginTooltip();
				ImGui::TextUnformatted("Left-click: toggle snapping");
				ImGui::TextUnformatted("Right-click: snapping settings");
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

	auto GetStableEditorFrameTiming() -> const FEngineFrameTiming&
	{
		ImGuiContext* Context = ImGui::GetCurrentContext();
		if (StableEditorFrameTiming.Context != Context)
			StableEditorFrameTiming = {.Context = Context};
		if (StableEditorFrameTiming.LastFrame != ImGui::GetFrameCount())
		{
			StableEditorFrameTiming.LastFrame = ImGui::GetFrameCount();
			StableEditorFrameTiming.PresentationAccumulatorSeconds += std::clamp(
				ImGui::GetIO().DeltaTime, 0.0f, 0.25f);
			constexpr float PresentationIntervalSeconds = 0.5f;
			if (!StableEditorFrameTiming.bInitialized
				|| StableEditorFrameTiming.PresentationAccumulatorSeconds
					>= PresentationIntervalSeconds)
			{
				StableEditorFrameTiming.Presented = GetEngineFrameTiming();
				StableEditorFrameTiming.PresentationAccumulatorSeconds = 0.0f;
				StableEditorFrameTiming.bInitialized = true;
			}
		}
		return StableEditorFrameTiming.Presented;
	}

	auto GetStableEditorFrameTimeMilliseconds() -> float
	{
		return GetStableEditorFrameTiming().FrameIntervalMilliseconds;
	}

	auto DrawViewportPlayStateBorder(const ImVec2& ViewportMin, const ImVec2& ViewportMax, bool bPaused) -> void
	{
		DrawPlayStateBorder(ViewportMin, ViewportMax, bPaused);
	}

	auto FViewportToolbar::CalculateLayout(
		const FViewportClient* RenderSettingsClient,
		bool bRenderSettingsTargetIsPlayWindow,
		const FLevelViewportEditModeManager* EditModeManager,
		const ImVec2& ViewportMin,
		const ImVec2& ViewportMax
	) const -> FViewportToolbarLayout
	{
		FViewportToolbarLayout Layout;
		Layout.ViewportMin = ViewportMin;
		Layout.ViewportMax = ViewportMax;
		Layout.bRenderSettingsTargetIsPlayWindow = bRenderSettingsTargetIsPlayWindow;
		if (RenderSettingsClient != nullptr)
		{
			const FSceneViewSettings& Settings = RenderSettingsClient->GetViewSettings();
			Layout.bEnableFXAA = Settings.PostProcess.bEnableFXAA;
			Layout.bEnableGroundTruthAmbientOcclusion =
				Settings.AmbientOcclusion.bEnabled;
			Layout.GroundTruthAmbientOcclusionQuality =
				Settings.AmbientOcclusion.Quality;
			Layout.RenderMode = Settings.Mode.RenderMode;
			Layout.RasterMode = Settings.Mode.RasterMode;
		}

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
		Layout.ViewModeButtonSize = ImVec2(
			TransformIconWidth + ContentGap + ChevronWidth + ContentPadding * 2.0f,
			Layout.Height);
		const float SecondaryWidth = Layout.bOverflow ? Layout.DropDownWidth : Layout.SpaceButtonWidth + Layout.SnapButtonWidth;
		const float ToolbarWidth = Layout.ViewModeButtonSize.x + Layout.Gap + Layout.EditModeButtonWidth + Layout.Gap + Layout.ModeButtonWidth * 3.0f + Layout.ToolButtonGap * 3.0f + SecondaryWidth + MonaImGui::ScaleUI(3.0f);
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
		FLevelEditorViewportClient* EditorViewportClient,
		FViewportClient* RenderSettingsClient,
		FLevelViewportEditModeManager* EditModeManager,
		::Durin::Editor::EPlayStartLocation& PreferredPlayStartLocation,
		::Durin::Editor::EPlayDestination& PreferredPlayDestination,
		const FViewportToolbarLayout& Layout
	) const -> void
	{
		FLevelEditorViewportClient* ViewportClient = EditorViewportClient;
		const bool bPlaying = GEditor && GEditor->IsPlaying();
		const bool bPlayingInNewWindow = GEditor && GEditor->IsPlayingInNewWindow();
		const FViewportToolbarCapabilities Capabilities = ResolveViewportToolbarCapabilities(
			Context.bReadOnly, bPlaying, bPlayingInNewWindow);
		const ImVec2& ViewportMin = Layout.ViewportMin;
		const ImVec2& ViewportMax = Layout.ViewportMax;

		ImDrawList* DrawList = ImGui::GetWindowDrawList();
		DrawList->PushClipRect(ViewportMin, ViewportMax, true);
		DrawViewportTopScrim(DrawList, ViewportMin, ViewportMax);
		DrawToolbarSurface(Layout.BackgroundMin, Layout.BackgroundMax);
		DrawToolbarSurface(Layout.PlayBackgroundMin, Layout.PlayBackgroundMax);
		const float ViewSeparatorX = Layout.ViewModeButtonPosition.x + Layout.ViewModeButtonSize.x + Layout.Gap * 0.5f;
		DrawToolbarSeparator(DrawList, ViewSeparatorX, Layout.ViewModeButtonPosition.y, Layout.Height);
		DrawList->PopClipRect();

		if (ViewportClient != nullptr)
		{
			char FpsText[32];
			snprintf(FpsText, sizeof(FpsText), "%.0f FPS", GAverageFPS);
			const float FpsWidth = ImGui::CalcTextSize(FpsText).x + MonaImGui::ScaleUI(14.0f);
			const float CurrentSpeed = ViewportClient->GetMovementSpeed();
			const std::string SpeedLabel = CurrentSpeed >= 1.0f
				? std::format("{:.0f}", CurrentSpeed)
				: std::format("{:.1f}", CurrentSpeed);
			const float SpeedWidth = FMath::Max(
				MonaImGui::ScaleUI(58.0f),
				ImGui::CalcTextSize(SpeedLabel.c_str()).x + MonaImGui::ScaleUI(38.0f));
			const ImVec2 SpeedPosition(
				ViewportMax.x - MonaImGui::ScaleUI(10.0f) - FpsWidth - MonaImGui::ScaleUI(6.0f) - SpeedWidth,
				Layout.ViewModeButtonPosition.y);
			const ImVec2 SpeedSize(SpeedWidth, Layout.Height);
			DrawList->PushClipRect(ViewportMin, ViewportMax, true);
			DrawViewportHudSurface(
				ImVec2(SpeedPosition.x - MonaImGui::ScaleUI(3.0f), SpeedPosition.y - MonaImGui::ScaleUI(3.0f)),
				ImVec2(
					ViewportMax.x - MonaImGui::ScaleUI(10.0f) + GetViewportHudSurfaceOutset(),
					SpeedPosition.y + SpeedSize.y + MonaImGui::ScaleUI(3.0f)));
			const float HudSeparatorX = SpeedPosition.x + SpeedSize.x + MonaImGui::ScaleUI(3.0f);
			DrawToolbarSeparator(DrawList, HudSeparatorX, SpeedPosition.y, SpeedSize.y);
			DrawList->PopClipRect();

			if (!Capabilities.bCanEditCamera) ImGui::BeginDisabled();
			if (DrawToolbarButton(
				"##CameraSpeedButton", SpeedPosition, SpeedSize, SpeedLabel.c_str(),
				EViewportToolbarIcon::Camera, ImGui::IsPopupOpen("CameraSpeedPopup"),
				"Camera position and fly speed", false, false, true))
			{
				ImGui::OpenPopup("CameraSpeedPopup");
			}
			SetNextToolbarPopupPosition(SpeedPosition, SpeedSize);
			ImGui::SetNextWindowSize(ImVec2(MonaImGui::ScaleUI(390.0f), 0.0f), ImGuiCond_Appearing);
			if (ImGui::BeginPopup("CameraSpeedPopup"))
			{
				ImGui::TextUnformatted("Camera");
				ImGui::Separator();
				if (MonaImGui::PropertyEdit::BeginTable("##CameraProperties"))
				{
					if (MonaImGui::PropertyEdit::BeginGroup("Transform", "Transform"))
					{
						FVector3 CameraLocation = ViewportClient->GetCameraTransform().GetLocation();
						MonaImGui::PropertyEdit::FValueWidgetConfig PositionConfig;
						PositionConfig.Format = "%.1f";
						if (MonaImGui::PropertyEdit::EditVector("Position", CameraLocation, false, 0.25,
							nullptr, PositionConfig, "Editor camera world position"))
							ViewportClient->SetCameraLocation(CameraLocation);
						MonaImGui::PropertyEdit::EndGroup();
					}

					if (MonaImGui::PropertyEdit::BeginGroup("Navigation", "Navigation"))
					{
						float MovementSpeed = ViewportClient->GetMovementSpeed();
						MonaImGui::PropertyEdit::BeginRow("Fly Speed", false, 0.0f, "Editor camera navigation speed");
						ImGui::SetNextItemWidth(-FLT_MIN);
						const char* SpeedFormat = MovementSpeed >= 1.0f ? "%.0f units/s" : "%.1f units/s";
						if (ImGui::DragFloat("##FlySpeed", &MovementSpeed, 0.25f, 0.05f, 10000.0f, SpeedFormat,
							ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoRoundToFormat))
							ViewportClient->SetMovementSpeed(MovementSpeed);
						MonaImGui::PropertyEdit::EndRow();
						MonaImGui::PropertyEdit::BeginRow("Speed Presets");
						for (const float Preset : {1.0f, 5.0f, 25.0f, 100.0f})
						{
							if (Preset != 1.0f) ImGui::SameLine();
							if (ImGui::SmallButton(std::format("{:g}##CameraSpeedPreset", Preset).c_str()))
								ViewportClient->SetMovementSpeed(Preset);
						}
						MonaImGui::PropertyEdit::EndRow();
						MonaImGui::PropertyEdit::EndGroup();
					}

					if (MonaImGui::PropertyEdit::BeginGroup("ViewDistance", "View Distance"))
					{
						float NearClip = ViewportClient->GetNearClip();
						float FarClip = ViewportClient->GetFarClip();
						float FadeStart = ViewportClient->GetViewFadeStart();
						float RenderDistance = ViewportClient->GetViewRenderDistance();
						auto DrawDistance = [](const char* Label, float& Value, float Speed, float Minimum,
							float Maximum, const char* Format) {
							MonaImGui::PropertyEdit::BeginRow(Label);
							ImGui::SetNextItemWidth(-FLT_MIN);
							const bool bChanged = ImGui::DragFloat("##Value", &Value, Speed, Minimum, Maximum,
								Format, ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoRoundToFormat);
							MonaImGui::PropertyEdit::EndRow();
							return bChanged;
						};
						ImGui::PushID("NearClip");
						const bool bNearChanged = DrawDistance("Near Clip", NearClip, 0.01f, 0.001f, FarClip - 1.0f, "%.3f");
						ImGui::PopID();
						ImGui::PushID("FarClip");
						const bool bFarChanged = DrawDistance("Far Clip", FarClip, 100.0f, NearClip + 1.0f, 10000000.0f, "%.1f");
						ImGui::PopID();
						if (bNearChanged || bFarChanged)
						{
							ViewportClient->SetClipDistances(NearClip, FarClip);
							NearClip = ViewportClient->GetNearClip();
							FarClip = ViewportClient->GetFarClip();
							FadeStart = ViewportClient->GetViewFadeStart();
							RenderDistance = ViewportClient->GetViewRenderDistance();
						}
						ImGui::PushID("FadeStart");
						const bool bFadeChanged = DrawDistance("Fade Start", FadeStart, 100.0f, 0.0f, RenderDistance - 1.0f, "%.1f");
						ImGui::PopID();
						ImGui::PushID("RenderDistance");
						const bool bRenderChanged = DrawDistance("Render Distance", RenderDistance, 100.0f, FadeStart + 1.0f,
							static_cast<float>(SceneViewProjection::GetMaximumViewRenderDistance(FarClip)), "%.1f");
						ImGui::PopID();
						if (bFadeChanged || bRenderChanged) ViewportClient->SetViewDistance(FadeStart, RenderDistance);
						MonaImGui::PropertyEdit::BeginRow("Defaults");
						if (ImGui::Button("Restore View Defaults")) ViewportClient->ResetViewDistances();
						MonaImGui::PropertyEdit::EndRow();
						MonaImGui::PropertyEdit::EndGroup();
					}
					MonaImGui::PropertyEdit::EndTable();
				}
				ImGui::TextDisabled("Main scene depth: Reversed Z");
				ImGui::TextDisabled("Hold the navigation mouse button and scroll to adjust.");
				ImGui::EndPopup();
			}
			if (!Capabilities.bCanEditCamera) ImGui::EndDisabled();
		}

		const bool bRenderSettingsAvailable = Capabilities.bCanEditRenderSettings
			&& RenderSettingsClient != nullptr;
		if (!bRenderSettingsAvailable) ImGui::BeginDisabled();

		const std::string ViewModeTooltip = std::format(
			"{} Display ({} - {})",
			Layout.bRenderSettingsTargetIsPlayWindow ? "Play Window" : "Viewport",
			GetModeLabel(Layout.RenderMode, RenderModeOptions),
			GetModeLabel(Layout.RasterMode, RasterModeOptions));
		if (DrawViewportDisplayButton(
			Layout.ViewModeButtonPosition,
			Layout.ViewModeButtonSize,
			ViewModeTooltip.c_str()))
		{
			ImGui::OpenPopup("ViewModePopup");
		}

		SetNextToolbarPopupPosition(Layout.ViewModeButtonPosition, Layout.ViewModeButtonSize);
		if (ImGui::BeginPopup("ViewModePopup"))
		{
			if (Layout.bRenderSettingsTargetIsPlayWindow)
			{
				ImGui::TextDisabled("Target: Play Window");
				ImGui::Separator();
			}
			if (ImGui::BeginMenu("Shading"))
			{
				DrawModeOptions(Layout.RenderMode, RenderModeOptions, [RenderSettingsClient](ERenderMode Mode) {
					if (RenderSettingsClient == nullptr) return;
					FSceneViewSettings Settings = RenderSettingsClient->GetViewSettings();
					Settings.Mode.RenderMode = Mode;
					RenderSettingsClient->SetViewSettings(Settings);
				});
				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu("Rasterization"))
			{
				DrawModeOptions(Layout.RasterMode, RasterModeOptions, [RenderSettingsClient](ERasterMode Mode) {
					if (RenderSettingsClient == nullptr) return;
					FSceneViewSettings Settings = RenderSettingsClient->GetViewSettings();
					Settings.Mode.RasterMode = Mode;
					RenderSettingsClient->SetViewSettings(Settings);
				});
				ImGui::EndMenu();
			}
			if (RenderSettingsClient != nullptr && ImGui::BeginMenu("Shadows"))
			{
				if (ImGui::BeginMenu("Directional Shadows"))
				{
					if (ImGui::BeginMenu("Filter Quality"))
					{
						DrawDirectionalShadowQualityOptions(RenderSettingsClient);
						ImGui::EndMenu();
					}
					if (ImGui::BeginMenu("Contact Shadows"))
					{
						const FSceneViewSettings CurrentSettings =
							RenderSettingsClient->GetViewSettings();
						bool bEnabled =
							CurrentSettings.DirectionalShadow.bEnableContactShadows;
						if (ImGui::Checkbox("Enabled", &bEnabled))
						{
							FSceneViewSettings Settings =
								RenderSettingsClient->GetViewSettings();
							Settings.DirectionalShadow.bEnableContactShadows = bEnabled;
							if (!bEnabled)
								Settings.DirectionalShadow.bShowContactDebug = false;
							RenderSettingsClient->SetViewSettings(Settings);
						}
						ImGui::Separator();
						if (ImGui::BeginMenu("Visibility Route"))
						{
							DrawModeOptions(
								CurrentSettings.DirectionalShadow.ContactRoutePreference,
								ContactShadowRouteOptions,
								[RenderSettingsClient](EContactShadowRoutePreference Route) {
									FSceneViewSettings Settings =
										RenderSettingsClient->GetViewSettings();
									Settings.DirectionalShadow.ContactRoutePreference = Route;
									RenderSettingsClient->SetViewSettings(Settings);
								});
							ImGui::Separator();
							ImGui::TextDisabled(
								"Compute Only disables fragment fallback for A/B testing.");
							ImGui::EndMenu();
						}
						ImGui::EndMenu();
					}
					ImGui::EndMenu();
				}
				ImGui::EndMenu();
			}
			if (RenderSettingsClient != nullptr && ImGui::BeginMenu("Post Processing"))
			{
				if (ImGui::BeginMenu("GTAO"))
				{
					bool bEnabled = Layout.bEnableGroundTruthAmbientOcclusion;
					if (ImGui::Checkbox("Enabled", &bEnabled))
					{
						FSceneViewSettings Settings =
							RenderSettingsClient->GetViewSettings();
						Settings.AmbientOcclusion.bEnabled = bEnabled;
						RenderSettingsClient->SetViewSettings(Settings);
					}
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip(
							"Ground Truth Ambient Occlusion for indirect environment lighting in Solid Lit views.");
					ImGui::Separator();
					ImGui::TextDisabled("Quality");
					DrawModeOptions(
						Layout.GroundTruthAmbientOcclusionQuality,
						GroundTruthAmbientOcclusionQualityOptions,
						[RenderSettingsClient](EGroundTruthAmbientOcclusionQuality Quality) {
							FSceneViewSettings Settings =
								RenderSettingsClient->GetViewSettings();
							Settings.AmbientOcclusion.Quality = Quality;
							RenderSettingsClient->SetViewSettings(Settings);
						});
					ImGui::EndMenu();
				}
				if (ImGui::BeginMenu("FXAA"))
				{
					bool bEnabled = Layout.bEnableFXAA;
					if (ImGui::Checkbox("Enabled", &bEnabled))
					{
						FSceneViewSettings Settings =
							RenderSettingsClient->GetViewSettings();
						Settings.PostProcess.bEnableFXAA = bEnabled;
						RenderSettingsClient->SetViewSettings(Settings);
					}
					ImGui::EndMenu();
				}
				ImGui::EndMenu();
			}
			if (RenderSettingsClient != nullptr && ImGui::BeginMenu("Volumetric Clouds"))
			{
				ImGui::TextDisabled("Quality");
				DrawModeOptions(
					RenderSettingsClient->GetViewSettings().VolumetricCloud.Quality,
					VolumetricCloudQualityOptions,
					[RenderSettingsClient](EVolumetricCloudQuality Quality) {
						FSceneViewSettings Settings = RenderSettingsClient->GetViewSettings();
						Settings.VolumetricCloud.Quality = Quality;
						RenderSettingsClient->SetViewSettings(Settings);
					});
				ImGui::Separator();
				ImGui::TextDisabled("Quality is view-only and does not dirty the level.");
				ImGui::EndMenu();
			}
			ImGui::Separator();
			if (ImGui::BeginMenu("Overlays"))
			{
				if (ViewportClient != nullptr)
				{
					if (!Capabilities.bCanEditScene) ImGui::BeginDisabled();
					bool bShowGrid = ViewportClient->IsGridVisible();
					if (ImGui::Checkbox("World Grid", &bShowGrid))
						ViewportClient->SetGridVisible(bShowGrid);
					if (!Capabilities.bCanEditScene) ImGui::EndDisabled();
				}
				if (Context.World)
				{
					if (!Capabilities.bCanToggleCollision) ImGui::BeginDisabled();
					bool bShowCollision = Context.World->IsCollisionDebugDrawEnabled();
					if (ImGui::Checkbox("Collision", &bShowCollision))
						Context.World->SetCollisionDebugDrawEnabled(bShowCollision);
					if (!Capabilities.bCanToggleCollision) ImGui::EndDisabled();
				}
				ImGui::EndMenu();
			}
			ImGui::Separator();
			if (RenderSettingsClient != nullptr && ImGui::BeginMenu("Debug Views"))
			{
				ImGui::TextDisabled("Temporary diagnostic rendering.");
				DrawDirectionalShadowDiagnosticOptions(RenderSettingsClient);
				ImGui::Separator();
				if (ImGui::BeginMenu("Volumetric Clouds"))
				{
					const EVolumetricCloudDebugMode Current =
						RenderSettingsClient->GetViewSettings().VolumetricCloud.DebugMode;
					if (ImGui::MenuItem("Reset Cloud Debug View", nullptr, false,
						Current != EVolumetricCloudDebugMode::Lit))
					{
						FSceneViewSettings Settings = RenderSettingsClient->GetViewSettings();
						Settings.VolumetricCloud.DebugMode = EVolumetricCloudDebugMode::Lit;
						RenderSettingsClient->SetViewSettings(Settings);
					}
					ImGui::Separator();
					DrawModeOptions(Current, VolumetricCloudDebugOptions,
						[RenderSettingsClient](EVolumetricCloudDebugMode Mode) {
							FSceneViewSettings Settings = RenderSettingsClient->GetViewSettings();
							Settings.VolumetricCloud.DebugMode = Mode;
							RenderSettingsClient->SetViewSettings(Settings);
						});
					ImGui::EndMenu();
				}
				ImGui::EndMenu();
			}
			ImGui::EndPopup();
		}
		if (!bRenderSettingsAvailable) ImGui::EndDisabled();

		if (ViewportClient == nullptr)
		{
			return;
		}
		if (!Capabilities.bCanEditScene) ImGui::BeginDisabled();
		FTransformGizmo& Gizmo = ViewportClient->GetTransformGizmo();
		bool bOpenSnapSettings = false;
		float X = Layout.ViewModeButtonPosition.x + Layout.ViewModeButtonSize.x + Layout.Gap;
		const float Y = Layout.ViewModeButtonPosition.y;
		auto ToolbarButton = [&](const char* Id, const char* Text, EViewportToolbarIcon Icon, float Width, bool bSelected, const char* Tooltip, auto&& Action) {
			if (DrawToolbarButton(Id, ImVec2(X, Y), ImVec2(Width, Layout.Height), Text, Icon, bSelected, Tooltip)) Action();
			X += Width;
		};
		const bool bEditModePopupOpen = ImGui::IsPopupOpen("ViewportEditModePopup");
		if (DrawToolbarButton("##EditModeButton", ImVec2(X, Y), ImVec2(Layout.EditModeButtonWidth, Layout.Height), Layout.EditModeLabel.c_str(), EViewportToolbarIcon::ChevronDown, bEditModePopupOpen, "Viewport editing mode"))
			ImGui::OpenPopup("ViewportEditModePopup");
		SetNextToolbarPopupPosition(ImVec2(X, Y), ImVec2(Layout.EditModeButtonWidth, Layout.Height));
		if (ImGui::BeginPopup("ViewportEditModePopup"))
		{
			if (EditModeManager)
			{
				for (const FLevelViewportEditModeDescriptor* Descriptor : FLevelViewportEditModeRegistry::Get().GetAvailable(Context))
					if (ImGui::RadioButton(Descriptor->DisplayName.c_str(), EditModeManager->GetActiveModeId() == Descriptor->Id)) EditModeManager->Activate(Descriptor->Id, Context);
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
				if (ImGui::RadioButton(SpaceLabel(Space), Gizmo.GetSpace() == Space)) Gizmo.SetSpace(Space);
			}
			ImGui::Separator();
			ImGui::TextDisabled("Scale always uses Local space");
		};
		ToolbarButton("##MoveMode", nullptr, EViewportToolbarIcon::Translate, Layout.ModeButtonWidth, Gizmo.GetMode() == ETransformGizmoMode::Translate, "Move tool (W)", [&] { Gizmo.SetMode(ETransformGizmoMode::Translate); });
		X += Layout.ToolButtonGap;
		ToolbarButton("##RotateMode", nullptr, EViewportToolbarIcon::Rotate, Layout.ModeButtonWidth, Gizmo.GetMode() == ETransformGizmoMode::Rotate, "Rotate tool (E)", [&] { Gizmo.SetMode(ETransformGizmoMode::Rotate); });
		X += Layout.ToolButtonGap;
		ToolbarButton("##ScaleMode", nullptr, EViewportToolbarIcon::Scale, Layout.ModeButtonWidth, Gizmo.GetMode() == ETransformGizmoMode::Scale, "Scale tool (R)", [&] { Gizmo.SetMode(ETransformGizmoMode::Scale); });
		X += Layout.ToolButtonGap;
		ImVec2 SnapPopupPosition(X, Y);
		if (Layout.bOverflow)
		{
			const ImVec2 OverflowPosition(X, Y);
			SnapPopupPosition = OverflowPosition;
			ToolbarButton("##ViewportOverflow", "...", EViewportToolbarIcon::None, Layout.DropDownWidth, false, "More viewport tools", [&] { ImGui::OpenPopup("ViewportToolsOverflow"); });
			SetNextToolbarPopupPosition(OverflowPosition, ImVec2(Layout.DropDownWidth, Layout.Height));
			if (ImGui::BeginPopup("ViewportToolsOverflow"))
			{
				if (ImGui::BeginMenu("Transform Space"))
				{
					DrawTransformSpaceOptions();
					ImGui::EndMenu();
				}
				ImGui::Checkbox("Snapping", &Gizmo.GetSnapSettings().bEnabled);
				if (ImGui::MenuItem("Snap Settings...")) bOpenSnapSettings = true;
				ImGui::EndPopup();
			}
		}
		else
		{
			const ImVec2 TransformSpacePosition(X, Y);
			ToolbarButton("##TransformSpace", SpaceLabel(Gizmo.GetSpace()), EViewportToolbarIcon::ChevronDown, Layout.SpaceButtonWidth, false, "Choose transform space; scale always uses Local", [&] { ImGui::OpenPopup("TransformSpacePopup"); });
			SetNextToolbarPopupPosition(TransformSpacePosition, ImVec2(Layout.SpaceButtonWidth, Layout.Height));
			if (ImGui::BeginPopup("TransformSpacePopup"))
			{
				DrawTransformSpaceOptions();
				ImGui::EndPopup();
			}
			SnapPopupPosition = ImVec2(X, Y);
			const FSplitButtonResult SnapResult = DrawSnapButton(ImVec2(X, Y), Layout.SnapButtonWidth, Layout.Height, Gizmo.GetSnapSettings().bEnabled, ImGui::IsPopupOpen("GizmoSnapSettings"));
			if (SnapResult.bPrimaryPressed) Gizmo.GetSnapSettings().bEnabled = !Gizmo.GetSnapSettings().bEnabled;
			if (SnapResult.bSecondaryPressed) bOpenSnapSettings = true;
		}
		if (bOpenSnapSettings) ImGui::OpenPopup("GizmoSnapSettings");
		SetNextToolbarPopupPosition(SnapPopupPosition, ImVec2(Layout.SnapButtonWidth, Layout.Height));
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
			ImGui::Checkbox("Enabled", &Settings.bEnabled);
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
		if (!Capabilities.bCanEditScene) ImGui::EndDisabled();

		const bool bPaused = bPlaying && GEditor->IsPlaySessionPaused();
		float PlayX = Layout.PlayButtonPosition.x;
		const float PlayY = Layout.PlayButtonPosition.y;
		if (!bPlaying)
		{
			const char* StartLabel = PreferredPlayStartLocation == ::Durin::Editor::EPlayStartLocation::EditorCamera ? "Editor Camera" : "Level Start";
			const char* DestinationLabel = PreferredPlayDestination == ::Durin::Editor::EPlayDestination::NewWindow ? "New Window" : "Viewport";
			const std::string PlayTooltip = std::format("Play from {} in {} (F5)", StartLabel, DestinationLabel);
			const FSplitButtonResult PlayResult = DrawPlaySplitButton(ImVec2(PlayX, PlayY), Layout.PlayButtonWidth, Layout.DropDownWidth, Layout.Height, "Play", PlayTooltip.c_str());
			if (PlayResult.bPrimaryPressed && Context.StartPlay)
				Context.StartPlay(PreferredPlayStartLocation, PreferredPlayDestination);
			if (PlayResult.bSecondaryPressed)
				ImGui::OpenPopup("ViewportPlayOptionsPopup");
			SetNextToolbarPopupPosition(ImVec2(PlayX, PlayY), ImVec2(Layout.PlayButtonWidth + Layout.DropDownWidth, Layout.Height));
			if (ImGui::BeginPopup("ViewportPlayOptionsPopup"))
			{
				ImGui::TextDisabled("Start Location");
				if (ImGui::RadioButton("Level Start", PreferredPlayStartLocation == ::Durin::Editor::EPlayStartLocation::LevelStart)) PreferredPlayStartLocation = ::Durin::Editor::EPlayStartLocation::LevelStart;
				if (ImGui::RadioButton("Editor Camera", PreferredPlayStartLocation == ::Durin::Editor::EPlayStartLocation::EditorCamera)) PreferredPlayStartLocation = ::Durin::Editor::EPlayStartLocation::EditorCamera;
				ImGui::Separator();
				ImGui::TextDisabled("Destination");
				if (ImGui::RadioButton("Embedded Viewport", PreferredPlayDestination == ::Durin::Editor::EPlayDestination::EmbeddedViewport)) PreferredPlayDestination = ::Durin::Editor::EPlayDestination::EmbeddedViewport;
				if (ImGui::RadioButton("New Window", PreferredPlayDestination == ::Durin::Editor::EPlayDestination::NewWindow)) PreferredPlayDestination = ::Durin::Editor::EPlayDestination::NewWindow;
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
			SetNextToolbarPopupPosition(ImVec2(PlayX, PlayY), ImVec2(Layout.RuntimeButtonWidth * 3.0f + Layout.DropDownWidth, Layout.Height));
			if (ImGui::BeginPopup("ViewportRuntimeOptionsPopup"))
			{
				const bool bHasSelection = !Context.GetSelectedActors().empty();
				if (ImGui::MenuItem("Apply Selected Runtime Changes", nullptr, false, bHasSelection) && Context.ApplyPlayChanges) Context.ApplyPlayChanges(true);
				if (ImGui::MenuItem("Apply All Runtime Changes") && Context.ApplyPlayChanges) Context.ApplyPlayChanges(false);
				ImGui::Separator();
				bool bPhysicsEnabled = GEditor->GetPlayWorld()
					&& GEditor->GetPlayWorld()->IsPhysicsSimulationEnabled();
				if (ImGui::Checkbox("Simulate Physics", &bPhysicsEnabled)
					&& GEditor->GetPlayWorld())
				{
					GEditor->GetPlayWorld()->SetPhysicsSimulationEnabled(bPhysicsEnabled);
				}
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

	auto DrawViewportCameraSpeedOverlay(const FLevelEditorViewportClient* ViewportClient, const ImVec2& ViewportMin, const ImVec2& ViewportMax) -> void
	{
		if (ViewportClient == nullptr || !ViewportClient->IsFlyNavigating()) return;
		const std::string Label = std::format("Fly speed  {:.2f} units/s", ViewportClient->GetMovementSpeed());
		const ImVec2 TextSize = ImGui::CalcTextSize(Label.c_str());
		const ImVec2 Padding(MonaImGui::ScaleUI(8.0f), MonaImGui::ScaleUI(4.0f));
		const ImVec2 BadgeSize(TextSize.x + Padding.x * 2.0f, TextSize.y + Padding.y * 2.0f);
		const ImVec2 BadgeMin((ViewportMin.x + ViewportMax.x - BadgeSize.x) * 0.5f, ViewportMax.y - BadgeSize.y - MonaImGui::ScaleUI(10.0f));
		const ImVec2 BadgeMax(BadgeMin.x + BadgeSize.x, BadgeMin.y + BadgeSize.y);
		ImDrawList* DrawList = ImGui::GetWindowDrawList();
		DrawList->PushClipRect(ViewportMin, ViewportMax, true);
		ImVec4 BadgeColor = ImGui::GetStyleColorVec4(ImGuiCol_PopupBg);
		BadgeColor.w = 0.88f;
		DrawList->AddRectFilled(BadgeMin, BadgeMax, ImGui::GetColorU32(BadgeColor), BadgeMax.y - BadgeMin.y);
		DrawList->AddText(ImVec2(BadgeMin.x + Padding.x, BadgeMin.y + Padding.y), ImGui::GetColorU32(ImGuiCol_Text), Label.c_str());
		DrawList->PopClipRect();
	}

	auto DrawViewportStatisticsOverlay(
		const ImVec2& ViewportMin,
		const ImVec2& ViewportMax,
		const FSceneViewportStatisticsSnapshot& Snapshot,
		bool& bExpanded,
		bool* OutDetailsRequested) -> FViewportStatisticsOverlayLayout
	{
		if (OutDetailsRequested != nullptr) *OutDetailsRequested = false;
		FViewportStatisticsOverlayLayout Layout =
			CalculateViewportStatisticsOverlayLayout(
				ViewportMin, ViewportMax, bExpanded);
		const ImVec2 SavedCursor = ImGui::GetCursorScreenPos();
		ImGui::PushClipRect(ViewportMin, ViewportMax, true);
		const ImVec2 BadgeSize(
			Layout.BadgeMax.x - Layout.BadgeMin.x,
			Layout.BadgeMax.y - Layout.BadgeMin.y);
		char FpsText[32];
		snprintf(FpsText, sizeof(FpsText), "%.0f FPS", GAverageFPS);
		const bool bActivated = DrawToolbarButton(
			"##ViewportStatisticsToggle", Layout.BadgeMin, BadgeSize, FpsText,
			EViewportToolbarIcon::None, bExpanded,
			bExpanded ? "Hide rendering statistics" : "Show rendering statistics",
			false, false, true);
		if (bActivated)
		{
			bExpanded = !bExpanded;
			Layout = CalculateViewportStatisticsOverlayLayout(
				ViewportMin, ViewportMax, bExpanded);
		}
		ImGui::SetCursorScreenPos(SavedCursor);

		ImDrawList* DrawList = ImGui::GetWindowDrawList();
		if (Layout.bPanelVisible)
		{
			ImVec4 PanelColor = ImGui::GetStyleColorVec4(ImGuiCol_PopupBg);
			PanelColor.w = 0.94f;
			const float Rounding = MonaImGui::ScaleUI(6.0f);
			DrawList->AddRectFilled(Layout.PanelMin, Layout.PanelMax,
				ImGui::GetColorU32(PanelColor), Rounding);
			DrawList->AddRect(Layout.PanelMin, Layout.PanelMax,
				ImGui::GetColorU32(ImGuiCol_Border), Rounding);

			const float HorizontalPadding = MonaImGui::ScaleUI(12.0f);
			const float RowHeight = MonaImGui::ScaleUI(20.0f);
			float Y = Layout.PanelMin.y + MonaImGui::ScaleUI(10.0f);
			const ImU32 LabelColor = ImGui::GetColorU32(ImGuiCol_TextDisabled);
			const ImU32 ValueColor = MonaImGui::GetThemeColorU32(
				MonaImGui::EUIThemeColor::ViewportText);
			DrawList->AddText(ImVec2(Layout.PanelMin.x + HorizontalPadding, Y),
				ValueColor, "Frame Summary");
			Y += RowHeight + MonaImGui::ScaleUI(3.0f);
			DrawList->AddLine(
				ImVec2(Layout.PanelMin.x + HorizontalPadding, Y),
				ImVec2(Layout.PanelMax.x - HorizontalPadding, Y),
				ImGui::GetColorU32(ImGuiCol_Border));
			Y += MonaImGui::ScaleUI(7.0f);

			auto DrawRow = [&](const char* Label, std::string Value) {
				DrawList->AddText(
					ImVec2(Layout.PanelMin.x + HorizontalPadding, Y),
					LabelColor, Label);
				const ImVec2 ValueSize = ImGui::CalcTextSize(Value.c_str());
				DrawList->AddText(
					ImVec2(Layout.PanelMax.x - HorizontalPadding - ValueSize.x, Y),
					ValueColor, Value.c_str());
				Y += RowHeight;
			};

			if (!Snapshot.bAvailable)
			{
				DrawList->AddText(
					ImVec2(Layout.PanelMin.x + HorizontalPadding, Y),
					LabelColor, "Statistics unavailable");
			}
			else
			{
				const FSceneViewStatistics& Statistics = Snapshot.Statistics;
				DrawRow("Frame interval", std::format("{:.2f} ms",
					GetStableEditorFrameTimeMilliseconds()));
				DrawRow("Visible primitives", std::format("{} / {}",
					FormatViewportStatistic(Statistics.Visibility.VisiblePrimitives),
					FormatViewportStatistic(Statistics.Visibility.SubmittedPrimitives)));
				DrawRow("Triangles", FormatViewportStatistic(Statistics.Summary.Triangles));
				DrawRow("Draw calls", FormatViewportStatistic(Statistics.Summary.DrawCalls));
			}

			const ImVec2 ButtonMin(
				Layout.PanelMin.x + HorizontalPadding,
				Layout.PanelMax.y - MonaImGui::ScaleUI(35.0f));
			const ImVec2 ButtonSize(
				Layout.PanelMax.x - Layout.PanelMin.x - HorizontalPadding * 2.0f,
				MonaImGui::ScaleUI(25.0f));
			ImGui::SetCursorScreenPos(ButtonMin);
			const bool bDetailsActivated = ImGui::InvisibleButton(
				"##ViewportRenderingDiagnostics", ButtonSize);
			const bool bDetailsHovered = ImGui::IsItemHovered();
			if (bDetailsHovered || ImGui::IsItemActive())
			{
				ImVec4 Feedback = ImGui::GetStyleColorVec4(ImGui::IsItemActive()
					? ImGuiCol_HeaderActive : ImGuiCol_HeaderHovered);
				Feedback.w *= 0.45f;
				DrawList->AddRectFilled(ButtonMin,
					ImVec2(ButtonMin.x + ButtonSize.x, ButtonMin.y + ButtonSize.y),
					ImGui::GetColorU32(Feedback), MonaImGui::ScaleUI(4.0f));
			}
			constexpr const char* DetailsLabel = "Details...";
			const ImVec2 DetailsTextSize = ImGui::CalcTextSize(DetailsLabel);
			DrawList->AddText(ImVec2(
				ButtonMin.x + (ButtonSize.x - DetailsTextSize.x) * 0.5f,
				ButtonMin.y + (ButtonSize.y - DetailsTextSize.y) * 0.5f),
				ImGui::GetColorU32(bDetailsHovered
					? ImGuiCol_Text : ImGuiCol_TextDisabled), DetailsLabel);
			if (bDetailsActivated && OutDetailsRequested != nullptr)
				*OutDetailsRequested = true;
			ImGui::SetCursorScreenPos(SavedCursor);
		}
		ImGui::PopClipRect();
		return Layout;
	}
} // namespace Durin::Editor::Level
