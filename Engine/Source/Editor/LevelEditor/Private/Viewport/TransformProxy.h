#pragma once

#include "CoreMinimal.h"
#include "DObject/Object.h"
#include "LevelEditorAPI.h"
#include "DObject/ObjectPtr.h"
#include "DObject/ObjectKey.h"
#include "Viewport/TransformGizmoTypes.h"
#include "TransformProxy.gen.h"

namespace Durin
{
	class FCommandChange;
	class FTransformProxyChange;

	// A selection binding and its shared frame. Element baselines exist only during an edit sequence.
	DCLASS(NoClassDefaultObject)
	class DTransformProxy : public DObject
	{
		GENERATED_BODY()
	public:
		explicit DTransformProxy(const FObjectInitializer& Initializer) : Super(Initializer) {}
		auto Initialize(const LevelEditor::FTransformGizmoTargetSet& Targets, const FVector3& Pivot, const FQuat& Basis) -> void;
		auto GetInitialTransform() const -> const FTransform& { return InitialFrame; }
		auto SetTransform(const FTransform& Frame) -> bool;
		auto MatchesSelection(const LevelEditor::FTransformGizmoTargetSet& Targets) const -> bool;
		auto SetHistoryFrame(const FTransform& Frame) -> void { SharedFrame = Frame; bObserveSelection = false; }
		auto GetTransform() const -> const FTransform& { return SharedFrame; }
		auto BeginTransformEditSequence(const LevelEditor::FTransformGizmoTargetSet& Targets) -> void;
		auto EndTransformEditSequence() -> void;
		void SyncSelectionFrame(const FVector3& Center, const FQuat& Rotation);
		auto Restore() -> bool;
		auto DescribeChange(std::string_view Action) const -> std::string;

	private:
		friend class FTransformProxyChangeSource;
		struct FTargetSnapshot
		{
			std::shared_ptr<LevelEditor::ITransformGizmoTarget> Target;
			FObjectKey Owner;
			FTransform Initial;
			bool bRegistered = false;
		};

		struct FSelectionBinding
		{
			LevelEditor::FTransformGizmoTargetIdentity Identity;
			FObjectKey Owner;
			bool operator==(const FSelectionBinding&) const = default;
		};
		std::vector<FSelectionBinding> Selection;
		FVector3 LastCenter{};
		FQuat LastRotation{1.0, 0.0, 0.0, 0.0};
		bool bObserveSelection = false;
		FTransform InitialFrame;
		FTransform SharedFrame;
		std::vector<FTargetSnapshot> Snapshots;
		std::unordered_set<FObjectKey, FObjectKeyHash> ModifiedOwners;
		FTransformProxyChange* RecordingChange = nullptr;
		std::vector<TObjectPtr<DPackage>> Packages;
		std::string CollectionLabel;
	};

	class FTransformProxyChangeSource
	{
	public:
		FTransformProxyChangeSource(DTransformProxy& InProxy, uint64 HistoryId, std::string Description);
		~FTransformProxyChangeSource();
		auto EndChange() -> std::unique_ptr<FCommandChange>;

	private:
		DTransformProxy& Proxy;
		std::unique_ptr<FTransformProxyChange> Change;
	};
}
