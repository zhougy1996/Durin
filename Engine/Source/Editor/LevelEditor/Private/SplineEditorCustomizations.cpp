#include "SplineEditorCustomizations.h"

#include "Components/SplineComponent.h"
#include "Engine/Actor.h"
#include "LevelEditorContext.h"
#include "MonaImGui.h"

namespace Durin
{
	namespace
	{
		auto ToColor(MonaImGui::EUIThemeColor ThemeColor) -> FVector4f
		{
			const ImVec4& Color = MonaImGui::GetThemeColor(ThemeColor);
			return {Color.x, Color.y, Color.z, Color.w};
		}

		auto TransformLocalPosition(const DSplineComponent& Spline, const FVector3& LocalPosition) -> FVector3
		{
			return FVector3(Spline.GetComponentToWorldMatrix() * FVector4(LocalPosition, 1.0));
		}

		class FSplineComponentVisualizer final : public IComponentEditorVisualizer
		{
		public:
			auto DrawVisualization(DActorComponent* Component, const FEditorVisualizationContext& Context, FEditorVisualizationCollector& Collector) const -> void override
			{
				auto* Spline = Cast<DSplineComponent>(Component);
				AActor* Actor = Spline ? Spline->GetOwner() : nullptr;
				if (!Spline || !Actor) return;

				const FVector4f CurveColor = ToColor(Context.bSelected ? MonaImGui::EUIThemeColor::SelectionPrimary
					: Context.bHovered ? MonaImGui::EUIThemeColor::Info
					: MonaImGui::EUIThemeColor::ViewportText);
				const uint32 SegmentCount = Spline->GetNumSplineSegments();
				const int32 StepsPerSegment = std::max(Spline->GetReparamStepsPerSegment(), 8);
				for (uint32 SegmentIndex = 0; SegmentIndex < SegmentCount; ++SegmentIndex)
				{
					FVector3 Previous = Spline->GetLocationAtParam(static_cast<double>(SegmentIndex), ESplineCoordinateSpace::World);
					for (int32 Step = 1; Step <= StepsPerSegment; ++Step)
					{
						const double Param = static_cast<double>(SegmentIndex) + static_cast<double>(Step) / StepsPerSegment;
						const FVector3 Current = Spline->GetLocationAtParam(Param, ESplineCoordinateSpace::World);
						Collector.AddLine({Previous, Current, CurveColor, Context.bSelected ? 3.0f : 2.0f, 7.0f, 20, Actor, Spline});
						Previous = Current;
					}
				}
				if (!Context.bSelected) return;

				const FVector4f PointColor = ToColor(MonaImGui::EUIThemeColor::Success);
				const FVector4f TangentColor = ToColor(MonaImGui::EUIThemeColor::Info);
				const FVector3 WorldX = glm::normalize(Spline->GetWorldRotation() * FVectorConstants::Forward);
				const FVector3 WorldY = glm::normalize(Spline->GetWorldRotation() * FVectorConstants::Right);
				const FVector3 WorldZ = glm::normalize(Spline->GetWorldRotation() * FVectorConstants::Up);
				constexpr double PointMarkerRadius = 2.5;
				for (uint32 PointIndex = 0; PointIndex < Spline->GetNumSplinePoints(); ++PointIndex)
				{
					const FSplinePoint* Point = Spline->GetSplinePoint(PointIndex);
					if (!Point) continue;
					const FVector3 WorldPoint = TransformLocalPosition(*Spline, Point->Position);
					auto AddPointLine = [&](const FVector3& Axis) {
						Collector.AddLine({WorldPoint - Axis * PointMarkerRadius, WorldPoint + Axis * PointMarkerRadius, PointColor, 2.5f, 8.0f, 50, Actor, Spline});
					};
					AddPointLine(WorldX);
					AddPointLine(WorldY);
					AddPointLine(WorldZ);

					if (Point->Type == ESplinePointType::Curve)
					{
						const FVector3 ArriveHandle = TransformLocalPosition(*Spline, Point->Position - Point->ArriveTangent / 3.0);
						const FVector3 LeaveHandle = TransformLocalPosition(*Spline, Point->Position + Point->LeaveTangent / 3.0);
						Collector.AddLine({ArriveHandle, LeaveHandle, TangentColor, 1.5f, 6.0f, 10, Actor, Spline});
					}
					else if (Point->Type == ESplinePointType::CurveAuto)
					{
						const FVector3 Tangent = Spline->GetTangentAtParam(static_cast<double>(PointIndex), ESplineCoordinateSpace::World) / 3.0;
						Collector.AddLine({WorldPoint - Tangent, WorldPoint + Tangent, TangentColor, 1.5f, 6.0f, 10, Actor, Spline});
					}
				}
			}
		};

		class FSplineDetailsCustomization final : public IObjectDetailsCustomization
		{
		public:
			auto DrawDetails(FLevelEditorContext& Context, DObject* Object) -> bool override
			{
				auto* Spline = Cast<DSplineComponent>(Object);
				if (!Spline) return false;
				const bool bReadOnly = Context.bReadOnly;

				FTransform RelativeTransform = Spline->GetRelativeTransform();
				if (MonaImGui::EditTransformProperty("Transform", RelativeTransform, bReadOnly) && !bReadOnly)
				{
					Spline->SetRelativeTransform(RelativeTransform);
				}

				bool bClosedLoop = Spline->IsClosedLoop();
				ImGui::PushID("ClosedLoop");
				MonaImGui::BeginPropertyRow("Closed Loop", bReadOnly);
				if (ImGui::Checkbox("##Value", &bClosedLoop) && !bReadOnly) Spline->SetClosedLoop(bClosedLoop);
				MonaImGui::EndPropertyRow(bReadOnly);
				ImGui::PopID();

				int32 ReparamSteps = Spline->GetReparamStepsPerSegment();
				ImGui::PushID("ReparamSteps");
				MonaImGui::BeginPropertyRow("Reparam Steps", bReadOnly);
				if (ImGui::DragInt("##Value", &ReparamSteps, 1.0f, 1, 1024, "%d", ImGuiSliderFlags_AlwaysClamp) && !bReadOnly)
				{
					Spline->SetReparamStepsPerSegment(ReparamSteps);
				}
				MonaImGui::EndPropertyRow(bReadOnly);
				ImGui::PopID();

				MonaImGui::BeginPropertyRow("Points", bReadOnly);
				if (ImGui::Button("Add Point") && !bReadOnly) AddPoint(*Spline);
				MonaImGui::EndPropertyRow(bReadOnly);

				std::optional<uint32> RemoveIndex;
				std::optional<std::pair<uint32, uint32>> SwapIndices;
				const uint32 PointCount = Spline->GetNumSplinePoints();
				for (uint32 PointIndex = 0; PointIndex < PointCount; ++PointIndex)
				{
					const FSplinePoint* ExistingPoint = Spline->GetSplinePoint(PointIndex);
					if (!ExistingPoint) continue;
					FSplinePoint Point = *ExistingPoint;
					ImGui::PushID(static_cast<int>(PointIndex));

					FTransform PointTransform;
					PointTransform.Translation = Point.Position;
					PointTransform.Rotation = Point.Rotation;
					PointTransform.Scale3D = Point.Scale;
					const std::string PointLabel = std::format("Point {}", PointIndex);
					if (MonaImGui::EditTransformProperty(PointLabel.c_str(), PointTransform, bReadOnly) && !bReadOnly)
					{
						Point.Position = PointTransform.Translation;
						Point.Rotation = PointTransform.Rotation;
						Point.Scale = PointTransform.Scale3D;
						Spline->UpdateSplinePoint(PointIndex, Point);
					}

					DrawPointType(*Spline, PointIndex, Point, bReadOnly);
					if (Point.Type == ESplinePointType::Curve)
					{
						DrawVector(*Spline, PointIndex, "Arrive Tangent", Point, Point.ArriveTangent, bReadOnly);
						DrawVector(*Spline, PointIndex, "Leave Tangent", Point, Point.LeaveTangent, bReadOnly);
					}

					MonaImGui::BeginPropertyRow("Actions", bReadOnly);
					if (ImGui::SmallButton("Up") && PointIndex > 0 && !bReadOnly) SwapIndices = {{PointIndex, PointIndex - 1}};
					ImGui::SameLine();
					if (ImGui::SmallButton("Down") && PointIndex + 1 < PointCount && !bReadOnly) SwapIndices = {{PointIndex, PointIndex + 1}};
					ImGui::SameLine();
					if (ImGui::SmallButton("Remove") && !bReadOnly) RemoveIndex = PointIndex;
					MonaImGui::EndPropertyRow(bReadOnly);
					ImGui::PopID();
				}

				if (SwapIndices)
				{
					std::vector<FSplinePoint> Points = Spline->GetSplinePoints();
					std::swap(Points[SwapIndices->first], Points[SwapIndices->second]);
					Spline->SetSplinePoints(std::move(Points));
				}
				else if (RemoveIndex)
				{
					Spline->RemoveSplinePoint(*RemoveIndex);
				}
				return true;
			}

		private:
			static auto AddPoint(DSplineComponent& Spline) -> void
			{
				FSplinePoint Point;
				const uint32 PointCount = Spline.GetNumSplinePoints();
				if (PointCount > 0)
				{
					if (const FSplinePoint* Previous = Spline.GetSplinePoint(PointCount - 1))
					{
						Point = *Previous;
						Point.Position += FVectorConstants::Forward * 100.0;
						Point.Type = ESplinePointType::CurveAuto;
					}
				}
				Spline.AddSplinePoint(Point);
			}

			static auto DrawPointType(DSplineComponent& Spline, uint32 PointIndex, FSplinePoint& Point, bool bReadOnly) -> void
			{
				static constexpr std::array<std::pair<ESplinePointType, const char*>, 4> Types = {{
					{ESplinePointType::Linear, "Linear"},
					{ESplinePointType::Curve, "Curve"},
					{ESplinePointType::CurveAuto, "Curve Auto"},
					{ESplinePointType::Constant, "Constant"},
				}};
				const auto Current = std::ranges::find_if(Types, [&](const auto& Entry) { return Entry.first == Point.Type; });
				const char* Preview = Current != Types.end() ? Current->second : "Unknown";
				MonaImGui::BeginPropertyRow("Type", bReadOnly);
				if (ImGui::BeginCombo("##Value", Preview))
				{
					for (const auto& [Type, Name] : Types)
					{
						if (ImGui::Selectable(Name, Point.Type == Type) && !bReadOnly)
						{
							Point.Type = Type;
							Spline.UpdateSplinePoint(PointIndex, Point);
						}
					}
					ImGui::EndCombo();
				}
				MonaImGui::EndPropertyRow(bReadOnly);
			}

			static auto DrawVector(DSplineComponent& Spline, uint32 PointIndex, const char* Label, FSplinePoint& Point, FVector3& Value, bool bReadOnly) -> void
			{
				ImGui::PushID(Label);
				MonaImGui::BeginPropertyRow(Label, bReadOnly);
				if (ImGui::DragScalarN("##Value", ImGuiDataType_Double, &Value.x, 3, 0.05f) && !bReadOnly)
				{
					Spline.UpdateSplinePoint(PointIndex, Point);
				}
				MonaImGui::EndPropertyRow(bReadOnly);
				ImGui::PopID();
			}
		};
	} // namespace

	auto CreateSplineComponentVisualizer() -> std::shared_ptr<IComponentEditorVisualizer>
	{
		return std::make_shared<FSplineComponentVisualizer>();
	}

	auto CreateSplineDetailsCustomization() -> std::shared_ptr<IObjectDetailsCustomization>
	{
		return std::make_shared<FSplineDetailsCustomization>();
	}
} // namespace Durin
