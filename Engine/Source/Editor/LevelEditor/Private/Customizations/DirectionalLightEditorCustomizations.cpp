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
				// Directional lights have no physical extent, so scale the cue with viewing distance
				// while preserving the component's origin as the transform and picking anchor.
				const double ViewDistance = Math::Length(Origin - Context.View.ViewLocation);
				const double DirectionLength = std::max(1.0, ViewDistance * 0.2);
				Collector.AddPrimitive({
					.Shape = EViewOverlayShape::Arrow,
					.LocalToWorld = Math::TranslationMatrix(Origin)
						* Math::RotationMatrix(Rotation)
						* Math::ScaleMatrix(FVector3(DirectionLength)),
					.Color = Color,
					.Actor = Actor,
					.Component = Light,
				});
			}
		};
	} // namespace

	auto CreateDirectionalLightComponentVisualizer() -> std::shared_ptr<IComponentEditorVisualizer>
	{
		return std::make_shared<FDirectionalLightComponentVisualizer>();
	}
}
