#pragma once

#include "DObject/ObjectPtr.h"
#include "IRendererModule.h"

namespace Durin
{
	class FEditorTransactionManager;
	class FLevelEditorContext;
	class AActor;
	class DPackage;
	struct FLevelEditorViewportInput;

	enum class ETransformGizmoMode : uint8 { Translate, Rotate, Scale };
	enum class ETransformGizmoSpace : uint8 { World, Local, Parent };
	enum class ETransformGizmoHandle : uint8
	{
		None,
		X, Y, Z,
		XY, XZ, YZ,
		Uniform
	};

	struct FTransformGizmoSnapSettings
	{
		bool bEnabled = false;
		float Translation = 0.5f;
		float RotationDegrees = 15.0f;
		float Scale = 0.1f;
	};

	class FTransformGizmo
	{
	public:
		auto Update(FLevelEditorContext& Context, const FSceneView& View, const FLevelEditorViewportInput& Input, FEditorTransactionManager* Transactions) -> void;
		auto AppendOverlayPrimitives(FSceneView& View) const -> void;
		auto CancelDrag() -> void;
		auto IsDragging() const -> bool { return ActiveHandle != ETransformGizmoHandle::None; }
		auto IsHovered() const -> bool { return HoveredHandle != ETransformGizmoHandle::None; }
		auto GetMode() const -> ETransformGizmoMode { return Mode; }
		auto SetMode(ETransformGizmoMode InMode) -> void { if (!IsDragging()) Mode = InMode; }
		auto GetSpace() const -> ETransformGizmoSpace { return Space; }
		// Scale remains local-only without changing the space preference used by move and rotate.
		auto GetEffectiveSpace() const -> ETransformGizmoSpace { return Mode == ETransformGizmoMode::Scale ? ETransformGizmoSpace::Local : Space; }
		auto SetSpace(ETransformGizmoSpace InSpace) -> void { if (!IsDragging()) Space = InSpace; }
		auto GetSnapSettings() -> FTransformGizmoSnapSettings& { return SnapSettings; }
		auto GetSnapSettings() const -> const FTransformGizmoSnapSettings& { return SnapSettings; }

	private:
		struct FActorSnapshot
		{
			TObjectPtr<AActor> Actor;
			FTransform Initial;
		};
		struct FPackageDirtySnapshot
		{
			TObjectPtr<DPackage> Package;
			bool bWasDirty = false;
		};
		auto RebuildState(const FLevelEditorContext& Context, const FSceneView& View) -> bool;
		auto HitTest(const FSceneView& View, const FVector2f& MousePosition) const -> ETransformGizmoHandle;
		auto BeginDrag(FLevelEditorContext& Context, const FSceneView& View, const FLevelEditorViewportInput& Input) -> bool;
		auto UpdateDrag(const FSceneView& View, const FLevelEditorViewportInput& Input) -> void;
		auto FinishDrag(FEditorTransactionManager* Transactions) -> void;
		auto RestoreSnapshots() -> void;
		auto RestoreInitialDirtyState() -> void;
		auto ApplyTranslation(const FVector3& Delta) -> void;
		auto ApplyRotation(double Radians) -> void;
		auto ApplyScale(const FVector3& Factors) -> void;

		ETransformGizmoMode Mode = ETransformGizmoMode::Translate;
		ETransformGizmoSpace Space = ETransformGizmoSpace::World;
		ETransformGizmoHandle HoveredHandle = ETransformGizmoHandle::None;
		ETransformGizmoHandle ActiveHandle = ETransformGizmoHandle::None;
		FTransformGizmoSnapSettings SnapSettings;
		FVector3 Pivot{0.0};
		FVector3 DisplayPivot{0.0};
		FQuat Basis{1.0, 0.0, 0.0, 0.0};
		FQuat DisplayBasis{1.0, 0.0, 0.0, 0.0};
		float WorldScale = 1.0f;
		std::vector<FActorSnapshot> Snapshots;
		std::vector<FPackageDirtySnapshot> PackageDirtySnapshots;
		size_t DragSelectionCount = 0;
		FVector3 DragAxis{0.0};
		FVector3 DragPlaneNormal{0.0};
		FVector3 DragStartPoint{0.0};
		FVector3 DragStartVector{0.0};
		float DragStartMouseY = 0.0f;
		bool bDragChanged = false;
	};
}
