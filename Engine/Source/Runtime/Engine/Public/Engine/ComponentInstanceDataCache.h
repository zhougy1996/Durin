#pragma once

#include "CoreMinimal.h"
#include "EngineAPI.h"
#include "DObject/TransactionObjectReference.h"
#include "Engine/ActorConstruction.h"
#include "Components/ActorComponent.h"
#include "Math/Transform.h"
#include "Misc/Transaction.h"
#include <typeinfo>

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
		ENGINE_API auto FindAndReplaceInstances(const FObjectReplacementMap& Map) -> FTransactionCaptureResult;
	private:
		FPersistentObjectRef Exact, GeneratedOwner, Class, Owner;
		FActorGeneratedComponentKey Key;
	};


	struct FInstanceCacheDataBase
	{
		virtual ~FInstanceCacheDataBase() = default;
		virtual auto AddReferencedObjects(FReferenceCollector& Collector) const -> void = 0;
		virtual auto GetAllocatedSize() const -> size_t = 0;
	};

	struct FActorComponentInstanceData;

	// Native type operations corresponding to UE's reflected struct storage.
	struct FComponentInstanceDataType
	{
		const std::type_info& Type;
		std::shared_ptr<FActorComponentInstanceData> (*Copy)(const FActorComponentInstanceData&);
	};

	// Component-owned supplemental instance state. Authored reflected fields remain
	// with object serialization; domain overrides belong to derived cache entries.
	struct FActorComponentInstanceData : FInstanceCacheDataBase
	{
		ENGINE_API explicit FActorComponentInstanceData(const DActorComponent& Component);
		virtual auto ContainsData() const -> bool { return false; }
		ENGINE_API virtual auto ApplyToComponent(DActorComponent& Component) const -> void;
		ENGINE_API auto MatchesComponent(const DActorComponent& Component, const FActorGeneratedComponentKey* CandidateKey = nullptr) const -> bool;
		ENGINE_API auto AddReferencedObjects(FReferenceCollector& Collector) const -> void override;
		ENGINE_API virtual auto FindAndReplaceInstances(const FObjectReplacementMap& Map) -> FTransactionCaptureResult;
		ENGINE_API virtual auto GetDataType() const -> const FComponentInstanceDataType&;
		ENGINE_API auto CreateReplacement(const FObjectReplacementMap& Map) const
			-> std::expected<std::shared_ptr<FActorComponentInstanceData>, std::string>;
		auto GetAllocatedSize() const -> size_t override { return sizeof(*this); }
		auto GetSourceInfo() const -> const FActorComponentInstanceSourceInfo& { return Source; }
	protected:
		template<typename T>
		static auto CopyType() -> const FComponentInstanceDataType&
		{
			static const FComponentInstanceDataType Type{typeid(T),
				[](const FActorComponentInstanceData& Value) -> std::shared_ptr<FActorComponentInstanceData> {
					return std::make_shared<T>(static_cast<const T&>(Value));
				}};
			return Type;
		}
		FActorComponentInstanceSourceInfo Source;
	};

	struct FSceneComponentInstanceData : FActorComponentInstanceData
	{
		ENGINE_API explicit FSceneComponentInstanceData(const DActorComponent& Component);
		auto ContainsData() const -> bool override { return true; }
		ENGINE_API auto ApplyToComponent(DActorComponent& Component) const -> void override;
		ENGINE_API auto GetDataType() const -> const FComponentInstanceDataType& override;
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
		ENGINE_API auto ApplyToActor(AActor& Actor, bool bRequireRoot = true) const -> std::expected<void, std::string>;
		ENGINE_API auto AddReferencedObjects(FReferenceCollector& Collector) const -> void;
		ENGINE_API auto GetAllocatedSize() const -> size_t;
		ENGINE_API auto FindAndReplaceInstances(const FObjectReplacementMap& Map) -> FTransactionCaptureResult;
	private:
		FPersistentObjectRef SourceActor;
		FActorComponentInstanceSourceInfo Root, RootParent;
		bool bCachedGeneratedRoot = false;
		FTransform RootRelative;
		std::vector<FAttachedComponent> Attached;
	};

	class FComponentInstanceDataCache
	{
	public:
		FComponentInstanceDataCache() = default;
		ENGINE_API explicit FComponentInstanceDataCache(const AActor& Actor);
		ENGINE_API auto HasInstanceData() const -> bool;
		ENGINE_API auto Validate(const AActor* ExpectedActor = nullptr) const -> FTransactionCaptureResult;
		ENGINE_API auto ValidateComponent(const DActorComponent& Component,
			const FActorGeneratedComponentKey* CandidateKey = nullptr) const -> FTransactionCaptureResult;
		ENGINE_API auto ApplyToActor(AActor& Actor) const -> void;
		ENGINE_API auto AddReferencedObjects(FReferenceCollector& Collector) const -> void;
		ENGINE_API auto GetAllocatedSize() const -> size_t;
		ENGINE_API auto FindAndReplaceInstances(const FObjectReplacementMap& Map) -> FTransactionCaptureResult;
	private:
		FPersistentObjectRef SourceActor;
		std::vector<std::shared_ptr<FActorComponentInstanceData>> ComponentsInstanceData;
	};
}
