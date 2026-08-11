#include "Customizations/PlayerStartEditorCustomizations.h"

#include "Actors/PlayerStart.h"
#include "Components/SceneComponent.h"
#include "Math/Operations.h"
#include "MonaImGui.h"

namespace Durin::Editor::Level
{
	namespace
	{
		// Draws a selectable spawn-volume outline and facing cue for player-start actors.
		class FPlayerStartActorVisualizer final : public IActorEditorVisualizer
		{
		public:
			auto DrawVisualization(AActor* Actor, const FEditorVisualizationContext& Context,
				FEditorVisualizationCollector& Collector) const -> void override
			{
				auto* PlayerStart = Cast<APlayerStart>(Actor);
				DSceneComponent* Root = PlayerStart ? PlayerStart->GetRootComponent() : nullptr;
				if (!PlayerStart || !Root) return;

				const MonaImGui::EUIThemeColor ThemeColor = Context.bSelected
					? MonaImGui::EUIThemeColor::SelectionPrimary
					: MonaImGui::EUIThemeColor::Success;
				const ImVec4& ImColor = MonaImGui::GetThemeColor(ThemeColor);
				const FVector4f Color{ImColor.x, ImColor.y, ImColor.z,
					Context.bSelected ? ImColor.w : ImColor.w * 0.72f};
				const ImVec4& ImHoverColor = MonaImGui::GetThemeColor(MonaImGui::EUIThemeColor::Info);
				const std::optional<FVector4f> HoverColor = Context.bSelected
					? std::nullopt
					: std::optional<FVector4f>{{ImHoverColor.x, ImHoverColor.y, ImHoverColor.z, ImHoverColor.w}};

				const FVector3 Origin = Root->GetWorldLocation();
				const FQuat Rotation = Root->GetWorldRotation();
				const FVector3 Forward = Math::Normalize(Rotation * FVectorConstants::Forward);
				const FVector3 Right = Math::Normalize(Rotation * FVectorConstants::Right);
				const FVector3 Up = Math::Normalize(Rotation * FVectorConstants::Up);
				constexpr double Radius = 0.4;
				constexpr double CylinderHalfHeight = 0.6;
				constexpr uint32 RingSegmentCount = 24;
				constexpr uint32 ArcSegmentCount = 12;
				const float OutlineWidth = MonaImGui::ScaleUI(Context.bSelected ? 2.0f : 1.5f);
				auto AddLine = [&](const FVector3& Start, const FVector3& End, float Width = 0.0f, int32 Priority = 80) {
					Collector.AddLine({Start, End, Color, Width > 0.0f ? Width : OutlineWidth,
						MonaImGui::ScaleUI(6.0f), Priority, PlayerStart, Root,
						EViewOverlayLinePattern::Solid, 12.0f, HoverColor});
				};

				for (uint32 Index = 0; Index < RingSegmentCount; ++Index)
				{
					const double AngleA = Math::TwoPi<double>() * static_cast<double>(Index) / RingSegmentCount;
					const double AngleB = Math::TwoPi<double>() * static_cast<double>(Index + 1) / RingSegmentCount;
					const FVector3 RadialA = Right * std::cos(AngleA) * Radius + Forward * std::sin(AngleA) * Radius;
					const FVector3 RadialB = Right * std::cos(AngleB) * Radius + Forward * std::sin(AngleB) * Radius;
					AddLine(Origin + Up * CylinderHalfHeight + RadialA, Origin + Up * CylinderHalfHeight + RadialB);
					AddLine(Origin - Up * CylinderHalfHeight + RadialA, Origin - Up * CylinderHalfHeight + RadialB);
					if (Index % (RingSegmentCount / 4) == 0)
						AddLine(Origin - Up * CylinderHalfHeight + RadialA, Origin + Up * CylinderHalfHeight + RadialA);
				}
				for (const FVector3& ArcAxis : {Right, Forward})
				{
					for (uint32 Index = 0; Index < ArcSegmentCount; ++Index)
					{
						const double AngleA = Math::Pi<double>() * static_cast<double>(Index) / ArcSegmentCount;
						const double AngleB = Math::Pi<double>() * static_cast<double>(Index + 1) / ArcSegmentCount;
						const FVector3 TopA = Origin + ArcAxis * (std::cos(AngleA) * Radius)
							+ Up * (CylinderHalfHeight + std::sin(AngleA) * Radius);
						const FVector3 TopB = Origin + ArcAxis * (std::cos(AngleB) * Radius)
							+ Up * (CylinderHalfHeight + std::sin(AngleB) * Radius);
						const FVector3 BottomA = Origin + ArcAxis * (std::cos(AngleA) * Radius)
							- Up * (CylinderHalfHeight + std::sin(AngleA) * Radius);
						const FVector3 BottomB = Origin + ArcAxis * (std::cos(AngleB) * Radius)
							- Up * (CylinderHalfHeight + std::sin(AngleB) * Radius);
						AddLine(TopA, TopB);
						AddLine(BottomA, BottomB);
					}
				}

				const float ArrowWidth = MonaImGui::ScaleUI(Context.bSelected ? 2.5f : 2.0f);
				const FVector3 ArrowStart = Origin + Forward * Radius;
				const FVector3 ArrowBase = Origin + Forward * 0.95;
				const FVector3 ArrowTip = Origin + Forward * 1.35;
				const FVector3 ArrowRight = ArrowBase + Right * 0.22;
				const FVector3 ArrowLeft = ArrowBase - Right * 0.22;
				AddLine(ArrowStart, ArrowTip, ArrowWidth, 90);
				AddLine(ArrowTip, ArrowRight, ArrowWidth, 90);
				AddLine(ArrowRight, ArrowLeft, ArrowWidth, 90);
				AddLine(ArrowLeft, ArrowTip, ArrowWidth, 90);
				// Preserve a generous depth-independent picking anchor without rendering a center cube.
				Collector.AddBox({Origin, FVector4f(0.0f), MonaImGui::ScaleUI(12.0f),
					MonaImGui::ScaleUI(5.0f), 100, PlayerStart, Root, true});
			}
		};
	} // namespace

	auto CreatePlayerStartActorVisualizer() -> std::shared_ptr<IActorEditorVisualizer>
	{
		return std::make_shared<FPlayerStartActorVisualizer>();
	}
}
