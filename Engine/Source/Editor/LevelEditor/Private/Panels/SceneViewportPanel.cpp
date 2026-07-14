#include "Panels/SceneViewportPanel.h"

#include "AssetSystem.h"
#include "Components/CameraComponent.h"
#include "Components/StaticMeshComponent.h"
#include "ContentBrowserDragDrop.h"
#include "Editor/EditorEngine.h"
#include "Editor/EditorTransaction.h"
#include "Engine/Engine.h"
#include "Engine/Actor.h"
#include "Engine/Level.h"
#include "Actors/StaticMeshActor.h"
#include "IRendererModule.h"
#include "LevelEditorContext.h"
#include "Math/Vector.h"
#include "Mona/SceneViewport.h"
#include "MonaImGui.h"
#include "SceneViewProjection.h"
#include "Viewport/LevelEditorViewportClient.h"
#include "Widgets/MViewport.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshResources.h"

namespace Durin
{
	namespace
	{
		template <typename T>
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

		template <typename T, size_t Size>
		auto GetModeLabel(T CurrentMode, const std::array<TViewportModeOption<T>, Size>& Options) -> const char*
		{
			for (const TViewportModeOption<T>& Option : Options)
			{
				if (Option.Value == CurrentMode) return Option.Label;
			}
			return "Unknown";
		}

		template <typename T, size_t Size, typename FSetMode>
		auto DrawModeOptions(T CurrentMode, const std::array<TViewportModeOption<T>, Size>& Options, FSetMode&& SetMode) -> void
		{
			for (const TViewportModeOption<T>& Option : Options)
			{
				if (ImGui::Selectable(Option.Label, Option.Value == CurrentMode)) SetMode(Option.Value);
			}
		}

		auto Add(const ImVec2& A, const ImVec2& B) -> ImVec2 { return ImVec2(A.x + B.x, A.y + B.y); }
		auto Mul(const ImVec2& Value, float Scale) -> ImVec2 { return ImVec2(Value.x * Scale, Value.y * Scale); }

		enum class EViewportToolbarIcon : uint8
		{
			None,
			Translate,
			Rotate,
			Scale,
			ChevronDown
		};

		auto DrawToolbarIcon(ImDrawList* DrawList, EViewportToolbarIcon Icon, const ImVec2& Center, ImU32 Color, float Scale) -> void
		{
			const float Thickness = FMath::Max(1.4f, 1.6f * Scale);
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
			case EViewportToolbarIcon::ChevronDown:
				DrawList->AddTriangleFilled(ImVec2(Center.x - 3.5f * Scale, Center.y - 1.5f * Scale), ImVec2(Center.x + 3.5f * Scale, Center.y - 1.5f * Scale), ImVec2(Center.x, Center.y + 2.5f * Scale), Color);
				break;
			default:
				break;
			}
		}

		auto DrawToolbarButton(const char* Id, const ImVec2& Position, const ImVec2& Size, const char* Label, EViewportToolbarIcon Icon, bool bSelected, const char* Tooltip) -> bool
		{
			ImGui::SetCursorScreenPos(Position);
			const bool bPressed = ImGui::InvisibleButton(Id, Size);
			const bool bHovered = ImGui::IsItemHovered();
			const bool bHeld = ImGui::IsItemActive();
			const ImGuiStyle& Style = ImGui::GetStyle();
			ImDrawList* DrawList = ImGui::GetWindowDrawList();
			const ImVec2 Max(Position.x + Size.x, Position.y + Size.y);
			if (bSelected || bHovered || bHeld)
			{
				const ImGuiCol Background = bSelected ? ImGuiCol_HeaderActive : bHeld ? ImGuiCol_ButtonActive : ImGuiCol_ButtonHovered;
				DrawList->AddRectFilled(Position, Max, ImGui::GetColorU32(Style.Colors[Background]), 4.0f);
			}

			const ImU32 TextColor = ImGui::GetColorU32(bSelected ? ImGuiCol_Text : (bHovered ? ImGuiCol_Text : ImGuiCol_TextDisabled));
			const float IconScale = ImGui::GetFontSize() / 13.0f;
			float ContentWidth = 0.0f;
			const ImVec2 TextSize = Label != nullptr ? ImGui::CalcTextSize(Label) : ImVec2(0.0f, 0.0f);
			const float IconWidth = Icon == EViewportToolbarIcon::None ? 0.0f : 16.0f * IconScale;
			const float Gap = Icon != EViewportToolbarIcon::None && Label != nullptr ? 5.0f : 0.0f;
			ContentWidth = IconWidth + Gap + TextSize.x;
			float ContentX = Position.x + (Size.x - ContentWidth) * 0.5f;
			if (Icon != EViewportToolbarIcon::None)
			{
				DrawToolbarIcon(DrawList, Icon, ImVec2(ContentX + IconWidth * 0.5f, Position.y + Size.y * 0.5f), TextColor, IconScale);
				ContentX += IconWidth + Gap;
			}
			if (Label != nullptr)
			{
				DrawList->AddText(ImVec2(ContentX, Position.y + (Size.y - TextSize.y) * 0.5f), TextColor, Label);
			}
			if (bSelected)
			{
				const float IndicatorWidth = FMath::Min(22.0f, Size.x - 10.0f);
				DrawList->AddRectFilled(ImVec2(Position.x + (Size.x - IndicatorWidth) * 0.5f, Max.y - 2.0f), ImVec2(Position.x + (Size.x + IndicatorWidth) * 0.5f, Max.y), ImGui::GetColorU32(ImGuiCol_CheckMark), 1.0f);
			}
			if (bHovered && Tooltip != nullptr)
			{
				ImGui::BeginTooltip();
				ImGui::TextUnformatted(Tooltip);
				ImGui::EndTooltip();
			}
			return bPressed;
		}

		struct FSplitButtonResult
		{
			bool bPrimaryPressed = false;
			bool bSecondaryPressed = false;
		};

		auto DrawSnapSplitButton(const ImVec2& Position, float PrimaryWidth, float SecondaryWidth, float Height, bool bEnabled, bool bPopupOpen) -> FSplitButtonResult
		{
			const ImGuiStyle& Style = ImGui::GetStyle();
			ImDrawList* DrawList = ImGui::GetWindowDrawList();
			const ImVec2 Max(Position.x + PrimaryWidth + SecondaryWidth, Position.y + Height);
			const ImGuiCol Background = bEnabled || bPopupOpen ? ImGuiCol_HeaderActive : ImGuiCol_FrameBg;
			DrawList->AddRectFilled(Position, Max, ImGui::GetColorU32(Style.Colors[Background]), 4.0f);
			DrawList->AddRect(Position, Max, ImGui::GetColorU32(ImGuiCol_Border), 4.0f);

			ImGui::SetCursorScreenPos(Position);
			const bool bPrimaryPressed = ImGui::InvisibleButton("##SnapToggle", ImVec2(PrimaryWidth, Height));
			const bool bPrimaryHovered = ImGui::IsItemHovered();
			const bool bPrimaryHeld = ImGui::IsItemActive();
			if (bPrimaryHovered || bPrimaryHeld)
			{
				DrawList->AddRectFilled(Position, ImVec2(Position.x + PrimaryWidth, Max.y), ImGui::GetColorU32(bPrimaryHeld ? ImGuiCol_ButtonActive : ImGuiCol_ButtonHovered), 4.0f, ImDrawFlags_RoundCornersLeft);
			}

			const ImVec2 SecondaryPosition(Position.x + PrimaryWidth, Position.y);
			ImGui::SetCursorScreenPos(SecondaryPosition);
			const bool bSecondaryPressed = ImGui::InvisibleButton("##SnapSettingsButton", ImVec2(SecondaryWidth, Height));
			const bool bSecondaryHovered = ImGui::IsItemHovered();
			const bool bSecondaryHeld = ImGui::IsItemActive();
			if (bSecondaryHovered || bSecondaryHeld)
			{
				DrawList->AddRectFilled(SecondaryPosition, Max, ImGui::GetColorU32(bSecondaryHeld ? ImGuiCol_ButtonActive : ImGuiCol_ButtonHovered), 4.0f, ImDrawFlags_RoundCornersRight);
			}

			DrawList->AddLine(ImVec2(SecondaryPosition.x, Position.y + 5.0f), ImVec2(SecondaryPosition.x, Max.y - 5.0f), ImGui::GetColorU32(ImGuiCol_Border));
			const ImU32 TextColor = ImGui::GetColorU32(bEnabled || bPopupOpen || bPrimaryHovered || bSecondaryHovered ? ImGuiCol_Text : ImGuiCol_TextDisabled);
			const ImVec2 TextSize = ImGui::CalcTextSize("Snap");
			DrawList->AddText(ImVec2(Position.x + (PrimaryWidth - TextSize.x) * 0.5f, Position.y + (Height - TextSize.y) * 0.5f), TextColor, "Snap");
			DrawToolbarIcon(DrawList, EViewportToolbarIcon::ChevronDown, ImVec2(SecondaryPosition.x + SecondaryWidth * 0.5f, Position.y + Height * 0.5f), TextColor, ImGui::GetFontSize() / 13.0f);
			if (bEnabled)
			{
				const float IndicatorWidth = FMath::Min(22.0f, PrimaryWidth - 10.0f);
				DrawList->AddRectFilled(ImVec2(Position.x + (PrimaryWidth - IndicatorWidth) * 0.5f, Max.y - 2.0f), ImVec2(Position.x + (PrimaryWidth + IndicatorWidth) * 0.5f, Max.y), ImGui::GetColorU32(ImGuiCol_CheckMark), 1.0f);
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
			DrawList->AddText(Add(Position, ImVec2(1.0f, 1.0f)), IM_COL32(0, 0, 0, 180), Text);
			DrawList->AddText(Position, Color, Text);
		}
	} // namespace

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
		ImVec2 ViewModeButtonPosition;
		ImVec2 ViewModeButtonSize;
		float Height = 0.0f;
		float Gap = 0.0f;
		float ModeButtonWidth = 0.0f;
		float CompactButtonWidth = 0.0f;
		float DropDownWidth = 0.0f;
	};

	FSceneViewportPanel::FSceneViewportPanel()
	{
		ViewportClient = std::make_unique<FLevelEditorViewportClient>();
		ViewportWidget = std::make_shared<MViewport>();
		const std::shared_ptr<FSceneViewport> SceneViewport = std::make_shared<FSceneViewport>(ViewportClient.get(), ViewportWidget);
		ViewportWidget->SetViewportInterface(SceneViewport);
		if (GEngine != nullptr)
		{
			GEngine->SetMainSceneViewport(SceneViewport);
		}
	}

	FSceneViewportPanel::~FSceneViewportPanel()
	{
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

	auto FSceneViewportPanel::GetTransformGizmo() -> FTransformGizmo*
	{
		return ViewportClient ? &ViewportClient->GetTransformGizmo() : nullptr;
	}

	auto FSceneViewportPanel::GetTransformGizmo() const -> const FTransformGizmo*
	{
		return ViewportClient ? &ViewportClient->GetTransformGizmo() : nullptr;
	}

	auto FSceneViewportPanel::Draw(FLevelEditorContext& Context) -> void
	{
		if (!ImGui::Begin("Scene Viewport###SceneViewport", GetOpenPtr(), ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
		{
			if (ViewportClient != nullptr) ViewportClient->ResetNavigation();
			bViewportHovered = false;
			bViewportFocused = false;
			ImGui::End();
			return;
		}

		if (Context.Level == nullptr)
		{
			if (ViewportClient != nullptr) ViewportClient->ResetNavigation();
			bViewportHovered = false;
			bViewportFocused = false;
			ImGui::TextDisabled("No level is open. Use File > New Level or Open Level.");
			ImGui::End();
			return;
		}
		UpdateViewportSize();
		if (ViewportClient != nullptr) ViewportClient->SetSelectedActors(Context.GetSelectedActors(), Context.GetPrimarySelectedActor());
		if (ViewportWidget != nullptr)
		{
			ViewportWidget->Draw();
			if (ViewportWidget->WasTextureDrawn())
			{
				const ImVec2 VpMin = ImGui::GetItemRectMin();
				const ImVec2 VpMax = ImGui::GetItemRectMax();
				if (ImGui::BeginDragDropTarget())
				{
					if (const ImGuiPayload* Payload = ImGui::AcceptDragDropPayload(ContentBrowserAssetPayloadType); Payload && Payload->IsDelivery() && Payload->DataSize == sizeof(FContentBrowserAssetPayload))
					{
						const auto* AssetPayload = static_cast<const FContentBrowserAssetPayload*>(Payload->Data);
						FAssetPath AssetPath;
						DStaticMesh* Mesh = nullptr;
						if (!FAssetPath::TryCreate(AssetPayload->AssetPath.data(), AssetPath)) Context.SetError("Dropped asset path is invalid.");
						else if (const Asset::FAssetResult Result = Asset::LoadAsset(AssetPath, Mesh); !Result) Context.SetError(Result.Message);
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
				const bool bNavigationMousePressed = ImGui::IsMouseClicked(ImGuiMouseButton_Right) ||
					ImGui::IsMouseClicked(ImGuiMouseButton_Middle) ||
					(ImGui::GetIO().KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left));
				if (bViewportHovered && bNavigationMousePressed)
				{
					ImGui::SetWindowFocus();
				}
				bViewportFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
				UpdateViewportInput(Context, ToolbarLayout);
				DrawToolbar(ToolbarLayout);
				DrawOrientationOverlay(VpMin, VpMax);
				DrawFPSOverlay(VpMin, VpMax);
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

		Layout.ViewModeLabel = std::format("{}  /  {}", GetModeLabel(Layout.RenderMode, RenderModeOptions), GetModeLabel(Layout.RasterMode, RasterModeOptions));
		Layout.Height = FMath::Max(28.0f, ImGui::GetFontSize() + 10.0f);
		Layout.Gap = 5.0f;
		Layout.ModeButtonWidth = FMath::Max(66.0f, ImGui::GetFontSize() * 4.2f);
		Layout.CompactButtonWidth = FMath::Max(54.0f, ImGui::GetFontSize() * 3.4f);
		Layout.DropDownWidth = Layout.Height;
		Layout.ViewModeButtonPosition = ImVec2(ViewportMin.x + 10.0f, ViewportMin.y + 8.0f);
		Layout.ViewModeButtonSize = ImVec2(ImGui::CalcTextSize(Layout.ViewModeLabel.c_str()).x + 32.0f, Layout.Height);
		const float ToolbarWidth = Layout.ViewModeButtonSize.x + Layout.Gap + Layout.ModeButtonWidth * 3.0f + Layout.Gap + Layout.CompactButtonWidth * 2.0f + Layout.DropDownWidth + 10.0f;
		Layout.BackgroundMin = ImVec2(Layout.ViewModeButtonPosition.x - 4.0f, Layout.ViewModeButtonPosition.y - 4.0f);
		Layout.BackgroundMax = ImVec2(FMath::Min(ViewportMax.x - 6.0f, Layout.ViewModeButtonPosition.x + ToolbarWidth), Layout.ViewModeButtonPosition.y + Layout.Height + 4.0f);
		return Layout;
	}

	auto FSceneViewportPanel::DrawToolbar(const FViewportToolbarLayout& Layout) -> void
	{
		const ImVec2& ViewportMin = Layout.ViewportMin;
		const ImVec2& ViewportMax = Layout.ViewportMax;

		ImDrawList* DrawList = ImGui::GetWindowDrawList();
		DrawList->PushClipRect(ViewportMin, ViewportMax, true);
		const ImGuiStyle& Style = ImGui::GetStyle();
		ImVec4 ToolbarColor = Style.Colors[ImGuiCol_PopupBg];
		ToolbarColor.w = 0.94f;
		DrawList->AddRectFilled(Layout.BackgroundMin, Layout.BackgroundMax, ImGui::GetColorU32(ToolbarColor), 6.0f);
		DrawList->AddRect(Layout.BackgroundMin, Layout.BackgroundMax, ImGui::GetColorU32(ImGuiCol_Border), 6.0f);
		DrawList->PopClipRect();

		if (DrawToolbarButton("##ViewModeButton", Layout.ViewModeButtonPosition, Layout.ViewModeButtonSize, Layout.ViewModeLabel.c_str(), EViewportToolbarIcon::ChevronDown, false, "Viewport shading and raster mode"))
		{
			ImGui::OpenPopup("ViewModePopup");
		}

		if (ImGui::BeginPopup("ViewModePopup"))
		{
			DrawModeOptions(Layout.RenderMode, RenderModeOptions, [RendererModule = Layout.RendererModule](ERenderMode Mode)
			{
				if (RendererModule != nullptr) RendererModule->SetRenderMode(Mode);
			});
			ImGui::Separator();
			DrawModeOptions(Layout.RasterMode, RasterModeOptions, [RendererModule = Layout.RendererModule](ERasterMode Mode)
			{
				if (RendererModule != nullptr) RendererModule->SetRasterMode(Mode);
			});
			ImGui::EndPopup();
		}

		if (ViewportClient == nullptr) return;
		FTransformGizmo& Gizmo = ViewportClient->GetTransformGizmo();
		float X = Layout.ViewModeButtonPosition.x + Layout.ViewModeButtonSize.x + Layout.Gap;
		const float Y = Layout.ViewModeButtonPosition.y;
		auto ToolbarButton = [&](const char* Id, const char* Text, EViewportToolbarIcon Icon, float Width, bool bSelected, const char* Tooltip, auto&& Action) {
			if (DrawToolbarButton(Id, ImVec2(X, Y), ImVec2(Width, Layout.Height), Text, Icon, bSelected, Tooltip)) Action();
			X += Width;
		};
		ToolbarButton("##MoveMode", "Move", EViewportToolbarIcon::Translate, Layout.ModeButtonWidth, Gizmo.GetMode() == ETransformGizmoMode::Translate, "Move tool (W)", [&] { Gizmo.SetMode(ETransformGizmoMode::Translate); });
		ToolbarButton("##RotateMode", "Rotate", EViewportToolbarIcon::Rotate, Layout.ModeButtonWidth, Gizmo.GetMode() == ETransformGizmoMode::Rotate, "Rotate tool (E)", [&] { Gizmo.SetMode(ETransformGizmoMode::Rotate); });
		ToolbarButton("##ScaleMode", "Scale", EViewportToolbarIcon::Scale, Layout.ModeButtonWidth, Gizmo.GetMode() == ETransformGizmoMode::Scale, "Scale tool (R)", [&] { Gizmo.SetMode(ETransformGizmoMode::Scale); });
		X += Layout.Gap;
		ToolbarButton("##TransformSpace", Gizmo.GetSpace() == ETransformGizmoSpace::World ? "World" : "Local", EViewportToolbarIcon::None, Layout.CompactButtonWidth, Gizmo.GetSpace() == ETransformGizmoSpace::Local, "Toggle world/local transform space", [&] {
			Gizmo.SetSpace(Gizmo.GetSpace() == ETransformGizmoSpace::World ? ETransformGizmoSpace::Local : ETransformGizmoSpace::World);
		});
		const FSplitButtonResult SnapResult = DrawSnapSplitButton(ImVec2(X, Y), Layout.CompactButtonWidth, Layout.DropDownWidth, Layout.Height, Gizmo.GetSnapSettings().bEnabled, ImGui::IsPopupOpen("GizmoSnapSettings"));
		if (SnapResult.bPrimaryPressed) Gizmo.GetSnapSettings().bEnabled = !Gizmo.GetSnapSettings().bEnabled;
		if (SnapResult.bSecondaryPressed) ImGui::OpenPopup("GizmoSnapSettings");
		ImGui::SetNextWindowSize(ImVec2(270.0f, 0.0f), ImGuiCond_Appearing);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 12.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 7.0f));
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

		const float AxisLength = FMath::Max(22.0f, FMath::Min(34.0f, FMath::Min(ViewportSize.x, ViewportSize.y) * 0.08f));
		const ImVec2 Origin(FMath::Max(ViewportMin.x + AxisLength + 18.0f, ViewportMax.x - 72.0f), FMath::Max(ViewportMin.y + AxisLength + 18.0f, ViewportMax.y - 46.0f));
		ImDrawList* DrawList = ImGui::GetWindowDrawList();
		DrawList->PushClipRect(ViewportMin, ViewportMax, true);
		DrawList->AddCircleFilled(Origin, 3.0f, IM_COL32(235, 235, 235, 220));

		const std::array<ImU32, 3> AxisColors = {IM_COL32(255, 72, 72, 255), IM_COL32(72, 230, 96, 255), IM_COL32(80, 135, 255, 255)};
		const std::array<const char*, 3> AxisLabels = {"X", "Y", "Z"};
		for (uint32 AxisIndex = 0; AxisIndex < 3; ++AxisIndex)
		{
			const ImVec2 End = Add(Origin, Mul(AxisDirections[AxisIndex], AxisLength));
			DrawList->AddLine(Origin, End, IM_COL32(0, 0, 0, 150), 4.0f);
			DrawList->AddLine(Origin, End, AxisColors[AxisIndex], 2.0f);
			const ImVec2 TextSize = ImGui::CalcTextSize(AxisLabels[AxisIndex]);
			DrawAxisText(DrawList, Add(End, Add(Mul(AxisDirections[AxisIndex], 5.0f), ImVec2(-TextSize.x * 0.5f, -TextSize.y * 0.5f))), AxisColors[AxisIndex], AxisLabels[AxisIndex]);
		}
		DrawList->PopClipRect();
	}

	auto FSceneViewportPanel::DrawFPSOverlay(const ImVec2& ViewportMin, const ImVec2& ViewportMax) const -> void
	{
		char FpsText[32];
		snprintf(FpsText, sizeof(FpsText), "FPS: %.1f", ImGui::GetIO().Framerate);
		const ImVec2 TextSize = ImGui::CalcTextSize(FpsText);
		const ImVec2 TextPos(ViewportMax.x - TextSize.x - 12.0f, ViewportMin.y + 8.0f);
		ImDrawList* DrawList = ImGui::GetWindowDrawList();
		DrawList->PushClipRect(ViewportMin, ViewportMax, true);
		DrawList->AddText(TextPos, IM_COL32(255, 255, 255, 200), FpsText);
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
		const bool bToolbarHovered = ImGui::IsMouseHoveringRect(ToolbarLayout.BackgroundMin, ToolbarLayout.BackgroundMax);
		const bool bPopupOpen = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
		if (bToolbarHovered || bPopupOpen) Input.bLeftMousePressed = false;
		FSceneView SceneView;
		ViewportClient->CalcSceneView(static_cast<uint32>(FMath::Max(1.0f, Input.ViewportSize.x)), static_cast<uint32>(FMath::Max(1.0f, Input.ViewportSize.y)), SceneView);
		FEditorTransactionManager* Transactions = GEditor != nullptr ? &GEditor->GetTransactionManager() : nullptr;
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
				if (IO.KeyCtrl) Context.ToggleActorSelection(HitActor);
				else Context.SelectActor(HitActor);
			}
			else Context.ClearSelection();
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
