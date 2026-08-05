#include "Customizations/SplineEditorCustomizations.h"

#include "Components/SplineComponent.h"
#include "DObject/Package.h"
#include "Editor/EditorTransaction.h"
#include "Engine/Actor.h"
#include "MonaImGui.h"
#include "SceneViewProjection.h"
#include "Viewport/LevelEditorViewportClient.h"
#include "Viewport/TransformGizmo.h"
#include "Workspace/LevelEditorContext.h"

namespace Durin
{
	namespace
	{
		constexpr double kHandleScale = 1.0 / 3.0;

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
			return FVector3(glm::inverse(Spline.GetComponentToWorldMatrix()) * FVector4(Position, 1.0));
		}

		auto IsSelected(std::span<const FEditorSubElementSelection> Selection, const FEditorSubElementSelection& Element) -> bool
		{
			return std::ranges::find(Selection, Element) != Selection.end();
		}

		class FSplineSnapshotTransaction final : public IEditorTransaction
		{
		public:
			FSplineSnapshotTransaction(DSplineComponent* InSpline, std::string InDescription,
				std::vector<FSplinePoint> InBefore, bool bInBeforeClosed, std::vector<FSplinePoint> InAfter, bool bInAfterClosed)
				: Spline(InSpline), Description(std::move(InDescription)), Before(std::move(InBefore)), After(std::move(InAfter)),
				  bBeforeClosed(bInBeforeClosed), bAfterClosed(bInAfterClosed)
			{
				if (InSpline && InSpline->GetPackage()) Packages.push_back(InSpline->GetPackage());
			}
			auto GetDescription() const -> std::string_view override { return Description; }
			auto GetAffectedPackages() const -> std::span<DPackage* const> override { return Packages; }
			auto MutatesContent() const -> bool override { return true; }
			auto Undo() -> bool override { return Apply(Before, bBeforeClosed); }
			auto Redo() -> bool override { return Apply(After, bAfterClosed); }
		private:
			auto Apply(const std::vector<FSplinePoint>& Points, bool bClosed) -> bool
			{
				DSplineComponent* Resolved = Spline.Get();
				if (!Resolved) return false;
				Resolved->SetSplinePoints(Points);
				Resolved->SetClosedLoop(bClosed);
				return true;
			}
			TWeakObjectPtr<DSplineComponent> Spline;
			std::string Description;
			std::vector<FSplinePoint> Before;
			std::vector<FSplinePoint> After;
			bool bBeforeClosed = false;
			bool bAfterClosed = false;
			std::vector<DPackage*> Packages;
		};

		template<typename F>
		auto CommitSplineEdit(DSplineComponent& Spline, FEditorTransactionManager* Transactions, std::string Description, F&& Edit) -> bool
		{
			const std::vector<FSplinePoint> Before = Spline.GetSplinePoints();
			const bool bBeforeClosed = Spline.IsClosedLoop();
			if (!Edit()) return false;
			const std::vector<FSplinePoint> After = Spline.GetSplinePoints();
			const bool bAfterClosed = Spline.IsClosedLoop();
			if (Before == After && bBeforeClosed == bAfterClosed) return false;
			if (Transactions) Transactions->CommitApplied(std::make_unique<FSplineSnapshotTransaction>(
				&Spline, std::move(Description), Before, bBeforeClosed, After, bAfterClosed));
			return true;
		}

		auto GetSelectedPointIndices(const FLevelEditorContext& Context, const DSplineComponent& Spline) -> std::vector<uint32>
		{
			std::vector<uint32> Result;
			if (Context.GetSelectedComponent() != &Spline) return Result;
			for (const FEditorSubElementSelection& Element : Context.GetSelectedSubElements())
				if (Element.Kind == EEditorSubElementKind::Point)
					if (const auto Index = Spline.GetSplineCurve().FindPointIndex(Element.StableId)) Result.push_back(*Index);
			return Result;
		}

		template<typename F>
		auto EditSelectedPoints(FLevelEditorContext& Context, DSplineComponent& Spline, FEditorTransactionManager* Transactions,
			std::string Description, F&& Edit) -> bool
		{
			const std::vector<uint32> Indices = GetSelectedPointIndices(Context, Spline);
			if (Indices.empty()) return false;
			return CommitSplineEdit(Spline, Transactions, std::move(Description), [&] {
				bool bChanged = false;
				for (uint32 Index : Indices)
				{
					const FSplinePoint* Existing = Spline.GetSplinePoint(Index);
					if (!Existing) continue;
					FSplinePoint Point = *Existing;
					if (Edit(Index, Point)) { Spline.UpdateSplinePoint(Index, Point); bChanged = true; }
				}
				return bChanged;
			});
		}

		auto SetTangentMode(DSplineComponent& Spline, uint32 Index, FSplinePoint& Point, ESplineTangentMode Mode) -> bool
		{
			if (Point.TangentMode == Mode) return false;
			if (Mode == ESplineTangentMode::ManualAligned || Mode == ESplineTangentMode::ManualBroken)
			{
				if (Index < Spline.GetNumSplineSegments()) Point.LeaveTangent = Spline.GetSampleAtParameter({Index, 0.0}).FirstDerivative;
				if (Index > 0 || Spline.IsClosedLoop())
				{
					const uint32 Previous = Index == 0 ? Spline.GetNumSplineSegments() - 1 : Index - 1;
					Point.ArriveTangent = Spline.GetSampleAtParameter({Previous, 1.0}).FirstDerivative;
				}
				if (Mode == ESplineTangentMode::ManualAligned)
				{
					const FVector3 Seed = glm::length(Point.LeaveTangent) > kSmallNumber ? Point.LeaveTangent : Point.ArriveTangent;
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
			auto IsValid() const -> bool override { return ResolvePoint() != nullptr; }
			auto GetIdentity() const -> const void* override { return ResolvePoint(); }
			auto GetTransform() const -> FTransform override
			{
				FTransform Result;
				const FSplinePoint* Point = ResolvePoint();
				DSplineComponent* Resolved = Spline.Get();
				if (!Point || !Resolved) return Result;
				FVector3 LocalPosition = Point->Position;
				if (Element.Kind == EEditorSubElementKind::ArriveTangent) LocalPosition -= Point->ArriveTangent * kHandleScale;
				if (Element.Kind == EEditorSubElementKind::LeaveTangent) LocalPosition += Point->LeaveTangent * kHandleScale;
				Result.Translation = LocalToWorld(*Resolved, LocalPosition);
				return Result;
			}
			auto SetTransform(const FTransform& Transform) -> bool override
			{
				DSplineComponent* Resolved = Spline.Get();
				if (!Resolved) return false;
				const std::optional<uint32> Index = Resolved->GetSplineCurve().FindPointIndex(Element.StableId);
				const FSplinePoint* Existing = Index ? Resolved->GetSplinePoint(*Index) : nullptr;
				if (!Existing) return false;
				FSplinePoint Point = *Existing;
				const FVector3 LocalPosition = WorldToLocal(*Resolved, Transform.Translation);
				if (Element.Kind == EEditorSubElementKind::Point) Point.Position = LocalPosition;
				else
				{
					const FVector3 Tangent = (Element.Kind == EEditorSubElementKind::ArriveTangent)
						? (Point.Position - LocalPosition) / kHandleScale : (LocalPosition - Point.Position) / kHandleScale;
					if (Element.Kind == EEditorSubElementKind::ArriveTangent) Point.ArriveTangent = Tangent;
					else Point.LeaveTangent = Tangent;
					if (Point.TangentMode == ESplineTangentMode::ManualAligned)
						Point.ArriveTangent = Point.LeaveTangent = Tangent;
				}
				return Resolved->UpdateSplinePoint(*Index, Point);
			}
			auto GetPackage() const -> DPackage* override { return Spline.Get() ? Spline.Get()->GetPackage() : nullptr; }
			auto GetLabel() const -> std::string override { return Element.Kind == EEditorSubElementKind::Point ? "Spline Point" : "Spline Tangent"; }
			auto GetCapabilities() const -> ETransformGizmoCapability override { return ETransformGizmoCapability::Translate; }
		private:
			auto ResolvePoint() const -> const FSplinePoint*
			{
				DSplineComponent* Resolved = Spline.Get();
				if (!Resolved) return nullptr;
				const std::optional<uint32> Index = Resolved->GetSplineCurve().FindPointIndex(Element.StableId);
				return Index ? Resolved->GetSplinePoint(*Index) : nullptr;
			}
			TWeakObjectPtr<DSplineComponent> Spline;
			FEditorSubElementSelection Element;
		};

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
					const FEditorSubElementSelection Element{EEditorSubElementKind::Segment, StartPoint->Id, static_cast<int32>(SegmentIndex)};
					FVector3 Previous = Spline->GetSampleAtParameter({SegmentIndex, 0.0}, ESplineCoordinateSpace::World).Position;
					const auto& Samples = Evaluation->GetSegments()[SegmentIndex].DistanceSamples;
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
							EViewOverlayLinePattern::Solid, 12.0f, HoverColor};
						Line.Element = Element;
						Collector.AddLine(Line);
						Previous = Current;
					}
				}
				if (!Context.bSelected) return;
				for (uint32 PointIndex = 0; PointIndex < Spline->GetNumSplinePoints(); ++PointIndex)
				{
					const FSplinePoint* Point = Spline->GetSplinePoint(PointIndex);
					if (!Point) continue;
					const FEditorSubElementSelection PointElement{EEditorSubElementKind::Point, Point->Id};
					const FVector4f PointColor = ToColor(IsSelected(Context.SelectedSubElements, PointElement)
						? MonaImGui::EUIThemeColor::SelectionPrimary : MonaImGui::EUIThemeColor::Success);
					const FVector3 WorldPoint = LocalToWorld(*Spline, Point->Position);
					FEditorVisualizationBox PointBox{WorldPoint, PointColor, 12.0f, 5.0f, 80, Actor, Spline, true, HoverColor};
					PointBox.Element = PointElement;
					Collector.AddBox(PointBox);
					if (Point->TangentMode != ESplineTangentMode::ManualAligned && Point->TangentMode != ESplineTangentMode::ManualBroken) continue;
					const FVector3 Arrive = LocalToWorld(*Spline, Point->Position - Point->ArriveTangent * kHandleScale);
					const FVector3 Leave = LocalToWorld(*Spline, Point->Position + Point->LeaveTangent * kHandleScale);
					for (const auto& [Handle, Kind] : {std::pair{Arrive, EEditorSubElementKind::ArriveTangent}, std::pair{Leave, EEditorSubElementKind::LeaveTangent}})
					{
						FEditorVisualizationLine Stem{WorldPoint, Handle, ToColor(MonaImGui::EUIThemeColor::Info), 1.5f, 6.0f, 40, Actor, Spline};
						Stem.Element = {Kind, Point->Id};
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
				const float Distance = glm::length(Screen - Mouse);
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
					Context.SelectSubElement(Spline, {EEditorSubElementKind::Point, Spline->GetSplinePoint(0)->Id});
			}
			auto Exit(FLevelEditorContext& Context, bool) -> void override
			{
				if (LastClient) LastClient->GetTransformGizmo().CancelDrag();
				if (Context.GetSelectedComponent() == LastSpline.Get()) Context.ClearSubElementSelection();
				LastClient = nullptr;
				LastSpline = nullptr;
				bExitRequested = false;
			}
			auto Tick(FLevelEditorContext& Context, FLevelEditorViewportClient& Client, const FSceneView& View,
				FLevelEditorViewportInput& Input, FEditorTransactionManager* Transactions) -> bool override
			{
				LastClient = &Client;
				auto* Spline = Cast<DSplineComponent>(Context.GetSelectedComponent());
				if (!Spline) return false;
				LastSpline = Spline;
				RepairSelection(Context, *Spline);
				const FTransformGizmoTargetSet Targets = GetGizmoTargets(Context);
				const bool bWasDragging = Client.GetTransformGizmo().IsDragging();
				Client.GetTransformGizmo().Update(Targets, View, Input, Transactions);
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
				if (Input.bDelete) return DeleteSelected(Context, *Spline, Transactions);
				if (Input.bDuplicate) return DuplicateSelected(Context, *Spline, Transactions);
				if (Input.bAppend) return AppendPoint(Context, *Spline, Transactions);
				if (!Input.bRequestSelection) return false;
				const FEditorVisualizationHit Hit = Client.PickVisualizationWithView(Context.Level, View, Input.MousePosition);
				if (Hit.Component != Spline || !Hit.Element.IsValid()) { Context.ClearSubElementSelection(); return true; }
				if (Input.bLeftMouseDoubleClicked && Hit.Element.Kind == EEditorSubElementKind::Segment)
				{
					FGuid NewId;
					const uint32 Segment = static_cast<uint32>(Hit.Element.SecondaryIndex);
					if (CommitSplineEdit(*Spline, Transactions, "Insert Spline Point", [&] { return SplitSplineSegment(*Spline, Segment, FindSegmentT(*Spline, Segment, View, Input.MousePosition), &NewId); }))
						Context.SelectSubElement(Spline, {EEditorSubElementKind::Point, NewId});
					return true;
				}
				if (Input.bCtrl && Hit.Element.Kind == EEditorSubElementKind::Point) Context.ToggleSubElement(Spline, Hit.Element);
				else Context.SelectSubElement(Spline, Hit.Element);
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

			static auto RepairSelection(FLevelEditorContext& Context, DSplineComponent& Spline) -> void
			{
				std::vector<FEditorSubElementSelection> Valid;
				for (const auto& Element : Context.GetSelectedSubElements())
					if (Spline.GetSplineCurve().FindPointIndex(Element.StableId)) Valid.push_back(Element);
				if (Valid.size() == Context.GetSelectedSubElements().size()) return;
				Context.ClearSubElementSelection();
				for (const auto& Element : Valid) Context.ToggleSubElement(&Spline, Element);
			}
			static auto DeleteSelected(FLevelEditorContext& Context, DSplineComponent& Spline, FEditorTransactionManager* Transactions) -> bool
			{
				std::unordered_set<FGuid> Ids;
				for (const auto& Element : Context.GetSelectedSubElements()) if (Element.Kind == EEditorSubElementKind::Point) Ids.insert(Element.StableId);
				if (Ids.empty()) return false;
				const bool Changed = CommitSplineEdit(Spline, Transactions, "Delete Spline Points", [&] {
					std::vector<FSplinePoint> Points = Spline.GetSplinePoints();
					std::erase_if(Points, [&Ids](const FSplinePoint& Point) { return Ids.contains(Point.Id); });
					Spline.SetSplinePoints(std::move(Points)); return true;
				});
				if (Changed) Context.ClearSubElementSelection();
				return Changed;
			}
			static auto DuplicateSelected(FLevelEditorContext& Context, DSplineComponent& Spline, FEditorTransactionManager* Transactions) -> bool
			{
				const auto Index = Spline.GetSplineCurve().FindPointIndex(Context.GetSelectedSubElement().StableId);
				FGuid NewId;
				const bool Changed = Index && CommitSplineEdit(Spline, Transactions, "Duplicate Spline Point", [&] {
					const auto NewIndex = Spline.DuplicateSplinePoint(*Index); if (!NewIndex) return false;
					NewId = Spline.GetSplinePoint(*NewIndex)->Id; return true;
				});
				if (Changed) Context.SelectSubElement(&Spline, {EEditorSubElementKind::Point, NewId});
				return Changed;
			}
			static auto AppendPoint(FLevelEditorContext& Context, DSplineComponent& Spline, FEditorTransactionManager* Transactions) -> bool
			{
				FGuid NewId;
				const bool Changed = CommitSplineEdit(Spline, Transactions, "Append Spline Point", [&] {
					FVector3 Position = Spline.GetNumSplinePoints() ? Spline.GetSplinePoint(Spline.GetNumSplinePoints() - 1)->Position + FVectorConstants::Forward * 100.0 : FVector3(0.0);
					const uint32 Index = Spline.AddSplinePoint(FSplinePoint(Position)); NewId = Spline.GetSplinePoint(Index)->Id; return true;
				});
				if (Changed) Context.SelectSubElement(&Spline, {EEditorSubElementKind::Point, NewId});
				return Changed;
			}
		};

		class FSplineDetailsCustomization final : public IObjectDetailsCustomization
		{
		public:
			auto CustomizeDetails(FLevelEditorContext& Context, DObject* Object, FObjectPropertyViewBuilder& Builder) -> void override
			{
				auto* Spline = Cast<DSplineComponent>(Object);
				if (!Spline) return;
				Builder.ReplaceDefaultProperties();
				Builder.AddCustomRow("Spline Edit Points Tangents Append Duplicate Delete Loop", [&Context, Spline](FReflectedPropertyView&, const FReflectedPropertyViewContext& ViewContext) {
					ImGui::PushID(Spline);
					bool Changed = false;
					if (ImGui::Button("Edit Spline") && Context.ActivateViewportEditMode) Context.ActivateViewportEditMode("Spline");
					ImGui::SameLine(); ImGui::TextDisabled("%u points", Spline->GetNumSplinePoints());
					if (Context.GetSelectedComponent() == Spline && Context.GetSelectedSubElement().IsValid())
						ImGui::Text("Selected: %s", Context.GetSelectedSubElement().StableId.ToString().c_str());
					const std::vector<uint32> Selected = GetSelectedPointIndices(Context, *Spline);
					if (!Selected.empty())
					{
						const FVector3 Position = Spline->GetSplinePoint(Selected.front())->Position;
						const bool bMixedPosition = std::ranges::any_of(Selected, [Spline, Position](uint32 Index) {
							return glm::length(Spline->GetSplinePoint(Index)->Position - Position) > 1e-9;
						});
						double PositionValue[3] = {Position.x, Position.y, Position.z};
						ImGui::TextDisabled(bMixedPosition ? "Position (Multiple Values)" : "Position");
						ImGui::SetNextItemWidth(-FLT_MIN);
						if (bMixedPosition) ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
						const bool bPositionCommitted = !ViewContext.bReadOnly && ImGui::InputScalarN("##SplinePointPosition", ImGuiDataType_Double,
							PositionValue, 3, nullptr, nullptr, "%.3f", ImGuiInputTextFlags_EnterReturnsTrue);
						if (bMixedPosition) ImGui::PopItemFlag();
						if (bPositionCommitted)
						{
							const FVector3 Delta = FVector3(PositionValue[0], PositionValue[1], PositionValue[2]) - Position;
							Changed |= EditSelectedPoints(Context, *Spline, ViewContext.Transactions, "Edit Spline Point Position",
								[Delta](uint32, FSplinePoint& Point) { Point.Position += Delta; return glm::length(Delta) > 1e-12; });
						}
						if (Selected.size() == 1)
						{
							const FSplinePoint* SelectedPoint = Spline->GetSplinePoint(Selected.front());
							if (SelectedPoint->TangentMode == ESplineTangentMode::ManualAligned || SelectedPoint->TangentMode == ESplineTangentMode::ManualBroken)
							{
								for (const auto& [Label, bArrive] : {std::pair{"Arrive Tangent", true}, {"Leave Tangent", false}})
								{
									const FVector3 Value = bArrive ? SelectedPoint->ArriveTangent : SelectedPoint->LeaveTangent;
									double Components[3] = {Value.x, Value.y, Value.z};
									ImGui::TextDisabled("%s", Label); ImGui::SetNextItemWidth(-FLT_MIN);
									if (!ViewContext.bReadOnly && ImGui::InputScalarN(bArrive ? "##ArriveTangent" : "##LeaveTangent", ImGuiDataType_Double,
										Components, 3, nullptr, nullptr, "%.3f", ImGuiInputTextFlags_EnterReturnsTrue))
									{
										const FVector3 NewValue(Components[0], Components[1], Components[2]);
										Changed |= EditSelectedPoints(Context, *Spline, ViewContext.Transactions, "Edit Spline Tangent",
											[bArrive, NewValue](uint32, FSplinePoint& Point) {
												if (bArrive) Point.ArriveTangent = NewValue; else Point.LeaveTangent = NewValue;
												if (Point.TangentMode == ESplineTangentMode::ManualAligned) Point.ArriveTangent = Point.LeaveTangent = NewValue;
												return true;
											});
									}
								}
							}
						}
						ImGui::TextDisabled("Interpolation"); ImGui::SameLine();
						if (!ViewContext.bReadOnly && ImGui::SmallButton("Linear")) Changed |= EditSelectedPoints(Context, *Spline, ViewContext.Transactions, "Set Spline Interpolation",
							[](uint32, FSplinePoint& Point) { if (Point.OutgoingInterpolation == ESplineSegmentInterpolation::Linear) return false; Point.OutgoingInterpolation = ESplineSegmentInterpolation::Linear; return true; });
						ImGui::SameLine();
						if (!ViewContext.bReadOnly && ImGui::SmallButton("Cubic")) Changed |= EditSelectedPoints(Context, *Spline, ViewContext.Transactions, "Set Spline Interpolation",
							[](uint32, FSplinePoint& Point) { if (Point.OutgoingInterpolation == ESplineSegmentInterpolation::Cubic) return false; Point.OutgoingInterpolation = ESplineSegmentInterpolation::Cubic; return true; });
						ImGui::TextDisabled("Tangent Mode");
						for (const auto& [Label, Mode] : {std::pair{"Automatic", ESplineTangentMode::Automatic}, {"Clamped", ESplineTangentMode::AutomaticClamped},
							{"Aligned", ESplineTangentMode::ManualAligned}, {"Broken", ESplineTangentMode::ManualBroken}})
						{
							ImGui::SameLine();
							if (!ViewContext.bReadOnly && ImGui::SmallButton(Label)) Changed |= EditSelectedPoints(Context, *Spline, ViewContext.Transactions, "Set Spline Tangent Mode",
								[Spline, Mode](uint32 Index, FSplinePoint& Point) { return SetTangentMode(*Spline, Index, Point, Mode); });
						}
						const auto PrimaryIndex = Spline->GetSplineCurve().FindPointIndex(Context.GetSelectedSubElement().StableId);
						if (!ViewContext.bReadOnly && PrimaryIndex && ImGui::Button("Move Up") && *PrimaryIndex > 0)
							Changed |= CommitSplineEdit(*Spline, ViewContext.Transactions, "Reorder Spline Point", [&] { return Spline->MoveSplinePoint(*PrimaryIndex, *PrimaryIndex - 1); });
						ImGui::SameLine();
						if (!ViewContext.bReadOnly && PrimaryIndex && ImGui::Button("Move Down") && *PrimaryIndex + 1 < Spline->GetNumSplinePoints())
							Changed |= CommitSplineEdit(*Spline, ViewContext.Transactions, "Reorder Spline Point", [&] { return Spline->MoveSplinePoint(*PrimaryIndex, *PrimaryIndex + 1); });
						ImGui::SameLine();
						if (!ViewContext.bReadOnly && PrimaryIndex && ImGui::Button("Duplicate"))
						{
							FGuid Id; Changed |= CommitSplineEdit(*Spline, ViewContext.Transactions, "Duplicate Spline Point", [&] {
								const auto NewIndex = Spline->DuplicateSplinePoint(*PrimaryIndex); if (!NewIndex) return false; Id = Spline->GetSplinePoint(*NewIndex)->Id; return true; });
							if (Id.IsValid()) Context.SelectSubElement(Spline, {EEditorSubElementKind::Point, Id});
						}
						ImGui::SameLine();
						if (!ViewContext.bReadOnly && ImGui::Button("Delete"))
						{
							std::unordered_set<FGuid> Ids;
							for (const auto& Element : Context.GetSelectedSubElements()) if (Element.Kind == EEditorSubElementKind::Point) Ids.insert(Element.StableId);
							Changed |= CommitSplineEdit(*Spline, ViewContext.Transactions, "Delete Spline Points", [&] {
								std::vector<FSplinePoint> Points = Spline->GetSplinePoints(); std::erase_if(Points, [&Ids](const FSplinePoint& Point) { return Ids.contains(Point.Id); });
								Spline->SetSplinePoints(std::move(Points)); return true; });
							Context.ClearSubElementSelection();
						}
					}
					if (!ViewContext.bReadOnly && ImGui::Button("Append Point"))
					{
						FGuid Id; Changed = CommitSplineEdit(*Spline, ViewContext.Transactions, "Append Spline Point", [&] {
							const FVector3 Position = Spline->GetNumSplinePoints() ? Spline->GetSplinePoint(Spline->GetNumSplinePoints() - 1)->Position + FVectorConstants::Forward * 100.0 : FVector3(0.0);
							const uint32 Index = Spline->AddSplinePoint(FSplinePoint(Position)); Id = Spline->GetSplinePoint(Index)->Id; return true; });
						if (Changed) Context.SelectSubElement(Spline, {EEditorSubElementKind::Point, Id});
					}
					ImGui::SameLine();
					if (!ViewContext.bReadOnly && ImGui::Button(Spline->IsClosedLoop() ? "Open Loop" : "Close Loop"))
						Changed |= CommitSplineEdit(*Spline, ViewContext.Transactions, "Toggle Spline Loop", [&] { Spline->SetClosedLoop(!Spline->IsClosedLoop()); return true; });
					ImGui::PopID();
					return Changed;
				});
			}
		};
	} // namespace

	auto SplitSplineSegment(DSplineComponent& Spline, uint32 SegmentIndex, double T, FGuid* OutPointId) -> bool
	{
		if (SegmentIndex >= Spline.GetNumSplineSegments() || !(T > 0.0 && T < 1.0)) return false;
		const uint32 StartIndex = SegmentIndex;
		const uint32 EndIndex = (SegmentIndex + 1) % Spline.GetNumSplinePoints();
		const FSplinePoint* StartPtr = Spline.GetSplinePoint(StartIndex);
		const FSplinePoint* EndPtr = Spline.GetSplinePoint(EndIndex);
		if (!StartPtr || !EndPtr) return false;
		FSplinePoint Start = *StartPtr;
		FSplinePoint End = *EndPtr;
		FSplinePoint Inserted(Spline.GetSampleAtParameter({SegmentIndex, T}).Position);
		Inserted.OutgoingInterpolation = Start.OutgoingInterpolation;
		if (Start.OutgoingInterpolation == ESplineSegmentInterpolation::Cubic)
		{
			const FSplineSample StartSample = Spline.GetSampleAtParameter({SegmentIndex, 0.0});
			const FSplineSample EndSample = Spline.GetSampleAtParameter({SegmentIndex, 1.0});
			const FVector3 P0 = StartSample.Position, P1 = P0 + StartSample.FirstDerivative / 3.0;
			const FVector3 P3 = EndSample.Position, P2 = P3 - EndSample.FirstDerivative / 3.0;
			const FVector3 A = glm::mix(P0, P1, T), B = glm::mix(P1, P2, T), C = glm::mix(P2, P3, T);
			const FVector3 D = glm::mix(A, B, T), E = glm::mix(B, C, T), Q = glm::mix(D, E, T);
			Start.LeaveTangent = (A - P0) * 3.0;
			Inserted.Position = Q;
			Inserted.ArriveTangent = (Q - D) * 3.0;
			Inserted.LeaveTangent = (E - Q) * 3.0;
			End.ArriveTangent = (P3 - C) * 3.0;
			Start.TangentMode = Inserted.TangentMode = End.TangentMode = ESplineTangentMode::ManualBroken;
		}
		if (!Spline.UpdateSplinePoint(StartIndex, Start) || !Spline.UpdateSplinePoint(EndIndex, End)) return false;
		const uint32 InsertIndex = SegmentIndex + 1;
		if (!Spline.InsertSplinePoint(InsertIndex, Inserted)) return false;
		if (OutPointId) *OutPointId = Spline.GetSplinePoint(InsertIndex)->Id;
		return true;
	}

	auto CreateSplineComponentVisualizer() -> std::shared_ptr<IComponentEditorVisualizer> { return std::make_shared<FSplineComponentVisualizer>(); }
	auto CreateSplineDetailsCustomization() -> std::shared_ptr<IObjectDetailsCustomization> { return std::make_shared<FSplineDetailsCustomization>(); }
	auto RegisterSplineViewportEditMode() -> FLevelViewportEditModeHandle
	{
		return FLevelViewportEditModeRegistry::Get().Register({
			.Id = "Spline", .DisplayName = "Spline", .Priority = 100,
			.CanActivate = [](const FLevelEditorContext& Context) { return !Context.bReadOnly && Cast<DSplineComponent>(Context.GetSelectedComponent()) != nullptr; },
			.Factory = [] { return std::make_unique<FSplineViewportEditMode>(); },
		});
	}
} // namespace Durin
