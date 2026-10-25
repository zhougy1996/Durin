#include "Viewport/TransformGizmo.h"
#include "Actors/ActorChange.h"

#include "Components/SceneComponent.h"
#include "Transactions/Transactor.h"
#include "Engine/Actor.h"
#include "Workspace/LevelEditorContext.h"
#include "Math/Operations.h"
#include "Math/TransformDecomposition.h"
#include "DObject/Package.h"
#include "DObject/Object.h"
#include "DObject/ObjectLifecycle.h"
#include "SceneViewProjection.h"
#include "Viewport/LevelEditorViewportClient.h"
#include "MonaImGui.h"

namespace Durin::Editor::Level
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

		auto SameTransform(const FTransform& A, const FTransform& B) -> bool
		{
			return Math::Length(A.Translation - B.Translation) <= Epsilon
				&& Math::Length(A.Scale3D - B.Scale3D) <= Epsilon
				&& 1.0 - std::abs(Math::Dot(Math::Normalize(A.Rotation), Math::Normalize(B.Rotation))) <= Epsilon;
		}
		auto TrySetTransform(ITransformGizmoTarget& Target, const FTransform& Value) -> bool
		{
			try { return Target.SetTransform(Value); }
			catch (...) { return false; }
		}
		// Restores before/after transforms for every target changed by one gizmo drag.
		class FTransformTargetTransaction final : public ::Durin::Editor::FActorCommandChange
		{
		public:
			struct FEntry
			{
				std::shared_ptr<ITransformGizmoTarget> Target;
				FTransform Before;
				FTransform After;
			};
			FTransformTargetTransaction(std::string_view Action, std::string_view CollectionLabel, std::vector<FEntry> InEntries)
				: Entries(std::move(InEntries))
			{
				for (const FEntry& Entry : Entries)
				{
					DPackage* Package = Entry.Target ? Entry.Target->GetPackage() : nullptr;
					if (Package && !std::ranges::contains(AffectedPackages, Package))
						AffectedPackages.push_back(Package);
				}
				if (Entries.size() == 1 && Entries.front().Target)
					Description = std::format("{} '{}'", Action, Entries.front().Target->GetLabel());
				else
					Description = std::format("{} {} {}", Action, Entries.size(), CollectionLabel);
			}
			auto GetDescription() const -> std::string_view override { return Description; }
			auto GetModifiedObjects() const -> std::vector<DObject*> override
			{
				std::vector<DObject*> Objects;
				for (const auto& Entry : Entries)
					if (Entry.Target)
					{
						auto Participants = Entry.Target->GetModifiedObjects();
						Objects.insert(Objects.end(), Participants.begin(), Participants.end());
					}
				return Objects;
			}
			auto GetOwningModule() const -> std::string_view override { return "LevelEditor"; }
			auto GetDetails(::Durin::ETransactionOperation Operation) const -> std::string override { return BuildDetails(Operation != ::Durin::ETransactionOperation::Undo); }
			auto GetAffectedPackages() const -> std::span<DPackage* const> override { return AffectedPackages; }
			auto Replay(::Durin::ETransactionOperation Operation) -> ::Durin::Editor::FActorChangeResult override
			{ return Apply(Operation != ::Durin::ETransactionOperation::Undo); }
			auto AddReferencedObjects(FReferenceCollector& Collector) const -> void override
			{
				for (const FEntry& Entry : Entries)
					if (Entry.Target) Entry.Target->AddReferencedObjects(Collector);
			}

		private:
			static auto FormatVector(const FVector3& Value) -> std::string
			{
				return std::format("({:.3f}, {:.3f}, {:.3f})", Value.x, Value.y, Value.z);
			}

			static auto VectorChanged(const FVector3& Before, const FVector3& After) -> bool
			{
				return Math::Length(After - Before) > Epsilon;
			}

			static auto RotationChanged(const FQuat& Before, const FQuat& After) -> bool
			{
				return 1.0 - std::abs(Math::Dot(Math::Normalize(Before), Math::Normalize(After))) > Epsilon;
			}

			auto BuildDetails(bool bForward) const -> std::string
			{
				std::string Result;
				for (const FEntry& Entry : Entries)
				{
					if (!Result.empty()) Result += '\n';
					Result += std::format("'{}'", Entry.Target ? Entry.Target->GetLabel() : "Missing Target");
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
						const FVector3 BeforeDegrees = Math::QuaternionToEulerDegrees(Before.Rotation);
						const FVector3 AfterDegrees = Math::QuaternionToEulerDegrees(After.Rotation);
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

			auto Apply(bool bAfter) -> ::Durin::Editor::FActorChangeResult
			{
				std::vector<FTransform> Previous;
				for (size_t Index = 0; Index < Entries.size(); ++Index)
				{
					const auto& Entry = Entries[Index];
					if (!Entry.Target || !Entry.Target->IsValid())
						return {{.Code = EActorChangeError::TargetUnavailable, .MemberIndex = Index}};
					Previous.push_back(Entry.Target->GetTransform());
					if (!SameTransform(Previous.back(), bAfter ? Entry.Before : Entry.After))
						return {{.Code = EActorChangeError::ActorChanged, .TargetLabel = Entry.Target->GetLabel(), .MemberIndex = Index}};
				}
				size_t FailedIndex = Entries.size();
				for (size_t Index = 0; Index < Entries.size(); ++Index)
					if (!TrySetTransform(*Entries[Index].Target, bAfter ? Entries[Index].After : Entries[Index].Before))
					{ FailedIndex = Index; break; }
				if (FailedIndex == Entries.size())
					for (size_t Index = 0; Index < Entries.size(); ++Index)
						if (!Entries[Index].Target->IsValid() || !SameTransform(Entries[Index].Target->GetTransform(), bAfter ? Entries[Index].After : Entries[Index].Before))
						{ FailedIndex = Index; break; }
				if (FailedIndex != Entries.size())
				{
					FActorChangeResult Result{{.Code = EActorChangeError::TransformWrite,
						.TargetLabel = Entries[FailedIndex].Target->GetLabel(), .NodeCount = Entries.size(), .MemberIndex = FailedIndex}};
					// A false/throwing setter may already have changed its own target.
					for (size_t Restore = Entries.size(); Restore-- > 0;)
						if (!Entries[Restore].Target->IsValid() || !TrySetTransform(*Entries[Restore].Target, Previous[Restore]))
							Result.Error.CleanupCause = std::make_shared<FActorChangeError>(FActorChangeError{
								.Code = EActorChangeError::RollbackIncomplete, .TargetLabel = Entries[Restore].Target->GetLabel(),
								.MemberIndex = Restore, .CleanupCause = Result.Error.CleanupCause});
					for (size_t Restore = 0; Restore < Entries.size(); ++Restore)
						if (!Entries[Restore].Target->IsValid() || !SameTransform(Entries[Restore].Target->GetTransform(), Previous[Restore]))
							Result.Error.CleanupCause = std::make_shared<FActorChangeError>(FActorChangeError{
								.Code = EActorChangeError::RollbackIncomplete, .MemberIndex = Restore, .CleanupCause = Result.Error.CleanupCause});
					return Result;
				}
				return {};
			}

			std::string Description;
			std::vector<FEntry> Entries;
			std::vector<DPackage*> AffectedPackages;
		};

		class FActorTransformGizmoTarget final : public ITransformGizmoTarget
		{
		public:
			explicit FActorTransformGizmoTarget(AActor* InActor) : Actor(InActor) {}
			auto IsValid() const -> bool override { return Actor && !Actor->IsHidden() && Actor->GetRootComponent(); }
			auto GetIdentity() const -> const void* override { return Actor.Get(); }
			auto GetModifiedObjects() const -> std::vector<DObject*> override
			{ return {Actor.Get(), Actor ? Actor->GetRootComponent() : nullptr}; }
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
			auto AddReferencedObjects(FReferenceCollector& Collector) const -> void override
			{
				DObject* Object = Actor.Get();
				if (Object) Collector.AddReferencedObject(Object);
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
		Snapshots.clear();
		PackageDirtySnapshots.clear();
		DragCollectionLabel = Targets.CollectionLabel;
		for (const std::shared_ptr<ITransformGizmoTarget>& Target : Targets.Targets)
		{
			if (Target && Target->IsValid())
			{
				const auto Initial = Target->GetTransform();
				Snapshots.push_back({Target, Target->GetIdentity(), Initial, Initial, Target->GetPackage()});
				if (DPackage* Package = Target->GetPackage(); Package && std::ranges::none_of(PackageDirtySnapshots, [Package](const FPackageDirtySnapshot& Entry) { return Entry.Package.Get() == Package; }))
					PackageDirtySnapshots.push_back({Package, Package->IsDirty(), Package->GetEditRevision()});
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
			return ApplyTranslation(Delta);
		}
		else if (Mode == ETransformGizmoMode::Rotate)
		{
			FVector3 Vector = Current - Pivot;
			if (Math::LengthSquared(Vector) <= Epsilon) return true;
			Vector = Math::Normalize(Vector);
			double Angle = std::atan2(Math::Dot(DragAxis, Math::Cross(DragStartVector, Vector)), Math::Dot(DragStartVector, Vector));
			if (bSnap) Angle = Math::DegreesToRadians(Snap(Math::RadiansToDegrees(Angle), SnapSettings.RotationDegrees));
			return ApplyRotation(Angle);
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
			return ApplyScale(Factors);
		}
	}

	auto FTransformGizmo::ApplyTranslation(const FVector3& Delta) -> bool
	{
		DisplayPivot = Pivot + Delta;
		DisplayBasis = Basis;
		for (FTargetSnapshot& Snapshot : Snapshots)
		{
			if (!Snapshot.Target || !Snapshot.Target->IsValid()) return false;
			FTransform Transform = Snapshot.Initial;
			Transform.Translation += Delta;
			if (!TrySetTransform(*Snapshot.Target, Transform)) return false;
		}
		return true;
	}

	auto FTransformGizmo::ApplyRotation(double Radians) -> bool
	{
		const FQuat Delta = Math::MakeQuaternionFromAxisAngleRadians(Radians, DragAxis);
		DisplayPivot = Pivot;
		DisplayBasis = GetEffectiveSpace() == ETransformGizmoSpace::Local ? Math::Normalize(Delta * Basis) : Basis;
		for (FTargetSnapshot& Snapshot : Snapshots)
		{
			if (!Snapshot.Target || !Snapshot.Target->IsValid()) return false;
			FTransform Transform = Snapshot.Initial;
			Transform.Translation = Pivot + Delta * (Transform.Translation - Pivot);
			Transform.Rotation = Math::Normalize(Delta * Transform.Rotation);
			if (!TrySetTransform(*Snapshot.Target, Transform)) return false;
		}
		return true;
	}

	auto FTransformGizmo::ApplyScale(const FVector3& Factors) -> bool
	{
		DisplayPivot = Pivot;
		DisplayBasis = Basis;
		const FMatrix Delta = Math::TranslationMatrix(Pivot) * Math::RotationMatrix(Basis)
			* Math::ScaleMatrix(Factors) * Math::RotationMatrix(Math::Inverse(Basis))
			* Math::TranslationMatrix(-Pivot);
		for (FTargetSnapshot& Snapshot : Snapshots)
		{
			if (!Snapshot.Target || !Snapshot.Target->IsValid()) return false;
			FTransform Transform;
			if (!TryMakeTransformFromMatrix(Delta * Snapshot.Initial.ToMatrix(), Transform) || !TrySetTransform(*Snapshot.Target, Transform)) return false;
		}
		return true;
	}

	auto FTransformGizmo::CheckPreviewOwnership() -> bool
	{
		bool bOwned = true;
		for (auto& Snapshot : Snapshots)
		{
			if (!Snapshot.bOwnsPreview || !Snapshot.Target || !Snapshot.Target->IsValid()
				|| !SameTransform(Snapshot.Target->GetTransform(), Snapshot.Preview))
			{
				Snapshot.bOwnsPreview = false;
				if (Snapshot.Package) Snapshot.Package->MarkDirty();
				bOwned = false;
			}
		}
		return bOwned;
	}

	auto FTransformGizmo::CapturePreviewValues() -> bool
	{
		bool bValid = true;
		for (auto& Snapshot : Snapshots)
		{
			if (Snapshot.Target && Snapshot.Target->IsValid()) Snapshot.Preview = Snapshot.Target->GetTransform();
			else bValid = false;
		}
		return bValid;
	}

	auto FTransformGizmo::MarkPackagesDirty() -> void
	{
		for (const auto& Snapshot : PackageDirtySnapshots)
			if (Snapshot.Package) Snapshot.Package->MarkDirty();
	}

	auto FTransformGizmo::ClearDrag() -> void
	{
		Snapshots.clear();
		PackageDirtySnapshots.clear();
		ActiveHandle = ETransformGizmoHandle::None;
		bRestorePending = false;
	}

	auto FTransformGizmo::RestoreSnapshots() -> bool
	{
		CheckPreviewOwnership();
		bool bRestored = true;
		FScopedPackageDirtySuppression SuppressPreviewRevision;
		for (auto& Snapshot : Snapshots)
			if (Snapshot.bOwnsPreview && !TrySetTransform(*Snapshot.Target, Snapshot.Initial)) bRestored = false;
		CapturePreviewValues();
		for (const auto& Snapshot : Snapshots)
			if (Snapshot.bOwnsPreview && (!Snapshot.Target->IsValid() || !SameTransform(Snapshot.Preview, Snapshot.Initial))) bRestored = false;
		DisplayPivot = Pivot;
		DisplayBasis = Basis;
		return bRestored;
	}

	auto FTransformGizmo::RestoreInitialDirtyState() -> void
	{
		for (const FPackageDirtySnapshot& Snapshot : PackageDirtySnapshots)
		{
			if (Snapshot.Package && !Snapshot.bWasDirty
				&& Snapshot.Package->GetEditRevision() == Snapshot.EditRevision) Snapshot.Package->ClearDirty();
		}
	}

	auto FTransformGizmo::FinishDrag(::Durin::DTransactor* Transactions) -> bool
	{
		if (!CheckPreviewOwnership()) { CancelDrag(); return false; }
		const bool bChanged = std::ranges::any_of(Snapshots, [](const auto& Snapshot) {
			return !SameTransform(Snapshot.Initial, Snapshot.Preview);
		});
		if (!bChanged) return CancelDrag();
		if (Transactions)
		{
			std::vector<FTransformTargetTransaction::FEntry> Entries;
			for (const auto& Snapshot : Snapshots) Entries.push_back({Snapshot.Target, Snapshot.Initial, Snapshot.Preview});
			const char* Action = Mode == ETransformGizmoMode::Translate ? "Translate" : Mode == ETransformGizmoMode::Rotate ? "Rotate" : "Scale";
			const auto Result = Transactions->StoreUndo(std::make_unique<FTransformTargetTransaction>(Action, DragCollectionLabel, std::move(Entries)));
			if (!Result)
			{
				if (Result.Code == ETransactorResultCode::RecoveryRequired) { MarkPackagesDirty(); ClearDrag(); }
				else CancelDrag();
				return false;
			}
		}
		else MarkPackagesDirty();
		ClearDrag();
		return true;
	}

	auto FTransformGizmo::CancelDrag() -> bool
	{
		if (IsDragging())
		{
			bRestorePending = true;
			if (GUndo) return false;
			if (!RestoreSnapshots()) { MarkPackagesDirty(); return false; }
			RestoreInitialDirtyState();
		}
		ClearDrag();
		return true;
	}

	auto FTransformGizmo::Update(FLevelEditorContext& Context, const FSceneView& View, const FLevelEditorViewportInput& Input, ::Durin::DTransactor* Transactions) -> bool
	{
		const bool bSucceeded = Update(MakeActorTransformGizmoTargets(Context), View, Input, Transactions);
		if (!bSucceeded) Context.SetError("Unable to apply or restore the transform gesture.");
		return bSucceeded;
	}

	auto FTransformGizmo::Update(const FTransformGizmoTargetSet& Targets, const FSceneView& View, const FLevelEditorViewportInput& Input, ::Durin::DTransactor* Transactions) -> bool
	{
		if (GUndo || (Transactions && (!Transactions->CanTransact() || Transactions->IsActive())))
		{
			// Restore only after the foreign recorder/replay has released GUndo.
			if (IsDragging()) bRestorePending = true;
			return false;
		}
		if (IsDragging())
		{
			if (bRestorePending) return CancelDrag();
			bool bSelectionValid = Targets.Targets.size() == Snapshots.size();
			for (const auto& Snapshot : Snapshots)
				bSelectionValid = bSelectionValid && Snapshot.Target && Snapshot.Target->IsValid()
					&& std::ranges::any_of(Targets.Targets, [&Snapshot](const auto& Target) { return Target && Target->GetIdentity() == Snapshot.Identity; });
			if (!bSelectionValid || Input.bCancel) return CancelDrag();
			if (!Input.bLeftMouseDown) return FinishDrag(Transactions);
			if (!CheckPreviewOwnership()) { CancelDrag(); return false; }
			bool bApplied;
			{
				FScopedPackageDirtySuppression SuppressPreviewRevision;
				bApplied = UpdateDrag(View, Input);
			}
			const bool bCaptured = CapturePreviewValues();
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
} // namespace Durin::Editor::Level
