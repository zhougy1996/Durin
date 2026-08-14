#include "Customizations/CameraEditorCustomizations.h"

#include "Actors/CameraActor.h"
#include "Components/CameraComponent.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/Property.h"
#include "Editor/PropertyView.h"
#include "Engine/Level.h"
#include "Math/Operations.h"
#include "Workspace/LevelEditorContext.h"
#include "MonaImGui.h"
#include "SceneViewProjection.h"

namespace Durin::Editor::Level
{
	namespace
	{
		// Draws camera icons and frusta for camera components in the level viewport.
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
					: bPrimary ? MonaImGui::EUIThemeColor::Success
					: MonaImGui::EUIThemeColor::ViewportText;
				const ImVec4& ImColor = MonaImGui::GetThemeColor(ThemeColor);
				const FVector4f Color{ImColor.x, ImColor.y, ImColor.z, ImColor.w};
				const ImVec4& ImHoverColor = MonaImGui::GetThemeColor(MonaImGui::EUIThemeColor::Info);
				const std::optional<FVector4f> HoverColor = Context.bSelected
					? std::nullopt
					: std::optional<FVector4f>{{ImHoverColor.x, ImHoverColor.y, ImHoverColor.z, ImHoverColor.w}};

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
					.HoverColor = HoverColor,
				});
				if (!Context.bSelected) return;

				const FQuat Rotation = Camera->GetWorldRotation();
				const FVector3 Forward = Math::Normalize(Rotation * FVectorConstants::Forward);
				const FVector3 Right = Math::Normalize(Rotation * FVectorConstants::Right);
				const FVector3 Up = Math::Normalize(Rotation * FVectorConstants::Up);
				const double NearDistance = Camera->GetNearClip();
				const double FarDistance = Camera->GetFarClip();
				const double HalfFovTangent = std::tan(
					Math::DegreesToRadians(static_cast<double>(Camera->GetFieldOfViewDegrees())) * 0.5);
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

		// Adds camera projection controls to camera component details.
		class FCameraDetailsCustomization final : public IObjectDetailsCustomization
		{
		public:
			auto CustomizeDetails(FLevelEditorContext& Context, DObject* Object,
				FObjectPropertyViewBuilder& Builder) -> void override
			{
				auto* Camera = Cast<DCameraComponent>(Object);
				if (!Camera) return;
				Builder.HideProperty(Camera->GetClass()->FindPropertyByName("ProjectionSettings"));
				Builder.AddCustomRow(
					"Camera Projection Field Of View Near Clip Far Clip Aspect Ratio Custom Ratio",
					[&Context, Camera](::Durin::Editor::FPropertyView& PropertyView, const ::Durin::Editor::FPropertyViewContext& ViewContext) {
						return DrawCameraDetails(Context, *Camera, PropertyView, ViewContext);
					});
			}

		private:
			static auto DrawCameraDetails(FLevelEditorContext& Context, DCameraComponent& Camera,
				::Durin::Editor::FPropertyView& PropertyView, const ::Durin::Editor::FPropertyViewContext& ViewContext) -> bool
			{
				const FReflection Reflection = ResolveReflection(Camera);
				if (!Reflection.IsValid())
				{
					Context.SetError("Camera projection reflection metadata is unavailable.");
					return false;
				}

				ImGui::PushID(&Camera);
				DrawValue(PropertyView, ViewContext, Camera, Reflection, "Field Of View", Reflection.FieldOfView,
					0.1f, 1.0f, 170.0f, "%.1f deg");
				DrawValue(PropertyView, ViewContext, Camera, Reflection, "Near Clip", Reflection.NearClip,
					0.01f, 0.001f, std::numeric_limits<float>::max(), "%.3f");
				DrawValue(PropertyView, ViewContext, Camera, Reflection, "Far Clip", Reflection.FarClip,
					1.0f, Camera.GetNearClip() + 1.0f, std::numeric_limits<float>::max(), "%.1f");
				DrawValue(PropertyView, ViewContext, Camera, Reflection, "Terrain Fade Start",
					Reflection.TerrainFadeStart, 100.0f, 0.0f,
					Camera.GetProjectionSettings().TerrainRenderDistance - 1.0f, "%.1f");
				DrawValue(PropertyView, ViewContext, Camera, Reflection, "Terrain Render Distance",
					Reflection.TerrainRenderDistance, 100.0f,
					Camera.GetProjectionSettings().TerrainFadeStart + 1.0f,
					static_cast<float>(SceneViewProjection::GetMaximumTerrainRenderDistance(
						Camera.GetFarClip())), "%.1f");
				DrawAspectRatioMode(PropertyView, ViewContext, Camera, Reflection);
				if (Camera.GetAspectRatioMode() == ECameraAspectRatioMode::Custom)
					DrawValue(PropertyView, ViewContext, Camera, Reflection, "Custom Ratio", Reflection.CustomAspectRatio,
						0.01f, 0.1f, 10.0f, "%.3f");
				ImGui::PopID();
				return false;
			}
			// Caches reflected camera properties required by the custom details rows.
			struct FReflection
			{
				FStructProperty* Projection = nullptr;
				FProperty* FieldOfView = nullptr;
				FProperty* NearClip = nullptr;
				FProperty* FarClip = nullptr;
				FProperty* TerrainFadeStart = nullptr;
				FProperty* TerrainRenderDistance = nullptr;
				FProperty* AspectRatioMode = nullptr;
				FProperty* CustomAspectRatio = nullptr;

				auto IsValid() const -> bool
				{
					return Projection && FieldOfView && NearClip && FarClip
						&& TerrainFadeStart && TerrainRenderDistance
						&& AspectRatioMode && CustomAspectRatio;
				}
				auto GetSettings(DCameraComponent& Camera) const -> FCameraProjectionSettings*
				{
					return Projection->ContainerPtrToValuePtr<FCameraProjectionSettings>(&Camera);
				}
				auto MakeTarget(DCameraComponent& Camera, FProperty* Field) const -> ::Durin::Editor::FPropertyEditTarget
				{
					return ::Durin::Editor::FPropertyEditTarget::ForMember(&Camera, Projection).ForStructMember(Field);
				}
			};

			// Couples a display label with its fixed camera aspect ratio.
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

			static auto ResolveReflection(DCameraComponent& Camera) -> FReflection
			{
				FReflection Result;
				FProperty* Projection = Camera.GetClass()->FindPropertyByName("ProjectionSettings");
				if (!Projection || Projection->GetKind() != DurinCodeGen::EPropertyGenFlags::Struct) return Result;
				Result.Projection = static_cast<FStructProperty*>(Projection);
				DStruct* Struct = Result.Projection->GetStruct();
				if (!Struct) return Result;
				Result.FieldOfView = Struct->FindPropertyByName(FName("FieldOfViewDegrees"));
				Result.NearClip = Struct->FindPropertyByName(FName("NearClip"));
				Result.FarClip = Struct->FindPropertyByName(FName("FarClip"));
				Result.TerrainFadeStart = Struct->FindPropertyByName(
					FName("TerrainFadeStart"));
				Result.TerrainRenderDistance = Struct->FindPropertyByName(
					FName("TerrainRenderDistance"));
				Result.AspectRatioMode = Struct->FindPropertyByName(FName("AspectRatioMode"));
				Result.CustomAspectRatio = Struct->FindPropertyByName(FName("CustomAspectRatio"));
				return Result;
			}

			static auto FinishContinuousEdit(::Durin::Editor::FPropertyView& PropertyView, const ::Durin::Editor::FPropertyViewContext& ViewContext,
				const ::Durin::Editor::FPropertyEditTarget& Target, const MonaImGui::PropertyEdit::FWidgetState& State) -> void
			{
				if (State.bDeactivatedAfterEdit && PropertyView.IsEditingTarget(Target)) PropertyView.FinishActiveEdit(&ViewContext, false);
				else if (State.bActive && ImGui::IsKeyPressed(ImGuiKey_Escape) && PropertyView.IsEditingTarget(Target))
					PropertyView.FinishActiveEdit(&ViewContext, true);
			}

			static auto DrawAspectRatioMode(::Durin::Editor::FPropertyView& PropertyView, const ::Durin::Editor::FPropertyViewContext& ViewContext,
				DCameraComponent& Camera, const FReflection& Reflection) -> void
			{
				const ECameraAspectRatioMode CurrentMode = Camera.GetAspectRatioMode();
				const char* Preview = "Unknown";
				for (const FAspectRatioOption& Option : AspectRatioOptions)
				{
					if (Option.Mode == CurrentMode) Preview = Option.Label;
				}
				ImGui::PushID("AspectRatioMode");
				MonaImGui::PropertyEdit::BeginRow("Aspect Ratio", ViewContext.bReadOnly, 0.0f,
					"Type: Enum (Camera Aspect Ratio Mode)");
				if (ImGui::BeginCombo("##Value", Preview))
				{
					for (const FAspectRatioOption& Option : AspectRatioOptions)
					{
						if (ImGui::Selectable(Option.Label, Option.Mode == CurrentMode) && !ViewContext.bReadOnly)
						{
							PropertyView.SubmitPropertyValueEdit(ViewContext, Reflection.MakeTarget(Camera, Reflection.AspectRatioMode),
								[&](FProperty* ScratchProperty, void* ScratchContainer, uint32 ScratchArrayIndex) {
									*ScratchProperty->ContainerPtrToValuePtr<ECameraAspectRatioMode>(ScratchContainer, ScratchArrayIndex) = Option.Mode;
							}, false);
						}
					}
					ImGui::EndCombo();
				}
				MonaImGui::PropertyEdit::EndRow(ViewContext.bReadOnly);
				ImGui::PopID();
			}

			static auto DrawValue(::Durin::Editor::FPropertyView& PropertyView, const ::Durin::Editor::FPropertyViewContext& ViewContext,
				DCameraComponent& Camera, const FReflection& Reflection, const char* Label, FProperty* Field,
				float Speed, float Min, float Max, const char* Format) -> void
			{
				float Value = *Field->ContainerPtrToValuePtr<float>(Reflection.GetSettings(Camera));
				const ::Durin::Editor::FPropertyEditTarget Target = Reflection.MakeTarget(Camera, Field);
				ImGui::PushID(Field);
				MonaImGui::PropertyEdit::BeginRow(Label, ViewContext.bReadOnly, 0.0f, "Type: Float");
				const bool bChanged = ImGui::DragFloat("##Value", &Value, Speed, Min, Max, Format, ImGuiSliderFlags_AlwaysClamp);
				const MonaImGui::PropertyEdit::FWidgetState State{
					ImGui::IsItemActive(), ImGui::IsItemActivated(), ImGui::IsItemDeactivatedAfterEdit()};
				if (bChanged && !ViewContext.bReadOnly)
				{
					PropertyView.SubmitPropertyValueEdit(ViewContext, Target,
						[&](FProperty* ScratchProperty, void* ScratchContainer, uint32 ScratchArrayIndex) {
							*ScratchProperty->ContainerPtrToValuePtr<float>(ScratchContainer, ScratchArrayIndex) = Value;
					}, true);
				}
				FinishContinuousEdit(PropertyView, ViewContext, Target, State);
				MonaImGui::PropertyEdit::EndRow(ViewContext.bReadOnly);
				ImGui::PopID();
			}
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
