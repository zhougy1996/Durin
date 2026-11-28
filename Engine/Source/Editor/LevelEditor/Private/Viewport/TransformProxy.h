#pragma once

#include "CoreMinimal.h"
#include "DObject/ObjectPtr.h"
#include "DObject/ObjectKey.h"
#include "Viewport/TransformGizmoTypes.h"

namespace Durin { class DTransactor; }

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
		auto GetPackageEditRevisions() const -> std::vector<std::pair<TObjectPtr<DPackage>, uint64>>;
		auto RecordChanges(DTransactor& Transactions, std::string_view Action) -> bool;

	private:
		struct FTargetSnapshot
		{
			std::shared_ptr<ITransformGizmoTarget> Target;
			const void* Identity = nullptr;
			FObjectKey Owner;
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
