#include "Widgets/MLevelEditor.h"

#include "Components/CameraComponent.h"
#include "Engine/Engine.h"
#include "IRendererModule.h"
#include "Math/Vector.h"
#include "MonaImGui.h"
#include "Mona/SceneViewport.h"
#include "Widgets/MViewport.h"

namespace Durin
{
	namespace
	{
		auto Add(const ImVec2& A, const ImVec2& B) -> ImVec2
		{
			return ImVec2(A.x + B.x, A.y + B.y);
		}

		auto Mul(const ImVec2& Value, float Scale) -> ImVec2
		{
			return ImVec2(Value.x * Scale, Value.y * Scale);
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

			const float InvLength = 1.0f / std::sqrt(LengthSquared);
			return Mul(Direction, InvLength);
		}

		auto DrawAxisText(ImDrawList* DrawList, const ImVec2& Position, ImU32 Color, const char* Text) -> void
		{
			DrawList->AddText(Add(Position, ImVec2(1.0f, 1.0f)), IM_COL32(0, 0, 0, 180), Text);
			DrawList->AddText(Position, Color, Text);
		}
	}

	auto MLevelEditor::Construct() -> void
	{
		ViewportWidget = std::make_shared<MViewport>();
		const std::shared_ptr<FSceneViewport> SceneViewport = std::make_shared<FSceneViewport>(nullptr, ViewportWidget);
		ViewportWidget->SetViewportInterface(SceneViewport);

		if (GEngine != nullptr)
		{
			GEngine->SetMainSceneViewport(SceneViewport);
		}
	}

	auto MLevelEditor::Draw() -> void
	{
		DrawViewportPanel();
	}

	auto MLevelEditor::DrawViewportPanel() -> void
	{
		ImGui::Begin("Level Editor");

		UpdateViewportSize();

		ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
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

		if (ViewportWidget != nullptr)
		{
			ViewportWidget->Draw();
			if (ViewportWidget->WasTextureDrawn())
			{
				DrawViewportOrientationOverlay(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
			}
		}
		if (ViewportWidget == nullptr || !ViewportWidget->WasTextureDrawn())
		{
			ImGui::TextUnformatted("Viewport initializing...");
		}

		ImGui::End();
	}

	auto MLevelEditor::DrawViewportOrientationOverlay(const ImVec2& ViewportMin, const ImVec2& ViewportMax) const -> void
	{
		const ImVec2 ViewportSize(ViewportMax.x - ViewportMin.x, ViewportMax.y - ViewportMin.y);
		if (ViewportSize.x <= 8.0f || ViewportSize.y <= 8.0f)
		{
			return;
		}

		std::array<ImVec2, 3> AxisDirections = {
			ImVec2(1.0f, 0.0f),
			ImVec2(-0.62f, 0.38f),
			ImVec2(0.0f, -1.0f)
		};
		if (GEngine != nullptr)
		{
			if (const DCameraComponent* Camera = GEngine->GetActiveCameraComponent())
			{
				const FMatrix ViewMatrix = Camera->GetViewMatrix();
				AxisDirections[0] = GetScreenAxisDirection(ViewMatrix, FVectorConstants::Forward, AxisDirections[0]);
				AxisDirections[1] = GetScreenAxisDirection(ViewMatrix, FVectorConstants::Right, AxisDirections[1]);
				AxisDirections[2] = GetScreenAxisDirection(ViewMatrix, FVectorConstants::Up, AxisDirections[2]);
			}
		}

		const float AxisLength = FMath::Max(22.0f, FMath::Min(34.0f, FMath::Min(ViewportSize.x, ViewportSize.y) * 0.08f));
		const ImVec2 Origin(
			FMath::Max(ViewportMin.x + AxisLength + 18.0f, ViewportMax.x - 72.0f),
			FMath::Max(ViewportMin.y + AxisLength + 18.0f, ViewportMax.y - 46.0f)
		);

		ImDrawList* DrawList = ImGui::GetWindowDrawList();
		DrawList->PushClipRect(ViewportMin, ViewportMax, true);
		DrawList->AddCircleFilled(Origin, 3.0f, IM_COL32(235, 235, 235, 220));

		const std::array<ImU32, 3> AxisColors = {
			IM_COL32(255, 72, 72, 255),
			IM_COL32(72, 230, 96, 255),
			IM_COL32(80, 135, 255, 255)
		};
		const std::array<const char*, 3> AxisLabels = {"X", "Y", "Z"};
		for (uint32 AxisIndex = 0; AxisIndex < 3; ++AxisIndex)
		{
			const ImVec2 End = Add(Origin, Mul(AxisDirections[AxisIndex], AxisLength));
			DrawList->AddLine(Origin, End, IM_COL32(0, 0, 0, 150), 4.0f);
			DrawList->AddLine(Origin, End, AxisColors[AxisIndex], 2.0f);

			const ImVec2 TextSize = ImGui::CalcTextSize(AxisLabels[AxisIndex]);
			const ImVec2 LabelPosition = Add(End, Add(Mul(AxisDirections[AxisIndex], 5.0f), ImVec2(-TextSize.x * 0.5f, -TextSize.y * 0.5f)));
			DrawAxisText(DrawList, LabelPosition, AxisColors[AxisIndex], AxisLabels[AxisIndex]);
		}

		DrawList->PopClipRect();
	}

	auto MLevelEditor::UpdateViewportSize() -> void
	{
		ImVec2 AvailableSize = ImGui::GetContentRegionAvail();
		AvailableSize.x = FMath::Max(8.0f, AvailableSize.x);
		AvailableSize.y = FMath::Max(8.0f, AvailableSize.y);

		const FVector2f ViewportSize = {AvailableSize.x, AvailableSize.y};
		if (ViewportWidget != nullptr)
		{
			ViewportWidget->SetDesiredSize(ViewportSize);
		}
	}
}
