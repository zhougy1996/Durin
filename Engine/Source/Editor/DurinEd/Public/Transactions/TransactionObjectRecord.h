#pragma once

#include "CoreMinimal.h"

#include "DObject/PropertyChange.h"
#include "DObject/TransactionDiff.h"
#include "DurinEdAPI.h"

namespace Durin::Editor
{
	struct FTransactionObjectRecordError
	{
		FObjectKey Owner;
		std::string SerializationCause;
		bool bRecoveryRequired = false;
	};
	DURINED_API auto FormatTransactionObjectRecordError(const FTransactionObjectRecordError& Error) -> std::string;

	// One canonical whole-object serialized owner, captured before mutation.
	class FTransactionObjectRecord
	{
	public:
		DURINED_API static auto CaptureWhole(DObject& Object)
			-> std::expected<FTransactionObjectRecord, FTransactionObjectRecordError>;
		DURINED_API auto IsNoOp() const -> bool;
		DURINED_API auto Validate() const -> std::expected<void, FTransactionObjectRecordError>;
		DURINED_API auto CaptureAfter() -> std::expected<void, FTransactionObjectRecordError>;
		DURINED_API auto Apply(bool bBefore, EPropertyChangeOrigin Origin, bool bFlipState = true) -> std::expected<void, FTransactionObjectRecordError>;
		DURINED_API auto PrepareReplay() -> std::expected<void, FTransactionObjectRecordError>;
		// Build after native completion; replay's opposite side owns the live inverse.
		DURINED_API auto BuildObjectEvents(const FTransactionContext& Context,
			ETransactionObjectEventType Type, ETransactionOperation Operation) const
			-> std::expected<std::vector<std::pair<FPersistentObjectRef, FTransactionObjectEvent>>, std::string>;
		DURINED_API auto Snapshot(const FTransactionContext& Context, std::span<const FProperty* const> Selected)
			-> std::expected<std::vector<std::pair<FPersistentObjectRef, FTransactionObjectEvent>>, std::string>;
		auto ClearSnapshot() -> void { SnapshotState.reset(); std::vector<FName>().swap(SelectedSnapshotProperties); bHasSnapshot = false; }
		auto ClearPreparedReplay(bool bClearAnnotation = true) -> void
		{
			ReplayState.reset();
			if (bClearAnnotation) CompletionAnnotation.reset();
		}
		auto GetReplayAnnotation() const -> std::shared_ptr<ITransactionObjectAnnotation> { return CompletionAnnotation; }
		DURINED_API auto AddReferencedObjects(FReferenceCollector& Collector) const -> void;
		DURINED_API auto TryGetAllocatedSize(size_t& OutBytes) const -> bool;
		DURINED_API auto GetAffectedPackages() const -> std::vector<DPackage*>;
		auto GetTarget() const -> const FPersistentObjectRef& { return Target; }
		DURINED_API auto CreateReplacement(const FObjectReplacementMap& Map) const
			-> std::expected<FTransactionObjectRecord, std::string>;
		DURINED_API auto HasSameState(const FTransactionObjectRecord& Other) const -> bool;
		DURINED_API auto HasReplacementReferences(const FObjectReplacementMap& Map) const -> bool;

	private:
		// UE's editor FSerializedObject is distinct from the runtime byte payload.
		struct FSerializedObject
		{
			FSerializedObjectCapture Serialized;
			FDiffableObject Observation;
			std::shared_ptr<ITransactionObjectAnnotation> Annotation;
			auto AddReferencedObjects(FReferenceCollector& Collector) const -> void;
			auto TryGetAllocatedSize(size_t& OutBytes) const -> bool;
			auto Validate(DObject& Object) const -> FTransactionCaptureResult;
			auto GetAnnotation() const -> const std::shared_ptr<ITransactionObjectAnnotation>& { return Annotation; }
			auto GetSerializedCapture() const -> const FSerializedObjectCapture& { return Serialized; }
			auto GetPackage() const -> const FPersistentObjectRef& { return Serialized.Info.Package; }
			auto HasSameSchema(const FSerializedObject& Other) const -> bool { return Serialized.HasSameSchema(Other.Serialized); }
		};
		static auto Capture(DObject& Object, const FSerializedObject* Policy = nullptr,
			const std::shared_ptr<ITransactionObjectAnnotation>* AnnotationPolicy = nullptr)
			-> std::expected<FSerializedObject, std::string>;
		auto Reject(std::string Message, bool bRecovery = false) const -> std::unexpected<FTransactionObjectRecordError>;
		FPersistentObjectRef Target;
		std::optional<FSerializedObject> BeforeState;
		std::optional<FSerializedObject> AfterState;
		std::optional<FSerializedObject> ReplayState;
		std::shared_ptr<ITransactionObjectAnnotation> CompletionAnnotation;
		struct FObjectSnapshot
		{
			FDiffableObject Observation;
			std::shared_ptr<ITransactionObjectAnnotation> Annotation;
			auto GetAnnotation() const -> const std::shared_ptr<ITransactionObjectAnnotation>& { return Annotation; }
			auto AddReferencedObjects(FReferenceCollector& Collector) const -> void;
			auto TryGetAllocatedSize(size_t& OutBytes) const -> bool;
		};
		std::optional<FObjectSnapshot> SnapshotState;
		std::vector<FName> SelectedSnapshotProperties;
		bool bHasSnapshot = false;
	};
}
