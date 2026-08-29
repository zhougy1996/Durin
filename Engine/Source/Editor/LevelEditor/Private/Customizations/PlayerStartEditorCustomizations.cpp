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
				Collector.AddIcon({
					.Icon = EEditorVisualizationIcon::PlayerStart,
					.WorldPosition = Origin,
					.Color = Color,
					.SizePixels = MonaImGui::ScaleUI(Context.bSelected ? 40.0f : 36.0f),
					.HitPaddingPixels = MonaImGui::ScaleUI(4.0f),
					.HitPriority = 100,
					.Actor = PlayerStart,
					.Component = Root,
					.bDepthIndependentHit = true,
					.HoverColor = HoverColor,
				});
				if (!Context.bSelected) return;

				constexpr double Radius = 0.4;
				constexpr double CylinderHalfHeight = 0.6;
				constexpr uint32 RingSegmentCount = 24;
				constexpr uint32 ArcSegmentCount = 12;
				const float OutlineWidth = MonaImGui::ScaleUI(Context.bSelected ? 2.0f : 1.5f);
				auto AddLine = [&](const FVector3& Start, const FVector3& End, float Width = 0.0f, int32 Priority = 80) {
					Collector.AddLine({Start, End, Color, Width > 0.0f ? Width : OutlineWidth,
						MonaImGui::ScaleUI(6.0f), Priority, PlayerStart, Root,
						ESimpleElementLinePattern::Solid, 12.0f, HoverColor});
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

				const FVector3 ArrowStart = Origin + Forward * Radius;
				constexpr double ArrowLength = 0.75;
				constexpr double HeadLength = 0.2;
				constexpr double HeadRadius = 0.12;
				const FVector3 ArrowTip = ArrowStart + Forward * ArrowLength;
				const FVector3 HeadBase = ArrowTip - Forward * HeadLength;
				AddLine(ArrowStart, ArrowTip);
				AddLine(ArrowTip, HeadBase + Right * HeadRadius);
				AddLine(ArrowTip, HeadBase - Right * HeadRadius);
				AddLine(ArrowTip, HeadBase + Up * HeadRadius);
				AddLine(ArrowTip, HeadBase - Up * HeadRadius);
			}
		};
	} // namespace

	auto CreatePlayerStartActorVisualizer() -> std::shared_ptr<IActorEditorVisualizer>
	{
		return std::make_shared<FPlayerStartActorVisualizer>();
	}
}
