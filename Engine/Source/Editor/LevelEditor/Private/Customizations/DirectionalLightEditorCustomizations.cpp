#include "Customizations/DirectionalLightEditorCustomizations.h"

#include "Components/DirectionalLightComponent.h"
#include "Math/Operations.h"
#include "MonaImGui.h"

namespace Durin::Editor::Level
{
	namespace
	{
		// Draws the direction indicator for directional-light components.
		class FDirectionalLightComponentVisualizer final : public IComponentEditorVisualizer
		{
		public:
			auto DrawVisualization(DActorComponent* Component, const FEditorVisualizationContext& Context, FEditorVisualizationCollector& Collector) const -> void override
			{
				auto* Light = Cast<DDirectionalLightComponent>(Component);
				AActor* Actor = Light ? Light->GetOwner() : nullptr;
				if (!Light || !Actor || Context.View.ViewportHeight == 0) return;

				const MonaImGui::EUIThemeColor ThemeColor = Context.bSelected ? MonaImGui::EUIThemeColor::SelectionPrimary : MonaImGui::EUIThemeColor::Warning;
				const ImVec4& ImColor = MonaImGui::GetThemeColor(ThemeColor);
				const FVector4f Color{ImColor.x, ImColor.y, ImColor.z, ImColor.w};
				const ImVec4& ImHoverColor = MonaImGui::GetThemeColor(MonaImGui::EUIThemeColor::Info);
				const std::optional<FVector4f> HoverColor = Context.bSelected
					? std::nullopt
					: std::optional<FVector4f>{{ImHoverColor.x, ImHoverColor.y, ImHoverColor.z, ImHoverColor.w}};
				const FVector3 Origin = Light->GetWorldLocation();
				Collector.AddIcon({
					.Icon = EViewOverlayIcon::DirectionalLight,
					.WorldPosition = Origin,
					.Color = Color,
					.SizePixels = MonaImGui::ScaleUI(Context.bSelected ? 40.0f : 36.0f),
					.HitPaddingPixels = MonaImGui::ScaleUI(4.0f),
					.HitPriority = 100,
					.Actor = Actor,
					.Component = Light,
					.bDepthIndependentHit = true,
					.HoverColor = HoverColor,
				});
				if (!Context.bSelected) return;

				const FQuat Rotation = Light->GetWorldRotation();
				const FVector3 Forward = Math::Normalize(Rotation * FVectorConstants::Forward);
				const FVector3 Right = Math::Normalize(Rotation * FVectorConstants::Right);
				const FVector3 Up = Math::Normalize(Rotation * FVectorConstants::Up);
				// Keep the direction cue legible at a distance without reusing the solid transform-gizmo
				// arrow. The open four-sided head reads as orientation instead of an editable axis.
				const double ViewDistance = Math::Length(Origin - Context.View.ViewLocation);
				const double DirectionLength = std::max(1.0, ViewDistance * 0.1);
				const double HeadLength = DirectionLength * 0.22;
				const double HeadRadius = DirectionLength * 0.1;
				const FVector3 Tip = Origin + Forward * DirectionLength;
				const FVector3 HeadBase = Tip - Forward * HeadLength;
				const float LineWidth = MonaImGui::ScaleUI(2.0f);
				auto AddDirectionLine = [&](const FVector3& Start, const FVector3& End) {
					Collector.AddLine({Start, End, Color, LineWidth, MonaImGui::ScaleUI(6.0f),
						80, Actor, Light});
				};
				AddDirectionLine(Origin, Tip);
				AddDirectionLine(Tip, HeadBase + Right * HeadRadius);
				AddDirectionLine(Tip, HeadBase - Right * HeadRadius);
				AddDirectionLine(Tip, HeadBase + Up * HeadRadius);
				AddDirectionLine(Tip, HeadBase - Up * HeadRadius);
			}
		};
	} // namespace

	auto CreateDirectionalLightComponentVisualizer() -> std::shared_ptr<IComponentEditorVisualizer>
	{
		return std::make_shared<FDirectionalLightComponentVisualizer>();
	}
}
