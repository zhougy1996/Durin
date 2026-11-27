#include "Transactions/Transactor.h"
#include "Threading/RunnableThread.h"
#include "CoreGlobals.h"

#include "DObject/Class.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"
#include "DObject/Property.h"
#include "DObject/TransactionObjectAnnotation.h"
#include "DObject/ObjectGraphReplacement.h"

namespace Durin
{
	auto FTransaction::FWholeObjectData::HasReplacementReferences(const FObjectReplacementMap& Map) const -> bool
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
		for (const auto* State : {&SerializedObject, &SerializedObjectFlip, &SnapshotState})
			if (*State && (*State)->GetSerializedCapture().HasReplacementReferences(Map)) return true;
		return false;
	}

	auto FTransaction::FWholeObjectData::Reinstance(const FObjectReplacementMap& Map) -> FTransactionCaptureResult
	{
		auto Reference = Target.CreateReplacement(Map);
		if (!Reference) return std::unexpected(Reference.error());
		if (auto* Object = Target.Get(); Object && Reference->Get() == Object)
			for (auto* Outer = Object->GetOuter(); Outer; Outer = Outer->GetOuter())
				if (Map.Find(Outer)) return std::unexpected("The snapshot target has no replacement under its reinstanced owner.");
		Target = std::move(*Reference);
		for (auto* State : {&SerializedObject, &SerializedObjectFlip, &SnapshotState})
			if (*State)
			{
				auto Observation = DiffUtil::PrepareObjectReplacement((*State)->Observation, Map);
				if (!Observation) return std::unexpected(Observation.error());
				(*State)->Observation = std::move(*Observation);
				auto Annotation = ReinstanceTransactionAnnotation((*State)->Annotation, Map);
				if (!Annotation) return std::unexpected(Annotation.error());
			}
		return Validate();
	}

	auto FTransaction::FWholeObjectData::Capture(DObject& Object, const FSerializedObject* CapturePolicy,
		const std::shared_ptr<ITransactionObjectAnnotation>* AnnotationPolicy) -> std::expected<FTransaction::FWholeObjectData::FSerializedObject, std::string>
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		if (!CanTransactObject(Object) && !(CapturePolicy && CapturePolicy->Observation.ObjectInfo.Target.Get() == &Object && Object.IsGarbage()))
			return std::unexpected("Transaction capture requires a live object or a transactional published default graph.");
		if (CapturePolicy && CapturePolicy->Observation.ObjectInfo.Target.Get() != &Object)
			return std::unexpected("Transaction capture policy belongs to a different object.");
		DObjectForceRegistration(Object.GetClass());
		if (!IsValid(Object.GetClass())) return std::unexpected("Transaction class metadata is unavailable.");
		const FSerializedObjectInfo Info{FPersistentObjectRef(&Object), FPersistentObjectRef(Object.GetClass()),
			FPersistentObjectRef(Object.GetOuter()), FPersistentObjectRef(Object.GetPackage()), CaptureTransactionObjectIdentity(Object), Object.GetObjectFlags(), Object.IsGarbage()};
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

	auto FTransaction::FWholeObjectData::FSerializedObject::Validate(DObject& Object) const -> FTransactionCaptureResult
	{
		if (auto Result = ValidateSerializedObject(Object, GetSerializedCapture()); !Result) return Result;
		if (Annotation)
			try { if (auto Result = Annotation->Validate(); !Result) return Result; }
			catch (const std::exception& Error) { return std::unexpected(std::format("Transaction annotation validation threw: {}", Error.what())); }
			catch (...) { return std::unexpected("Transaction annotation validation threw an unknown exception."); }
		return {};
	}

	auto FTransaction::FWholeObjectData::FSerializedObject::AddReferencedObjects(FReferenceCollector& Collector) const -> void
	{
		Observation.AddReferencedObjects(Collector);
		if (Annotation) Annotation->AddReferencedObjects(Collector);
	}

	auto FTransaction::FWholeObjectData::FSerializedObject::TryGetAllocatedSize(size_t& OutBytes) const -> bool
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

	auto FTransaction::FWholeObjectData::CaptureWhole(DObject& Object)
		-> std::expected<FTransaction::FWholeObjectData, std::string>
	{
		FTransaction::FWholeObjectData Record;
		Record.Target = FPersistentObjectRef(&Object);
		auto State = Capture(Object);
		if (!State) return std::unexpected(State.error());
		Record.SerializedObject = std::move(*State);
		return Record;
	}

	auto FTransaction::FWholeObjectData::IsNoOp() const -> bool
	{
		return SerializedObject && SerializedObjectFlip && !SerializedObject->GetAnnotation() && !SerializedObjectFlip->GetAnnotation()
			&& SerializedObject->Observation.ObjectInfo == SerializedObjectFlip->Observation.ObjectInfo
			&& *SerializedObject->Observation.SerializedObject == *SerializedObjectFlip->Observation.SerializedObject && SerializedObject->HasSameSchema(*SerializedObjectFlip);
	}

	auto FTransaction::FWholeObjectData::HasExpired() const noexcept -> bool
	{
		const auto Expired = [](const auto& State) {
			return State && State->GetAnnotation() && State->GetAnnotation()->HasExpired();
		};
		return Expired(SerializedObject) || Expired(SerializedObjectFlip);
	}

	auto FTransaction::FWholeObjectData::Validate() const -> FTransactionCaptureResult
	{
		auto* Object = Target.Get();
		if (!Object) return std::unexpected("Transaction object target is unavailable.");
		if (!SerializedObject) return std::unexpected("Transaction object state is unavailable.");
		if (auto Result = SerializedObject->Validate(*Object); !Result) return Result;
		if (SerializedObjectFlip) return SerializedObjectFlip->Validate(*Object);
		return {};
	}

	// Only a field explicitly requiring an exact starting state opts into this check.
	// GenerateObjectDiff compares reference identities through each capture's tables.
	auto FTransaction::FWholeObjectData::ValidateSource() const -> FTransactionCaptureResult
	{
		const auto& SourceState = SerializedObjectFlip ? *SerializedObjectFlip : *SerializedObject;
		const auto& Source = SourceState.Observation;
		auto* Object = Target.Get();
		auto RequiresMatch = [&](FName Name) {
			const auto* Field = Object->GetClass()->FindPropertyByName(Name);
			return Field && Field->GetMetaData("ValidateTransactionSource") == "true";
		};
		if (!std::ranges::any_of(Source.SerializedTaggedData, [&](const auto& Tag) {
			return Tag.bReflected && RequiresMatch(Tag.Name);
		})) return {};
		const auto Policy = SourceState.GetSerializedCapture();
		FDiffableObject Current;
		if (auto Captured = CaptureSerializedObject(*Object, &Policy, &Current); !Captured) return std::unexpected(Captured.error());
		auto Diff = DiffUtil::GenerateObjectDiff(Source, Current, {.bFullDiff = false,
			.ShouldSkipProperty = [&](FName Name) { return !RequiresMatch(Name); }});
		if (!Diff) return std::unexpected(Diff.error());
		if (!Diff->ChangedProperties.empty())
			return std::unexpected(std::format("Transaction source property '{}' changed outside history.", Diff->ChangedProperties.front().ToString()));
		return {};
	}

	auto FTransaction::FWholeObjectData::CaptureAfter() -> void
	{
		if (bFinalized) return;
		bFinalized = true;
		if (!SerializedObject) throw std::runtime_error("Transaction object initial state is unavailable.");
		auto* Object = Target.Get();
		if (!Object) throw std::runtime_error("Transaction final capture target is unavailable.");
		auto State = Capture(*Object, &*SerializedObject);
		if (!State) throw std::runtime_error(State.error());
		if (!SerializedObject->HasSameSchema(*State)) throw std::runtime_error("Transaction field schema changed during recording.");
		auto Diff = DiffUtil::GenerateObjectDiff(SerializedObject->Observation, State->Observation);
		if (!Diff) throw std::runtime_error(Diff.error());
		auto& Change = *Diff;
		if (SnapshotState)
		{
			auto Closure = DiffUtil::GenerateObjectDiff(SnapshotState->Observation, State->Observation, {.bFullDiff = false,
				.ShouldSkipProperty = [&](FName Name) { return !SelectedSnapshotProperties.empty() && !std::ranges::contains(SelectedSnapshotProperties, Name); }});
			if (!Closure) throw std::runtime_error(Closure.error());
			for (const auto Name : Closure->ChangedProperties)
				if (!std::ranges::contains(Change.ChangedProperties, Name)) Change.ChangedProperties.push_back(Name);
			Change.bNonPropertyDataChanged |= Closure->bNonPropertyDataChanged;
		}
		if (Change.HasChanges() || *SerializedObject->Observation.SerializedObject != *State->Observation.SerializedObject)
		{
			// UE keeps the delta; the current load slot supplies identity when the event is delivered.
			static_cast<FTransactionObjectId&>(Change) = {};
			FinalizedChange = std::move(Change);
		}
		SerializedObjectFlip = std::move(*State);
	}

	auto FTransaction::FWholeObjectData::CaptureInverse() -> void
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		if (!SerializedObject) throw std::runtime_error("Transaction object initial state is unavailable.");
		auto* Object = Target.Get();
		if (!Object) throw std::runtime_error("Transaction inverse capture target is unavailable.");
		auto State = Capture(*Object, &*SerializedObject);
		if (!State) throw std::runtime_error(State.error());
		// Preserve the lifecycle transition recorded at finalization.
		if (SerializedObjectFlip)
			State->Observation.ObjectInfo.bIsPendingKill = SerializedObjectFlip->Observation.ObjectInfo.bIsPendingKill;
		SerializedObjectFlip = std::move(*State);
	}

	auto FTransaction::FWholeObjectData::Apply(EPropertyChangeOrigin Origin) -> void
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		auto* Object = Target.Get();
		if (!Object) throw std::runtime_error("Transaction restore target is unavailable.");
		if (auto Validated = SerializedObject->Validate(*Object); !Validated) throw std::runtime_error(Validated.error());
		LoadSerializedObject(*Object, SerializedObject->GetSerializedCapture());
		// Per-member notifications describe the fields this load actually replaced.
		auto Changes = DiffUtil::GenerateObjectDiff(SerializedObjectFlip->Observation, SerializedObject->Observation);
		if (!Changes) throw std::runtime_error(Changes.error());
		for (const auto Name : Changes->ChangedProperties)
			if (auto* Member = Object->GetClass()->FindPropertyByName(Name))
			{
				const FPropertyPathSegment Path{Member};
				Object->PostEditChangeProperty({.MemberProperty = Member, .LeafProperty = Member,
					.Path = {&Path, 1}, .ChangeType = EPropertyChangeType::ValueSet, .Origin = Origin});
			}
		Object->MarkPackageDirty();
		SerializedObject.swap(SerializedObjectFlip);
	}

	auto FTransaction::FWholeObjectData::BuildObjectEvents(const FTransactionContext& Context,
		ETransactionObjectEventType Type, const std::shared_ptr<ITransactionObjectAnnotation>& CurrentAnnotation) const
		-> std::expected<std::vector<std::pair<FPersistentObjectRef, FTransactionObjectEvent>>, std::string>
	{
		std::vector<std::pair<FPersistentObjectRef, FTransactionObjectEvent>> Events;
		if (!FinalizedChange && !CurrentAnnotation) return Events;
		auto* Object = Target.Get();
		if (!Object) return Events;
		const auto& Baseline = SerializedObject;
		FTransactionObjectChange Change = FinalizedChange.value_or(FTransactionObjectChange{});
		static_cast<FTransactionObjectId&>(Change) = Baseline->Observation.ObjectInfo.Identity;
		Change.OriginalFlags = Baseline->Observation.ObjectInfo.Flags;
		if (Type == ETransactionObjectEventType::UndoRedo && Change.bHasPendingKillChange && !Object->IsGarbage())
		{
			const auto Policy = Baseline->GetSerializedCapture();
			auto Live = DiffUtil::GetDiffableObject(*Object, &Policy);
			if (!Live) return std::unexpected(Live.error());
			FDiffableObject Dead{.ObjectInfo = Baseline->Observation.ObjectInfo};
			Dead.ObjectInfo.bIsPendingKill = true;
			auto Diff = DiffUtil::GenerateObjectDiff(Dead, *Live);
			if (!Diff) return std::unexpected(Diff.error());
			Change = std::move(*Diff);
		}
		const auto& OriginalAnnotation = Baseline->GetAnnotation();
		auto Add = [&](FPersistentObjectRef TargetRef, FTransactionObjectChange Value, std::shared_ptr<ITransactionObjectAnnotation> Annotation) {
			Events.emplace_back(TargetRef, FTransactionObjectEvent{Context.TransactionId, Context.OperationId, Type, std::move(Value), std::move(Annotation)});
		};
		Add(Target, std::move(Change), CurrentAnnotation);
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
						Value.bNonPropertyDataChanged |= AdditionalChange.bNonPropertyDataChanged; Value.bHasPendingKillChange |= AdditionalChange.bHasPendingKillChange;
					Value.bEditorPresenceChanged |= AdditionalChange.bEditorPresenceChanged;
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

	auto FTransaction::FWholeObjectData::Snapshot(const FTransactionContext& Context, std::span<const FProperty* const> Selected)
		-> std::expected<std::vector<std::pair<FPersistentObjectRef, FTransactionObjectEvent>>, std::string>
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		auto* Object = Target.Get();
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
		const auto& Baseline = SerializedObject;
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
				ETransactionObjectEventType::Snapshot, std::move(Change), CurrentAnnotation});
			if (CurrentAnnotation)
				for (auto& [AdditionalObject, AdditionalChange] : CurrentAnnotation->ComputeAdditionalObjectChanges(OriginalAnnotation.get()))
				{
					if (!IsValid(AdditionalObject)) return std::unexpected("An annotation reported an unavailable snapshot target.");
					if (AdditionalChange.HasChanges()) Events.emplace_back(FPersistentObjectRef(AdditionalObject), FTransactionObjectEvent{
						Context.TransactionId, Context.OperationId, ETransactionObjectEventType::Snapshot,
						std::move(AdditionalChange), {}, ETransactionObjectChangeCreatedBy::TransactionAnnotation});
				}
		}
		SnapshotState = std::move(*Captured); SelectedSnapshotProperties = std::move(Selection);
		return Events;
	}

	auto FTransaction::FWholeObjectData::AddReferencedObjects(FReferenceCollector& Collector) const -> void
	{
		Target.AddReferencedObjects(Collector);
		if (SerializedObject) SerializedObject->AddReferencedObjects(Collector);
		if (SerializedObjectFlip) SerializedObjectFlip->AddReferencedObjects(Collector);
		if (SnapshotState) SnapshotState->AddReferencedObjects(Collector);
	}

	auto FTransaction::FWholeObjectData::TryGetAllocatedSize(size_t& OutBytes) const -> bool
	{
		size_t Total = Target.GetAllocatedSize();
		auto Add = [&](size_t Value) {
			if (Total > std::numeric_limits<size_t>::max() - Value) return false;
			Total += Value; return true;
		};
		if (SelectedSnapshotProperties.capacity() > std::numeric_limits<size_t>::max() / sizeof(FName)
			|| !Add(SelectedSnapshotProperties.capacity() * sizeof(FName))) return false;
		if (FinalizedChange && (FinalizedChange->ChangedProperties.capacity() > std::numeric_limits<size_t>::max() / sizeof(FName)
			|| !Add(FinalizedChange->ChangedProperties.capacity() * sizeof(FName)))) return false;
		for (const auto* State : {&SerializedObject, &SerializedObjectFlip})
			if (*State) { size_t Size = 0; if (!(**State).TryGetAllocatedSize(Size) || !Add(Size)) return false; }
		if (SnapshotState) { size_t Size = 0; if (!SnapshotState->TryGetAllocatedSize(Size) || !Add(Size)) return false; }
		OutBytes = Total; return true;
	}

	auto FTransaction::FWholeObjectData::GetAffectedPackages() const -> std::vector<DPackage*>
	{
		std::vector<DPackage*> Result;
		for (const auto* State : {&SerializedObject, &SerializedObjectFlip})
			if (*State)
				if (auto* Package = Cast<DPackage>((**State).Observation.ObjectInfo.Package.Resolve()); Package && !std::ranges::contains(Result, Package))
					Result.push_back(Package);
		return Result;
	}
}
