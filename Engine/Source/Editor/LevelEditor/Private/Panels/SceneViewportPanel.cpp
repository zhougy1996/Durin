#include "Panels/SceneViewportPanel.h"

#include "Components/CameraComponent.h"
#include "Engine/Engine.h"
#include "IRendererModule.h"
#include "LevelEditorContext.h"
#include "Math/Vector.h"
#include "Mona/SceneViewport.h"
#include "MonaImGui.h"
#include "Viewport/LevelEditorViewportClient.h"
#include "Widgets/MViewport.h"

namespace Durin
{
	namespace
	{
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

		DrawToolbar();
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
				DrawOrientationOverlay(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
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

	auto FSceneViewportPanel::DrawToolbar() -> void
	{
		ImGui::Text("FPS %.1f", ImGui::GetIO().Framerate);
		ImGui::SameLine();
		ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
		ImGui::SameLine();
		if (GEngine != nullptr)
		{
			if (IRendererModule* RendererModule = GEngine->GetRendererModule())
			{
				bool bEnableFXAA = RendererModule->IsFXAAEnabled();
				if (ImGui::Checkbox("FXAA", &bEnableFXAA))
				{
					RendererModule->SetFXAAEnabled(bEnableFXAA);
				}
			}
		}
		ImGui::SameLine();
		ImGui::TextDisabled("Lit");
		ImGui::Separator();
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
		ViewportClient->Update(Context.Level, Context.SelectedActor.Get(), Input);
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
