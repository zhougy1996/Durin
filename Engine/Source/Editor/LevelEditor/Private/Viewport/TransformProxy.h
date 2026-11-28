#pragma once

#include "CoreMinimal.h"
#include "DObject/ObjectPtr.h"
#include "DObject/ObjectKey.h"
#include "Viewport/TransformGizmoTypes.h"

namespace Durin { class FCommandChange; }

namespace Durin::LevelEditor
{
	class FTransformProxyChangeSource;

	// A fixed group of edit targets and their shared frame at the start of a gesture.
	class FTransformProxy
	{
	public:
		FTransformProxy(const FTransformGizmoTargetSet& Targets, const FVector3& Pivot, const FQuat& Basis);
		auto GetInitialTransform() const -> const FTransform& { return InitialFrame; }
		auto UpdatePreview(const std::optional<FTransform>& Frame) -> bool;
		auto MatchesSelection(const FTransformGizmoTargetSet& Targets) const -> bool;
		auto CaptureFinalValues() -> std::optional<bool>;
		auto CreateChangeSources(uint64 HistoryId, std::string_view Action) const -> std::vector<FTransformProxyChangeSource>;
		auto Restore() -> bool;
		auto GetPackageEditRevisions() const -> std::vector<std::pair<TObjectPtr<DPackage>, uint64>>;
		auto DescribeChange(std::string_view Action) const -> std::string;

	private:
		friend class FTransformProxyChangeSource;
		auto ApplyFrame(const FTransform& Frame) -> bool;
		struct FTargetSnapshot
		{
			std::shared_ptr<ITransformGizmoTarget> Target;
			const void* Identity = nullptr;
			FObjectKey Owner;
			FTransform Initial;
			FTransform Final;
		};
		FTransform InitialFrame;
		std::vector<FTargetSnapshot> Snapshots;
		std::vector<TObjectPtr<DPackage>> Packages;
		std::string CollectionLabel;
	};

	// One drag's capture for one data owner. Indices refer to the fixed proxy snapshots.
	class FTransformProxyChangeSource
	{
	public:
		auto GetChangeTarget() const -> DObject* { return Owner.ResolveObjectPtr(); }
		auto EndChange() const -> std::unique_ptr<FCommandChange>;

	private:
		friend class FTransformProxy;
		FTransformProxyChangeSource(const FTransformProxy& InProxy, FObjectKey InOwner, uint64 InHistoryId, std::string_view InAction)
			: Proxy(InProxy), Owner(InOwner), HistoryId(InHistoryId), Action(InAction) {}
		const FTransformProxy& Proxy;
		FObjectKey Owner;
		uint64 HistoryId;
		std::string Action;
		std::vector<size_t> SnapshotIndices;
	};
}
