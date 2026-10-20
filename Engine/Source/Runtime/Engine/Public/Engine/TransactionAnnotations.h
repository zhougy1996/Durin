#pragma once

#include "CoreMinimal.h"
#include "EngineAPI.h"
#include "DObject/TransactionObjectAnnotation.h"
#include "DObject/TransactionObjectReference.h"
#include "Engine/ComponentInstanceDataCache.h"
#include "Engine/TickFunction.h"
#include "Math/Transform.h"
#include "Misc/Transaction.h"

namespace Durin
{
	class AActor;
	class DActorComponent;
	class DLevel;

	class FComponentTransactionAnnotation final : public ITransactionObjectAnnotation
	{
	public:
		FComponentTransactionAnnotation() = default;
		explicit FComponentTransactionAnnotation(const DActorComponent& Component) : Data(Component) {}
		static auto DefaultFor(const DActorComponent& Component) -> FComponentTransactionAnnotation
		{ FComponentTransactionAnnotation Result; Result.Data = FComponentInstanceLifecycleState::DefaultFor(Component); return Result; }
		auto IsDefault() const -> bool { return Data.IsDefault(); }
		auto Validate() const -> FTransactionCaptureResult override { return Data.Validate(); }
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
	private:
		FActorTransactionAnnotationData Data;
	};
}
