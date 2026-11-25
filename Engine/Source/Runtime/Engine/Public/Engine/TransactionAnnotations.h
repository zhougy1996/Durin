#pragma once

#include "CoreMinimal.h"
#include "EngineAPI.h"
#include "DObject/TransactionObjectAnnotation.h"
#include "DObject/TransactionObjectReference.h"
#include "DObject/TransactionDiff.h"
#include "Engine/ComponentInstanceDataCache.h"
#include "Engine/TickFunction.h"
#include "Math/Transform.h"
#include "Misc/Transaction.h"

namespace Durin
{
	class AActor;
	class DActorComponent;
	class DLevel;

	// Contains native component lifecycle state, never another copy of authored fields.
	class FComponentInstanceLifecycleState
	{
	public:
		FComponentInstanceLifecycleState() = default;
		ENGINE_API explicit FComponentInstanceLifecycleState(const DActorComponent& Component);
		ENGINE_API auto IsDefault() const -> bool;
		ENGINE_API auto Validate() const -> FTransactionCaptureResult;
		ENGINE_API auto ValidateReferences() const -> FTransactionCaptureResult;
		ENGINE_API auto Restore() const -> void;
		ENGINE_API auto AddReferencedObjects(FReferenceCollector& Collector) const -> void;
		ENGINE_API auto Serialize(FArchive& Archive) -> void;
		ENGINE_API auto FindAndReplaceInstances(const FObjectReplacementMap& Map) -> FTransactionCaptureResult;
		auto GetAllocatedSize() const -> size_t { return sizeof(*this); }
		auto GetComponent() const -> DActorComponent* { return Target.Resolve(); }
	private:
		FActorComponentInstanceSourceInfo Target;
		FActorComponentLifecycleState State;
	};

	struct FActorTransactionAnnotationData
	{
		FPersistentObjectRef Target;
		FTickFunctionState Tick;
		FComponentInstanceDataCache ComponentInstanceData;
		std::vector<FComponentInstanceLifecycleState> ComponentLifecycle;
		FActorRootComponentReconstructionData RootData;
	};

	// Supplements Actor serialization with runtime component state and conditional
	// generated-root reconstruction data. Construction owns derived component fields.
	class FActorTransactionAnnotation final : public ITransactionObjectAnnotation
	{
	public:
		FActorTransactionAnnotation() = default;
		ENGINE_API explicit FActorTransactionAnnotation(const AActor& Actor, const FActorTransactionAnnotation* Previous = nullptr);
		ENGINE_API static auto CreateIfRequired(const AActor& Actor) -> std::shared_ptr<FActorTransactionAnnotation>;
		ENGINE_API auto HasInstanceData() const -> bool;
		ENGINE_API static auto RestoreDefault(AActor& Actor) -> void;
		ENGINE_API auto CreateSnapshot(const DObject& Object) const -> std::shared_ptr<ITransactionObjectAnnotation> override;
		ENGINE_API auto Validate() const -> FTransactionCaptureResult override;
		ENGINE_API auto Restore(AActor& Actor, bool bApplyRootData = true) const -> void;
		auto GetComponentInstanceDataCache() const -> const FComponentInstanceDataCache& { return Data.ComponentInstanceData; }
		auto GetRootReconstructionData() const -> const FActorRootComponentReconstructionData& { return Data.RootData; }
		ENGINE_API auto AddReferencedObjects(FReferenceCollector& Collector) -> void override;
		ENGINE_API auto Serialize(FArchive& Archive) -> void override;
		ENGINE_API auto GetAllocatedSize() const -> size_t override;
		ENGINE_API auto FindAndReplaceInstances(const FObjectReplacementMap& Map) -> FTransactionCaptureResult override;
		ENGINE_API auto ComputeAdditionalObjectChanges(const ITransactionObjectAnnotation* Original) const
			-> std::vector<std::pair<DObject*, FTransactionObjectChange>> override;
	private:
		FActorTransactionAnnotationData Data;
		// Observation only: not serialized reconstruction state or an obsolete instance root.
		struct FDiffableComponentInfo
		{
			FActorComponentInstanceSourceInfo ComponentSourceInfo;
			FDiffableObject DiffableComponent;
		};
		std::vector<FDiffableComponentInfo> DiffableComponentInfos;
	};
}
