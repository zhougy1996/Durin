#pragma once

#include "CoreMinimal.h"
#include "EngineAPI.h"
#include "DObject/TransactionObjectReference.h"
#include "Engine/ActorConstruction.h"
#include "Components/ActorComponent.h"
#include "Math/Transform.h"
#include "Misc/Transaction.h"

namespace Durin
{
	class AActor;
	class DActorComponent;
	class DLevel;
	class FObjectReplacementMap;
	// Authored components keep exact identity; generated components resolve through
	// their owning actor's construction key without retaining a retired generation.
	struct FActorComponentInstanceSourceInfo
	{
		ENGINE_API explicit FActorComponentInstanceSourceInfo(const DActorComponent* Component = nullptr);
		ENGINE_API auto Resolve() const -> DActorComponent*;
		ENGINE_API auto Validate() const -> FTransactionCaptureResult;
		ENGINE_API auto MatchesComponent(const DActorComponent& Component, const FActorGeneratedComponentKey* CandidateKey = nullptr) const -> bool;
		ENGINE_API auto MatchesGenerated(const AActor& Owner, const FActorGeneratedComponentKey& Key) const -> bool;
		auto operator==(const FActorComponentInstanceSourceInfo&) const -> bool = default;
		auto IsNull() const -> bool { return Exact.IsNull() && GeneratedOwner.IsNull(); }
		ENGINE_API auto AddReferencedObjects(FReferenceCollector& Collector) const -> void;
		ENGINE_API auto Serialize(FArchive& Archive) -> void;
		ENGINE_API auto FindAndReplaceInstances(const FObjectReplacementMap& Map) -> FTransactionCaptureResult;
	private:
		FPersistentObjectRef Exact, GeneratedOwner, Class, Owner;
		FActorGeneratedComponentKey Key;
	};

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
		auto GetSourceInfo() const -> const FActorComponentInstanceSourceInfo& { return Target; }
	private:
		FActorComponentInstanceSourceInfo Target;
		FPersistentObjectRef Owner;
		FActorComponentLifecycleState State;
	};


	enum class ECacheApplyPhase : uint8
	{
		PostSimpleConstructionScript,
		PostUserConstructionScript,
		NonConstructionScript
	};

	struct FInstanceCacheDataBase
	{
		virtual ~FInstanceCacheDataBase() = default;
		virtual auto AddReferencedObjects(FReferenceCollector& Collector) const -> void = 0;
		virtual auto Serialize(FArchive& Archive) -> void = 0;
		virtual auto GetAllocatedSize() const -> size_t = 0;
	};

	// Component-owned supplemental instance state. Authored reflected fields remain
	// with object serialization; domain overrides belong to derived cache entries.
	struct FActorComponentInstanceData : FInstanceCacheDataBase
	{
		FActorComponentInstanceData() = default;
		ENGINE_API explicit FActorComponentInstanceData(const DActorComponent& Component);
		virtual auto ContainsData() const -> bool { return false; }
		ENGINE_API virtual auto ApplyToComponent(DActorComponent& Component, ECacheApplyPhase Phase) const -> FCommandChangeResult;
		ENGINE_API auto MatchesComponent(const DActorComponent& Component, const FActorGeneratedComponentKey* CandidateKey = nullptr) const -> bool;
		ENGINE_API auto AddReferencedObjects(FReferenceCollector& Collector) const -> void override;
		ENGINE_API auto Serialize(FArchive& Archive) -> void override;
		ENGINE_API virtual auto FindAndReplaceInstances(const FObjectReplacementMap& Map) -> FTransactionCaptureResult;
		// Native counterpart of copying UE's typed instance-data storage. Derived
		// entries must preserve their dynamic type and all supplemental data.
		ENGINE_API virtual auto CreateReplacement(const FObjectReplacementMap& Map) const
			-> std::expected<std::shared_ptr<FActorComponentInstanceData>, std::string>;
		auto GetAllocatedSize() const -> size_t override { return sizeof(*this); }
		auto GetSourceInfo() const -> const FActorComponentInstanceSourceInfo& { return Source; }
	protected:
		FActorComponentInstanceSourceInfo Source;
	};

	struct FSceneComponentInstanceData : FActorComponentInstanceData
	{
		FSceneComponentInstanceData() = default;
		ENGINE_API explicit FSceneComponentInstanceData(const DActorComponent& Component);
		auto ContainsData() const -> bool override { return true; }
		ENGINE_API auto ApplyToComponent(DActorComponent& Component, ECacheApplyPhase Phase) const -> FCommandChangeResult override;
		ENGINE_API auto Serialize(FArchive& Archive) -> void override;
		ENGINE_API auto CreateReplacement(const FObjectReplacementMap& Map) const
			-> std::expected<std::shared_ptr<FActorComponentInstanceData>, std::string> override;
		auto GetAllocatedSize() const -> size_t override { return sizeof(*this); }
	private:
		FTransform Relative;
	};

	// Shared reconstruction data, including the attachment participants retained
	// across an absent generated root. No transaction operation or history owner.
	struct FActorRootComponentReconstructionData
	{
		struct FAttachedComponent
		{
			FActorComponentInstanceSourceInfo Target, Parent;
			FTransform Relative;
		};
		ENGINE_API explicit FActorRootComponentReconstructionData(const AActor& Actor,
			const FActorRootComponentReconstructionData* Previous = nullptr);
		FActorRootComponentReconstructionData() = default;
		auto HasInstanceData() const -> bool { return bCachedGeneratedRoot || !Attached.empty(); }
		ENGINE_API auto Validate(const AActor* ExpectedActor = nullptr) const -> FTransactionCaptureResult;
		ENGINE_API auto ApplyToActor(AActor& Actor, bool bRequireRoot = true) const -> FCommandChangeResult;
		ENGINE_API auto AddReferencedObjects(FReferenceCollector& Collector) const -> void;
		ENGINE_API auto Serialize(FArchive& Archive) -> void;
		ENGINE_API auto GetAllocatedSize() const -> size_t;
		ENGINE_API auto FindAndReplaceInstances(const FObjectReplacementMap& Map) -> FTransactionCaptureResult;
	private:
		FPersistentObjectRef SourceActor;
		FActorComponentInstanceSourceInfo Root, RootParent;
		bool bCachedGeneratedRoot = false;
		FTransform RootRelative, RootWorld;
		std::vector<FAttachedComponent> Attached;
	};

	class FComponentInstanceDataCache
	{
	public:
		FComponentInstanceDataCache() = default;
		ENGINE_API explicit FComponentInstanceDataCache(const AActor& Actor, bool bCaptureLifecycle = false);
		ENGINE_API auto HasInstanceData() const -> bool;
		ENGINE_API auto Validate(const AActor* ExpectedActor = nullptr) const -> FTransactionCaptureResult;
		ENGINE_API auto ValidateComponent(const DActorComponent& Component,
			const FActorGeneratedComponentKey* CandidateKey = nullptr) const -> FTransactionCaptureResult;
		ENGINE_API auto ApplyToComponent(DActorComponent& Component, ECacheApplyPhase Phase,
			const FActorGeneratedComponentKey* CandidateKey = nullptr) const -> FCommandChangeResult;
		ENGINE_API auto ApplyToActor(AActor& Actor, ECacheApplyPhase Phase) const -> FCommandChangeResult;
		ENGINE_API auto AddReferencedObjects(FReferenceCollector& Collector) const -> void;
		ENGINE_API auto Serialize(FArchive& Archive) -> void;
		ENGINE_API auto GetAllocatedSize() const -> size_t;
		ENGINE_API auto FindAndReplaceInstances(const FObjectReplacementMap& Map) -> FTransactionCaptureResult;
	private:
		FPersistentObjectRef SourceActor;
		std::vector<std::shared_ptr<FActorComponentInstanceData>> ComponentsInstanceData;
		std::vector<FComponentInstanceLifecycleState> ComponentLifecycle;
	};
}
