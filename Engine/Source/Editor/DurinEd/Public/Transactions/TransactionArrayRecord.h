#pragma once

#include "CoreMinimal.h"
#include "DObject/Archive.h"
#include "DObject/TransactionObjectReference.h"
#include "DObject/TransactionObjectEvent.h"
#include "DObject/PropertyChange.h"
#include "Misc/ITransaction.h"
#include "DurinEdAPI.h"

namespace Durin { class FArrayProperty; class DPackage; }

namespace Durin::Editor
{
	// UE-style operation record. Resolve reflected storage anew after every vector
	// relocation; only the affected elements are owned by the history entry.
	class FTransactionArrayRecord
	{
	public:
		DURINED_API static auto Capture(const FTransactionArrayCapture& Request)
			-> std::expected<FTransactionArrayRecord, std::string>;
		DURINED_API auto Validate() const -> FTransactionCaptureResult;
		DURINED_API auto Apply(bool bCompensate, EPropertyChangeOrigin Origin, bool* OutRecoveryRequired) -> FTransactionCaptureResult;
		DURINED_API auto CommitReplay() -> void;
		auto ClearPreparedReplay() -> void { Inverse.reset(); CompletionAnnotation.reset(); InverseAnnotation.reset(); bHasCompletion = false; }
		auto IsNoOp() const -> bool { return Count == 0; }
		auto GetTarget() const -> const FPersistentObjectRef& { return Target; }
		auto GetReplayAnnotation() const -> const std::shared_ptr<ITransactionObjectAnnotation>& { return CompletionAnnotation; }
		auto HasReplayCompletion() const -> bool { return bHasCompletion; }
		DURINED_API auto AddReferencedObjects(FReferenceCollector& Collector) const -> void;
		DURINED_API auto TryGetAllocatedSize(size_t& OutBytes) const -> bool;
		DURINED_API auto GetAffectedPackages() const -> std::vector<DPackage*>;
		DURINED_API auto HasReplacementReferences(const FObjectReplacementMap& Map) const -> bool;
		DURINED_API auto CreateReplacement(const FObjectReplacementMap& Map) const -> std::expected<FTransactionArrayRecord, std::string>;
		DURINED_API auto BuildObjectEvents(const FTransactionContext& Context,
			ETransactionObjectEventType Type, ETransactionOperation Operation) const
			-> std::expected<std::vector<std::pair<FPersistentObjectRef, FTransactionObjectEvent>>, std::string>;
		auto HasSameState(const FTransactionArrayRecord& Other) const -> bool
		{
			return Target == Other.Target && Class == Other.Class && Member == Other.Member && MemberName == Other.MemberName
				&& MemberIndex == Other.MemberIndex && Index == Other.Index && Count == Other.Count && Oper == Other.Oper
				&& StructPath == Other.StructPath && Data == Other.Data && Inverse == Other.Inverse && Annotation == Other.Annotation
				&& InverseAnnotation == Other.InverseAnnotation && CompletionAnnotation == Other.CompletionAnnotation && bHasCompletion == Other.bHasCompletion;
		}

	private:
		struct FElement
		{
			FPropertyValueSnapshotPayload Payload;
			std::vector<FPersistentObjectRef> HardReferences;
			auto operator==(const FElement&) const -> bool = default;
		};
		using FRange = std::vector<FElement>;
		auto ResolveContainer() const -> void*;
		auto CaptureRange(size_t Num) const -> std::expected<FRange, std::string>;
		FPersistentObjectRef Target, Class;
		const FArrayProperty* Member = nullptr;
		FName MemberName;
		std::vector<const FProperty*> StructPath;
		uint32 MemberIndex = 0;
		size_t Index = 0, Count = 0;
		int8 Oper = 0;
		FRange Data;
		std::optional<FRange> Inverse;
		std::shared_ptr<ITransactionObjectAnnotation> Annotation, InverseAnnotation, CompletionAnnotation;
		bool bHasCompletion = false;
	};
}
