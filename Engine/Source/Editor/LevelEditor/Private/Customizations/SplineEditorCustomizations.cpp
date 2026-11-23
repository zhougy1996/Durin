#include "LevelDetailCustomization.h"
#include "PropertyEditor/DetailCustomizationRegistry.h"
#include "Customizations/SplineEditorCustomizations.h"
#include "ThirdParty/ImGui/imgui_internal.h"

#include "Components/SplineComponent.h"
#include "Components/SplineMeshComponent.h"
#include "Actors/SplineMeshActor.h"
#include "DObject/Class.h"
#include "DObject/Package.h"
#include "Transactions/Transactor.h"
#include "Engine/Actor.h"
#include "Math/Operations.h"
#include "MonaImGui.h"
#include "SceneViewProjection.h"
#include "Viewport/LevelEditorViewportClient.h"
#include "Viewport/TransformGizmo.h"
#include "Workspace/LevelEditorContext.h"

namespace Durin::LevelEditor
{
	namespace
	{
		constexpr double HandleScale = 1.0 / 3.0;

		auto ToColor(MonaImGui::EUIThemeColor ThemeColor) -> FVector4f
		{
			const ImVec4& Color = MonaImGui::GetThemeColor(ThemeColor);
			return {Color.x, Color.y, Color.z, Color.w};
		}

		auto LocalToWorld(const DSplineComponent& Spline, const FVector3& Position) -> FVector3
		{
			return FVector3(Spline.GetComponentToWorldMatrix() * FVector4(Position, 1.0));
		}

		auto WorldToLocal(const DSplineComponent& Spline, const FVector3& Position) -> FVector3
		{
			return FVector3(Math::Inverse(Spline.GetComponentToWorldMatrix()) * FVector4(Position, 1.0));
		}

		auto IsSelected(std::span<const FEditorSubElementSelection> Selection, const FEditorSubElementSelection& Element) -> bool
		{
			return std::ranges::contains(Selection, Element);
		}

		template<typename F>
		auto CommitSplineEdit(DSplineComponent& Spline, bool bShouldTransact, std::string Description, F&& Edit) -> bool
		{
			auto Service = ResolveEditorTransactor(bShouldTransact);
			if (!Service) return false;
			FScopedTransaction Transaction("SplineEdit", Description, nullptr, bShouldTransact);
			if (bShouldTransact && !Transaction.IsOutstanding()) return false;
			if (!Edit()) return false;
			return true;
		}

		auto SplineElement(const DSplineComponent& Spline, EEditorSubElementKind Kind, uint32 Index) -> FEditorSubElementSelection
		{
			return {Kind, static_cast<int32>(Index), Spline.GetSplineTopologyRevision()};
		}

		auto GetSelectedPointIndices(const FLevelEditorContext& Context, const DSplineComponent& Spline) -> std::vector<uint32>
		{
			std::vector<uint32> Result;
			if (Context.GetSelectedComponent() != &Spline) return Result;
			for (const FEditorSubElementSelection& Element : Context.GetSelectedSubElements())
				if (Element.Kind == EEditorSubElementKind::Point)
					Result.push_back(static_cast<uint32>(Element.Index));
			return Result;
		}

		auto DeleteSelectedSplinePoints(FLevelEditorContext& Context, DSplineComponent& Spline, bool bShouldTransact) -> bool
		{
			auto Indices = GetSelectedPointIndices(Context, Spline);
			if (Indices.empty()) return false;
			std::ranges::sort(Indices, std::greater{});
			return CommitSplineEdit(Spline, bShouldTransact, "Delete Spline Points", [&] {
				auto Points = Spline.GetSplinePoints();
				for (uint32 Index : Indices) Points.erase(Points.begin() + Index);
				return Spline.SetSplinePoints(std::move(Points));
			});
		}

		template<typename F>
		auto EditSelectedPoints(FLevelEditorContext& Context, DSplineComponent& Spline, bool bShouldTransact,
			std::string Description, F&& Edit) -> bool
		{
			const std::vector<uint32> Indices = GetSelectedPointIndices(Context, Spline);
			if (Indices.empty()) return false;
			return CommitSplineEdit(Spline, bShouldTransact, std::move(Description), [&] {
				bool bChanged = false;
				for (uint32 Index : Indices)
				{
					const FSplinePoint* Existing = Spline.GetSplinePoint(Index);
					if (!Existing) continue;
					FSplinePoint Point = *Existing;
					if (Edit(Index, Point)) bChanged |= Spline.UpdateSplinePoint(Index, Point, false);
				}
				if (bChanged) Spline.UpdateSpline(ESplineChangeFlags::Geometry | ESplineChangeFlags::Build);
				return bChanged;
			});
		}

		auto GetKeySpan(const DSplineComponent& Spline, uint32 SegmentIndex) -> double
		{
			const auto Evaluation = Spline.GetEvaluationData();
			if (SegmentIndex >= Evaluation->GetNumSegments()) return 1.0;
			const auto& Segment = Evaluation->GetSegments()[SegmentIndex];
			return Segment.EndKey - Segment.StartKey;
		}

		auto GetHandleScale(const DSplineComponent& Spline, uint32 PointIndex, bool bArrive) -> double
		{
			if (bArrive && PointIndex == 0 && !Spline.IsClosedLoop()) return HandleScale;
			const uint32 Segment = bArrive
				? (PointIndex == 0 ? Spline.GetNumSplineSegments() - 1 : PointIndex - 1) : PointIndex;
			return HandleScale * GetKeySpan(Spline, Segment);
		}

		auto SetTangentMode(DSplineComponent& Spline, uint32 Index, FSplinePoint& Point, ESplineTangentMode Mode) -> bool
		{
			if (Point.TangentMode == Mode) return false;
			if (Mode == ESplineTangentMode::ManualAligned || Mode == ESplineTangentMode::ManualBroken)
			{
				if (Index < Spline.GetNumSplineSegments()) Point.LeaveTangent = Spline.GetSampleAtParameter({Index, 0.0}).FirstDerivative / GetKeySpan(Spline, Index);
				if (Index > 0 || Spline.IsClosedLoop())
				{
					const uint32 Previous = Index == 0 ? Spline.GetNumSplineSegments() - 1 : Index - 1;
					Point.ArriveTangent = Spline.GetSampleAtParameter({Previous, 1.0}).FirstDerivative / GetKeySpan(Spline, Previous);
				}
				if (Mode == ESplineTangentMode::ManualAligned)
				{
					const FVector3 Seed = Math::Length(Point.LeaveTangent) > SmallNumber ? Point.LeaveTangent : Point.ArriveTangent;
					Point.ArriveTangent = Point.LeaveTangent = Seed;
				}
			}
			Point.TangentMode = Mode;
			return true;
		}

		class FSplineElementTransformTarget final : public ITransformGizmoTarget
		{
		public:
			FSplineElementTransformTarget(DSplineComponent* InSpline, FEditorSubElementSelection InElement)
				: Spline(InSpline), Element(InElement) {}
			auto CreateHistoryTarget(uint64 InHistoryId) const -> std::shared_ptr<ITransformGizmoTarget> override
			{
				auto Target = std::make_shared<FSplineElementTransformTarget>(Spline.Get(), Element);
				if (const auto* Resolved = Spline.Get())
				{
					Target->HistoryId = InHistoryId;
					Target->ForeignTopologyRevision = Resolved->GetForeignTopologyRevision(InHistoryId);
				}
				return Target;
			}
			auto IsHistoryInvalidated() const -> bool override
			{
				const auto* Resolved = Spline.Get();
				return Resolved && HistoryId != 0 && ForeignTopologyRevision != Resolved->GetForeignTopologyRevision(HistoryId);
			}
			auto IsValid() const -> bool override { return ResolvePoint() != nullptr; }
			auto GetIdentity() const -> const void* override { return ResolvePoint(); }
			auto GetModifiedObjects() const -> std::vector<DObject*> override { return {Spline.Get()}; }
			auto GetTransform() const -> FTransform override
			{
				FTransform Result;
				const FSplinePoint* Point = ResolvePoint();
				DSplineComponent* Resolved = Spline.Get();
				if (!Point || !Resolved) return Result;
				const uint32 Index = static_cast<uint32>(Element.Index);
				FVector3 LocalPosition = Point->Position;
				if (Element.Kind == EEditorSubElementKind::ArriveTangent) LocalPosition -= Point->ArriveTangent * GetHandleScale(*Resolved, Index, true);
				if (Element.Kind == EEditorSubElementKind::LeaveTangent) LocalPosition += Point->LeaveTangent * GetHandleScale(*Resolved, Index, false);
				Result.Translation = LocalToWorld(*Resolved, LocalPosition);
				if (Element.Kind == EEditorSubElementKind::Point)
				{
					Result.Rotation = Resolved->GetWorldRotation() * Point->Rotation;
					Result.Scale3D = Point->Scale;
				}
				return Result;
			}
			auto SetTransform(const FTransform& Transform) -> bool override
			{
				DSplineComponent* Resolved = Spline.Get();
				const FSplinePoint* Existing = ResolvePoint();
				if (!Resolved || !Existing) return false;
				const uint32 Index = static_cast<uint32>(Element.Index);
				FSplinePoint Point = *Existing;
				const FVector3 LocalPosition = WorldToLocal(*Resolved, Transform.Translation);
				if (Element.Kind == EEditorSubElementKind::Point)
				{
					Point.Position = LocalPosition;
					Point.Rotation = glm::inverse(Resolved->GetWorldRotation()) * Transform.Rotation;
					Point.Scale = Transform.Scale3D;
				}
				else
				{
					const bool bArrive = Element.Kind == EEditorSubElementKind::ArriveTangent;
					const FVector3 Tangent = (bArrive ? Point.Position - LocalPosition : LocalPosition - Point.Position)
						/ GetHandleScale(*Resolved, Index, bArrive);
					if (Element.Kind == EEditorSubElementKind::ArriveTangent) Point.ArriveTangent = Tangent;
					else Point.LeaveTangent = Tangent;
					if (Point.TangentMode == ESplineTangentMode::ManualAligned)
						Point.ArriveTangent = Point.LeaveTangent = Tangent;
				}
				return Resolved->UpdateSplinePoint(Index, Point);
			}
			auto GetPackage() const -> DPackage* override { return Spline.Get() ? Spline.Get()->GetPackage() : nullptr; }
			auto GetLabel() const -> std::string override { return Element.Kind == EEditorSubElementKind::Point ? "Spline Point" : "Spline Tangent"; }
			auto GetCapabilities() const -> ETransformGizmoCapability override { return Element.Kind == EEditorSubElementKind::Point
				? ETransformGizmoCapability::All
				: ETransformGizmoCapability::Translate; }
		private:
			auto ResolvePoint() const -> const FSplinePoint*
			{
				DSplineComponent* Resolved = Spline.Get();
				if (!Resolved || IsHistoryInvalidated()
					|| (HistoryId == 0 && Element.TopologyRevision != Resolved->GetSplineTopologyRevision())) return nullptr;
				return Resolved->GetSplinePoint(static_cast<uint32>(Element.Index));
			}
			TWeakObjectPtr<DSplineComponent> Spline;
			FEditorSubElementSelection Element;
			uint64 HistoryId = 0;
			uint64 ForeignTopologyRevision = 0;
		};

		auto DrawSplineAttributeSamples(DSplineComponent& Spline, const FSplineEvaluationSegment& Segment,
			AActor* Actor, FEditorVisualizationCollector& Collector) -> void
		{
			const FVector4f RightColor = ToColor(MonaImGui::EUIThemeColor::AxisY);
			const FVector4f UpColor = ToColor(MonaImGui::EUIThemeColor::AxisZ);
			constexpr double AxisLength = 0.4;
			for (double T : {0.25, 0.5, 0.75})
			{
				const double Key = std::lerp(Segment.StartKey, Segment.EndKey, T);
				const FTransform Frame = Spline.GetTransformAtSplineInputKey(Key, ESplineCoordinateSpace::World, true);
				const FVector3 Right = Frame.Rotation * FVector3(0.0, Frame.Scale3D.y * AxisLength, 0.0);
				const FVector3 Up = Frame.Rotation * FVector3(0.0, 0.0, Frame.Scale3D.z * AxisLength);
				Collector.AddLine({.Start = Frame.Translation - Right, .End = Frame.Translation + Right,
					.Color = RightColor, .Actor = Actor, .Component = &Spline, .bHitTest = false});
				Collector.AddLine({.Start = Frame.Translation, .End = Frame.Translation + Up,
					.Color = UpColor, .Actor = Actor, .Component = &Spline, .bHitTest = false});
			}
		}

		class FSplineComponentVisualizer final : public IComponentEditorVisualizer
		{
		public:
			auto DrawVisualization(DActorComponent* Component, const FEditorVisualizationContext& Context,
				FEditorVisualizationCollector& Collector) const -> void override
			{
				auto* Spline = Cast<DSplineComponent>(Component);
				AActor* Actor = Spline ? Spline->GetOwner() : nullptr;
				if (!Spline || !Actor) return;
				const FVector4f CurveColor = ToColor(Context.bSelected ? MonaImGui::EUIThemeColor::SelectionPrimary : MonaImGui::EUIThemeColor::ViewportText);
				const FVector4f HoverColor = ToColor(MonaImGui::EUIThemeColor::Info);
				const auto Evaluation = Spline->GetEvaluationData();
				for (uint32 SegmentIndex = 0; SegmentIndex < Evaluation->GetNumSegments(); ++SegmentIndex)
				{
					const FSplinePoint* StartPoint = Spline->GetSplinePoint(SegmentIndex);
					if (!StartPoint) continue;
					const auto Element = SplineElement(*Spline, EEditorSubElementKind::Segment, SegmentIndex);
					FVector3 Previous = Spline->GetSampleAtParameter({SegmentIndex, 0.0}, ESplineCoordinateSpace::World).Position;
					const auto& Segment = Evaluation->GetSegments()[SegmentIndex];
					const auto& Samples = Segment.DistanceSamples;
					std::vector<double> SampleParameters;
					for (const FSplineDistanceSample& Sample : Samples) SampleParameters.push_back(Sample.T);
					for (uint32 Step = 0; Step <= 16; ++Step) SampleParameters.push_back(static_cast<double>(Step) / 16.0);
					std::ranges::sort(SampleParameters);
					const auto UniqueEnd = std::unique(SampleParameters.begin(), SampleParameters.end(), [](double A, double B) { return std::abs(A - B) < 1e-9; });
					SampleParameters.erase(UniqueEnd, SampleParameters.end());
					for (size_t SampleIndex = 1; SampleIndex < SampleParameters.size(); ++SampleIndex)
					{
						const FVector3 Current = Spline->GetSampleAtParameter({SegmentIndex, SampleParameters[SampleIndex]}, ESplineCoordinateSpace::World).Position;
						FEditorVisualizationLine Line{Previous, Current, CurveColor, Context.bSelected ? 3.0f : 2.0f, 7.0f, 20, Actor, Spline,
							StartPoint->OutgoingInterpolation == ESplineSegmentInterpolation::Constant
								? ESimpleElementLinePattern::Dashed : ESimpleElementLinePattern::Solid, 12.0f, HoverColor};
						Line.Element = Element;
						Collector.AddLine(Line);
						Previous = Current;
					}
					const uint32 EndIndex = (SegmentIndex + 1) % Spline->GetNumSplinePoints();
					if (Context.bComponentSelected
						&& (IsSelected(Context.SelectedSubElements, SplineElement(*Spline, EEditorSubElementKind::Point, SegmentIndex))
							|| IsSelected(Context.SelectedSubElements, SplineElement(*Spline, EEditorSubElementKind::Point, EndIndex))))
						DrawSplineAttributeSamples(*Spline, Segment, Actor, Collector);
				}
				if (!Context.bSelected) return;
				for (uint32 PointIndex = 0; PointIndex < Spline->GetNumSplinePoints(); ++PointIndex)
				{
					const FSplinePoint* Point = Spline->GetSplinePoint(PointIndex);
					if (!Point) continue;
					const auto PointElement = SplineElement(*Spline, EEditorSubElementKind::Point, PointIndex);
					const FVector4f PointColor = ToColor(IsSelected(Context.SelectedSubElements, PointElement)
						? MonaImGui::EUIThemeColor::SelectionPrimary : MonaImGui::EUIThemeColor::Success);
					const FVector3 WorldPoint = LocalToWorld(*Spline, Point->Position);
					FEditorVisualizationBox PointBox{WorldPoint, PointColor, 12.0f, 5.0f, 80, Actor, Spline, true, HoverColor};
					PointBox.Element = PointElement;
					Collector.AddBox(PointBox);
					if (Point->TangentMode != ESplineTangentMode::ManualAligned && Point->TangentMode != ESplineTangentMode::ManualBroken) continue;
					const FVector3 Arrive = LocalToWorld(*Spline, Point->Position - Point->ArriveTangent * GetHandleScale(*Spline, PointIndex, true));
					const FVector3 Leave = LocalToWorld(*Spline, Point->Position + Point->LeaveTangent * GetHandleScale(*Spline, PointIndex, false));
					for (const auto& [Handle, Kind] : {std::pair{Arrive, EEditorSubElementKind::ArriveTangent}, std::pair{Leave, EEditorSubElementKind::LeaveTangent}})
					{
						FEditorVisualizationLine Stem{WorldPoint, Handle, ToColor(MonaImGui::EUIThemeColor::Info), 1.5f, 6.0f, 40, Actor, Spline};
						Stem.Element = SplineElement(*Spline, Kind, PointIndex);
						Collector.AddLine(Stem);
						FEditorVisualizationBox HandleBox{Handle, Stem.Color, 9.0f, 4.0f, 90, Actor, Spline, true,
							ToColor(MonaImGui::EUIThemeColor::SelectionPrimary)};
						HandleBox.Element = Stem.Element;
						Collector.AddBox(HandleBox);
					}
				}
			}
		};

		auto FindSegmentT(const DSplineComponent& Spline, uint32 SegmentIndex, const FSceneView& View, const FVector2f& Mouse) -> double
		{
			double BestT = 0.5;
			float BestDistance = std::numeric_limits<float>::max();
			for (uint32 Step = 0; Step <= 64; ++Step)
			{
				const double T = static_cast<double>(Step) / 64.0;
				FVector2f Screen;
				if (!SceneViewProjection::ProjectWorldToViewport(View, Spline.GetSampleAtParameter({SegmentIndex, T}, ESplineCoordinateSpace::World).Position, Screen)) continue;
				const float Distance = Math::Length(Screen - Mouse);
				if (Distance < BestDistance) { BestDistance = Distance; BestT = T; }
			}
			return std::clamp(BestT, 1.0 / 64.0, 63.0 / 64.0);
		}

		class FSplineViewportEditMode final : public ILevelViewportEditMode
		{
		public:
			auto Enter(FLevelEditorContext& Context) -> void override
			{
				bExitRequested = false;
				auto* Spline = Cast<DSplineComponent>(Context.GetSelectedComponent());
				LastSpline = Spline;
				if (Spline && Context.GetSelectedSubElements().empty() && Spline->GetNumSplinePoints() > 0)
					Context.SelectSubElement(Spline, SplineElement(*Spline, EEditorSubElementKind::Point, 0));
			}
			auto Exit(FLevelEditorContext& Context, bool) -> void override
			{
				if (LastClient && !LastClient->GetTransformGizmo().CancelDrag())
					throw std::runtime_error("Unable to restore the transform gesture.");
				if (Context.GetSelectedComponent() == LastSpline.Get()) Context.ClearSubElementSelection();
				LastClient = nullptr;
				LastSpline = nullptr;
				bExitRequested = false;
			}
			auto Tick(FLevelEditorContext& Context, FLevelEditorViewportClient& Client, const FSceneView& View,
				FLevelEditorViewportInput& Input, DTransactor* Transactions) -> bool override
			{
				constexpr auto bShouldTransact = true;
				LastClient = &Client;
				auto* Spline = Cast<DSplineComponent>(Context.GetSelectedComponent());
				if (!Spline) return false;
				LastSpline = Spline;
				const FTransformGizmoTargetSet Targets = GetGizmoTargets(Context);
				const bool bWasDragging = Client.GetTransformGizmo().IsDragging();
				if (!Client.GetTransformGizmo().Update(Targets, View, Input, Transactions))
					Context.SetError("Unable to apply or restore the transform gesture.");
				if (Input.bCancel)
				{
					if (bWasDragging) return true;
					if (!Context.GetSelectedSubElements().empty()) Context.ClearSubElementSelection();
					else bExitRequested = true;
					return true;
				}
				if (Client.GetTransformGizmo().IsHovered() || Client.GetTransformGizmo().IsDragging()) return true;
				if (Input.bFocusSelection)
				{
					FVector3 Center(0.0); uint32 Count = 0;
					for (const auto& Target : Targets.Targets) if (Target && Target->IsValid()) { Center += Target->GetTransform().Translation; ++Count; }
					if (Count > 0) Client.FocusLocation(Center / static_cast<double>(Count)); else Client.FocusActor(Spline->GetOwner());
					return true;
				}
				if (Input.bDelete) return DeleteSelectedSplinePoints(Context, *Spline, bShouldTransact);
				if (Input.bDuplicate) return DuplicateSelected(Context, *Spline, bShouldTransact);
				if (Input.bAppend) return AppendPoint(Context, *Spline, bShouldTransact);
				if (!Input.bRequestSelection) return false;
				const FViewportPickResult Result = Client.PickViewport(
					Context.Level, View, Input.MousePosition, EViewportPickLayer::EditorVisualization);
				const std::optional<FViewportPickHit> Hit = Result.Status == EViewportPickStatus::Completed
					? Result.Hit : std::nullopt;
				if (!Hit || Hit->Component.Get() != Spline || !Hit->Element.IsValid()
					|| Hit->Element.TopologyRevision != Spline->GetSplineTopologyRevision()) { Context.ClearSubElementSelection(); return true; }
				if (Input.bLeftMouseDoubleClicked && Hit->Element.Kind == EEditorSubElementKind::Segment)
				{
					const uint32 Segment = static_cast<uint32>(Hit->Element.Index);
					if (CommitSplineEdit(*Spline, bShouldTransact, "Insert Spline Point", [&] { return SplitSplineSegment(*Spline, Segment, FindSegmentT(*Spline, Segment, View, Input.MousePosition)); }))
						Context.SelectSubElement(Spline, SplineElement(*Spline, EEditorSubElementKind::Point, Segment + 1));
					return true;
				}
				if (Input.bCtrl && Hit->Element.Kind == EEditorSubElementKind::Point) Context.ToggleSubElement(Spline, Hit->Element);
				else Context.SelectSubElement(Spline, Hit->Element);
				return true;
			}

			auto ShouldExit() const -> bool override { return bExitRequested; }

			auto GetGizmoTargets(const FLevelEditorContext& Context) const -> FTransformGizmoTargetSet override
			{
				FTransformGizmoTargetSet Result;
				Result.CollectionLabel = "Spline Points";
				auto* Spline = Cast<DSplineComponent>(Context.GetSelectedComponent());
				if (!Spline) return Result;
				const auto& Elements = Context.GetSelectedSubElements();
				const FEditorSubElementSelection& Primary = Context.GetSelectedSubElement();
				if (Primary.Kind == EEditorSubElementKind::ArriveTangent || Primary.Kind == EEditorSubElementKind::LeaveTangent)
					Result.Targets.push_back(std::make_shared<FSplineElementTransformTarget>(Spline, Primary));
				else for (const FEditorSubElementSelection& Element : Elements)
					if (Element.Kind == EEditorSubElementKind::Point) Result.Targets.push_back(std::make_shared<FSplineElementTransformTarget>(Spline, Element));
				return Result;
			}
		private:
			// Non-owning; the viewport panel keeps its client alive longer than its mode manager.
			FLevelEditorViewportClient* LastClient = nullptr;
			TWeakObjectPtr<DSplineComponent> LastSpline;
			bool bExitRequested = false;

			static auto DuplicateSelected(FLevelEditorContext& Context, DSplineComponent& Spline, bool bShouldTransact) -> bool
			{
				const auto Element = Context.GetSelectedSubElement();
				if (Element.Kind != EEditorSubElementKind::Point) return false;
				std::optional<uint32> NewIndex;
				const bool Changed = CommitSplineEdit(Spline, bShouldTransact, "Duplicate Spline Point", [&] {
					NewIndex = Spline.DuplicateSplinePoint(static_cast<uint32>(Element.Index));
					return NewIndex.has_value();
				});
				if (Changed) Context.SelectSubElement(&Spline, SplineElement(Spline, EEditorSubElementKind::Point, *NewIndex));
				return Changed;
			}
			static auto AppendPoint(FLevelEditorContext& Context, DSplineComponent& Spline, bool bShouldTransact) -> bool
			{
				uint32 NewIndex = 0;
				const bool Changed = CommitSplineEdit(Spline, bShouldTransact, "Append Spline Point", [&] {
					NewIndex = Spline.AddSplinePoint(FSplinePoint(CalculateSplineAppendPosition(Spline)));
					return true;
				});
				if (Changed) Context.SelectSubElement(&Spline, SplineElement(Spline, EEditorSubElementKind::Point, NewIndex));
				return Changed;
			}
		};

		auto DrawSplineVectorInput(const char* Label, const char* Id, FVector3& Value, bool bMixed, bool bReadOnly) -> bool
		{
			MonaImGui::PropertyEdit::BeginRow(Label, bReadOnly);
			double Components[] = {Value.x, Value.y, Value.z};
			ImGui::SetNextItemWidth(-FLT_MIN);
			if (bMixed) ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
			const bool bCommitted = ImGui::InputScalarN(Id, ImGuiDataType_Double, Components, 3, nullptr, nullptr,
				"%.4f", ImGuiInputTextFlags_EnterReturnsTrue);
			if (bMixed) ImGui::PopItemFlag();
			MonaImGui::PropertyEdit::EndRow(bReadOnly);
			Value = FVector3(Components[0], Components[1], Components[2]);
			return bCommitted && !bReadOnly && Math::IsFinite(Value);
		}

		class FSplineDetailsCustomization final : public IDetailCustomization
		{
		public:
			auto CustomizeDetails(IDetailLayoutBuilder& Builder) -> void override
			{
				DObject* Object = Builder.GetObject();
				auto& Context = GetLevelDetailHostContext(Builder).Context;
				auto* Spline = Cast<DSplineComponent>(Object);
				if (!Spline) return;
				if (FProperty* SplineCurve = Spline->GetClass()->FindPropertyByName("SplineCurve")) Builder.HideProperty(SplineCurve);
				Builder.EditCategory("Spline").AddCustomRow("Spline Edit Points Segments Length Position Rotation Scale Input Key Loop Key Tangents Interpolation Automatic Clamped Aligned Broken Add Duplicate Delete Loop Reorder",
					[&Context, Spline](FPropertyView&, const FPropertyViewContext& ViewContext) {
					ImGui::PushID(Spline);
					bool Changed = false;
					{
						MonaImGui::PropertyEdit::BeginRow("Overview", true);
						ImGui::Text("%u points  |  %u segments", Spline->GetNumSplinePoints(), Spline->GetNumSplineSegments());
						ImGui::TextDisabled("%.1f units  |  %s", Spline->GetLocalSplineLength(), Spline->IsClosedLoop() ? "Closed loop" : "Open curve");
						MonaImGui::PropertyEdit::EndRow(true);

						MonaImGui::PropertyEdit::BeginRow("Viewport", ViewContext.bReadOnly);
						if (ImGui::Button("Edit in Viewport", ImVec2(-FLT_MIN, 0.0f)) && !ViewContext.bReadOnly && Context.ActivateViewportEditMode)
							Context.ActivateViewportEditMode("Spline");
						if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
							ImGui::SetTooltip("Enter Spline mode to select, move, and insert control points.");
						MonaImGui::PropertyEdit::EndRow(ViewContext.bReadOnly);

						MonaImGui::PropertyEdit::BeginRow("Closed Loop", ViewContext.bReadOnly);
						bool bClosedLoop = Spline->IsClosedLoop();
						if (ImGui::Checkbox("##ClosedLoop", &bClosedLoop) && !ViewContext.bReadOnly)
							Changed |= CommitSplineEdit(*Spline, ViewContext.bShouldTransact, "Toggle Spline Loop", [&] { Spline->SetClosedLoop(bClosedLoop); return true; });
						ImGui::SameLine();
						ImGui::TextDisabled(bClosedLoop ? "Last point connects to first" : "Endpoints remain separate");
						MonaImGui::PropertyEdit::EndRow(ViewContext.bReadOnly);

						if (Spline->IsClosedLoop() && Spline->GetNumSplineSegments() > 0)
						{
							MonaImGui::PropertyEdit::BeginRow("Loop Key", ViewContext.bReadOnly);
							double LoopKey = Spline->GetSplineCurve().GetLoopKey();
							ImGui::SetNextItemWidth(-FLT_MIN);
							if (ImGui::InputDouble("##LoopKey", &LoopKey, 0.0, 0.0, "%.9g", ImGuiInputTextFlags_EnterReturnsTrue)
								&& !ViewContext.bReadOnly && LoopKey != Spline->GetSplineCurve().GetLoopKey())
							{
								Changed |= CommitSplineEdit(*Spline, ViewContext.bShouldTransact, "Edit Spline Loop Key",
									[&] { return Spline->SetClosedLoopAtPosition(LoopKey); });
							}
							if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
								ImGui::SetTooltip("The loop key must be greater than the last point's input key.");
							MonaImGui::PropertyEdit::EndRow(ViewContext.bReadOnly);
						}

						MonaImGui::PropertyEdit::BeginRow("Curve Actions", ViewContext.bReadOnly);
						if (ImGui::Button("Add Point", ImVec2(-FLT_MIN, 0.0f)) && !ViewContext.bReadOnly)
						{
							uint32 Index = 0;
							if (CommitSplineEdit(*Spline, ViewContext.bShouldTransact, "Append Spline Point", [&] {
								Index = Spline->AddSplinePoint(FSplinePoint(CalculateSplineAppendPosition(*Spline)));
								return true;
							}))
							{
								Context.SelectSubElement(Spline, SplineElement(*Spline, EEditorSubElementKind::Point, Index));
								Changed = true;
							}
						}
						if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("Append a control point after the current endpoint.");
						MonaImGui::PropertyEdit::EndRow(ViewContext.bReadOnly);
					}

					const std::vector<uint32> Selected = GetSelectedPointIndices(Context, *Spline);
					const std::string SelectionLabel = Selected.empty() ? "Selected Point"
						: Selected.size() == 1 ? std::format("Selected Point ({}/{})", Selected.front() + 1, Spline->GetNumSplinePoints())
						: std::format("Selected Points ({})", Selected.size());
					if (MonaImGui::PropertyEdit::BeginGroup("##SplineSelection", SelectionLabel.c_str()))
					{
						if (Selected.empty())
						{
							MonaImGui::PropertyEdit::BeginRow("Selection", true);
							ImGui::TextDisabled("Choose a control point in the viewport.");
							MonaImGui::PropertyEdit::EndRow(true);
							MonaImGui::PropertyEdit::EndGroup();
							ImGui::PopID();
							return Changed;
						}

						const auto Primary = Context.GetSelectedSubElement();
						const auto PrimaryIndex = Primary.Kind == EEditorSubElementKind::Point
							? std::optional(static_cast<uint32>(Primary.Index)) : std::nullopt;

						const FVector3 Position = Spline->GetSplinePoint(Selected.front())->Position;
						const bool bMixedPosition = std::ranges::any_of(Selected, [Spline, Position](uint32 Index) {
							return Math::Length(Spline->GetSplinePoint(Index)->Position - Position) > 1e-9;
						});
						double PositionValue[3] = {Position.x, Position.y, Position.z};
						MonaImGui::PropertyEdit::BeginRow(bMixedPosition ? "Position (Multiple Values)" : "Position", ViewContext.bReadOnly);
						ImGui::SetNextItemWidth(-FLT_MIN);
						if (bMixedPosition) ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
						const bool bPositionCommitted = ImGui::InputScalarN("##SplinePointPosition", ImGuiDataType_Double,
							PositionValue, 3, nullptr, nullptr, "%.3f", ImGuiInputTextFlags_EnterReturnsTrue);
						if (bMixedPosition) ImGui::PopItemFlag();
						MonaImGui::PropertyEdit::EndRow(ViewContext.bReadOnly);
						if (bPositionCommitted && !ViewContext.bReadOnly)
						{
							const FVector3 Delta = FVector3(PositionValue[0], PositionValue[1], PositionValue[2]) - Position;
							Changed |= EditSelectedPoints(Context, *Spline, ViewContext.bShouldTransact, "Edit Spline Point Position",
								[Delta](uint32, FSplinePoint& Point) { Point.Position += Delta; return Math::Length(Delta) > 1e-12; });
						}
						const auto* FirstPoint = Spline->GetSplinePoint(Selected.front());
						const FQuat FirstRotation = FirstPoint->Rotation;
						const FVector3 FirstScale = FirstPoint->Scale;
						FVector3 RotationDegrees = Math::QuaternionToEulerDegrees(FirstRotation);
						const bool bMixedRotation = std::ranges::any_of(Selected, [Spline, FirstRotation](uint32 Index) {
							return !Math::AreRotationsEquivalent(FirstRotation, Spline->GetSplinePoint(Index)->Rotation);
						});
						if (DrawSplineVectorInput("Rotation (Degrees)", "##SplinePointRotation", RotationDegrees, bMixedRotation, ViewContext.bReadOnly))
						{
							const FQuat Rotation = Math::MakeQuaternionFromEulerDegrees(RotationDegrees);
							Changed |= EditSelectedPoints(Context, *Spline, ViewContext.bShouldTransact, "Edit Spline Point Rotation",
								[Rotation](uint32, FSplinePoint& Point) {
									if (Math::AreRotationsEquivalent(Point.Rotation, Rotation)) return false;
									Point.Rotation = glm::dot(Point.Rotation, Rotation) < 0.0 ? -Rotation : Rotation;
									return true;
								});
						}

						FVector3 Scale = FirstScale;
						const bool bMixedScale = std::ranges::any_of(Selected, [Spline, FirstScale](uint32 Index) {
							return Spline->GetSplinePoint(Index)->Scale != FirstScale;
						});
						if (DrawSplineVectorInput("Scale", "##SplinePointScale", Scale, bMixedScale, ViewContext.bReadOnly))
						{
							Changed |= EditSelectedPoints(Context, *Spline, ViewContext.bShouldTransact, "Edit Spline Point Scale",
								[Scale](uint32, FSplinePoint& Point) { Point.Scale = Scale; return true; });
						}

						if (Selected.size() == 1)
						{
							const FSplinePoint* SelectedPoint = Spline->GetSplinePoint(Selected.front());
							double Key = SelectedPoint->InputKey;
							MonaImGui::PropertyEdit::BeginRow("Input Key", ViewContext.bReadOnly);
							ImGui::SetNextItemWidth(-FLT_MIN);
							const bool bKeyCommitted = ImGui::InputDouble("##SplinePointKey", &Key, 0.0, 0.0, "%.9g", ImGuiInputTextFlags_EnterReturnsTrue);
							if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
								ImGui::SetTooltip("Point keys must be strictly increasing and stay before the loop key override.");
							MonaImGui::PropertyEdit::EndRow(ViewContext.bReadOnly);
							if (bKeyCommitted && !ViewContext.bReadOnly && Key != SelectedPoint->InputKey
								&& Spline->GetSplineCurve().IsValidPointKey(Selected.front(), Key))
							{
								Changed |= EditSelectedPoints(Context, *Spline, ViewContext.bShouldTransact, "Edit Spline Point Key",
									[Key](uint32, FSplinePoint& Point) { Point.InputKey = Key; return true; });
								SelectedPoint = Spline->GetSplinePoint(Selected.front());
							}

							if (SelectedPoint->TangentMode == ESplineTangentMode::ManualAligned || SelectedPoint->TangentMode == ESplineTangentMode::ManualBroken)
							{
								for (const auto& [Label, bArrive] : {std::pair{"Arrive Tangent", true}, {"Leave Tangent", false}})
								{
									const FVector3 Value = bArrive ? SelectedPoint->ArriveTangent : SelectedPoint->LeaveTangent;
									double Components[3] = {Value.x, Value.y, Value.z};
									MonaImGui::PropertyEdit::BeginRow(Label, ViewContext.bReadOnly);
									ImGui::SetNextItemWidth(-FLT_MIN);
									const bool bTangentCommitted = ImGui::InputScalarN(bArrive ? "##ArriveTangent" : "##LeaveTangent", ImGuiDataType_Double,
										Components, 3, nullptr, nullptr, "%.3f", ImGuiInputTextFlags_EnterReturnsTrue);
									MonaImGui::PropertyEdit::EndRow(ViewContext.bReadOnly);
									if (bTangentCommitted && !ViewContext.bReadOnly)
									{
										const FVector3 NewValue(Components[0], Components[1], Components[2]);
										Changed |= EditSelectedPoints(Context, *Spline, ViewContext.bShouldTransact, "Edit Spline Tangent",
											[bArrive, NewValue](uint32, FSplinePoint& Point) {
												if (bArrive) Point.ArriveTangent = NewValue; else Point.LeaveTangent = NewValue;
												if (Point.TangentMode == ESplineTangentMode::ManualAligned) Point.ArriveTangent = Point.LeaveTangent = NewValue;
												return true;
											});
									}
								}
							}
						}

						const ESplineSegmentInterpolation Interpolation = Spline->GetSplinePoint(Selected.front())->OutgoingInterpolation;
						const bool bMixedInterpolation = std::ranges::any_of(Selected, [Spline, Interpolation](uint32 Index) {
							return Spline->GetSplinePoint(Index)->OutgoingInterpolation != Interpolation;
						});
						MonaImGui::PropertyEdit::BeginRow("Interpolation", ViewContext.bReadOnly);
						const char* InterpolationPreview = bMixedInterpolation ? "Multiple Values"
							: Interpolation == ESplineSegmentInterpolation::Linear ? "Linear"
							: Interpolation == ESplineSegmentInterpolation::Constant ? "Constant" : "Cubic";
						if (ImGui::BeginCombo("##Interpolation", InterpolationPreview))
						{
							for (const auto& [Label, Value] : {std::pair{"Linear", ESplineSegmentInterpolation::Linear}, std::pair{"Cubic", ESplineSegmentInterpolation::Cubic}, std::pair{"Constant", ESplineSegmentInterpolation::Constant}})
								if (ImGui::Selectable(Label, !bMixedInterpolation && Value == Interpolation) && !ViewContext.bReadOnly)
									Changed |= EditSelectedPoints(Context, *Spline, ViewContext.bShouldTransact, "Set Spline Interpolation",
										[Value](uint32, FSplinePoint& Point) { if (Point.OutgoingInterpolation == Value) return false; Point.OutgoingInterpolation = Value; return true; });
							ImGui::EndCombo();
						}
						MonaImGui::PropertyEdit::EndRow(ViewContext.bReadOnly);

						const ESplineTangentMode TangentMode = Spline->GetSplinePoint(Selected.front())->TangentMode;
						const bool bMixedTangentMode = std::ranges::any_of(Selected, [Spline, TangentMode](uint32 Index) {
							return Spline->GetSplinePoint(Index)->TangentMode != TangentMode;
						});
						MonaImGui::PropertyEdit::BeginRow("Tangent Mode", ViewContext.bReadOnly);
						const char* TangentPreview = bMixedTangentMode ? "Multiple Values"
							: TangentMode == ESplineTangentMode::Automatic ? "Automatic"
							: TangentMode == ESplineTangentMode::AutomaticClamped ? "Automatic Clamped"
							: TangentMode == ESplineTangentMode::ManualAligned ? "Manual Aligned" : "Manual Broken";
						if (ImGui::BeginCombo("##TangentMode", TangentPreview))
						{
							for (const auto& [Label, Mode] : {std::pair{"Automatic", ESplineTangentMode::Automatic}, {"Clamped", ESplineTangentMode::AutomaticClamped},
								{"Manual Aligned", ESplineTangentMode::ManualAligned}, {"Manual Broken", ESplineTangentMode::ManualBroken}})
							{
								if (ImGui::Selectable(Label, !bMixedTangentMode && Mode == TangentMode) && !ViewContext.bReadOnly) Changed |= EditSelectedPoints(Context, *Spline, ViewContext.bShouldTransact, "Set Spline Tangent Mode",
									[Spline, Mode](uint32 Index, FSplinePoint& Point) { return SetTangentMode(*Spline, Index, Point, Mode); });
							}
							ImGui::EndCombo();
						}
						MonaImGui::PropertyEdit::EndRow(ViewContext.bReadOnly);

						MonaImGui::PropertyEdit::BeginRow("Order", ViewContext.bReadOnly);
						const bool bCanMoveUp = PrimaryIndex && *PrimaryIndex > 0;
						ImGui::BeginDisabled(!bCanMoveUp);
						if (ImGui::Button("Earlier") && !ViewContext.bReadOnly)
							Changed |= CommitSplineEdit(*Spline, ViewContext.bShouldTransact, "Reorder Spline Point", [&] { return Spline->MoveSplinePoint(*PrimaryIndex, *PrimaryIndex - 1); });
						ImGui::EndDisabled();
						ImGui::SameLine();
						const bool bCanMoveDown = PrimaryIndex && *PrimaryIndex + 1 < Spline->GetNumSplinePoints();
						ImGui::BeginDisabled(!bCanMoveDown);
						if (ImGui::Button("Later") && !ViewContext.bReadOnly)
							Changed |= CommitSplineEdit(*Spline, ViewContext.bShouldTransact, "Reorder Spline Point", [&] { return Spline->MoveSplinePoint(*PrimaryIndex, *PrimaryIndex + 1); });
						ImGui::EndDisabled();
						MonaImGui::PropertyEdit::EndRow(ViewContext.bReadOnly);

						MonaImGui::PropertyEdit::BeginRow("Point Actions", ViewContext.bReadOnly);
						if (ImGui::Button("Duplicate") && !ViewContext.bReadOnly && PrimaryIndex)
						{
							std::optional<uint32> Index;
							if (CommitSplineEdit(*Spline, ViewContext.bShouldTransact, "Duplicate Spline Point", [&] {
								Index = Spline->DuplicateSplinePoint(*PrimaryIndex);
								return Index.has_value();
							}))
							{
								Context.SelectSubElement(Spline, SplineElement(*Spline, EEditorSubElementKind::Point, *Index));
								Changed = true;
							}
						}
						ImGui::SameLine();
						ImGui::PushStyleColor(ImGuiCol_Text, MonaImGui::GetThemeColor(MonaImGui::EUIThemeColor::Error));
						if (ImGui::Button("Delete") && !ViewContext.bReadOnly)
							Changed |= DeleteSelectedSplinePoints(Context, *Spline, ViewContext.bShouldTransact);
						ImGui::PopStyleColor();
						MonaImGui::PropertyEdit::EndRow(ViewContext.bReadOnly);
						MonaImGui::PropertyEdit::EndGroup();
					}
					ImGui::PopID();
					return Changed;
					});
			}
		};

		class FSplineMeshActorDetailsCustomization final : public IDetailCustomization
		{
		public:
			auto CustomizeDetails(IDetailLayoutBuilder& Builder) -> void override
			{
				DObject* Object = Builder.GetObject();
				auto& Context = GetLevelDetailHostContext(Builder).Context;
				auto* Actor = Cast<ASplineMeshActor>(Object);
				if (!Actor) return;
				Builder.EditCategory("Diagnostics").AddCustomRow("Spline Path Generated Segments Diagnostics Edit",
					[Actor = TWeakObjectPtr<ASplineMeshActor>(Actor), &Context](
						FPropertyView&, const FPropertyViewContext& ViewContext) {
						auto* Resolved = Actor.Get();
						if (!Resolved) return false;
						MonaImGui::PropertyEdit::BeginRow("Path Output", ViewContext.bReadOnly);
						ImGui::Text("%zu generated segment(s)",
							Resolved->FindComponentsByClass<DSplineMeshComponent>().size());
						if (!Resolved->GetConstructionError().empty())
							ImGui::TextColored(MonaImGui::GetThemeColor(MonaImGui::EUIThemeColor::Error),
								"%s", Resolved->GetConstructionError().c_str());
						if (ImGui::Button("Edit Spline") && !ViewContext.bReadOnly)
						{
							Context.SelectComponent(Resolved->GetSplineComponent());
							if (Context.ActivateViewportEditMode) Context.ActivateViewportEditMode("Spline");
						}
						MonaImGui::PropertyEdit::EndRow(ViewContext.bReadOnly);
						return false;
					});
			}
		};
	} // namespace

	auto CalculateSplineAppendPosition(const DSplineComponent& Spline) -> FVector3
	{
		constexpr double DefaultAppendDistance = 10.0;
		const uint32 PointCount = Spline.GetNumSplinePoints();
		if (PointCount == 0) return FVector3(0.0);
		const FVector3 LastPosition = Spline.GetSplinePoint(PointCount - 1)->Position;
		if (PointCount == 1) return LastPosition + FVectorConstants::Forward * DefaultAppendDistance;

		const FVector3 LastChord = LastPosition - Spline.GetSplinePoint(PointCount - 2)->Position;
		const double LastChordLength = Math::Length(LastChord);
		if (LastChordLength <= SmallNumber) return LastPosition + FVectorConstants::Forward * DefaultAppendDistance;
		return LastPosition + LastChord / LastChordLength * std::min(LastChordLength, DefaultAppendDistance);
	}

	auto SplitSplineSegment(DSplineComponent& Spline, uint32 SegmentIndex, double T) -> bool
	{
		if (SegmentIndex >= Spline.GetNumSplineSegments() || !(T > 0.0 && T < 1.0)) return false;
		const uint32 StartIndex = SegmentIndex;
		const uint32 EndIndex = (SegmentIndex + 1) % Spline.GetNumSplinePoints();
		const FSplinePoint* StartPtr = Spline.GetSplinePoint(StartIndex);
		const FSplinePoint* EndPtr = Spline.GetSplinePoint(EndIndex);
		if (!StartPtr || !EndPtr) return false;
		auto Points = Spline.GetSplinePoints();
		FSplinePoint& Start = Points[StartIndex];
		FSplinePoint& End = Points[EndIndex];
		const auto Evaluation = Spline.GetEvaluationData();
		const auto& Segment = Evaluation->GetSegments()[SegmentIndex];
		const FSplineSample Sample = Evaluation->Evaluate({SegmentIndex, T});
		FSplinePoint Inserted(Sample.Position);
		Inserted.InputKey = std::lerp(Segment.StartKey, Segment.EndKey, T);
		if (Inserted.InputKey <= Segment.StartKey || Inserted.InputKey >= Segment.EndKey) return false;
		Inserted.Rotation = Sample.Rotation;
		Inserted.Scale = Sample.Scale;
		Inserted.OutgoingInterpolation = Start.OutgoingInterpolation;
		if (Start.OutgoingInterpolation == ESplineSegmentInterpolation::Cubic)
		{
			// Restrict the original Hermite polynomial in key space.
			if (Start.TangentMode != ESplineTangentMode::ManualBroken)
				SetTangentMode(Spline, StartIndex, Start, ESplineTangentMode::ManualBroken);
			if (End.TangentMode != ESplineTangentMode::ManualBroken)
				SetTangentMode(Spline, EndIndex, End, ESplineTangentMode::ManualBroken);
			Inserted.ArriveTangent = Inserted.LeaveTangent = Sample.FirstDerivative / (Segment.EndKey - Segment.StartKey);
			Inserted.TangentMode = ESplineTangentMode::ManualBroken;
		}
		Points.insert(Points.begin() + SegmentIndex + 1, Inserted);
		return Spline.SetSplinePoints(std::move(Points), EndIndex == 0 ? std::optional(Segment.EndKey) : std::nullopt);
	}

	auto CreateSplineComponentVisualizer() -> std::shared_ptr<IComponentEditorVisualizer> { return std::make_shared<FSplineComponentVisualizer>(); }
	auto CreateSplineDetailsCustomization() -> std::shared_ptr<IDetailCustomization> { return std::make_shared<FSplineDetailsCustomization>(); }
	auto CreateSplineMeshActorDetailsCustomization() -> std::shared_ptr<IDetailCustomization> { return std::make_shared<FSplineMeshActorDetailsCustomization>(); }
	auto RegisterSplineViewportEditMode(
		) -> FLevelViewportEditModeHandle
	{
		return FLevelViewportEditModeRegistry::Get().Register({
			.Id = "Spline", .DisplayName = "Spline", .Priority = 100,
			.CanActivate = [](const FLevelEditorContext& Context) { return !Context.bReadOnly && Cast<DSplineComponent>(Context.GetSelectedComponent()) != nullptr; },
			.Factory = [] { return std::make_unique<FSplineViewportEditMode>(); },
		});
	}
} // namespace Durin::LevelEditor
