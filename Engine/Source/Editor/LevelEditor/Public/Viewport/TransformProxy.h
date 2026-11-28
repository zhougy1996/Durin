#pragma once

#include "CoreMinimal.h"
#include "DObject/Object.h"
#include "LevelEditorAPI.h"
#include "DObject/ObjectPtr.h"
#include "DObject/ObjectKey.h"
#include "DObject/StrongObjectPtr.h"
#include "Viewport/TransformGizmoTypes.h"
#include "TransformProxy.gen.h"

namespace Durin
{
	class FCommandChange;
	class FTransformProxyChange;

	// The binding is fixed for the lifetime of this proxy.
	DCLASS(NoClassDefaultObject)
	class DTransformProxy : public DObject
	{
		GENERATED_BODY()
	public:
		explicit DTransformProxy(const FObjectInitializer& Initializer) : Super(Initializer) {}
		LEVELEDITOR_API void AddComponent(DSceneComponent* Component);
		LEVELEDITOR_API void AddComponentCustom(DSceneComponent* Component, LevelEditor::FComponentTransformGetter Getter,
			std::function<bool(const FTransform&)> Setter);
		LEVELEDITOR_API void AddComponentCustom(DSceneComponent* Component, LevelEditor::FComponentTransformGetter Getter,
			LevelEditor::FComponentTransformSetter Setter, LevelEditor::FTransformComponentOptions Options = {});
		auto GetInitialTransform() const -> const FTransform& { return InitialFrame; }
		LEVELEDITOR_API auto SetTransform(const FTransform& Frame) -> bool;
		void AddReferencedObjects(FReferenceCollector& Collector) override;
		auto GetTransform() const -> const FTransform& { return SharedFrame; }
		LEVELEDITOR_API auto BeginTransformEditSequence() -> void;
		LEVELEDITOR_API auto EndTransformEditSequence() -> void;
		LEVELEDITOR_API auto Restore() -> bool;

	private:
		friend class DTransformGizmo;
		friend class DTransformGizmoStateTarget;
		friend class FTransformProxyChangeSource;
		friend class FTransformProxyChange;
		LevelEditor::FTransformGizmoTargetSet GetSelection() const;
		auto Initialize(const LevelEditor::FTransformGizmoTargetSet& Targets, const FVector3& Pivot, const FQuat& Basis, const FVector3& Scale = FVector3(1.0)) -> void;
		auto MatchesSelection(const LevelEditor::FTransformGizmoTargetSet& Targets) const -> bool;
		bool MatchesTargetTransforms() const;
		void Activate(const std::shared_ptr<TStrongObjectPtr<DTransformProxy>>& ActiveTarget);
		void SyncSelectionFrame(const FVector3& Center, const FQuat& Rotation);
		auto DescribeChange(std::string_view Action) const -> std::string;

		struct FBoundTarget
		{
			std::shared_ptr<const LevelEditor::FTransformComponentBinding> Target;
			LevelEditor::FTransformGizmoTargetIdentity Identity;
			FObjectKey Owner;
			FTransform Relative;
		};
		std::vector<FBoundTarget> Targets;
		bool bSealed = false;
		void RebuildBinding(const LevelEditor::FTransformGizmoTargetSet& Selection);
		FVector3 ReferenceScale{1.0};
		void CollectTargetReferences(FReferenceCollector& Collector) const;
		using FEditStates = std::unordered_map<size_t, std::shared_ptr<const LevelEditor::FTransformGizmoEditState>>;
		FEditStates EditStates;
		bool SetTransform(const FTransform& Frame, const FEditStates& States, uint64 ReplayHistoryId);
		std::weak_ptr<TStrongObjectPtr<DTransformProxy>> ActiveSelection;
		FVector3 LastCenter{};
		FQuat LastRotation{1.0, 0.0, 0.0, 0.0};
		bool bObserveSelection = false;
		FTransform InitialFrame;
		FTransform SharedFrame;
		std::vector<FTransform> InitialTransforms;
		FTransformProxyChange* RecordingChange = nullptr;
		std::vector<TObjectPtr<DPackage>> Packages;
		std::string CollectionLabel = "Components";
	};

	class FTransformProxyChangeSource
	{
	public:
		LEVELEDITOR_API FTransformProxyChangeSource(DTransformProxy& InProxy, uint64 HistoryId, std::string Description);
		LEVELEDITOR_API ~FTransformProxyChangeSource();
		LEVELEDITOR_API auto EndChange() -> std::unique_ptr<FCommandChange>;

	private:
		DTransformProxy& Proxy;
		std::unique_ptr<FTransformProxyChange> Change;
	};
}
