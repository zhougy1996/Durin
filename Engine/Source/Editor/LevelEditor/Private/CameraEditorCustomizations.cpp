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
			ECameraAspectRatioMode AspectRatioMode = ECameraAspectRatioMode::Viewport;
			float CustomAspectRatio = 16.0f / 9.0f;

			auto operator==(const FCameraProjectionSettings&) const -> bool = default;
		};

		auto GetProjectionSettings(const DCameraComponent* Camera) -> FCameraProjectionSettings
		{
			return {Camera->GetFieldOfViewDegrees(), Camera->GetNearClip(), Camera->GetFarClip(), Camera->GetAspectRatioMode(), Camera->GetCustomAspectRatio()};
		}

		auto ApplyProjectionSettings(DCameraComponent* Camera, const FCameraProjectionSettings& Settings) -> bool
		{
			if (!Camera) return false;
			Camera->SetProjectionParameters(Settings.FieldOfViewDegrees, Settings.NearClip, Settings.FarClip);
			Camera->SetAspectRatio(Settings.AspectRatioMode, Settings.CustomAspectRatio);
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
				return Camera ? std::format("Camera '{}' projection: FOV {:.1f}, Near {:.3f}, Far {:.3f}, Aspect {:.3f}", Camera->GetName(), After.FieldOfViewDegrees, After.NearClip, After.FarClip, Camera->ResolveAspectRatio(After.CustomAspectRatio)) : std::string{};
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

				const FVector3 Origin = Camera->GetWorldLocation();
				Collector.AddIcon({
					.Icon = EViewOverlayIcon::Camera,
					.WorldPosition = Origin,
					.Color = Color,
					.SizePixels = MonaImGui::ScaleUI(Context.bSelected ? 40.0f : 36.0f),
					.HitPaddingPixels = MonaImGui::ScaleUI(4.0f),
					.HitPriority = 100,
					.Actor = Actor,
					.Component = Camera,
					.bDepthIndependentHit = true,
				});
				if (!Context.bSelected) return;

				const FQuat Rotation = Camera->GetWorldRotation();
				const FVector3 Forward = glm::normalize(Rotation * FVectorConstants::Forward);
				const FVector3 Right = glm::normalize(Rotation * FVectorConstants::Right);
				const FVector3 Up = glm::normalize(Rotation * FVectorConstants::Up);
				const double NearDistance = Camera->GetNearClip();
				const double FarDistance = Camera->GetFarClip();
				const double HalfFovTangent = std::tan(glm::radians(static_cast<double>(Camera->GetFieldOfViewDegrees())) * 0.5);
				const double ViewportAspectRatio = static_cast<double>(Context.View.ViewportWidth) / Context.View.ViewportHeight;
				const double AspectRatio = Camera->ResolveAspectRatio(static_cast<float>(ViewportAspectRatio));
				const double SafeAspectRatio = std::max(AspectRatio, 0.001);
				auto MakePlaneCorners = [&](double PlaneDistance) {
					const double HalfHeight = HalfFovTangent * PlaneDistance;
					const double HalfWidth = HalfHeight * SafeAspectRatio;
					const FVector3 Center = Origin + Forward * PlaneDistance;
					return std::array<FVector3, 4>{
						Center + Right * HalfWidth + Up * HalfHeight,
						Center - Right * HalfWidth + Up * HalfHeight,
						Center - Right * HalfWidth - Up * HalfHeight,
						Center + Right * HalfWidth - Up * HalfHeight,
					};
				};
				const std::array<FVector3, 4> NearCorners = MakePlaneCorners(NearDistance);
				const std::array<FVector3, 4> FarCorners = MakePlaneCorners(FarDistance);
				const float Width = 2.5f;

				auto AddLine = [&](const FVector3& Start, const FVector3& End, EViewOverlayLinePattern Pattern = EViewOverlayLinePattern::Solid, int32 Priority = 10) {
					Collector.AddLine({Start, End, Color, Width, 7.0f, Priority, Actor, Camera, Pattern, 12.0f});
				};
				for (size_t Index = 0; Index < NearCorners.size(); ++Index)
				{
					const size_t Next = (Index + 1) % NearCorners.size();
					AddLine(NearCorners[Index], NearCorners[Next]);
					AddLine(NearCorners[Index], FarCorners[Index]);
					AddLine(FarCorners[Index], FarCorners[Next]);
				}
				// Keep the orientation cue stable in world space instead of tying it to the editor view distance.
				const double DirectionLength = std::max(NearDistance, FarDistance * 0.1);
				const ImVec4& ImForwardColor = MonaImGui::GetThemeColor(MonaImGui::EUIThemeColor::AxisX);
				const FVector4f ForwardColor{ImForwardColor.x, ImForwardColor.y, ImForwardColor.z, ImForwardColor.w};
				Collector.AddLine({Origin, Origin + Forward * DirectionLength, ForwardColor, 3.0f, 7.0f, 20, Actor, Camera});
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
				DrawAspectRatioMode(Camera);
				if (Camera->GetAspectRatioMode() == ECameraAspectRatioMode::Custom) DrawCustomAspectRatio(Camera);
				ImGui::PopID();
				return Cast<DCameraComponent>(Object) != nullptr;
			}

		private:
			enum class EField : uint8 { FieldOfView, NearClip, FarClip };
			struct FAspectRatioOption
			{
				ECameraAspectRatioMode Mode;
				const char* Label;
			};

			static constexpr std::array AspectRatioOptions = {
				FAspectRatioOption{ECameraAspectRatioMode::Viewport, "Viewport"},
				FAspectRatioOption{ECameraAspectRatioMode::Ratio16By9, "16:9"},
				FAspectRatioOption{ECameraAspectRatioMode::Ratio16By10, "16:10"},
				FAspectRatioOption{ECameraAspectRatioMode::Ratio4By3, "4:3"},
				FAspectRatioOption{ECameraAspectRatioMode::Ratio1By1, "1:1"},
				FAspectRatioOption{ECameraAspectRatioMode::Custom, "Custom"}
			};

			auto DrawAspectRatioMode(DCameraComponent* Camera) -> void
			{
				const FCameraProjectionSettings Before = GetProjectionSettings(Camera);
				const char* Preview = "Unknown";
				for (const FAspectRatioOption& Option : AspectRatioOptions)
				{
					if (Option.Mode == Before.AspectRatioMode) Preview = Option.Label;
				}
				ImGui::PushID("AspectRatioMode");
				MonaImGui::BeginPropertyRow("Aspect Ratio", false);
				if (ImGui::BeginCombo("##Value", Preview))
				{
					for (const FAspectRatioOption& Option : AspectRatioOptions)
					{
						if (ImGui::Selectable(Option.Label, Option.Mode == Before.AspectRatioMode))
						{
							FCameraProjectionSettings After = Before;
							After.AspectRatioMode = Option.Mode;
							ApplyProjectionSettings(Camera, After);
							if (After != Before && GEditor) GEditor->GetTransactionManager().CommitApplied(std::make_unique<FCameraProjectionTransaction>(Camera, Before, After));
						}
					}
					ImGui::EndCombo();
				}
				MonaImGui::EndPropertyRow(false);
				ImGui::PopID();
			}

			auto DrawCustomAspectRatio(DCameraComponent* Camera) -> void
			{
				const FCameraProjectionSettings BeforeWidget = GetProjectionSettings(Camera);
				float Value = BeforeWidget.CustomAspectRatio;
				MonaImGui::BeginPropertyRow("Custom Ratio", false);
				const bool bChanged = ImGui::DragFloat("##CustomAspectRatio", &Value, 0.01f, 0.1f, 10.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
				if (ImGui::IsItemActivated())
				{
					EditingCamera = Camera;
					EditingBefore = BeforeWidget;
				}
				if (bChanged)
				{
					FCameraProjectionSettings Changed = BeforeWidget;
					Changed.CustomAspectRatio = Value;
					ApplyProjectionSettings(Camera, Changed);
				}
				CommitActiveEdit(Camera);
				MonaImGui::EndPropertyRow(false);
			}

			auto CommitActiveEdit(DCameraComponent* Camera) -> void
			{
				if (ImGui::IsItemDeactivatedAfterEdit() && EditingCamera.Get() == Camera)
				{
					const FCameraProjectionSettings After = GetProjectionSettings(Camera);
					if (After != EditingBefore && GEditor)
						GEditor->GetTransactionManager().CommitApplied(std::make_unique<FCameraProjectionTransaction>(Camera, EditingBefore, After));
					EditingCamera = nullptr;
				}
			}

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
				CommitActiveEdit(Camera);
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
