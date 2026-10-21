#pragma once

#include "CoreMinimal.h"
#include "EngineAPI.h"
#include "DObject/TransactionObjectReference.h"
#include "DObject/StructOps.h"
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
		ENGINE_API auto Serialize(FArchive& Archive) -> void;
		ENGINE_API auto FindAndReplaceInstances(const FObjectReplacementMap& Map) -> FTransactionCaptureResult;
	private:
		FPersistentObjectRef Exact, GeneratedOwner, Class, Owner;
		FActorGeneratedComponentKey Key;
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

	struct FActorComponentInstanceData;

	// Native type operations corresponding to UE's reflected struct storage.
	struct FComponentInstanceDataType
	{
		std::string Name;
		const std::type_info& Type;
		std::shared_ptr<FActorComponentInstanceData> (*Construct)();
		std::shared_ptr<FActorComponentInstanceData> (*Copy)(const FActorComponentInstanceData&);
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
		ENGINE_API virtual auto GetDataType() const -> const FComponentInstanceDataType&;
		template<typename T>
		static auto RegisterType(std::string_view Name) -> const FComponentInstanceDataType&;
		ENGINE_API static auto FindDataType(std::string_view Name) -> const FComponentInstanceDataType*;
		ENGINE_API auto CreateReplacement(const FObjectReplacementMap& Map) const
			-> std::expected<std::shared_ptr<FActorComponentInstanceData>, std::string>;
		auto GetAllocatedSize() const -> size_t override { return sizeof(*this); }
		auto GetSourceInfo() const -> const FActorComponentInstanceSourceInfo& { return Source; }
	private:
		ENGINE_API static auto RegisterDataType(const FComponentInstanceDataType& Type) -> void;
		template<typename T>
		static auto AllocateInstanceData(const T* Source = nullptr) -> std::shared_ptr<FActorComponentInstanceData>
		{
			static_assert(std::is_base_of_v<FActorComponentInstanceData, T>
				&& std::is_default_constructible_v<T> && std::is_copy_constructible_v<T>);
			struct FStorage
			{
				alignas(T) std::byte Memory[sizeof(T)];
				explicit FStorage(const T* Value)
				{
					const auto& Ops = GetDStructOps<T>();
					if (!Ops.DefaultConstruct || !Ops.CopyConstruct || !Ops.Destroy)
						throw std::logic_error("Instance data requires native construction, copy and destruction operations.");
					if (Value) Ops.CopyConstruct(Memory, Value);
					else Ops.DefaultConstruct(Memory);
				}
				~FStorage() { GetDStructOps<T>().Destroy(Memory); }
			};
			auto Storage = std::make_shared<FStorage>(Source);
			auto* Value = std::launder(reinterpret_cast<T*>(Storage->Memory));
			return std::shared_ptr<FActorComponentInstanceData>(std::move(Storage), static_cast<FActorComponentInstanceData*>(Value));
		}
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
		ENGINE_API auto GetDataType() const -> const FComponentInstanceDataType& override;
		auto GetAllocatedSize() const -> size_t override { return sizeof(*this); }
	private:
		FTransform Relative;
	};

	template<typename T>
	auto FActorComponentInstanceData::RegisterType(std::string_view Name) -> const FComponentInstanceDataType&
	{
		static const FComponentInstanceDataType Type{std::string(Name), typeid(T),
			[]() -> std::shared_ptr<FActorComponentInstanceData> { return AllocateInstanceData<T>(); },
			[](const FActorComponentInstanceData& Source) -> std::shared_ptr<FActorComponentInstanceData> {
				return AllocateInstanceData<T>(&static_cast<const T&>(Source));
			}};
		if (Type.Name != Name) throw std::logic_error("Component instance data type has conflicting names.");
		RegisterDataType(Type);
		return Type;
	}

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
		ENGINE_API explicit FComponentInstanceDataCache(const AActor& Actor);
		ENGINE_API auto HasInstanceData() const -> bool;
		ENGINE_API auto Validate(const AActor* ExpectedActor = nullptr) const -> FTransactionCaptureResult;
		ENGINE_API auto ValidateComponent(const DActorComponent& Component,
			const FActorGeneratedComponentKey* CandidateKey = nullptr) const -> FTransactionCaptureResult;
		ENGINE_API auto ApplyToActor(AActor& Actor, ECacheApplyPhase Phase) const -> FCommandChangeResult;
		ENGINE_API auto AddReferencedObjects(FReferenceCollector& Collector) const -> void;
		ENGINE_API auto Serialize(FArchive& Archive) -> void;
		ENGINE_API auto GetAllocatedSize() const -> size_t;
		ENGINE_API auto FindAndReplaceInstances(const FObjectReplacementMap& Map) -> FTransactionCaptureResult;
	private:
		FPersistentObjectRef SourceActor;
		std::vector<std::shared_ptr<FActorComponentInstanceData>> ComponentsInstanceData;
	};
}
