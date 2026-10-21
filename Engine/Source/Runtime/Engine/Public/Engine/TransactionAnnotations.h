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
		ENGINE_API static auto DefaultFor(const DActorComponent& Component) -> FComponentInstanceLifecycleState;
		ENGINE_API auto IsDefault() const -> bool;
		ENGINE_API auto Validate() const -> FTransactionCaptureResult;
		ENGINE_API auto ValidateReferences() const -> FTransactionCaptureResult;
		ENGINE_API auto Restore() const -> FCommandChangeResult;
		ENGINE_API auto AddReferencedObjects(FReferenceCollector& Collector) const -> void;
		ENGINE_API auto Serialize(FArchive& Archive) -> void;
		ENGINE_API auto FindAndReplaceInstances(const FObjectReplacementMap& Map) -> FTransactionCaptureResult;
		auto GetAllocatedSize() const -> size_t { return sizeof(*this); }
		auto GetComponent() const -> DActorComponent* { return Target.Resolve(); }
	private:
		FActorComponentInstanceSourceInfo Target;
		FActorComponentLifecycleState State;
	};

	class FComponentTransactionAnnotation final : public ITransactionObjectAnnotation
	{
	public:
		FComponentTransactionAnnotation() = default;
		explicit FComponentTransactionAnnotation(const DActorComponent& Component) : Data(Component) {}
		auto IsDefault() const -> bool { return Data.IsDefault(); }
		// Membership is settled by structural records before native completion.
		auto Validate() const -> FTransactionCaptureResult override { return Data.ValidateReferences(); }
		auto Restore() const -> FCommandChangeResult { return Data.Restore(); }
		auto AddReferencedObjects(FReferenceCollector& Collector) const -> void override { Data.AddReferencedObjects(Collector); }
		auto Serialize(FArchive& Archive) -> void override { Data.Serialize(Archive); }
		auto GetAllocatedSize() const -> size_t override { return sizeof(*this); }
		auto GetComponent() const -> DActorComponent* { return Data.GetComponent(); }
		ENGINE_API auto CreateReplacement(const FObjectReplacementMap& Map) const
			-> std::expected<std::shared_ptr<ITransactionObjectAnnotation>, std::string> override;
	private:
		FComponentInstanceLifecycleState Data;
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
		ENGINE_API static auto RestoreDefault(AActor& Actor) -> FCommandChangeResult;
		ENGINE_API auto CreateSnapshot(const DObject& Object) const -> std::shared_ptr<ITransactionObjectAnnotation> override;
		ENGINE_API auto Validate() const -> FTransactionCaptureResult override;
		ENGINE_API auto Restore(AActor& Actor, bool bApplyRootData = true) const -> FCommandChangeResult;
		auto GetComponentInstanceDataCache() const -> const FComponentInstanceDataCache& { return Data.ComponentInstanceData; }
		auto GetRootReconstructionData() const -> const FActorRootComponentReconstructionData& { return Data.RootData; }
		ENGINE_API auto AddReferencedObjects(FReferenceCollector& Collector) const -> void override;
		ENGINE_API auto Serialize(FArchive& Archive) -> void override;
		ENGINE_API auto GetAllocatedSize() const -> size_t override;
		ENGINE_API auto CreateReplacement(const FObjectReplacementMap& Map) const
			-> std::expected<std::shared_ptr<ITransactionObjectAnnotation>, std::string> override;
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
