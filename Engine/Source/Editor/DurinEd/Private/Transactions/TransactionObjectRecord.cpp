#include "Transactions/TransactionObjectRecord.h"

#include "DObject/Class.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"
#include "DObject/Property.h"
#include "DObject/TransactionObjectAnnotation.h"
#include "DObject/ObjectGraphReplacement.h"

namespace Durin::Editor
{
	auto FTransactionObjectRecord::HasReplacementReferences(const FObjectReplacementMap& Map) const -> bool
	{
		class FCheck final : public FReferenceCollector
		{
		public:
			explicit FCheck(const FObjectReplacementMap& InMap) : Map(InMap) {}
			auto AddReferencedObject(DObject*& Object) -> void override { bMapped |= Map.Find(Object) != nullptr; }
			const FObjectReplacementMap& Map;
			bool bMapped = false;
		} Check(Map);
		AddReferencedObjects(Check);
		if (Check.bMapped) return true;
		for (const auto* State : {&BeforeState, &AfterState, &ReplayState, &SnapshotState})
			if (*State && (*State)->GetSerializedCapture().HasReplacementReferences(Map)) return true;
		return false;
	}

	auto FTransactionObjectRecord::HasSameState(const FTransactionObjectRecord& Other) const -> bool
	{
		auto SameInfo = [](const FSerializedObjectInfo& A, const FSerializedObjectInfo& B) {
			return A == B && A.Identity == B.Identity;
		};
		auto SameObservation = [&](const FDiffableObject& A, const FDiffableObject& B) {
			return SameInfo(A.ObjectInfo, B.ObjectInfo) && A.SerializedObject == B.SerializedObject
				&& A.SerializationContext == B.SerializationContext && A.SerializedTaggedData == B.SerializedTaggedData;
		};
		auto SameState = [&](const auto& A, const auto& B) {
			if (A.has_value() != B.has_value()) return false;
			return !A || (SameObservation(A->Observation, B->Observation) && A->Annotation == B->Annotation);
		};
		return Target == Other.Target && SameState(BeforeState, Other.BeforeState) && SameState(AfterState, Other.AfterState)
			&& SameState(ReplayState, Other.ReplayState) && CompletionAnnotation == Other.CompletionAnnotation
			&& SameState(SnapshotState, Other.SnapshotState)
			&& SelectedSnapshotProperties == Other.SelectedSnapshotProperties;
	}

	auto FTransactionObjectRecord::CreateReplacement(const FObjectReplacementMap& Map) const
		-> std::expected<FTransactionObjectRecord, std::string>
	{
		auto Result = *this;
		auto Key = Map.RemapObjectKey(Target.GetKey());
		if (!Key) return std::unexpected(ToString(Key.error()));
		Result.Target = FPersistentObjectRef::FromKey(*Key);
		for (auto* State : {&Result.BeforeState, &Result.AfterState, &Result.ReplayState, &Result.SnapshotState})
			if (*State)
			{
				auto Observation = DiffUtil::PrepareObjectReplacement((*State)->Observation, Map);
				if (!Observation) return std::unexpected(Observation.error());
				(*State)->Observation = std::move(*Observation);
				auto Annotation = PrepareTransactionAnnotationReplacement((*State)->Annotation, Map);
				if (!Annotation) return std::unexpected(Annotation.error());
				(*State)->Annotation = std::move(*Annotation);
			}
		auto Completion = PrepareTransactionAnnotationReplacement(Result.CompletionAnnotation, Map);
		if (!Completion) return std::unexpected(Completion.error());
		Result.CompletionAnnotation = std::move(*Completion);
		if (auto Valid = Result.Validate(); !Valid) return std::unexpected(FormatTransactionObjectRecordError(Valid.error()));
		return Result;
	}

	auto FTransactionObjectRecord::Capture(DObject& Object, const FSerializedObject* CapturePolicy,
		const std::shared_ptr<ITransactionObjectAnnotation>* AnnotationPolicy) -> std::expected<FTransactionObjectRecord::FSerializedObject, std::string>
	{
		FGarbageCollectionDeferralScope DeferCollection;
		if (!IsValid(&Object) || !Object.GetClass() || Object.IsTemplateObject())
			return std::unexpected("Transaction object capture requires a live ordinary object.");
		if (CapturePolicy && CapturePolicy->Observation.ObjectInfo.Target.Resolve() != &Object)
			return std::unexpected("Transaction capture policy belongs to a different object.");
		DObjectForceRegistration(Object.GetClass());
		if (!IsValid(Object.GetClass())) return std::unexpected("Transaction class metadata is unavailable.");
		const FSerializedObjectInfo Info{FPersistentObjectRef(&Object), FPersistentObjectRef(Object.GetClass()),
			FPersistentObjectRef(Object.GetOuter()), FPersistentObjectRef(Object.GetPackage()), CaptureTransactionObjectIdentity(Object), Object.GetObjectFlags()};
		FSerializedObject State;
		try
		{
			// UE 5.8 FSerializedObject::SetObject captures supplemental state before Serialize.
			const auto Previous = AnnotationPolicy ? *AnnotationPolicy : (CapturePolicy ? CapturePolicy->Annotation : nullptr);
			State.Annotation = Previous ? Previous->CreateSnapshot(Object) : Object.FindOrCreateTransactionAnnotation();
			const auto Policy = CapturePolicy ? CapturePolicy->GetSerializedCapture() : FSerializedObjectCapture{};
			auto Captured = CaptureSerializedObject(Object, CapturePolicy ? &Policy : nullptr, &State.Observation);
			if (!Captured) return std::unexpected(Captured.error());
			State.Observation.ObjectInfo = Info;
		}
		catch (const std::exception& Error) { return std::unexpected(std::format("Transaction serialization capture threw: {}", Error.what())); }
		catch (...) { return std::unexpected("Transaction serialization capture threw an unknown exception."); }
		return State;
	}

	auto FTransactionObjectRecord::FSerializedObject::Validate(DObject& Object) const -> FTransactionCaptureResult
	{
		if (auto Result = ValidateSerializedObject(Object, GetSerializedCapture()); !Result) return Result;
		if (Annotation)
			try { if (auto Result = Annotation->Validate(); !Result) return Result; }
			catch (const std::exception& Error) { return std::unexpected(std::format("Transaction annotation validation threw: {}", Error.what())); }
			catch (...) { return std::unexpected("Transaction annotation validation threw an unknown exception."); }
		return {};
	}

	auto FTransactionObjectRecord::FSerializedObject::AddReferencedObjects(FReferenceCollector& Collector) const -> void
	{
		Observation.AddReferencedObjects(Collector);
		if (Annotation) Annotation->AddReferencedObjects(Collector);
	}

	auto FTransactionObjectRecord::FSerializedObject::TryGetAllocatedSize(size_t& OutBytes) const -> bool
	{
		// Account the retained identity once, together with its transfer allocations.
		size_t Total = 0;
		if (!Observation.TryGetAllocatedSize(Total)) return false;
		if (Annotation)
		{
			size_t Size = 0;
			try { Size = Annotation->GetAllocatedSize(); } catch (...) { return false; }
			if (Size > std::numeric_limits<size_t>::max() - Total) return false;
			Total += Size;
		}
		OutBytes = Total; return true;
	}

	auto FTransactionObjectRecord::Reject(std::string Message, bool bRecovery) const
		-> std::unexpected<FTransactionObjectRecordError>
	{
		return std::unexpected(FTransactionObjectRecordError{.Owner = Target.GetKey(), .SerializationCause = std::move(Message), .bRecoveryRequired = bRecovery});
	}

	auto FormatTransactionObjectRecordError(const FTransactionObjectRecordError& Error) -> std::string
	{ return Error.SerializationCause; }

	auto FTransactionObjectRecord::CaptureWhole(DObject& Object)
		-> std::expected<FTransactionObjectRecord, FTransactionObjectRecordError>
	{
		FTransactionObjectRecord Record;
		Record.Target = FPersistentObjectRef(&Object);
		auto State = Capture(Object);
		if (!State) return Record.Reject(State.error());
		Record.BeforeState = std::move(*State);
		Record.AfterState = Record.BeforeState;
		return Record;
	}

	auto FTransactionObjectRecord::IsNoOp() const -> bool
	{
		return BeforeState && AfterState && !BeforeState->GetAnnotation() && !AfterState->GetAnnotation()
			&& BeforeState->Observation.ObjectInfo == AfterState->Observation.ObjectInfo
			&& *BeforeState->Observation.SerializedObject == *AfterState->Observation.SerializedObject && BeforeState->HasSameSchema(*AfterState);
	}

	auto FTransactionObjectRecord::Validate() const -> std::expected<void, FTransactionObjectRecordError>
	{
		auto* Object = Target.Resolve();
		if (!Object) return Reject("Transaction object target is unavailable.");
		if (!BeforeState || !AfterState) return Reject("Transaction object state is unavailable.");
		if (auto Result = BeforeState->Validate(*Object); !Result) return Reject(Result.error());
		if (auto Result = AfterState->Validate(*Object); !Result) return Reject(Result.error());
		return {};
	}

	auto FTransactionObjectRecord::CaptureAfter() -> std::expected<void, FTransactionObjectRecordError>
	{
		FGarbageCollectionDeferralScope DeferCollection;
		if (!BeforeState) return Reject("Transaction object initial state is unavailable.");
		auto* Object = Target.Resolve();
		if (!Object) return Reject("Transaction final capture target is unavailable.");
		auto State = Capture(*Object, &*BeforeState);
		if (!State) return Reject(State.error());
		if (!BeforeState->HasSameSchema(*State)) return Reject("Transaction field schema changed during recording.");
		AfterState = std::move(*State);
		return {};
	}

	auto FTransactionObjectRecord::PrepareReplay() -> std::expected<void, FTransactionObjectRecordError>
	{
		FGarbageCollectionDeferralScope DeferCollection;
		ClearPreparedReplay();
		if (!BeforeState) return Reject("Transaction object initial state is unavailable.");
		auto* Object = Target.Resolve();
		if (!Object) return Reject("Transaction inverse capture target is unavailable.");
		auto State = Capture(*Object, &*BeforeState);
		if (!State) return Reject(State.error());
		ReplayState = std::move(*State);
		CompletionAnnotation = ReplayState->GetAnnotation();
		return {};
	}

	auto FTransactionObjectRecord::Apply(bool bBefore, EPropertyChangeOrigin Origin, bool bFlipState)
		-> std::expected<void, FTransactionObjectRecordError>
	{
		FGarbageCollectionDeferralScope DeferCollection;
		auto* Object = Target.Resolve();
		if (!Object) return Reject("Transaction restore target is unavailable.");
		if (!ReplayState)
			if (auto Captured = PrepareReplay(); !Captured) return Captured;
		auto& Desired = bBefore ? BeforeState : AfterState;
		if (!Desired) return Reject("Transaction restore state is unavailable.");
		CompletionAnnotation = Desired->GetAnnotation();
		if (auto Validated = Desired->Validate(*Object); !Validated) return Reject(Validated.error());
		if (auto Restored = LoadSerializedObject(*Object, Desired->GetSerializedCapture()); !Restored)
		{
			CompletionAnnotation = ReplayState->GetAnnotation();
			auto Error = std::move(Restored.error());
			if (auto Compensated = LoadSerializedObject(*Object, ReplayState->GetSerializedCapture()); !Compensated)
			{
				Error.bRecoveryRequired = true;
				Error.Message += std::format(" Object compensation failed: {}", Compensated.error().Message);
			}
			return Reject(std::move(Error.Message), Error.bRecoveryRequired);
		}
		// Per-member notifications use the same actual-field diff as object events.
		auto Changes = DiffUtil::GenerateObjectDiff(ReplayState->Observation, Desired->Observation);
		if (!Changes) return Reject(Changes.error(), true);
		for (const auto Name : Changes->ChangedProperties)
			if (auto* Member = Object->GetClass()->FindPropertyByName(Name))
			{
				const FPropertyPathSegment Path{Member};
				Object->PostEditChangeProperty({.MemberProperty = Member, .LeafProperty = Member,
					.Path = {&Path, 1}, .Phase = EPropertyChangePhase::Committed, .Origin = Origin});
			}
		if (bFlipState) (bBefore ? AfterState : BeforeState) = std::move(*ReplayState);
		ClearPreparedReplay(false);
		Object->MarkPackageDirty();
		return {};
	}

	auto FTransactionObjectRecord::BuildObjectEvents(const FTransactionContext& Context,
		ETransactionObjectEventType Type, ETransactionOperation Operation) const
		-> std::expected<std::vector<std::pair<FPersistentObjectRef, FTransactionObjectEvent>>, std::string>
	{
		auto* Object = Target.Resolve();
		if (!Object) return std::unexpected("Transaction event target is unavailable.");
		const bool bReplay = Type == ETransactionObjectEventType::UndoRedo;
		const bool bUndo = Operation == ETransactionOperation::Undo;
		FTransactionObjectChange Change;
		std::shared_ptr<ITransactionObjectAnnotation> OriginalAnnotation, CurrentAnnotation;
		const auto& Baseline = bReplay && bUndo ? AfterState : BeforeState;
		if (!Baseline) return std::unexpected("Transaction event baseline is unavailable.");
		std::optional<FSerializedObject> Live;
		if (bReplay)
		{
			auto Captured = Capture(*Object, &*Baseline);
			if (!Captured) return std::unexpected(Captured.error());
			Live = std::move(*Captured);
		}
		const auto* Current = bReplay ? &*Live : (AfterState ? &*AfterState : nullptr);
		if (!Current) return std::unexpected("Transaction event final state is unavailable.");
		auto Diff = DiffUtil::GenerateObjectDiff(Baseline->Observation, Current->Observation);
		if (!Diff) return std::unexpected(Diff.error());
		Change = std::move(*Diff);
		if (Type == ETransactionObjectEventType::Finalized && SnapshotState)
		{
			auto Closure = DiffUtil::GenerateObjectDiff(SnapshotState->Observation, Current->Observation, {.bFullDiff = false,
				.ShouldSkipProperty = [&](FName Name) { return !SelectedSnapshotProperties.empty() && !std::ranges::contains(SelectedSnapshotProperties, Name); }});
			if (!Closure) return std::unexpected(Closure.error());
			for (const auto Name : Closure->ChangedProperties)
				if (!std::ranges::contains(Change.ChangedProperties, Name)) Change.ChangedProperties.push_back(Name);
			Change.bNonPropertyDataChanged |= Closure->bNonPropertyDataChanged;
		}
		OriginalAnnotation = Baseline->GetAnnotation(); CurrentAnnotation = Current->GetAnnotation();
		std::vector<std::pair<FPersistentObjectRef, FTransactionObjectEvent>> Events;
		auto Add = [&](FPersistentObjectRef TargetRef, FTransactionObjectChange Value, std::shared_ptr<ITransactionObjectAnnotation> Annotation) {
			Events.emplace_back(TargetRef, FTransactionObjectEvent{Context.TransactionId, Context.OperationId, Type, Operation, std::move(Value), std::move(Annotation)});
		};
		if (Change.HasChanges() || OriginalAnnotation || CurrentAnnotation) Add(Target, std::move(Change), CurrentAnnotation);
		if (CurrentAnnotation)
		{
			std::vector<std::pair<DObject*, FTransactionObjectChange>> Additional;
			auto Gather = [&](const ITransactionObjectAnnotation* Baseline) {
				for (auto& [AdditionalObject, AdditionalChange] : CurrentAnnotation->ComputeAdditionalObjectChanges(Baseline))
				{
					auto Existing = std::ranges::find(Additional, AdditionalObject, [](const auto& Entry) { return Entry.first; });
					if (Existing == Additional.end()) Additional.emplace_back(AdditionalObject, std::move(AdditionalChange));
					else
					{
						auto& Value = Existing->second;
						for (const auto Name : AdditionalChange.ChangedProperties)
							if (!std::ranges::contains(Value.ChangedProperties, Name)) Value.ChangedProperties.push_back(Name);
						Value.bNameChanged |= AdditionalChange.bNameChanged; Value.bOuterChanged |= AdditionalChange.bOuterChanged;
						Value.bNonPropertyDataChanged |= AdditionalChange.bNonPropertyDataChanged; Value.bEditorPresenceChanged |= AdditionalChange.bEditorPresenceChanged;
					}
				}
			};
			Gather(OriginalAnnotation.get());
			const auto& ObservedAnnotation = SnapshotState ? SnapshotState->GetAnnotation() : nullptr;
			if (Type == ETransactionObjectEventType::Finalized && ObservedAnnotation) Gather(ObservedAnnotation.get());
			for (auto& [AdditionalObject, AdditionalChange] : Additional)
			{
				if (!IsValid(AdditionalObject)) return std::unexpected("An annotation reported an unavailable event target.");
				if (AdditionalChange.HasChanges())
				{
					Add(FPersistentObjectRef(AdditionalObject), std::move(AdditionalChange), {});
					Events.back().second.CreatedBy = ETransactionObjectChangeCreatedBy::TransactionAnnotation;
				}
			}
		}
		return Events;
	}

	auto FTransactionObjectRecord::Snapshot(const FTransactionContext& Context, std::span<const FProperty* const> Selected)
		-> std::expected<std::vector<std::pair<FPersistentObjectRef, FTransactionObjectEvent>>, std::string>
	{
		FGarbageCollectionDeferralScope DeferCollection;
		auto* Object = Target.Resolve();
		if (!Object) return std::unexpected("Transaction snapshot target is unavailable.");
		for (const auto* Property : Selected)
			if (!Property || Object->GetClass()->FindPropertyByName(Property->NamePrivate) != Property)
				return std::unexpected("Transaction snapshots require top-level properties of the captured object.");
		auto Selection = SelectedSnapshotProperties;
		if (Selected.empty()) Selection.clear();
		else if (!SnapshotState || !Selection.empty())
			for (const auto* Property : Selected)
				if (!std::ranges::contains(Selection, Property->NamePrivate)) Selection.push_back(Property->NamePrivate);
		// Observation owns separate baselines. Never promote or replace undo storage.
		FTransactionObjectChange Change;
		std::shared_ptr<ITransactionObjectAnnotation> OriginalAnnotation, CurrentAnnotation;
		const auto& Baseline = BeforeState;
		if (!Baseline) return std::unexpected("Transaction snapshot baseline is unavailable.");
		auto Captured = Capture(*Object, &*Baseline, SnapshotState ? &SnapshotState->Annotation : nullptr);
		if (!Captured) return std::unexpected(Captured.error());
		auto Diff = DiffUtil::GenerateObjectDiff(SnapshotState ? SnapshotState->Observation : Baseline->Observation, Captured->Observation,
			{.ShouldSkipProperty = [&](FName Name) { return !Selection.empty() && !std::ranges::contains(Selection, Name); }});
		if (!Diff) return std::unexpected(Diff.error());
		Change = std::move(*Diff); CurrentAnnotation = Captured->GetAnnotation(); OriginalAnnotation = SnapshotState ? SnapshotState->Annotation : Baseline->Annotation;
		std::vector<std::pair<FPersistentObjectRef, FTransactionObjectEvent>> Events;
		if (Change.HasChanges() || CurrentAnnotation)
		{
			Events.emplace_back(Target, FTransactionObjectEvent{Context.TransactionId, Context.OperationId,
				ETransactionObjectEventType::Snapshot, ETransactionOperation::Execute, std::move(Change), CurrentAnnotation});
			if (CurrentAnnotation)
				for (auto& [AdditionalObject, AdditionalChange] : CurrentAnnotation->ComputeAdditionalObjectChanges(OriginalAnnotation.get()))
				{
					if (!IsValid(AdditionalObject)) return std::unexpected("An annotation reported an unavailable snapshot target.");
					if (AdditionalChange.HasChanges()) Events.emplace_back(FPersistentObjectRef(AdditionalObject), FTransactionObjectEvent{
						Context.TransactionId, Context.OperationId, ETransactionObjectEventType::Snapshot, ETransactionOperation::Execute,
						std::move(AdditionalChange), {}, ETransactionObjectChangeCreatedBy::TransactionAnnotation});
				}
		}
		SnapshotState = std::move(*Captured); SelectedSnapshotProperties = std::move(Selection);
		return Events;
	}

	auto FTransactionObjectRecord::AddReferencedObjects(FReferenceCollector& Collector) const -> void
	{
		Target.AddReferencedObjects(Collector);
		if (BeforeState) BeforeState->AddReferencedObjects(Collector);
		if (AfterState) AfterState->AddReferencedObjects(Collector);
		if (ReplayState) ReplayState->AddReferencedObjects(Collector);
		if (SnapshotState) SnapshotState->AddReferencedObjects(Collector);
		if (CompletionAnnotation) CompletionAnnotation->AddReferencedObjects(Collector);
	}

	auto FTransactionObjectRecord::TryGetAllocatedSize(size_t& OutBytes) const -> bool
	{
		size_t Total = 0;
		auto Add = [&](size_t Value) {
			if (Total > std::numeric_limits<size_t>::max() - Value) return false;
			Total += Value; return true;
		};
		if (SelectedSnapshotProperties.capacity() > std::numeric_limits<size_t>::max() / sizeof(FName)
			|| !Add(SelectedSnapshotProperties.capacity() * sizeof(FName))) return false;
		for (const auto* State : {&BeforeState, &AfterState, &ReplayState})
			if (*State) { size_t Size = 0; if (!(**State).TryGetAllocatedSize(Size) || !Add(Size)) return false; }
		if (SnapshotState) { size_t Size = 0; if (!SnapshotState->TryGetAllocatedSize(Size) || !Add(Size)) return false; }
		OutBytes = Total; return true;
	}

	auto FTransactionObjectRecord::GetAffectedPackages() const -> std::vector<DPackage*>
	{
		std::vector<DPackage*> Result;
		for (const auto* State : {&BeforeState, &AfterState})
			if (*State)
				if (auto* Package = Cast<DPackage>((**State).Observation.ObjectInfo.Package.Resolve()); Package && !std::ranges::contains(Result, Package))
					Result.push_back(Package);
		return Result;
	}
}
