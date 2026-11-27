#pragma once

#include "DObject/ObjectPtr.h"
#include "Viewport/TransformGizmoTypes.h"

namespace Durin { class FCommandChange; }

namespace Durin::LevelEditor
{
	// A fixed group of edit targets and their shared frame at the start of a gesture.
	class FTransformProxy
	{
	public:
		FTransformProxy(const FTransformGizmoTargetSet& Targets, const FVector3& Pivot, const FQuat& Basis);
		auto GetInitialTransform() const -> const FTransform& { return InitialFrame; }
		auto SetTransform(const FTransform& Frame) -> bool;
		auto MatchesSelection(const FTransformGizmoTargetSet& Targets) const -> bool;
		auto HasChanges() const -> bool;
		auto CheckPreviewOwnership() -> bool;
		auto CapturePreviewValues() -> bool;
		auto Restore() -> bool;
		auto MarkPackagesDirty() const -> void;
		auto CreateChange(std::string_view Action, uint64 HistoryId) const -> std::unique_ptr<FCommandChange>;

	private:
		struct FTargetSnapshot
		{
			std::shared_ptr<ITransformGizmoTarget> Target;
			const void* Identity = nullptr;
			FTransform Initial;
			FTransform Preview;
			TObjectPtr<DPackage> Package;
			bool bOwnsPreview = true;
		};
		FTransform InitialFrame;
		std::vector<FTargetSnapshot> Snapshots;
		std::vector<TObjectPtr<DPackage>> Packages;
		std::string CollectionLabel;
	};
}
