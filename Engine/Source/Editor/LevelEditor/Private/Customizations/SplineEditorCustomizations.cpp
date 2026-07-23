#include "Customizations/SplineEditorCustomizations.h"

#include "Components/SplineComponent.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/Property.h"
#include "Editor/ReflectedPropertyView.h"
#include "Engine/Actor.h"
#include "Workspace/LevelEditorContext.h"
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
			auto CustomizeDetails(FLevelEditorContext& Context, DObject* Object,
				FObjectPropertyViewBuilder& Builder) -> void override
			{
				auto* Spline = Cast<DSplineComponent>(Object);
				if (!Spline) return;
				Builder.ReplaceDefaultProperties();
				Builder.AddCustomRow(
					"Spline Transform Location Rotation Scale Closed Loop Reparam Steps Points Tangent Type Actions",
					[&Context, Spline](FReflectedPropertyView& PropertyView, const FReflectedPropertyViewContext& ViewContext) {
						return DrawSplineDetails(Context, *Spline, PropertyView, ViewContext);
					});
			}

		private:
			static auto DrawSplineDetails(FLevelEditorContext& Context, DSplineComponent& SplineObject,
				FReflectedPropertyView& PropertyView, const FReflectedPropertyViewContext& ViewContext) -> bool
			{
				auto* Spline = &SplineObject;
				const bool bReadOnly = ViewContext.bReadOnly;
				const FReflection Reflection = ResolveReflection(*Spline);
				if (!Reflection.IsValid())
				{
					Context.SetError("Spline reflection metadata is unavailable.");
					return false;
				}

				PropertyView.EditProperty(ViewContext, Spline, Reflection.RelativeTransform, 0, {.Label = "Transform"});

				bool bClosedLoop = Spline->IsClosedLoop();
				ImGui::PushID("ClosedLoop");
				MonaImGui::BeginPropertyRow("Closed Loop", bReadOnly);
				if (ImGui::Checkbox("##Value", &bClosedLoop) && !bReadOnly)
				{
					PropertyView.SubmitPropertyValueEdit(ViewContext, Reflection.MakeCurveFieldTarget(Spline, Reflection.ClosedLoop),
						[&](FProperty* ScratchProperty, void* ScratchContainer, uint32 ScratchArrayIndex) {
							*ScratchProperty->ContainerPtrToValuePtr<bool>(ScratchContainer, ScratchArrayIndex) = bClosedLoop;
					}, false);
				}
				MonaImGui::EndPropertyRow(bReadOnly);
				ImGui::PopID();

				int32 ReparamSteps = Spline->GetReparamStepsPerSegment();
				ImGui::PushID("ReparamSteps");
				MonaImGui::BeginPropertyRow("Reparam Steps", bReadOnly);
				const bool bReparamChanged = ImGui::DragInt("##Value", &ReparamSteps, 1.0f, 1, 1024, "%d", ImGuiSliderFlags_AlwaysClamp);
				const MonaImGui::FPropertyEditWidgetState ReparamState{
					ImGui::IsItemActive(), ImGui::IsItemActivated(), ImGui::IsItemDeactivatedAfterEdit()};
				const FReflectedPropertyEditTarget ReparamTarget = Reflection.MakeCurveFieldTarget(Spline, Reflection.ReparamSteps);
				if (bReparamChanged && !bReadOnly)
				{
					PropertyView.SubmitPropertyValueEdit(ViewContext, ReparamTarget,
						[&](FProperty* ScratchProperty, void* ScratchContainer, uint32 ScratchArrayIndex) {
							*ScratchProperty->ContainerPtrToValuePtr<int32>(ScratchContainer, ScratchArrayIndex) = ReparamSteps;
					}, true);
				}
				FinishContinuousEdit(PropertyView, ViewContext, ReparamTarget, ReparamState);
				MonaImGui::EndPropertyRow(bReadOnly);
				ImGui::PopID();

				MonaImGui::BeginPropertyRow("Points", bReadOnly);
				if (ImGui::Button("Add Point") && !bReadOnly) AddPoint(PropertyView, ViewContext, *Spline, Reflection);
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
					MonaImGui::FPropertyEditWidgetState PointTransformState;
					const bool bPointTransformChanged = MonaImGui::EditTransformProperty(
						PointLabel.c_str(), PointTransform, bReadOnly, &PointTransformState);
					const FReflectedPropertyEditTarget PointTarget = Reflection.MakePointTarget(Spline, PointIndex);
					if (bPointTransformChanged && !bReadOnly)
					{
						Point.Position = PointTransform.Translation;
						Point.Rotation = PointTransform.Rotation;
						Point.Scale = PointTransform.Scale3D;
						SubmitPoint(PropertyView, ViewContext, *Spline, Reflection, PointTarget, PointIndex, Point, true);
					}
					FinishContinuousEdit(PropertyView, ViewContext, PointTarget, PointTransformState);

					DrawPointType(PropertyView, ViewContext, *Spline, Reflection, PointIndex, Point, bReadOnly);
					if (Point.Type == ESplinePointType::Curve)
					{
						DrawVector(PropertyView, ViewContext, *Spline, Reflection, PointIndex, "Arrive Tangent",
							Reflection.ArriveTangent, Point.ArriveTangent, bReadOnly);
						DrawVector(PropertyView, ViewContext, *Spline, Reflection, PointIndex, "Leave Tangent",
							Reflection.LeaveTangent, Point.LeaveTangent, bReadOnly);
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
					SubmitPoints(PropertyView, ViewContext, *Spline, Reflection, std::move(Points), EPropertyChangeKind::ValueSet);
				}
				else if (RemoveIndex)
				{
					std::vector<FSplinePoint> Points = Spline->GetSplinePoints();
					Points.erase(Points.begin() + *RemoveIndex);
					SubmitPoints(PropertyView, ViewContext, *Spline, Reflection, std::move(Points), EPropertyChangeKind::ArrayRemove);
				}
				return false;
			}
			struct FReflection
			{
				FProperty* RelativeTransform = nullptr;
				FStructProperty* SplineCurve = nullptr;
				FArrayProperty* Points = nullptr;
				FProperty* Point = nullptr;
				FProperty* ClosedLoop = nullptr;
				FProperty* ReparamSteps = nullptr;
				FProperty* ArriveTangent = nullptr;
				FProperty* LeaveTangent = nullptr;
				FProperty* Type = nullptr;

				auto IsValid() const -> bool
				{
					return RelativeTransform && SplineCurve && Points && Point && ClosedLoop && ReparamSteps
						&& ArriveTangent && LeaveTangent && Type;
				}
				auto GetCurve(DSplineComponent* Spline) const -> FSplineCurve*
				{
					return SplineCurve->ContainerPtrToValuePtr<FSplineCurve>(Spline);
				}
				auto MakeCurveTarget(DSplineComponent* Spline) const -> FReflectedPropertyEditTarget
				{
					return FReflectedPropertyEditTarget::ForMember(Spline, SplineCurve);
				}
				auto MakeCurveFieldTarget(DSplineComponent* Spline, FProperty* Field) const -> FReflectedPropertyEditTarget
				{
					return MakeCurveTarget(Spline).ForStructMember(Field, GetCurve(Spline));
				}
				auto MakePointsTarget(DSplineComponent* Spline) const -> FReflectedPropertyEditTarget
				{
					return MakeCurveTarget(Spline).ForStructMember(Points, GetCurve(Spline));
				}
				auto MakePointTarget(DSplineComponent* Spline, uint32 PointIndex) const -> FReflectedPropertyEditTarget
				{
					void* PointContainer = Points->GetMutableElementPtr(GetCurve(Spline), PointIndex);
					return MakePointsTarget(Spline).ForArrayElement(Point, PointContainer, PointIndex);
				}
				auto MakePointFieldTarget(DSplineComponent* Spline, uint32 PointIndex, FProperty* Field) const -> FReflectedPropertyEditTarget
				{
					void* PointContainer = Points->GetMutableElementPtr(GetCurve(Spline), PointIndex);
					return MakePointTarget(Spline, PointIndex).ForStructMember(Field, PointContainer);
				}
			};

			static auto ResolveReflection(DSplineComponent& Spline) -> FReflection
			{
				FReflection Result;
				Result.RelativeTransform = Spline.GetClass()->FindPropertyByName("RelativeTransform");
				FProperty* CurveProperty = Spline.GetClass()->FindPropertyByName("SplineCurve");
				if (!CurveProperty || CurveProperty->GetKind() != DurinCodeGen::EPropertyGenFlags::Struct) return Result;
				Result.SplineCurve = static_cast<FStructProperty*>(CurveProperty);
				DStruct* CurveStruct = Result.SplineCurve->GetStruct();
				if (!CurveStruct) return Result;
				FProperty* PointsProperty = CurveStruct->FindPropertyByName(FName("Points"));
				if (!PointsProperty || PointsProperty->GetKind() != DurinCodeGen::EPropertyGenFlags::Array) return Result;
				Result.Points = static_cast<FArrayProperty*>(PointsProperty);
				Result.Point = Result.Points->GetInner();
				Result.ClosedLoop = CurveStruct->FindPropertyByName(FName("bClosedLoop"));
				Result.ReparamSteps = CurveStruct->FindPropertyByName(FName("ReparamStepsPerSegment"));
				auto* PointProperty = Result.Point && Result.Point->GetKind() == DurinCodeGen::EPropertyGenFlags::Struct
					? static_cast<FStructProperty*>(Result.Point) : nullptr;
				DStruct* PointStruct = PointProperty ? PointProperty->GetStruct() : nullptr;
				if (PointStruct)
				{
					Result.ArriveTangent = PointStruct->FindPropertyByName(FName("ArriveTangent"));
					Result.LeaveTangent = PointStruct->FindPropertyByName(FName("LeaveTangent"));
					Result.Type = PointStruct->FindPropertyByName(FName("Type"));
				}
				return Result;
			}

			static auto FinishContinuousEdit(FReflectedPropertyView& PropertyView, const FReflectedPropertyViewContext& ViewContext,
				const FReflectedPropertyEditTarget& Target, const MonaImGui::FPropertyEditWidgetState& State) -> void
			{
				if (State.bDeactivatedAfterEdit && PropertyView.IsEditingTarget(Target)) PropertyView.FinishActiveEdit(&ViewContext, false);
				else if (State.bActive && ImGui::IsKeyPressed(ImGuiKey_Escape) && PropertyView.IsEditingTarget(Target))
					PropertyView.FinishActiveEdit(&ViewContext, true);
			}

			static auto SubmitPoints(FReflectedPropertyView& PropertyView, const FReflectedPropertyViewContext& ViewContext,
				DSplineComponent& Spline, const FReflection& Reflection, std::vector<FSplinePoint> Points,
				EPropertyChangeKind Kind) -> bool
			{
				FReflectedPropertyEditTarget Target = Reflection.MakePointsTarget(&Spline);
				Target.Kind = Kind;
				return PropertyView.SubmitPropertyValueEdit(ViewContext, Target,
					[Points = std::move(Points)](FProperty* ScratchProperty, void* ScratchContainer, uint32 ScratchArrayIndex) {
						auto* ScratchArray = static_cast<FArrayProperty*>(ScratchProperty);
						ScratchArray->Resize(ScratchContainer, Points.size(), ScratchArrayIndex);
						for (size_t Index = 0; Index < Points.size(); ++Index)
							*static_cast<FSplinePoint*>(ScratchArray->GetMutableElementPtr(ScratchContainer, Index, ScratchArrayIndex)) = Points[Index];
				}, false);
			}

			static auto AddPoint(FReflectedPropertyView& PropertyView, const FReflectedPropertyViewContext& ViewContext,
				DSplineComponent& Spline, const FReflection& Reflection) -> void
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
				std::vector<FSplinePoint> Points = Spline.GetSplinePoints();
				Points.push_back(Point);
				SubmitPoints(PropertyView, ViewContext, Spline, Reflection, std::move(Points), EPropertyChangeKind::ArrayAdd);
			}

			static auto SubmitPoint(FReflectedPropertyView& PropertyView, const FReflectedPropertyViewContext& ViewContext,
				DSplineComponent& Spline, const FReflection& Reflection, const FReflectedPropertyEditTarget& Target,
				uint32 PointIndex, const FSplinePoint& Point, bool bContinuous) -> bool
			{
				return PropertyView.SubmitPropertyValueEdit(ViewContext, Target,
					[&](FProperty* ScratchProperty, void* ScratchContainer, uint32 ScratchArrayIndex) {
						*ScratchProperty->ContainerPtrToValuePtr<FSplinePoint>(ScratchContainer, ScratchArrayIndex) = Point;
				}, bContinuous);
			}

			static auto DrawPointType(FReflectedPropertyView& PropertyView, const FReflectedPropertyViewContext& ViewContext,
				DSplineComponent& Spline, const FReflection& Reflection, uint32 PointIndex, FSplinePoint& Point, bool bReadOnly) -> void
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
							const FReflectedPropertyEditTarget Target = Reflection.MakePointFieldTarget(&Spline, PointIndex, Reflection.Type);
							SubmitPoint(PropertyView, ViewContext, Spline, Reflection, Target, PointIndex, Point, false);
						}
					}
					ImGui::EndCombo();
				}
				MonaImGui::EndPropertyRow(bReadOnly);
			}

			static auto DrawVector(FReflectedPropertyView& PropertyView, const FReflectedPropertyViewContext& ViewContext,
				DSplineComponent& Spline, const FReflection& Reflection, uint32 PointIndex, const char* Label,
				FProperty* Field, FVector3& Value, bool bReadOnly) -> void
			{
				ImGui::PushID(Label);
				MonaImGui::BeginPropertyRow(Label, bReadOnly);
				const bool bChanged = ImGui::DragScalarN("##Value", ImGuiDataType_Double, &Value.x, 3, 0.05f);
				const MonaImGui::FPropertyEditWidgetState State{
					ImGui::IsItemActive(), ImGui::IsItemActivated(), ImGui::IsItemDeactivatedAfterEdit()};
				const FReflectedPropertyEditTarget Target = Reflection.MakePointFieldTarget(&Spline, PointIndex, Field);
				if (bChanged && !bReadOnly)
				{
					PropertyView.SubmitPropertyValueEdit(ViewContext, Target,
						[&](FProperty* ScratchProperty, void* ScratchContainer, uint32 ScratchArrayIndex) {
							*ScratchProperty->ContainerPtrToValuePtr<FVector3>(ScratchContainer, ScratchArrayIndex) = Value;
					}, true);
				}
				FinishContinuousEdit(PropertyView, ViewContext, Target, State);
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
