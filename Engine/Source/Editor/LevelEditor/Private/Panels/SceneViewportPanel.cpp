#include "Panels/SceneViewportPanel.h"

#include "Components/CameraComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/Actor.h"
#include "IRendererModule.h"
#include "LevelEditorContext.h"
#include "Math/Vector.h"
#include "Mona/SceneViewport.h"
#include "MonaImGui.h"
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
		if (ViewportWidget != nullptr)
		{
			ViewportWidget->Draw();
			if (ViewportWidget->WasTextureDrawn())
			{
				bViewportHovered = ImGui::IsItemHovered();
				const bool bNavigationMousePressed = ImGui::IsMouseClicked(ImGuiMouseButton_Right) ||
					ImGui::IsMouseClicked(ImGuiMouseButton_Middle) ||
					(ImGui::GetIO().KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left));
				if (bViewportHovered && bNavigationMousePressed)
				{
					ImGui::SetWindowFocus();
				}
				bViewportFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
				UpdateViewportInput(Context);
				const ImVec2 VpMin = ImGui::GetItemRectMin();
				const ImVec2 VpMax = ImGui::GetItemRectMax();
				DrawSelectionBounds(Context, VpMin, VpMax);
				DrawToolbar(VpMin, VpMax);
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

	auto FSceneViewportPanel::DrawToolbar(const ImVec2& ViewportMin, const ImVec2& ViewportMax) -> void
	{
		(void)ViewportMax;
		ERenderMode CurrentMode = ERenderMode::Lit;
		ERasterMode CurrentRasterMode = ERasterMode::Solid;
		IRendererModule* RendererModule = nullptr;
		if (GEngine != nullptr)
		{
			RendererModule = GEngine->GetRendererModule();
			if (RendererModule != nullptr)
			{
				CurrentMode = RendererModule->GetRenderMode();
				CurrentRasterMode = RendererModule->GetRasterMode();
			}
		}

		const std::string Label = std::format("{} / {}", GetModeLabel(CurrentMode, RenderModeOptions), GetModeLabel(CurrentRasterMode, RasterModeOptions));
		const ImVec2 LabelSize = ImGui::CalcTextSize(Label.c_str());
		const ImVec2 ButtonPos(ViewportMin.x + 8.0f, ViewportMin.y + 4.0f);
		const ImVec2 ButtonSize(LabelSize.x + 12.0f, LabelSize.y + 4.0f);

		ImDrawList* DrawList = ImGui::GetWindowDrawList();
		DrawList->PushClipRect(ViewportMin, ViewportMax, true);

		const bool bHovered = ImGui::IsMouseHoveringRect(ButtonPos, ImVec2(ButtonPos.x + ButtonSize.x, ButtonPos.y + ButtonSize.y));
		if (bHovered)
		{
			DrawList->AddRectFilled(ButtonPos, ImVec2(ButtonPos.x + ButtonSize.x, ButtonPos.y + ButtonSize.y), IM_COL32(60, 60, 60, 160), 3.0f);
		}

		DrawList->AddText(ImVec2(ButtonPos.x + 6.0f, ButtonPos.y + 2.0f), IM_COL32(220, 220, 220, 255), Label.c_str());
		DrawList->PopClipRect();

		ImGui::SetCursorScreenPos(ButtonPos);
		if (ImGui::InvisibleButton("##ViewModeButton", ButtonSize))
		{
			ImGui::OpenPopup("ViewModePopup");
		}

		if (ImGui::BeginPopup("ViewModePopup"))
		{
			DrawModeOptions(CurrentMode, RenderModeOptions, [RendererModule](ERenderMode Mode)
			{
				if (RendererModule != nullptr) RendererModule->SetRenderMode(Mode);
			});
			ImGui::Separator();
			DrawModeOptions(CurrentRasterMode, RasterModeOptions, [RendererModule](ERasterMode Mode)
			{
				if (RendererModule != nullptr) RendererModule->SetRasterMode(Mode);
			});
			ImGui::EndPopup();
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

	auto FSceneViewportPanel::UpdateViewportInput(FLevelEditorContext& Context) -> void
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
		const ImVec2 ViewportMin = ImGui::GetItemRectMin();
		const ImVec2 ViewportMax = ImGui::GetItemRectMax();
		const ImVec2 MousePosition = ImGui::GetMousePos();
		Input.MousePosition = {MousePosition.x - ViewportMin.x, MousePosition.y - ViewportMin.y};
		Input.ViewportSize = {ViewportMax.x - ViewportMin.x, ViewportMax.y - ViewportMin.y};
		const ImVec2 ToolbarMin(ViewportMin.x + 8.0f, ViewportMin.y + 4.0f);
		ERenderMode RenderMode = ERenderMode::Lit;
		ERasterMode RasterMode = ERasterMode::Solid;
		if (GEngine != nullptr)
		{
			if (IRendererModule* Renderer = GEngine->GetRendererModule())
			{
				RenderMode = Renderer->GetRenderMode();
				RasterMode = Renderer->GetRasterMode();
			}
		}
		const std::string ToolbarLabel = std::format("{} / {}", RenderMode == ERenderMode::Lit ? "Lit" : "Unlit", RasterMode == ERasterMode::Solid ? "Solid" : "Wireframe");
		const ImVec2 ToolbarLabelSize = ImGui::CalcTextSize(ToolbarLabel.c_str());
		const bool bToolbarHovered = ImGui::IsMouseHoveringRect(ToolbarMin, ImVec2(ToolbarMin.x + ToolbarLabelSize.x + 12.0f, ToolbarMin.y + ToolbarLabelSize.y + 4.0f));
		Input.bRequestSelection = Input.bHovered && Input.bLeftMousePressed && !Input.bAlt && !Input.bWantTextInput && !bToolbarHovered && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
		if (Input.bRequestSelection) ImGui::SetWindowFocus();
		ViewportClient->Update(Context.Level, Context.SelectedActor.Get(), Input);
		if (Input.bRequestSelection)
		{
			if (AActor* HitActor = ViewportClient->PickActor(Context.Level, Input.MousePosition, Input.ViewportSize)) Context.SelectActor(HitActor);
			else Context.ClearSelection();
		}
	}

	auto FSceneViewportPanel::DrawSelectionBounds(const FLevelEditorContext& Context, const ImVec2& ViewportMin, const ImVec2& ViewportMax) const -> void
	{
		AActor* Actor = Context.SelectedActor.Get();
		if (Actor == nullptr || ViewportClient == nullptr) return;
		FBox WorldBounds;
		for (const TObjectPtr<DActorComponent>& ComponentPtr : Actor->GetOwnedComponents())
		{
			auto* Component = Cast<DStaticMeshComponent>(ComponentPtr.Get());
			const DStaticMesh* Mesh = Component != nullptr ? Component->GetStaticMesh() : nullptr;
			const FStaticMeshRenderData* Data = Mesh != nullptr ? Mesh->GetRenderData() : nullptr;
			if (Data == nullptr || !Data->LocalBounds.bIsValid) continue;
			const FMatrix LocalToWorld = Component->GetRenderMatrix();
			for (uint32 Corner = 0; Corner < 8; ++Corner)
			{
				const FVector3 Local(
					(Corner & 1) != 0 ? Data->LocalBounds.Max.x : Data->LocalBounds.Min.x,
					(Corner & 2) != 0 ? Data->LocalBounds.Max.y : Data->LocalBounds.Min.y,
					(Corner & 4) != 0 ? Data->LocalBounds.Max.z : Data->LocalBounds.Min.z);
				WorldBounds.AddPoint(FVector3(LocalToWorld * FVector4(Local, 1.0)));
			}
		}
		if (!WorldBounds.bIsValid) return;
		std::array<ImVec2, 8> ScreenCorners;
		std::array<bool, 8> bProjected{};
		const FVector2f ViewportSize(ViewportMax.x - ViewportMin.x, ViewportMax.y - ViewportMin.y);
		for (uint32 Corner = 0; Corner < 8; ++Corner)
		{
			const FVector3 World(
				(Corner & 1) != 0 ? WorldBounds.Max.x : WorldBounds.Min.x,
				(Corner & 2) != 0 ? WorldBounds.Max.y : WorldBounds.Min.y,
				(Corner & 4) != 0 ? WorldBounds.Max.z : WorldBounds.Min.z);
			FVector2f Screen;
			bProjected[Corner] = ViewportClient->ProjectWorldToViewport(World, ViewportSize, Screen);
			ScreenCorners[Corner] = ImVec2(ViewportMin.x + Screen.x, ViewportMin.y + Screen.y);
		}
		static constexpr std::array<std::array<uint32, 2>, 12> Edges = {{{0, 1}, {0, 2}, {0, 4}, {1, 3}, {1, 5}, {2, 3}, {2, 6}, {3, 7}, {4, 5}, {4, 6}, {5, 7}, {6, 7}}};
		ImDrawList* DrawList = ImGui::GetWindowDrawList();
		DrawList->PushClipRect(ViewportMin, ViewportMax, true);
		for (const auto& Edge : Edges)
		{
			if (bProjected[Edge[0]] && bProjected[Edge[1]]) DrawList->AddLine(ScreenCorners[Edge[0]], ScreenCorners[Edge[1]], IM_COL32(255, 184, 48, 255), 2.0f);
		}
		DrawList->PopClipRect();
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
