#include "Viewport/TransformGizmo.h"

#include "Components/SceneComponent.h"
#include "Editor/EditorTransaction.h"
#include "Engine/Actor.h"
#include "LevelEditorContext.h"
#include "Math/TransformDecomposition.h"
#include "DObject/Package.h"
#include "SceneViewProjection.h"
#include "Viewport/LevelEditorViewportClient.h"
#include "MonaImGui.h"

namespace Durin
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
			if (bHovered) Color = glm::min(Color * FVector4f(1.35f, 1.35f, 1.35f, 1.0f), FVector4f(1.0f));
			Color.a = Alpha;
			return Color;
		}

		auto RayPlane(const FVector3& Origin, const FVector3& Direction, const FVector3& PlanePoint, const FVector3& PlaneNormal, FVector3& Out) -> bool
		{
			const double Denominator = glm::dot(Direction, PlaneNormal);
			if (std::abs(Denominator) <= Epsilon) return false;
			const double T = glm::dot(PlanePoint - Origin, PlaneNormal) / Denominator;
			if (!std::isfinite(T)) return false;
			Out = Origin + Direction * T;
			return true;
		}

		auto DistanceToSegment(const FVector2f& Point, const FVector2f& A, const FVector2f& B) -> float
		{
			const FVector2f AB = B - A;
			const float LengthSq = glm::dot(AB, AB);
			const float T = LengthSq > 0.001f ? std::clamp(glm::dot(Point - A, AB) / LengthSq, 0.0f, 1.0f) : 0.0f;
			return glm::length(Point - (A + AB * T));
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
			const FVector3 To = glm::normalize(Direction);
			const double Dot = glm::dot(From, To);
			if (Dot > 1.0 - Epsilon) return glm::identity<FQuat>();
			if (Dot < -1.0 + Epsilon) return glm::angleAxis(glm::pi<double>(), FVectorConstants::Up);
			const FVector3 Cross = glm::cross(From, To);
			return glm::normalize(FQuat(1.0 + Dot, Cross.x, Cross.y, Cross.z));
		}

		class FActorTransformTransaction final : public IEditorTransaction
		{
		public:
			struct FEntry
			{
				TObjectPtr<AActor> Actor;
				FTransform Before;
				FTransform After;
			};
			FActorTransformTransaction(std::string_view Action, std::vector<FEntry> InEntries)
				: Entries(std::move(InEntries))
			{
				if (Entries.size() == 1 && Entries.front().Actor)
					Description = std::format("{} '{}'", Action, Entries.front().Actor->GetName());
				else
					Description = std::format("{} {} Actors", Action, Entries.size());
			}
			auto GetDescription() const -> std::string_view override { return Description; }
			auto GetDetails(EEditorTransactionOperation Operation) const -> std::string override { return BuildDetails(Operation != EEditorTransactionOperation::Undo); }
			auto Undo() -> bool override { return Apply(false); }
			auto Redo() -> bool override { return Apply(true); }

		private:
			static auto FormatVector(const FVector3& Value) -> std::string
			{
				return std::format("({:.3f}, {:.3f}, {:.3f})", Value.x, Value.y, Value.z);
			}

			static auto VectorChanged(const FVector3& Before, const FVector3& After) -> bool
			{
				return glm::length(After - Before) > Epsilon;
			}

			static auto RotationChanged(const FQuat& Before, const FQuat& After) -> bool
			{
				return 1.0 - std::abs(glm::dot(glm::normalize(Before), glm::normalize(After))) > Epsilon;
			}

			auto BuildDetails(bool bForward) const -> std::string
			{
				std::string Result;
				for (const FEntry& Entry : Entries)
				{
					if (!Result.empty()) Result += '\n';
					Result += std::format("'{}'", Entry.Actor ? Entry.Actor->GetName() : "Missing Actor");
					const FTransform& Before = bForward ? Entry.Before : Entry.After;
					const FTransform& After = bForward ? Entry.After : Entry.Before;
					bool bHasChange = false;
					if (VectorChanged(Before.Translation, After.Translation))
					{
						Result += std::format("\n  Location  {} -> {}", FormatVector(Before.Translation), FormatVector(After.Translation));
						Result += std::format("\n  Delta     {}", FormatVector(After.Translation - Before.Translation));
						bHasChange = true;
					}
					if (RotationChanged(Before.Rotation, After.Rotation))
					{
						const FVector3 BeforeDegrees = glm::degrees(glm::eulerAngles(Before.Rotation));
						const FVector3 AfterDegrees = glm::degrees(glm::eulerAngles(After.Rotation));
						Result += std::format("\n  Rotation  {} -> {} degrees", FormatVector(BeforeDegrees), FormatVector(AfterDegrees));
						bHasChange = true;
					}
					if (VectorChanged(Before.Scale3D, After.Scale3D))
					{
						Result += std::format("\n  Scale     {} -> {}", FormatVector(Before.Scale3D), FormatVector(After.Scale3D));
						bHasChange = true;
					}
					if (!bHasChange) Result += "\n  Transform changed below display precision";
				}
				return Result;
			}

			auto Apply(bool bAfter) -> bool
			{
				bool bSuccess = true;
				for (FEntry& Entry : Entries)
				{
					if (!Entry.Actor || !Entry.Actor->SetActorTransform(bAfter ? Entry.After : Entry.Before)) bSuccess = false;
				}
				return bSuccess;
			}
			std::string Description;
			std::vector<FEntry> Entries;
		};
	} // namespace

	auto FTransformGizmo::RebuildState(const FLevelEditorContext& Context, const FSceneView& View) -> bool
	{
		FVector3 Sum(0.0);
		size_t Count = 0;
		for (const TObjectPtr<AActor>& ActorPtr : Context.GetSelectedActors())
		{
			if (const AActor* Actor = ActorPtr.Get(); Actor && Actor->GetRootComponent())
			{
				Sum += Actor->GetRootComponent()->GetWorldLocation();
				++Count;
			}
		}
		if (Count == 0)
		{
			WorldScale = 0.0f;
			return false;
		}
		Pivot = Sum / static_cast<double>(Count);
		Basis = glm::identity<FQuat>();
		const ETransformGizmoSpace EffectiveSpace = GetEffectiveSpace();
		if (const AActor* Primary = Context.GetPrimarySelectedActor(); Primary && Primary->GetRootComponent())
		{
			if (EffectiveSpace == ETransformGizmoSpace::Local)
				Basis = Primary->GetRootComponent()->GetWorldRotation();
			else if (EffectiveSpace == ETransformGizmoSpace::Parent)
			{
				if (const DSceneComponent* Parent = Primary->GetRootComponent()->GetAttachParent()) Basis = Parent->GetWorldRotation();
			}
		}
		DisplayPivot = Pivot;
		DisplayBasis = Basis;
		const double Distance = std::max(0.05, glm::length(Pivot - View.ViewLocation));
		WorldScale = static_cast<float>(Distance * 2.0 * std::tan(glm::radians(60.0) * 0.5) * GizmoPixels / std::max(1u, View.ViewportHeight));
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
					const double Angle = glm::two_pi<double>() * Segment / 64.0;
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

		if (Mode == ETransformGizmoMode::Scale && glm::length(MousePosition - Center) <= HitRadiusPixels) return ETransformGizmoHandle::Uniform;
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

	auto FTransformGizmo::BeginDrag(FLevelEditorContext& Context, const FSceneView& View, const FLevelEditorViewportInput& Input) -> bool
	{
		ActiveHandle = HitTest(View, Input.MousePosition);
		if (ActiveHandle == ETransformGizmoHandle::None) return false;
		Snapshots.clear();
		PackageDirtySnapshots.clear();
		DragSelectionCount = Context.GetSelectedActors().size();
		for (const TObjectPtr<AActor>& ActorPtr : Context.GetSelectedActors())
		{
			AActor* Actor = ActorPtr.Get();
			if (!Actor || !Actor->GetRootComponent()) continue;
			bool bSelectedAncestor = false;
			for (AActor* Parent = Actor->GetAttachParentActor(); Parent; Parent = Parent->GetAttachParentActor())
			{
				if (Context.IsActorSelected(Parent))
				{
					bSelectedAncestor = true;
					break;
				}
			}
			if (!bSelectedAncestor)
			{
				Snapshots.push_back({Actor, Actor->GetActorTransform()});
				if (DPackage* Package = Actor->GetPackage(); Package && std::ranges::none_of(PackageDirtySnapshots, [Package](const FPackageDirtySnapshot& Entry) { return Entry.Package.Get() == Package; }))
					PackageDirtySnapshots.push_back({Package, Package->IsDirty()});
			}
		}
		if (Snapshots.empty())
		{
			ActiveHandle = ETransformGizmoHandle::None;
			return false;
		}

		FVector3 RayOrigin, RayDirection;
		if (!SceneViewProjection::BuildViewportRay(View, Input.MousePosition, RayOrigin, RayDirection))
		{
			ActiveHandle = ETransformGizmoHandle::None;
			return false;
		}
		DragAxis = ActiveHandle == ETransformGizmoHandle::Uniform ? FVectorConstants::Zero : glm::normalize(Basis * AxisForHandle(ActiveHandle));
		if (ActiveHandle == ETransformGizmoHandle::XY)
			DragPlaneNormal = Basis * FVectorConstants::Up;
		else if (ActiveHandle == ETransformGizmoHandle::XZ)
			DragPlaneNormal = Basis * FVectorConstants::Right;
		else if (ActiveHandle == ETransformGizmoHandle::YZ)
			DragPlaneNormal = Basis * FVectorConstants::Forward;
		else if (Mode == ETransformGizmoMode::Rotate)
			DragPlaneNormal = DragAxis;
		else if (ActiveHandle == ETransformGizmoHandle::Uniform)
			DragPlaneNormal = glm::normalize(View.ViewLocation - Pivot);
		else
		{
			const FVector3 Side = glm::cross(RayDirection, DragAxis);
			if (glm::dot(Side, Side) <= Epsilon)
			{
				ActiveHandle = ETransformGizmoHandle::None;
				return false;
			}
			DragPlaneNormal = glm::normalize(glm::cross(DragAxis, Side));
		}
		if (!RayPlane(RayOrigin, RayDirection, Pivot, DragPlaneNormal, DragStartPoint))
		{
			ActiveHandle = ETransformGizmoHandle::None;
			return false;
		}
		DragStartVector = DragStartPoint - Pivot;
		if (glm::dot(DragStartVector, DragStartVector) > Epsilon) DragStartVector = glm::normalize(DragStartVector);
		DragStartMouseY = Input.MousePosition.y;
		bDragChanged = false;
		return true;
	}

	auto FTransformGizmo::UpdateDrag(const FSceneView& View, const FLevelEditorViewportInput& Input) -> void
	{
		FVector3 RayOrigin, RayDirection, Current;
		if (!SceneViewProjection::BuildViewportRay(View, Input.MousePosition, RayOrigin, RayDirection) || !RayPlane(RayOrigin, RayDirection, Pivot, DragPlaneNormal, Current)) return;
		const bool bSnap = SnapSettings.bEnabled || Input.bCtrl;
		if (Mode == ETransformGizmoMode::Translate)
		{
			FVector3 Delta = Current - DragStartPoint;
			if (ActiveHandle == ETransformGizmoHandle::X || ActiveHandle == ETransformGizmoHandle::Y || ActiveHandle == ETransformGizmoHandle::Z) Delta = DragAxis * glm::dot(Delta, DragAxis);
			if (bSnap)
			{
				const std::array<FVector3, 3> Axes = {Basis * FVectorConstants::Forward, Basis * FVectorConstants::Right, Basis * FVectorConstants::Up};
				FVector3 Snapped(0.0);
				for (const FVector3& Axis : Axes)
					Snapped += Axis * Snap(glm::dot(Delta, Axis), SnapSettings.Translation);
				Delta = Snapped;
			}
			ApplyTranslation(Delta);
		}
		else if (Mode == ETransformGizmoMode::Rotate)
		{
			FVector3 Vector = Current - Pivot;
			if (glm::dot(Vector, Vector) <= Epsilon) return;
			Vector = glm::normalize(Vector);
			double Angle = std::atan2(glm::dot(DragAxis, glm::cross(DragStartVector, Vector)), glm::dot(DragStartVector, Vector));
			if (bSnap) Angle = glm::radians(Snap(glm::degrees(Angle), SnapSettings.RotationDegrees));
			ApplyRotation(Angle);
		}
		else
		{
			double Factor = ActiveHandle == ETransformGizmoHandle::Uniform ? 1.0 + static_cast<double>(DragStartMouseY - Input.MousePosition.y) / GizmoPixels : 1.0 + glm::dot(Current - DragStartPoint, DragAxis) / std::max(0.001f, WorldScale);
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
			ApplyScale(Factors);
		}
	}

	auto FTransformGizmo::ApplyTranslation(const FVector3& Delta) -> void
	{
		bDragChanged = glm::dot(Delta, Delta) > Epsilon;
		DisplayPivot = Pivot + Delta;
		DisplayBasis = Basis;
		for (FActorSnapshot& Snapshot : Snapshots)
		{
			if (!Snapshot.Actor) continue;
			FTransform Transform = Snapshot.Initial;
			Transform.Translation += Delta;
			Snapshot.Actor->SetActorTransform(Transform);
		}
	}

	auto FTransformGizmo::ApplyRotation(double Radians) -> void
	{
		bDragChanged = std::abs(Radians) > Epsilon;
		const FQuat Delta = glm::angleAxis(Radians, DragAxis);
		DisplayPivot = Pivot;
		DisplayBasis = GetEffectiveSpace() == ETransformGizmoSpace::Local ? glm::normalize(Delta * Basis) : Basis;
		for (FActorSnapshot& Snapshot : Snapshots)
		{
			if (!Snapshot.Actor) continue;
			FTransform Transform = Snapshot.Initial;
			Transform.Translation = Pivot + Delta * (Transform.Translation - Pivot);
			Transform.Rotation = glm::normalize(Delta * Transform.Rotation);
			Snapshot.Actor->SetActorTransform(Transform);
		}
	}

	auto FTransformGizmo::ApplyScale(const FVector3& Factors) -> void
	{
		bDragChanged = glm::length(Factors - FVector3(1.0)) > Epsilon;
		DisplayPivot = Pivot;
		DisplayBasis = Basis;
		const FMatrix Delta = glm::translate(FMatrix(1.0), Pivot) * glm::mat4_cast(Basis) * glm::scale(FMatrix(1.0), Factors) * glm::mat4_cast(glm::inverse(Basis)) * glm::translate(FMatrix(1.0), -Pivot);
		for (FActorSnapshot& Snapshot : Snapshots)
		{
			if (!Snapshot.Actor) continue;
			FTransform Transform;
			if (TryMakeTransformFromMatrix(Delta * Snapshot.Initial.ToMatrix(), Transform)) Snapshot.Actor->SetActorTransform(Transform);
		}
	}

	auto FTransformGizmo::RestoreSnapshots() -> void
	{
		for (FActorSnapshot& Snapshot : Snapshots)
			if (Snapshot.Actor) Snapshot.Actor->SetActorTransform(Snapshot.Initial);
		DisplayPivot = Pivot;
		DisplayBasis = Basis;
	}

	auto FTransformGizmo::RestoreInitialDirtyState() -> void
	{
		for (const FPackageDirtySnapshot& Snapshot : PackageDirtySnapshots)
		{
			if (Snapshot.Package && !Snapshot.bWasDirty) Snapshot.Package->ClearDirty();
		}
	}

	auto FTransformGizmo::FinishDrag(FEditorTransactionManager* Transactions) -> void
	{
		if (bDragChanged && Transactions)
		{
			std::vector<FActorTransformTransaction::FEntry> Entries;
			for (const FActorSnapshot& Snapshot : Snapshots)
			{
				if (Snapshot.Actor) Entries.push_back({Snapshot.Actor, Snapshot.Initial, Snapshot.Actor->GetActorTransform()});
			}
			const char* Action = Mode == ETransformGizmoMode::Translate ? "Translate" : Mode == ETransformGizmoMode::Rotate ? "Rotate" :
																															  "Scale";
			Transactions->CommitApplied(std::make_unique<FActorTransformTransaction>(Action, std::move(Entries)));
		}
		else if (!bDragChanged)
			RestoreInitialDirtyState();
		Snapshots.clear();
		PackageDirtySnapshots.clear();
		ActiveHandle = ETransformGizmoHandle::None;
		bDragChanged = false;
	}

	auto FTransformGizmo::CancelDrag() -> void
	{
		if (IsDragging()) RestoreSnapshots();
		if (IsDragging()) RestoreInitialDirtyState();
		Snapshots.clear();
		PackageDirtySnapshots.clear();
		ActiveHandle = ETransformGizmoHandle::None;
		bDragChanged = false;
	}

	auto FTransformGizmo::Update(FLevelEditorContext& Context, const FSceneView& View, const FLevelEditorViewportInput& Input, FEditorTransactionManager* Transactions) -> void
	{
		if (IsDragging())
		{
			bool bSelectionValid = Context.GetSelectedActors().size() == DragSelectionCount;
			for (const FActorSnapshot& Snapshot : Snapshots)
				bSelectionValid = bSelectionValid && Snapshot.Actor && Context.IsActorSelected(Snapshot.Actor.Get());
			if (!bSelectionValid || Input.bCancel)
			{
				CancelDrag();
				return;
			}
			if (!Input.bLeftMouseDown)
			{
				FinishDrag(Transactions);
				return;
			}
			UpdateDrag(View, Input);
			return;
		}
		if (!RebuildState(Context, View))
		{
			HoveredHandle = ETransformGizmoHandle::None;
			return;
		}
		if (Input.bFocused && Input.bHovered && !Input.bWantTextInput)
		{
			if (Input.bModeTranslate) Mode = ETransformGizmoMode::Translate;
			if (Input.bModeRotate) Mode = ETransformGizmoMode::Rotate;
			if (Input.bModeScale) Mode = ETransformGizmoMode::Scale;
		}
		HoveredHandle = Input.bHovered ? HitTest(View, Input.MousePosition) : ETransformGizmoHandle::None;
		if (Input.bLeftMousePressed && HoveredHandle != ETransformGizmoHandle::None) BeginDrag(Context, View, Input);
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
				FMatrix Matrix = glm::translate(FMatrix(1.0), DisplayPivot) * glm::mat4_cast(Rotation) * glm::scale(FMatrix(1.0), FVector3(WorldScale));
				Add(EViewOverlayShape::Ring, Matrix, Handles[Index]);
			}
			return;
		}
		for (size_t Index = 0; Index < 3; ++Index)
		{
			const FQuat Rotation = RotationFromX(Axes[Index]);
			FMatrix Matrix = glm::translate(FMatrix(1.0), DisplayPivot) * glm::mat4_cast(Rotation) * glm::scale(FMatrix(1.0), FVector3(WorldScale));
			Add(Mode == ETransformGizmoMode::Translate ? EViewOverlayShape::Arrow : EViewOverlayShape::Axis, Matrix, Handles[Index]);
			if (Mode == ETransformGizmoMode::Scale)
			{
				FMatrix Box = glm::translate(FMatrix(1.0), DisplayPivot + Axes[Index] * static_cast<double>(WorldScale)) * glm::scale(FMatrix(1.0), FVector3(WorldScale * 0.11f));
				Add(EViewOverlayShape::Box, Box, Handles[Index]);
			}
		}
		if (Mode == ETransformGizmoMode::Translate)
		{
			struct FPlane
			{
				size_t A;
				size_t B;
				ETransformGizmoHandle Handle;
			};
			for (const FPlane Plane : {FPlane{0, 1, ETransformGizmoHandle::XY}, FPlane{0, 2, ETransformGizmoHandle::XZ}, FPlane{1, 2, ETransformGizmoHandle::YZ}})
			{
				const FVector3 A = Axes[Plane.A], B = Axes[Plane.B], N = glm::normalize(glm::cross(A, B));
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
			FMatrix Box = glm::translate(FMatrix(1.0), DisplayPivot) * glm::scale(FMatrix(1.0), FVector3(WorldScale * 0.13f));
			Add(EViewOverlayShape::Box, Box, ETransformGizmoHandle::Uniform);
		}
	}
} // namespace Durin
