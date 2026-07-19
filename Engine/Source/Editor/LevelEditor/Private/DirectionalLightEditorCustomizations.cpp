#include "DirectionalLightEditorCustomizations.h"

#include "Components/DirectionalLightComponent.h"
#include "MonaImGui.h"

namespace Durin
{
	namespace
	{
		class FDirectionalLightComponentVisualizer final : public IComponentEditorVisualizer
		{
		public:
			auto DrawVisualization(DActorComponent* Component, const FEditorVisualizationContext& Context, FEditorVisualizationCollector& Collector) const -> void override
			{
				auto* Light = Cast<DDirectionalLightComponent>(Component);
				AActor* Actor = Light ? Light->GetOwner() : nullptr;
				if (!Light || !Actor || Context.View.ViewportHeight == 0) return;

				const MonaImGui::EUIThemeColor ThemeColor = Context.bSelected ? MonaImGui::EUIThemeColor::SelectionPrimary
					: Context.bHovered ? MonaImGui::EUIThemeColor::Info
					: MonaImGui::EUIThemeColor::Warning;
				const ImVec4& ImColor = MonaImGui::GetThemeColor(ThemeColor);
				const FVector4f Color{ImColor.x, ImColor.y, ImColor.z, ImColor.w};
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
				});
				if (!Context.bSelected) return;

				const FQuat Rotation = Light->GetWorldRotation();
				const FVector3 Forward = glm::normalize(Rotation * FVectorConstants::Forward);
				const FVector3 Right = glm::normalize(Rotation * FVectorConstants::Right);
				const FVector3 Up = glm::normalize(Rotation * FVectorConstants::Up);
				// Directional lights have no physical extent, so scale the cue with viewing distance
				// while preserving the component's origin as the transform and picking anchor.
				const double ViewDistance = glm::length(Origin - Context.View.ViewLocation);
				const double DirectionLength = std::max(1.0, ViewDistance * 0.2);
				const double HeadLength = DirectionLength * 0.28;
				const double HeadRadius = DirectionLength * 0.13;
				const FVector3 Tip = Origin + Forward * DirectionLength;
				const FVector3 HeadCenter = Tip - Forward * HeadLength;
				auto AddLine = [&](const FVector3& Start, const FVector3& End, int32 Priority = 20) {
					Collector.AddLine({Start, End, Color, 3.0f, 7.0f, Priority, Actor, Light});
				};
				AddLine(Origin, Tip);
				AddLine(Tip, HeadCenter + Right * HeadRadius);
				AddLine(Tip, HeadCenter - Right * HeadRadius);
				AddLine(Tip, HeadCenter + Up * HeadRadius);
				AddLine(Tip, HeadCenter - Up * HeadRadius);
			}
		};
	} // namespace

	auto CreateDirectionalLightComponentVisualizer() -> std::shared_ptr<IComponentEditorVisualizer>
	{
		return std::make_shared<FDirectionalLightComponentVisualizer>();
	}
}
