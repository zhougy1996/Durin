#include "CameraEditorCustomizations.h"

#include "Actors/CameraActor.h"
#include "Components/CameraComponent.h"
#include "Editor/EditorEngine.h"
#include "Editor/EditorTransaction.h"
#include "Engine/Level.h"
#include "LevelEditorContext.h"
#include "MonaImGui.h"

namespace Durin
{
	namespace
	{
		struct FCameraProjectionSettings
		{
			float FieldOfViewDegrees = 60.0f;
			float NearClip = 0.1f;
			float FarClip = 1000.0f;

			auto operator==(const FCameraProjectionSettings&) const -> bool = default;
		};

		auto GetProjectionSettings(const DCameraComponent* Camera) -> FCameraProjectionSettings
		{
			return {Camera->GetFieldOfViewDegrees(), Camera->GetNearClip(), Camera->GetFarClip()};
		}

		auto ApplyProjectionSettings(DCameraComponent* Camera, const FCameraProjectionSettings& Settings) -> bool
		{
			if (!Camera) return false;
			Camera->SetProjectionParameters(Settings.FieldOfViewDegrees, Settings.NearClip, Settings.FarClip);
			return true;
		}

		class FCameraProjectionTransaction final : public IEditorTransaction
		{
		public:
			FCameraProjectionTransaction(DCameraComponent* InCamera, FCameraProjectionSettings InBefore, FCameraProjectionSettings InAfter)
				: Camera(InCamera), Before(InBefore), After(InAfter) {}

			auto GetDescription() const -> std::string_view override { return "Edit camera projection"; }
			auto GetDetails(EEditorTransactionOperation) const -> std::string override
			{
				return Camera ? std::format("Camera '{}' projection: FOV {:.1f}, Near {:.3f}, Far {:.3f}", Camera->GetName(), After.FieldOfViewDegrees, After.NearClip, After.FarClip) : std::string{};
			}
			auto Undo() -> bool override { return ApplyProjectionSettings(Camera.Get(), Before); }
			auto Redo() -> bool override { return ApplyProjectionSettings(Camera.Get(), After); }

		private:
			TObjectPtr<DCameraComponent> Camera;
			FCameraProjectionSettings Before;
			FCameraProjectionSettings After;
		};

		class FCameraComponentVisualizer final : public IComponentEditorVisualizer
		{
		public:
			auto DrawVisualization(DActorComponent* Component, const FEditorVisualizationContext& Context, FEditorVisualizationCollector& Collector) const -> void override
			{
				auto* Camera = Cast<DCameraComponent>(Component);
				AActor* Actor = Camera ? Camera->GetOwner() : nullptr;
				if (!Camera || !Actor || Context.View.ViewportHeight == 0) return;

				const bool bPrimary = Context.Level && Context.Level->GetPrimaryCameraActor() == Actor;
				const MonaImGui::EUIThemeColor ThemeColor = Context.bSelected ? MonaImGui::EUIThemeColor::SelectionPrimary
					: Context.bHovered ? MonaImGui::EUIThemeColor::Info
					: bPrimary ? MonaImGui::EUIThemeColor::Success
					: MonaImGui::EUIThemeColor::ViewportText;
				const ImVec4& ImColor = MonaImGui::GetThemeColor(ThemeColor);
				const FVector4f Color{ImColor.x, ImColor.y, ImColor.z, ImColor.w};
				const float Width = Context.bSelected || Context.bHovered ? 2.5f : bPrimary ? 2.0f : 1.5f;

				const FVector3 Origin = Camera->GetWorldLocation();
				const FQuat Rotation = Camera->GetWorldRotation();
				const FVector3 Forward = glm::normalize(Rotation * FVectorConstants::Forward);
				const FVector3 Right = glm::normalize(Rotation * FVectorConstants::Right);
				const FVector3 Up = glm::normalize(Rotation * FVectorConstants::Up);
				const double Distance = std::max(0.05, glm::length(Origin - Context.View.ViewLocation));
				const double Scale = Distance * 2.0 * std::tan(glm::radians(60.0) * 0.5) * 44.0 / Context.View.ViewportHeight;
				const double FrustumLength = Scale * 2.2;
				const double HalfHeight = std::tan(glm::radians(static_cast<double>(Camera->GetFieldOfViewDegrees())) * 0.5) * FrustumLength;
				const double AspectRatio = static_cast<double>(Context.View.ViewportWidth) / Context.View.ViewportHeight;
				const double HalfWidth = HalfHeight * std::max(AspectRatio, 0.001);
				const FVector3 FarCenter = Origin + Forward * FrustumLength;
				const std::array<FVector3, 4> FrustumCorners = {
					FarCenter + Right * HalfWidth + Up * HalfHeight,
					FarCenter - Right * HalfWidth + Up * HalfHeight,
					FarCenter - Right * HalfWidth - Up * HalfHeight,
					FarCenter + Right * HalfWidth - Up * HalfHeight,
				};

				auto AddLine = [&](const FVector3& Start, const FVector3& End, int32 Priority = 10) {
					Collector.AddLine({Start, End, Color, Width, 7.0f, Priority, Actor, Camera});
				};
				for (const FVector3& Corner : FrustumCorners) AddLine(Origin, Corner);
				for (size_t Index = 0; Index < FrustumCorners.size(); ++Index) AddLine(FrustumCorners[Index], FrustumCorners[(Index + 1) % FrustumCorners.size()]);

				const FVector3 BodyCenter = Origin - Forward * (Scale * 0.12);
				const FVector3 BodyForward = Forward * (Scale * 0.34);
				const FVector3 BodyRight = Right * (Scale * 0.38);
				const FVector3 BodyUp = Up * (Scale * 0.28);
				std::array<FVector3, 8> BodyCorners;
				for (uint32 Corner = 0; Corner < 8; ++Corner)
				{
					BodyCorners[Corner] = BodyCenter
						+ ((Corner & 1) ? BodyForward : -BodyForward)
						+ ((Corner & 2) ? BodyRight : -BodyRight)
						+ ((Corner & 4) ? BodyUp : -BodyUp);
				}
				static constexpr uint32 BodyEdges[] = {0,1,0,2,0,4,1,3,1,5,2,3,2,6,3,7,4,5,4,6,5,7,6,7};
				for (size_t Index = 0; Index < std::size(BodyEdges); Index += 2) AddLine(BodyCorners[BodyEdges[Index]], BodyCorners[BodyEdges[Index + 1]], 20);
			}
		};

		class FCameraDetailsCustomization final : public IObjectDetailsCustomization
		{
		public:
			auto DrawDetails(FLevelEditorContext&, DObject* Object) -> bool override
			{
				DCameraComponent* Camera = Cast<DCameraComponent>(Object);
				if (!Camera)
				{
					if (auto* Actor = Cast<ACameraActor>(Object)) Camera = Actor->GetCameraComponent();
				}
				if (!Camera) return false;

				ImGui::PushID(Camera);
				DrawValue(Camera, "Field Of View", EField::FieldOfView, 0.1f, 1.0f, 170.0f, "%.1f deg");
				DrawValue(Camera, "Near Clip", EField::NearClip, 0.01f, 0.001f, std::numeric_limits<float>::max(), "%.3f");
				DrawValue(Camera, "Far Clip", EField::FarClip, 1.0f, Camera->GetNearClip() + 1.0f, std::numeric_limits<float>::max(), "%.1f");
				ImGui::PopID();
				return Cast<DCameraComponent>(Object) != nullptr;
			}

		private:
			enum class EField : uint8 { FieldOfView, NearClip, FarClip };

			auto DrawValue(DCameraComponent* Camera, const char* Label, EField Field, float Speed, float Min, float Max, const char* Format) -> void
			{
				const FCameraProjectionSettings BeforeWidget = GetProjectionSettings(Camera);
				float Value = Field == EField::FieldOfView ? BeforeWidget.FieldOfViewDegrees : Field == EField::NearClip ? BeforeWidget.NearClip : BeforeWidget.FarClip;
				ImGui::PushID(static_cast<int>(Field));
				MonaImGui::BeginPropertyRow(Label, false);
				const bool bChanged = ImGui::DragFloat("##Value", &Value, Speed, Min, Max, Format, ImGuiSliderFlags_AlwaysClamp);
				if (ImGui::IsItemActivated())
				{
					EditingCamera = Camera;
					EditingBefore = BeforeWidget;
				}
				if (bChanged)
				{
					FCameraProjectionSettings Changed = BeforeWidget;
					if (Field == EField::FieldOfView) Changed.FieldOfViewDegrees = Value;
					else if (Field == EField::NearClip) Changed.NearClip = Value;
					else Changed.FarClip = Value;
					ApplyProjectionSettings(Camera, Changed);
				}
				if (ImGui::IsItemDeactivatedAfterEdit() && EditingCamera.Get() == Camera)
				{
					const FCameraProjectionSettings After = GetProjectionSettings(Camera);
					if (After != EditingBefore && GEditor)
						GEditor->GetTransactionManager().CommitApplied(std::make_unique<FCameraProjectionTransaction>(Camera, EditingBefore, After));
					EditingCamera = nullptr;
				}
				MonaImGui::EndPropertyRow(false);
				ImGui::PopID();
			}

			TObjectPtr<DCameraComponent> EditingCamera;
			FCameraProjectionSettings EditingBefore;
		};
	} // namespace

	auto CreateCameraComponentVisualizer() -> std::shared_ptr<IComponentEditorVisualizer>
	{
		return std::make_shared<FCameraComponentVisualizer>();
	}

	auto CreateCameraDetailsCustomization() -> std::shared_ptr<IObjectDetailsCustomization>
	{
		return std::make_shared<FCameraDetailsCustomization>();
	}
}
