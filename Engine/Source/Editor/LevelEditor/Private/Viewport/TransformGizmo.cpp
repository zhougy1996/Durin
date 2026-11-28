#include "Viewport/TransformGizmo.h"

#include "Components/SceneComponent.h"
#include "Transactions/Transactor.h"
#include "Engine/Actor.h"
#include "Workspace/LevelEditorContext.h"
#include "Math/Operations.h"
#include "DObject/Package.h"
#include "DObject/Object.h"
#include "DObject/ObjectLifecycle.h"
#include "SceneViewProjection.h"
#include "Viewport/LevelEditorViewportClient.h"
#include "MonaImGui.h"

namespace Durin::LevelEditor
{
	namespace
	{
		// Keep the manipulator comfortably readable without letting its projected size vary with scene scale.
		constexpr float GizmoPixels = 112.0f;
		constexpr float HitRadiusPixels = 12.0f;
		constexpr double Epsilon = 1.e-8;

		auto ThemeColor(MonaImGui::EUIThemeColor Color, float Alpha) -> FVector4f
		{
			const ImVec4& Value = MonaImGui::GetThemeColor(Color);
			return {Value.x, Value.y, Value.z, Alpha};
		}

		auto MixedThemeColor(MonaImGui::EUIThemeColor A, MonaImGui::EUIThemeColor B, float Alpha) -> FVector4f
		{
			FVector4f Color = (ThemeColor(A, Alpha) + ThemeColor(B, Alpha)) * 0.5f;
			Color.a = Alpha;
			return Color;
		}

		auto AxisForHandle(ETransformGizmoHandle Handle) -> FVector3
		{
			switch (Handle)
			{
			case ETransformGizmoHandle::X: return FVectorConstants::Forward;
			case ETransformGizmoHandle::Y: return FVectorConstants::Right;
			case ETransformGizmoHandle::Z: return FVectorConstants::Up;
			default: return FVectorConstants::Zero;
			}
		}

		auto ColorForHandle(ETransformGizmoHandle Handle, bool bHovered, bool bActive, float Alpha = 1.0f) -> FVector4f
		{
			if (bActive) return ThemeColor(MonaImGui::EUIThemeColor::Warning, Alpha);
			FVector4f Color;
			switch (Handle)
			{
			case ETransformGizmoHandle::X: Color = ThemeColor(MonaImGui::EUIThemeColor::AxisX, Alpha); break;
			case ETransformGizmoHandle::Y: Color = ThemeColor(MonaImGui::EUIThemeColor::AxisY, Alpha); break;
			case ETransformGizmoHandle::Z: Color = ThemeColor(MonaImGui::EUIThemeColor::AxisZ, Alpha); break;
			case ETransformGizmoHandle::XY: Color = MixedThemeColor(MonaImGui::EUIThemeColor::AxisX, MonaImGui::EUIThemeColor::AxisY, Alpha); break;
			case ETransformGizmoHandle::XZ: Color = MixedThemeColor(MonaImGui::EUIThemeColor::AxisX, MonaImGui::EUIThemeColor::AxisZ, Alpha); break;
			case ETransformGizmoHandle::YZ: Color = MixedThemeColor(MonaImGui::EUIThemeColor::AxisY, MonaImGui::EUIThemeColor::AxisZ, Alpha); break;
			default: Color = ThemeColor(MonaImGui::EUIThemeColor::ViewportText, Alpha); break;
			}
			if (bHovered) Color = Math::Min(Color * FVector4f(1.35f, 1.35f, 1.35f, 1.0f), FVector4f(1.0f));
			Color.a = Alpha;
			return Color;
		}

		auto RayPlane(const FVector3& Origin, const FVector3& Direction, const FVector3& PlanePoint, const FVector3& PlaneNormal, FVector3& Out) -> bool
		{
			const double Denominator = Math::Dot(Direction, PlaneNormal);
			if (std::abs(Denominator) <= Epsilon) return false;
			const double T = Math::Dot(PlanePoint - Origin, PlaneNormal) / Denominator;
			if (!std::isfinite(T)) return false;
			Out = Origin + Direction * T;
			return true;
		}

		auto DistanceToSegment(const FVector2f& Point, const FVector2f& A, const FVector2f& B) -> float
		{
			const FVector2f AB = B - A;
			const float LengthSq = Math::LengthSquared(AB);
			const float T = LengthSq > 0.001f ? std::clamp(Math::Dot(Point - A, AB) / LengthSq, 0.0f, 1.0f) : 0.0f;
			return Math::Length(Point - (A + AB * T));
		}

		auto PointInTriangle(const FVector2f& P, const FVector2f& A, const FVector2f& B, const FVector2f& C) -> bool
		{
			const auto Sign = [](const FVector2f& P1, const FVector2f& P2, const FVector2f& P3) { return (P1.x - P3.x) * (P2.y - P3.y) - (P2.x - P3.x) * (P1.y - P3.y); };
			const float D1 = Sign(P, A, B), D2 = Sign(P, B, C), D3 = Sign(P, C, A);
			return !((D1 < 0 || D2 < 0 || D3 < 0) && (D1 > 0 || D2 > 0 || D3 > 0));
		}

		auto Snap(double Value, double Step) -> double
		{
			return Step > Epsilon ? std::round(Value / Step) * Step : Value;
		}

		auto RotationFromX(const FVector3& Direction) -> FQuat
		{
			const FVector3 From = FVectorConstants::Forward;
			const FVector3 To = Math::Normalize(Direction);
			const double Dot = Math::Dot(From, To);
			if (Dot > 1.0 - Epsilon) return FQuatConstants::Identity;
			if (Dot < -1.0 + Epsilon)
				return Math::MakeQuaternionFromAxisAngleRadians(Math::Pi<double>(), FVectorConstants::Up);
			const FVector3 Cross = Math::Cross(From, To);
			return Math::Normalize(FQuat(1.0 + Dot, Cross.x, Cross.y, Cross.z));
		}

		class FActorTransformGizmoTarget final : public ITransformGizmoTarget
		{
		public:
			explicit FActorTransformGizmoTarget(AActor* InActor) : Actor(InActor) {}
			auto IsValid() const -> bool override { return Actor && !Actor->IsHidden() && Actor->GetRootComponent(); }
			auto GetIdentity() const -> const void* override { return Actor.Get(); }
			auto GetDataOwner() const -> DObject* override { return Actor ? Actor->GetRootComponent() : nullptr; }
			auto GetTransform() const -> FTransform override { return Actor ? Actor->GetActorTransform() : FTransform{}; }
			auto SetTransform(const FTransform& Transform) -> bool override { return Actor && Actor->SetActorTransform(Transform); }
			auto GetParentRotation() const -> FQuat override
			{
				if (Actor && Actor->GetRootComponent())
					if (const DSceneComponent* Parent = Actor->GetRootComponent()->GetAttachParent()) return Parent->GetWorldRotation();
				return FQuatConstants::Identity;
			}
			auto GetPackage() const -> DPackage* override { return Actor ? Actor->GetPackage() : nullptr; }
			auto GetLabel() const -> std::string override { return Actor ? Actor->GetActorLabel() : "Missing Actor"; }
			auto AddReferencedObjects(FReferenceCollector& Collector) -> void override
			{
				DObject* Object = Actor.Get();
				if (Object) Collector.AddReferencedObject(Object);
				if (Object != Actor.Get()) Actor = Cast<AActor>(Object);
			}

		private:
			TObjectPtr<AActor> Actor;
		};
	} // namespace

	auto MakeActorTransformGizmoTargets(const FLevelEditorContext& Context) -> FTransformGizmoTargetSet
	{
		FTransformGizmoTargetSet Result;
		Result.CollectionLabel = "Actors";
		for (const TObjectPtr<AActor>& ActorPtr : Context.GetSelectedActors())
		{
			AActor* Actor = ActorPtr.Get();
			if (!Actor || Actor->IsHidden() || !Actor->GetRootComponent()) continue;
			bool bSelectedAncestor = false;
			for (AActor* Parent = Actor->GetAttachParentActor(); Parent; Parent = Parent->GetAttachParentActor())
				if (Context.IsActorSelected(Parent)) { bSelectedAncestor = true; break; }
			if (!bSelectedAncestor) Result.Targets.push_back(std::make_shared<FActorTransformGizmoTarget>(Actor));
		}
		return Result;
	}

	auto FTransformGizmo::RebuildState(const FTransformGizmoTargetSet& Targets, const FSceneView& View) -> bool
	{
		FVector3 Sum(0.0);
		size_t Count = 0;
		ActiveCapabilities = ETransformGizmoCapability::All;
		for (const std::shared_ptr<ITransformGizmoTarget>& Target : Targets.Targets)
		{
			if (Target && Target->IsValid())
			{
				Sum += Target->GetTransform().Translation;
				ActiveCapabilities = static_cast<ETransformGizmoCapability>(static_cast<uint8>(ActiveCapabilities) & static_cast<uint8>(Target->GetCapabilities()));
				++Count;
			}
		}
		if (Count == 0)
		{
			WorldScale = 0.0f;
			return false;
		}
		Pivot = Sum / static_cast<double>(Count);
		Basis = FQuatConstants::Identity;
		const ETransformGizmoSpace EffectiveSpace = GetEffectiveSpace();
		if (!Targets.Targets.empty() && Targets.Targets.front() && Targets.Targets.front()->IsValid())
		{
			const std::shared_ptr<ITransformGizmoTarget>& Primary = Targets.Targets.front();
			if (EffectiveSpace == ETransformGizmoSpace::Local)
				Basis = Primary->GetTransform().Rotation;
			else if (EffectiveSpace == ETransformGizmoSpace::Parent)
				Basis = Primary->GetParentRotation();
		}
		const ETransformGizmoCapability Required = Mode == ETransformGizmoMode::Translate ? ETransformGizmoCapability::Translate
			: Mode == ETransformGizmoMode::Rotate ? ETransformGizmoCapability::Rotate : ETransformGizmoCapability::Scale;
		if (!HasCapability(ActiveCapabilities, Required))
		{
			WorldScale = 0.0f;
			return false;
		}
		DisplayPivot = Pivot;
		DisplayBasis = Basis;
		const double Distance = std::max(0.05, Math::Length(Pivot - View.ViewLocation));
		WorldScale = static_cast<float>(Distance * 2.0 * std::tan(Math::DegreesToRadians(60.0) * 0.5) * GizmoPixels / std::max(1u, View.ViewportHeight));
		return std::isfinite(WorldScale) && WorldScale > 0.0f;
	}

	auto FTransformGizmo::HitTest(const FSceneView& View, const FVector2f& MousePosition) const -> ETransformGizmoHandle
	{
		FVector2f Center;
		if (!SceneViewProjection::ProjectWorldToViewport(View, Pivot, Center)) return ETransformGizmoHandle::None;
		std::array<FVector3, 3> Axes = {Basis * FVectorConstants::Forward, Basis * FVectorConstants::Right, Basis * FVectorConstants::Up};
		std::array<ETransformGizmoHandle, 3> Handles = {ETransformGizmoHandle::X, ETransformGizmoHandle::Y, ETransformGizmoHandle::Z};
		ETransformGizmoHandle Best = ETransformGizmoHandle::None;
		float BestDistance = HitRadiusPixels;
		if (Mode == ETransformGizmoMode::Rotate)
		{
			for (size_t AxisIndex = 0; AxisIndex < 3; ++AxisIndex)
			{
				FVector3 U = Axes[(AxisIndex + 1) % 3];
				FVector3 V = Axes[(AxisIndex + 2) % 3];
				FVector2f Previous;
				bool bPrevious = false;
				for (uint32 Segment = 0; Segment <= 64; ++Segment)
				{
					const double Angle = Math::TwoPi<double>() * Segment / 64.0;
					FVector2f Current;
					const bool bCurrent = SceneViewProjection::ProjectWorldToViewport(View, Pivot + (U * std::cos(Angle) + V * std::sin(Angle)) * static_cast<double>(WorldScale), Current);
					if (bPrevious && bCurrent)
					{
						const float Distance = DistanceToSegment(MousePosition, Previous, Current);
						if (Distance < BestDistance)
						{
							BestDistance = Distance;
							Best = Handles[AxisIndex];
						}
					}
					Previous = Current;
					bPrevious = bCurrent;
				}
			}
			return Best;
		}

		if (Mode == ETransformGizmoMode::Translate)
		{
			// Couples a translation plane normal with its screen-space ranking.
			struct FPlane
			{
				size_t A;
				size_t B;
				ETransformGizmoHandle Handle;
			};
			for (const FPlane Plane : {FPlane{0, 1, ETransformGizmoHandle::XY}, FPlane{0, 2, ETransformGizmoHandle::XZ}, FPlane{1, 2, ETransformGizmoHandle::YZ}})
			{
				std::array<FVector2f, 4> P;
				const FVector3 A = Axes[Plane.A] * static_cast<double>(WorldScale);
				const FVector3 B = Axes[Plane.B] * static_cast<double>(WorldScale);
				if (SceneViewProjection::ProjectWorldToViewport(View, Pivot + A * 0.22 + B * 0.22, P[0]) && SceneViewProjection::ProjectWorldToViewport(View, Pivot + A * 0.42 + B * 0.22, P[1]) && SceneViewProjection::ProjectWorldToViewport(View, Pivot + A * 0.42 + B * 0.42, P[2]) && SceneViewProjection::ProjectWorldToViewport(View, Pivot + A * 0.22 + B * 0.42, P[3]))
				{
					if (PointInTriangle(MousePosition, P[0], P[1], P[2]) || PointInTriangle(MousePosition, P[0], P[2], P[3])) return Plane.Handle;
				}
			}
		}

		if (Mode == ETransformGizmoMode::Scale && Math::Length(MousePosition - Center) <= HitRadiusPixels) return ETransformGizmoHandle::Uniform;
		for (size_t AxisIndex = 0; AxisIndex < 3; ++AxisIndex)
		{
			FVector2f End;
			if (!SceneViewProjection::ProjectWorldToViewport(View, Pivot + Axes[AxisIndex] * static_cast<double>(WorldScale), End)) continue;
			const float Distance = DistanceToSegment(MousePosition, Center, End);
			if (Distance < BestDistance)
			{
				BestDistance = Distance;
				Best = Handles[AxisIndex];
			}
		}
		return Best;
	}

	auto FTransformGizmo::BeginDrag(const FTransformGizmoTargetSet& Targets, const FSceneView& View, const FLevelEditorViewportInput& Input) -> bool
	{
		ActiveHandle = HitTest(View, Input.MousePosition);
		if (ActiveHandle == ETransformGizmoHandle::None) return false;

		FVector3 RayOrigin, RayDirection;
		if (!SceneViewProjection::BuildViewportRay(View, Input.MousePosition, RayOrigin, RayDirection))
		{
			ActiveHandle = ETransformGizmoHandle::None;
			return false;
		}
		DragAxis = ActiveHandle == ETransformGizmoHandle::Uniform ? FVectorConstants::Zero : Math::Normalize(Basis * AxisForHandle(ActiveHandle));
		if (ActiveHandle == ETransformGizmoHandle::XY)
			DragPlaneNormal = Basis * FVectorConstants::Up;
		else if (ActiveHandle == ETransformGizmoHandle::XZ)
			DragPlaneNormal = Basis * FVectorConstants::Right;
		else if (ActiveHandle == ETransformGizmoHandle::YZ)
			DragPlaneNormal = Basis * FVectorConstants::Forward;
		else if (Mode == ETransformGizmoMode::Rotate)
			DragPlaneNormal = DragAxis;
		else if (ActiveHandle == ETransformGizmoHandle::Uniform)
			DragPlaneNormal = Math::Normalize(View.ViewLocation - Pivot);
		else
		{
			const FVector3 Side = Math::Cross(RayDirection, DragAxis);
			if (Math::LengthSquared(Side) <= Epsilon)
			{
				ActiveHandle = ETransformGizmoHandle::None;
				return false;
			}
			DragPlaneNormal = Math::Normalize(Math::Cross(DragAxis, Side));
		}
		if (!RayPlane(RayOrigin, RayDirection, Pivot, DragPlaneNormal, DragStartPoint))
		{
			ActiveHandle = ETransformGizmoHandle::None;
			return false;
		}
		DragStartVector = DragStartPoint - Pivot;
		if (Math::LengthSquared(DragStartVector) > Epsilon) DragStartVector = Math::Normalize(DragStartVector);
		DragStartMouseY = Input.MousePosition.y;
		if (!Proxy.emplace(Targets, Pivot, Basis).MatchesSelection(Targets))
		{
			ClearDrag();
			return false;
		}
		bRestorePending = false;
		return true;
	}

	auto FTransformGizmo::UpdateDrag(const FSceneView& View, const FLevelEditorViewportInput& Input) -> bool
	{
		FVector3 RayOrigin, RayDirection, Current;
		if (!SceneViewProjection::BuildViewportRay(View, Input.MousePosition, RayOrigin, RayDirection) || !RayPlane(RayOrigin, RayDirection, Pivot, DragPlaneNormal, Current)) return true;
		const bool bSnap = SnapSettings.bEnabled || Input.bCtrl;
		if (Mode == ETransformGizmoMode::Translate)
		{
			FVector3 Delta = Current - DragStartPoint;
			if (ActiveHandle == ETransformGizmoHandle::X || ActiveHandle == ETransformGizmoHandle::Y || ActiveHandle == ETransformGizmoHandle::Z) Delta = DragAxis * Math::Dot(Delta, DragAxis);
			if (bSnap)
			{
				const std::array<FVector3, 3> Axes = {Basis * FVectorConstants::Forward, Basis * FVectorConstants::Right, Basis * FVectorConstants::Up};
				FVector3 Snapped(0.0);
				for (const FVector3& Axis : Axes)
					Snapped += Axis * Snap(Math::Dot(Delta, Axis), SnapSettings.Translation);
				Delta = Snapped;
			}
			auto Frame = Proxy->GetInitialTransform();
			Frame.Translation += Delta;
			DisplayPivot = Frame.Translation;
			return Proxy->SetTransform(Frame);
		}
		else if (Mode == ETransformGizmoMode::Rotate)
		{
			FVector3 Vector = Current - Pivot;
			if (Math::LengthSquared(Vector) <= Epsilon) return true;
			Vector = Math::Normalize(Vector);
			double Angle = std::atan2(Math::Dot(DragAxis, Math::Cross(DragStartVector, Vector)), Math::Dot(DragStartVector, Vector));
			if (bSnap) Angle = Math::DegreesToRadians(Snap(Math::RadiansToDegrees(Angle), SnapSettings.RotationDegrees));
			auto Frame = Proxy->GetInitialTransform();
			Frame.Rotation = Math::Normalize(Math::MakeQuaternionFromAxisAngleRadians(Angle, DragAxis) * Basis);
			if (GetEffectiveSpace() == ETransformGizmoSpace::Local) DisplayBasis = Frame.Rotation;
			return Proxy->SetTransform(Frame);
		}
		else
		{
			double Factor = ActiveHandle == ETransformGizmoHandle::Uniform ? 1.0 + static_cast<double>(DragStartMouseY - Input.MousePosition.y) / GizmoPixels : 1.0 + Math::Dot(Current - DragStartPoint, DragAxis) / std::max(0.001f, WorldScale);
			if (bSnap) Factor = 1.0 + Snap(Factor - 1.0, SnapSettings.Scale);
			Factor = std::max(0.001, Factor);
			FVector3 Factors(1.0);
			if (ActiveHandle == ETransformGizmoHandle::Uniform)
				Factors = FVector3(Factor);
			else if (ActiveHandle == ETransformGizmoHandle::X)
				Factors.x = Factor;
			else if (ActiveHandle == ETransformGizmoHandle::Y)
				Factors.y = Factor;
			else if (ActiveHandle == ETransformGizmoHandle::Z)
				Factors.z = Factor;
			auto Frame = Proxy->GetInitialTransform();
			Frame.Scale3D = Factors;
			return Proxy->SetTransform(Frame);
		}
	}

	auto FTransformGizmo::ClearDrag() -> void
	{
		Proxy.reset();
		ActiveHandle = ETransformGizmoHandle::None;
		bRestorePending = false;
	}

	auto FTransformGizmo::FinishDrag(DTransactor* Transactions) -> bool
	{
		if (!Proxy->CheckPreviewOwnership()) { CancelDrag(); return false; }
		if (!Proxy->HasChanges()) return CancelDrag();
		const auto PackageRevisions = Proxy->GetPackageEditRevisions();
		if (Transactions)
		{
			const char* Action = Mode == ETransformGizmoMode::Translate ? "Translate" : Mode == ETransformGizmoMode::Rotate ? "Rotate" : "Scale";
			if (!Proxy->RecordChanges(*Transactions, Action))
			{
				CancelDrag();
				return false;
			}
		}
		// A committed transaction marks its packages. A release without history still needs a dirty mark.
		for (const auto& [Package, Revision] : PackageRevisions)
			if (Package && Package->GetEditRevision() == Revision) Package->MarkDirty();
		ClearDrag();
		return true;
	}

	auto FTransformGizmo::CancelDrag() -> bool
	{
		if (IsDragging())
		{
			bRestorePending = true;
			if (GUndo) return false;
			const bool bRestored = Proxy->Restore();
			DisplayPivot = Pivot;
			DisplayBasis = Basis;
			if (!bRestored) return false;
		}
		ClearDrag();
		return true;
	}

	auto FTransformGizmo::Update(FLevelEditorContext& Context, const FSceneView& View, const FLevelEditorViewportInput& Input, DTransactor* Transactions) -> bool
	{
		const bool bSucceeded = Update(MakeActorTransformGizmoTargets(Context), View, Input, Transactions);
		if (!bSucceeded) Context.SetError("Unable to apply or restore the transform gesture.");
		return bSucceeded;
	}

	auto FTransformGizmo::Update(const FTransformGizmoTargetSet& Targets, const FSceneView& View, const FLevelEditorViewportInput& Input, DTransactor* Transactions) -> bool
	{
		if (GUndo || (Transactions && ResolveEditorTransactor(true).value_or(nullptr) != Transactions))
		{
			// Restore only after the foreign recorder/replay has released GUndo.
			if (IsDragging()) bRestorePending = true;
			HoveredHandle = ETransformGizmoHandle::None;
			return true;
		}
		if (IsDragging())
		{
			if (bRestorePending) return CancelDrag();
			if (Input.bCancel || !Proxy->MatchesSelection(Targets)) return CancelDrag();
			if (!Input.bLeftMouseDown) return FinishDrag(Transactions);
			if (!Proxy->CheckPreviewOwnership()) { CancelDrag(); return false; }
			const bool bApplied = UpdateDrag(View, Input);
			const bool bCaptured = Proxy->CapturePreviewValues();
			if (!bApplied || !bCaptured) { CancelDrag(); return false; }
			return true;
		}
		if (!RebuildState(Targets, View))
		{
			HoveredHandle = ETransformGizmoHandle::None;
			return true;
		}
		if (Input.bFocused && Input.bHovered && !Input.bWantTextInput)
		{
			if (Input.bModeTranslate) Mode = ETransformGizmoMode::Translate;
			if (Input.bModeRotate) Mode = ETransformGizmoMode::Rotate;
			if (Input.bModeScale) Mode = ETransformGizmoMode::Scale;
		}
		HoveredHandle = Input.bHovered ? HitTest(View, Input.MousePosition) : ETransformGizmoHandle::None;
		return !Input.bLeftMousePressed || HoveredHandle == ETransformGizmoHandle::None || BeginDrag(Targets, View, Input);
	}
	auto FTransformGizmo::AppendOverlayPrimitives(FSceneView& View) const -> void
	{
		if (WorldScale <= 0.0f) return;
		const std::array<FVector3, 3> Axes = {DisplayBasis * FVectorConstants::Forward, DisplayBasis * FVectorConstants::Right, DisplayBasis * FVectorConstants::Up};
		const std::array<ETransformGizmoHandle, 3> Handles = {ETransformGizmoHandle::X, ETransformGizmoHandle::Y, ETransformGizmoHandle::Z};
		auto Add = [&](EViewOverlayShape Shape, const FMatrix& Matrix, ETransformGizmoHandle Handle, float Alpha = 1.0f) {
			View.OverlayPrimitives.push_back({Shape, Matrix, ColorForHandle(Handle, HoveredHandle == Handle, ActiveHandle == Handle, Alpha)});
		};
		if (Mode == ETransformGizmoMode::Rotate)
		{
			for (size_t Index = 0; Index < 3; ++Index)
			{
				const FQuat Rotation = RotationFromX(Axes[Index]);
				FMatrix Matrix = Math::TranslationMatrix(DisplayPivot) * Math::RotationMatrix(Rotation)
					* Math::ScaleMatrix(FVector3(WorldScale));
				Add(EViewOverlayShape::Ring, Matrix, Handles[Index]);
			}
			return;
		}
		for (size_t Index = 0; Index < 3; ++Index)
		{
			const FQuat Rotation = RotationFromX(Axes[Index]);
			FMatrix Matrix = Math::TranslationMatrix(DisplayPivot) * Math::RotationMatrix(Rotation)
				* Math::ScaleMatrix(FVector3(WorldScale));
			Add(Mode == ETransformGizmoMode::Translate ? EViewOverlayShape::Arrow : EViewOverlayShape::Axis, Matrix, Handles[Index]);
			if (Mode == ETransformGizmoMode::Scale)
			{
				FMatrix Box = Math::TranslationMatrix(DisplayPivot + Axes[Index] * static_cast<double>(WorldScale))
					* Math::ScaleMatrix(FVector3(WorldScale * 0.11f));
				Add(EViewOverlayShape::Box, Box, Handles[Index]);
			}
		}
		if (Mode == ETransformGizmoMode::Translate)
		{
			// Couples a translation plane normal with its screen-space ranking.
			struct FPlane
			{
				size_t A;
				size_t B;
				ETransformGizmoHandle Handle;
			};
			for (const FPlane Plane : {FPlane{0, 1, ETransformGizmoHandle::XY}, FPlane{0, 2, ETransformGizmoHandle::XZ}, FPlane{1, 2, ETransformGizmoHandle::YZ}})
			{
				const FVector3 A = Axes[Plane.A], B = Axes[Plane.B], N = Math::Normalize(Math::Cross(A, B));
				FMatrix Matrix(1.0);
				Matrix[0] = FVector4(A * static_cast<double>(WorldScale * 0.2f), 0.0);
				Matrix[1] = FVector4(B * static_cast<double>(WorldScale * 0.2f), 0.0);
				Matrix[2] = FVector4(N * static_cast<double>(WorldScale * 0.01f), 0.0);
				Matrix[3] = FVector4(DisplayPivot + (A + B) * static_cast<double>(WorldScale * 0.22f), 1.0);
				Add(EViewOverlayShape::Plane, Matrix, Plane.Handle, 0.36f);
			}
		}
		else
		{
			FMatrix Box = Math::TranslationMatrix(DisplayPivot) * Math::ScaleMatrix(FVector3(WorldScale * 0.13f));
			Add(EViewOverlayShape::Box, Box, ETransformGizmoHandle::Uniform);
		}
	}
} // namespace Durin::LevelEditor
