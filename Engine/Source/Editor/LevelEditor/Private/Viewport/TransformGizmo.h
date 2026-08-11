#pragma once

#include "DObject/ObjectPtr.h"
#include "SceneView.h"
#include "LevelEditorTransformTargets.h"

namespace Durin::Editor
{
	class FTransactionManager;
	class ITransaction;
}

namespace Durin
{
	class FLevelEditorContext;
	class AActor;
	class DPackage;
	struct FLevelEditorViewportInput;

	// Selects translation, rotation, or scale manipulation.
	enum class ETransformGizmoMode : uint8 { Translate, Rotate, Scale };
	// Selects the basis used by translation and rotation handles.
	enum class ETransformGizmoSpace : uint8 { World, Local, Parent };
	// Identifies the active axis, plane, or uniform gizmo handle.
	enum class ETransformGizmoHandle : uint8
	{
		None,
		X, Y, Z,
		XY, XZ, YZ,
		Uniform
	};

	// Defines translation, rotation-degree, and scale increments for snapping.
	struct FTransformGizmoSnapSettings
	{
		bool bEnabled = false;
		float Translation = 0.5f;
		float RotationDegrees = 15.0f;
		float Scale = 0.1f;
	};

	// Owns transform-drag state and commits one transaction for the selection.
	class FTransformGizmo
	{
	public:
		auto Update(FLevelEditorContext& Context, const FSceneView& View, const FLevelEditorViewportInput& Input, Editor::FTransactionManager* Transactions) -> void;
		auto Update(const FTransformGizmoTargetSet& Targets, const FSceneView& View, const FLevelEditorViewportInput& Input, Editor::FTransactionManager* Transactions) -> void;
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
		// Retains an actor's starting transform for drag cancellation and history.
		struct FTargetSnapshot
		{
			std::shared_ptr<ITransformGizmoTarget> Target;
			const void* Identity = nullptr;
			FTransform Initial;
		};

		// Restores each affected package's pre-drag dirty state when cancelled.
		struct FPackageDirtySnapshot
		{
			TObjectPtr<DPackage> Package;
			bool bWasDirty = false;
		};
		auto RebuildState(const FTransformGizmoTargetSet& Targets, const FSceneView& View) -> bool;
		auto HitTest(const FSceneView& View, const FVector2f& MousePosition) const -> ETransformGizmoHandle;
		auto BeginDrag(const FTransformGizmoTargetSet& Targets, const FSceneView& View, const FLevelEditorViewportInput& Input) -> bool;
		auto UpdateDrag(const FSceneView& View, const FLevelEditorViewportInput& Input) -> void;
		auto FinishDrag(Editor::FTransactionManager* Transactions) -> void;
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
		std::vector<FTargetSnapshot> Snapshots;
		std::vector<FPackageDirtySnapshot> PackageDirtySnapshots;
		std::string DragCollectionLabel = "Targets";
		ETransformGizmoCapability ActiveCapabilities = ETransformGizmoCapability::None;
		FVector3 DragAxis{0.0};
		FVector3 DragPlaneNormal{0.0};
		FVector3 DragStartPoint{0.0};
		FVector3 DragStartVector{0.0};
		float DragStartMouseY = 0.0f;
		bool bDragChanged = false;
	};
}
