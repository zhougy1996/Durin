#pragma once

#include "Viewport/TransformGizmoTypes.h"

#include "CoreMinimal.h"

#include "DObject/StrongObjectPtr.h"
#include "SceneView.h"
#include "LevelEditorAPI.h"
#include "Viewport/LevelEditorViewportInput.h"
#include "DObject/Object.h"
#include "TransformGizmo.gen.h"

namespace Durin
{
	class DTransactor;
	class DTransformGizmoStateTarget;
	class DTransformProxy;
}

namespace Durin::LevelEditor
{
	class FLevelEditorContext;

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

}

namespace Durin
{
	// Owns transform-drag state and commits one transaction for the selection.
	DCLASS(NoClassDefaultObject)
	class DTransformGizmo : public DObject
	{
		GENERATED_BODY()
	public:
		explicit DTransformGizmo(const FObjectInitializer& Initializer) : Super(Initializer) {}
		LEVELEDITOR_API void SetActiveTarget(DTransformProxy* Target, DTransactor* Transactions = nullptr);
		LEVELEDITOR_API bool Update(const FSceneView& View, const LevelEditor::FLevelEditorViewportInput& Input);
		LEVELEDITOR_API auto Update(LevelEditor::FLevelEditorContext& Context, const FSceneView& View, const LevelEditor::FLevelEditorViewportInput& Input, DTransactor* Transactions) -> bool;
		LEVELEDITOR_API auto Update(const LevelEditor::FTransformGizmoTargetSet& Targets, const FSceneView& View, const LevelEditor::FLevelEditorViewportInput& Input, DTransactor* Transactions) -> bool;
		LEVELEDITOR_API auto AppendOverlayPrimitives(FSceneView& View) const -> void;
		// A failed restore keeps IsDragging() true so callers can retry.
		LEVELEDITOR_API auto CancelUpdate() -> void;
		LEVELEDITOR_API ~DTransformGizmo() override;
		LEVELEDITOR_API auto GetActiveTarget() const -> DTransformProxy*;
		auto IsDragging() const -> bool { return ActiveHandle != LevelEditor::ETransformGizmoHandle::None; }
		auto IsHovered() const -> bool { return HoveredHandle != LevelEditor::ETransformGizmoHandle::None; }
		auto GetMode() const -> LevelEditor::ETransformGizmoMode { return Mode; }
		auto SetMode(LevelEditor::ETransformGizmoMode InMode) -> void { if (!IsDragging()) Mode = InMode; }
		auto GetSpace() const -> LevelEditor::ETransformGizmoSpace { return Space; }
		// Scale remains local-only without changing the space preference used by move and rotate.
		auto GetEffectiveSpace() const -> LevelEditor::ETransformGizmoSpace { return Mode == LevelEditor::ETransformGizmoMode::Scale ? LevelEditor::ETransformGizmoSpace::Local : Space; }
		auto SetSpace(LevelEditor::ETransformGizmoSpace InSpace) -> void { if (!IsDragging()) Space = InSpace; }
		auto GetSnapSettings() -> LevelEditor::FTransformGizmoSnapSettings& { return SnapSettings; }
		auto GetSnapSettings() const -> const LevelEditor::FTransformGizmoSnapSettings& { return SnapSettings; }

	private:
		auto RebuildState(const LevelEditor::FTransformGizmoTargetSet& Targets, const FSceneView& View, DTransactor* Transactions) -> bool;
		auto HitTest(const FSceneView& View, const FVector2f& MousePosition) const -> LevelEditor::ETransformGizmoHandle;
		auto BeginDrag(const LevelEditor::FTransformGizmoTargetSet& Targets, const FSceneView& View, const LevelEditor::FLevelEditorViewportInput& Input, DTransactor* Transactions) -> bool;
		auto ComputeDragFrame(const FSceneView& View, const LevelEditor::FLevelEditorViewportInput& Input) -> std::optional<FTransform>;
		auto ClearDrag() -> void;

		LevelEditor::ETransformGizmoMode Mode = LevelEditor::ETransformGizmoMode::Translate;
		LevelEditor::ETransformGizmoSpace Space = LevelEditor::ETransformGizmoSpace::World;
		LevelEditor::ETransformGizmoHandle HoveredHandle = LevelEditor::ETransformGizmoHandle::None;
		LevelEditor::ETransformGizmoHandle ActiveHandle = LevelEditor::ETransformGizmoHandle::None;
		LevelEditor::FTransformGizmoSnapSettings SnapSettings;
		FVector3 Pivot{0.0};
		FQuat Basis{1.0, 0.0, 0.0, 0.0};
		float WorldScale = 1.0f;
		// Proxies for the same selection can reactivate their frame on history replay.
		std::shared_ptr<TStrongObjectPtr<DTransformProxy>> ActiveTarget;
		TStrongObjectPtr<DTransformGizmoStateTarget> StateTarget;
		TWeakObjectPtr<DTransactor> TransactionProvider;
		LevelEditor::ETransformGizmoCapability ActiveCapabilities = LevelEditor::ETransformGizmoCapability::None;
		FVector3 DragAxis{0.0};
		FVector3 DragPlaneNormal{0.0};
		FVector3 DragStartPoint{0.0};
		FVector3 DragStartVector{0.0};
		float DragStartMouseY = 0.0f;
		bool bUpdating = false;
	};
}
