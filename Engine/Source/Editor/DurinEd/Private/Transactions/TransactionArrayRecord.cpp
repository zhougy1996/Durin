#include "Transactions/TransactionArrayRecord.h"

#include "DObject/Class.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/ObjectGraphReplacement.h"
#include "DObject/TransactionArray.h"
#include "DObject/Package.h"
#include "DObject/TransactionObjectAnnotation.h"

namespace Durin::Editor
{
	auto FTransactionArrayRecord::HasReplacementReferences(const FObjectReplacementMap& Map) const -> bool
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
		if (Check.bMapped || Map.Find(Target.Resolve()) || Map.Find(Class.Resolve())) return true;
		for (const auto* Range : {&Data, Inverse ? &*Inverse : nullptr})
			if (Range) for (const auto& Value : *Range)
			{
				if (Value.Payload.HasReplacementReferences(Map)) return true;
				for (const auto& Reference : Value.HardReferences) if (Map.Find(Reference.Resolve())) return true;
			}
		return false;
	}

	auto FTransactionArrayRecord::CreateReplacement(const FObjectReplacementMap& Map) const -> std::expected<FTransactionArrayRecord, std::string>
	{
		if (Map.Find(Class.Resolve())) return std::unexpected("Array history cannot migrate reflected class descriptors.");
		auto Result = *this;
		auto Rebound = Target.CreateReplacement(Map);
		if (!Rebound) return std::unexpected(Rebound.error());
		Result.Target = std::move(*Rebound);
		for (auto* Range : {&Result.Data, Result.Inverse ? &*Result.Inverse : nullptr})
			if (Range) for (auto& Value : *Range)
			{
				auto Resolved = Value.Payload.WithResolvedReferences(Value.HardReferences);
				if (!Resolved) return std::unexpected(Resolved.error());
				auto Replaced = Resolved->CreateReplacement(Map);
				if (!Replaced) return std::unexpected(Replaced.error());
				Value.Payload = std::move(*Replaced);
				for (auto& Reference : Value.HardReferences)
				{
					auto ReboundReference = Reference.CreateReplacement(Map);
					if (!ReboundReference) return std::unexpected(ReboundReference.error());
					Reference = std::move(*ReboundReference);
				}
			}
		for (auto* Value : {&Result.Annotation, &Result.InverseAnnotation, &Result.CompletionAnnotation})
		{
			auto Replaced = PrepareTransactionAnnotationReplacement(*Value, Map);
			if (!Replaced) return std::unexpected(Replaced.error());
			*Value = std::move(*Replaced);
		}
		if (auto Valid = Result.Validate(); !Valid) return std::unexpected(Valid.error());
		return Result;
	}

	auto FTransactionArrayRecord::Capture(const FTransactionArrayCapture& Request)
		-> std::expected<FTransactionArrayRecord, std::string>
	{
		if (!Request.Object || !CanTransactObject(*Request.Object) || !Request.Member
			|| Request.Member->GetKind() != DurinCodeGen::EPropertyGenFlags::Array)
			return std::unexpected("Array capture requires a live reflected array member.");
		const auto* Property = static_cast<const FArrayProperty*>(Request.Member);
		if (Request.MemberIndex >= Property->GetArrayDim()
			|| Property->HasAnyPropertyFlags(EPropertyFlags::NonTransactional | EPropertyFlags::Deprecated)
			|| !Property->HasArrayOps() || !IsValidArrayOps(&Property->GetOps())
			|| !Property->GetInner() || Property->GetInner()->GetOffset() != 0
			|| !Property->HasCapability(EArrayOpsFlags::Count | EArrayOpsFlags::RandomAccess | EArrayOpsFlags::DefaultGrow | EArrayOpsFlags::DetachedStorage)
			|| !Property->GetOps().Splice || !Property->GetOps().Resize)
			return std::unexpected("The array requires an explicit whole-object snapshot fallback.");
		if (auto Valid = ValidatePropertyValueSnapshotType(Property->GetInner(), true); !Valid)
			return std::unexpected(ToString(Valid.error()));
		if (Request.Operation == ETransactionArrayOperation::Replace && !Property->GetInner()->CanCopyAssignValue())
			return std::unexpected("Array value replacement requires a copyable omitted-field baseline.");
		FTransactionArrayRecord Result;
		Result.Target = FPersistentObjectRef(Request.Object);
		Result.Class = FPersistentObjectRef(Request.Object->GetClass());
		Result.Member = Property;
		Result.MemberName = Property->NamePrivate;
		Result.StructPath.assign(Request.StructPath.begin(), Request.StructPath.end());
		Result.MemberIndex = Request.MemberIndex;
		Result.Index = Request.Index;
		Result.Count = Request.Count;
		Result.Oper = static_cast<int8>(Request.Operation);
		if (Result.Oper < -1 || Result.Oper > 1) return std::unexpected("Invalid array transaction operation.");
		const auto& Ops = Property->GetOps();
		if (!Result.ResolveContainer()) return std::unexpected("Array reflected member path is invalid.");
		const auto Num = Ops.Num(Result.ResolveContainer());
		if (Request.Index > Num || (Result.Oper != 1 && Request.Count > Num - Request.Index)
			|| (Result.Oper == 1 && Request.Count > std::numeric_limits<size_t>::max() - Num))
			return std::unexpected("Array transaction range is out of bounds.");
		auto Captured = Result.CaptureRange(Result.Oper == 1 ? 0 : Request.Count);
		if (!Captured) return std::unexpected(Captured.error());
		Result.Data = std::move(*Captured);
		Result.Annotation = Request.Object->FindOrCreateTransactionAnnotation();
		if (auto Valid = Result.Validate(); !Valid) return std::unexpected(Valid.error());
		return Result;
	}

	auto FTransactionArrayRecord::Validate() const -> FTransactionCaptureResult
	{
		auto* Object = Target.Resolve();
		if (!Object || !CanTransactObject(*Object) || Object->GetClass() != Class.Resolve()
			|| !ResolveContainer())
			return std::unexpected("Array transaction owner or reflected schema is unavailable.");
		for (const auto* Range : {&Data, Inverse ? &*Inverse : nullptr})
			if (Range) for (const auto& Element : *Range)
				for (const auto& Reference : Element.HardReferences)
					if (Reference.IsStale()) return std::unexpected("Array transaction reference is unavailable.");
		try
		{
			for (const auto& Value : {Annotation, InverseAnnotation, CompletionAnnotation})
				if (Value) if (auto Valid = Value->Validate(); !Valid) return Valid;
		}
		catch (const std::exception& Error) { return std::unexpected(std::format("Array annotation validation failed: {}", Error.what())); }
		catch (...) { return std::unexpected("Array annotation validation failed."); }
		return {};
	}

	auto FTransactionArrayRecord::ResolveContainer() const -> void*
	{
		auto* Object = Target.Resolve();
		if (!Object || Object->GetClass() != Class.Resolve()) return nullptr;
		return ResolveTransactionArray({.Object = Object, .Member = Member, .MemberIndex = MemberIndex, .StructPath = StructPath});
	}

	auto FTransactionArrayRecord::CaptureRange(size_t Num) const -> std::expected<FRange, std::string>
	{
		const auto& Ops = Member->GetOps();
		const void* Container = ResolveContainer();
		const auto Size = Ops.Num(Container);
		if (Index > Size || Num > Size - Index) return std::unexpected("Array replay range is out of bounds.");
		FRange Result;
		Result.reserve(Num);
		for (size_t Offset = 0; Offset < Num; ++Offset)
		{
			const void* Element = nullptr;
			if (Ops.GetConstAt(Container, Index + Offset, &Element) != EContainerOpResult::Success)
				return std::unexpected("Array element lookup failed.");
			FPropertyValueSnapshotPayload Payload;
			if (auto Captured = CapturePropertyValuePayload(Member->GetInner(), Element, 0, Payload, true); !Captured)
				return std::unexpected(ToString(Captured.error()));
			FElement Captured{.Payload = std::move(Payload)};
			Captured.HardReferences.reserve(Captured.Payload.GetReferencedObjectKeys().size());
			for (auto Key : Captured.Payload.GetReferencedObjectKeys())
			{
				auto* Object = ResolveObjectKey(Key);
				if (!Object) return std::unexpected("Array capture contains an unavailable hard reference.");
				Captured.HardReferences.emplace_back(Object);
			}
			Result.push_back(std::move(Captured));
		}
		return Result;
	}

	auto FTransactionArrayRecord::Apply(bool bCompensate, EPropertyChangeOrigin Origin, bool* OutRecoveryRequired) -> FTransactionCaptureResult
	{
		if (auto Valid = Validate(); !Valid) return Valid;
		const int8 Direction = bCompensate ? -Oper : Oper;
		const auto* Payload = bCompensate ? (Inverse ? &*Inverse : nullptr) : &Data;
		if (!Payload) return std::unexpected("Array compensation has no prepared inverse.");
		const auto& Ops = Member->GetOps();
		const size_t RemoveCount = Direction == -1 ? 0 : Count;
		if (!bCompensate)
		{
			// Capture at this ordered step, after preceding array operations have
			// restored their indices. Saving every range from the final array is invalid.
			auto Captured = CaptureRange(RemoveCount);
			if (!Captured) return std::unexpected(Captured.error());
			Inverse = std::move(*Captured);
			InverseAnnotation = Annotation ? Annotation->CreateSnapshot(*Target.Resolve()) : Target.Resolve()->FindOrCreateTransactionAnnotation();
		}
		FDetachedContainerStorage Detached;
		if (Detached.Create(Ops) != EContainerOpResult::Success) return std::unexpected("Array replay range allocation failed.");
		if (Ops.Resize(Detached.Get(), Payload->size()) != EContainerOpResult::Success)
			return std::unexpected("Array replay range construction failed.");
		for (size_t Offset = 0; Offset < Payload->size(); ++Offset)
		{
			void* Element = nullptr;
			if (Ops.GetMutableAt(Detached.Get(), Offset, &Element) != EContainerOpResult::Success)
				return std::unexpected("Array replay element lookup failed.");
			if (Direction == 0)
			{
				const void* Current = nullptr;
				if (Ops.GetConstAt(ResolveContainer(), Index + Offset, &Current) != EContainerOpResult::Success)
					return std::unexpected("Array replacement baseline is unavailable.");
				if (auto Copied = Member->GetInner()->CopyAssignValue(Element, Current); !Copied)
					return std::unexpected(ToString(Copied.error()));
			}
			const auto& Stored = (*Payload)[Offset];
			auto Resolved = Stored.Payload.WithResolvedReferences(Stored.HardReferences);
			if (!Resolved) return std::unexpected(Resolved.error());
			if (auto Restored = RestorePropertyValuePayload(Member->GetInner(), Element, 0, *Resolved, Direction == 0); !Restored)
				return std::unexpected(ToString(Restored.error()));
		}
		if (Ops.Splice(ResolveContainer(), Index, RemoveCount, Detached.Get()) != EContainerOpResult::Success)
			return std::unexpected("Array replay splice rejected the range.");
		CompletionAnnotation = bCompensate ? InverseAnnotation : Annotation;
		bHasCompletion = true;
		if (OutRecoveryRequired) *OutRecoveryRequired = true;
		auto* Object = Target.Resolve();
		const FProperty* Root = StructPath.empty() ? Member : StructPath.front();
		const FPropertyPathSegment Path{Root};
		Object->PostEditChangeProperty({.MemberProperty = Root, .LeafProperty = Root, .Path = {&Path, 1},
			.Phase = EPropertyChangePhase::Committed, .Kind = Direction == 0 ? EPropertyChangeKind::ValueSet
				: Direction == 1 ? EPropertyChangeKind::ArrayRemove : EPropertyChangeKind::ArrayAdd, .Origin = Origin});
		Object->MarkPackageDirty();
		if (OutRecoveryRequired) *OutRecoveryRequired = false;
		return {};
	}

	auto FTransactionArrayRecord::CommitReplay() -> void
	{
		if (!Inverse) return;
		Data = std::move(*Inverse);
		Annotation = std::move(InverseAnnotation);
		Oper = -Oper;
		Inverse.reset();
	}

	auto FTransactionArrayRecord::AddReferencedObjects(FReferenceCollector& Collector) const -> void
	{
		Target.AddReferencedObjects(Collector); Class.AddReferencedObjects(Collector);
		for (const auto* Range : {&Data, Inverse ? &*Inverse : nullptr})
			if (Range) for (const auto& Element : *Range)
				for (const auto& Reference : Element.HardReferences) Reference.AddReferencedObjects(Collector);
		for (const auto& Value : {Annotation, InverseAnnotation, CompletionAnnotation})
			if (Value) Value->AddReferencedObjects(Collector);
	}

	auto FTransactionArrayRecord::TryGetAllocatedSize(size_t& OutBytes) const -> bool
	{
		OutBytes = sizeof(*this);
		auto Add = [&](size_t Value) { if (Value > std::numeric_limits<size_t>::max() - OutBytes) return false; OutBytes += Value; return true; };
		if (!Add(Target.GetAllocatedSize()) || !Add(Class.GetAllocatedSize()) || !Add(StructPath.capacity() * sizeof(const FProperty*))) return false;
		for (const auto* Range : {&Data, Inverse ? &*Inverse : nullptr}) if (Range)
		{
			if (Range->capacity() > std::numeric_limits<size_t>::max() / sizeof(FElement)
				|| !Add(Range->capacity() * sizeof(FElement))) return false;
			for (const auto& Element : *Range)
			{
				size_t Bytes = 0;
				if (!Element.Payload.TryGetAllocatedSize(Bytes) || !Add(Bytes)
					|| Element.HardReferences.capacity() > std::numeric_limits<size_t>::max() / sizeof(FPersistentObjectRef)
					|| !Add(Element.HardReferences.capacity() * sizeof(FPersistentObjectRef))) return false;
				for (const auto& Reference : Element.HardReferences) if (!Add(Reference.GetAllocatedSize())) return false;
			}
		}
		for (const auto& Value : {Annotation, InverseAnnotation, CompletionAnnotation})
			if (Value && !Add(Value->GetAllocatedSize())) return false;
		return true;
	}

	auto FTransactionArrayRecord::GetAffectedPackages() const -> std::vector<DPackage*>
	{
		if (auto* Object = Target.Resolve(); Object && Object->GetPackage()) return {Object->GetPackage()};
		return {};
	}

	auto FTransactionArrayRecord::BuildObjectEvents(const FTransactionContext& Context,
		ETransactionObjectEventType Type, ETransactionOperation Operation) const
		-> std::expected<std::vector<std::pair<FPersistentObjectRef, FTransactionObjectEvent>>, std::string>
	{
		auto* Object = Target.Resolve();
		if (!Object) return std::unexpected("Array event target is unavailable.");
		const auto CurrentAnnotation = Type == ETransactionObjectEventType::UndoRedo ? CompletionAnnotation
			: Annotation ? Annotation->CreateSnapshot(*Object) : Object->FindOrCreateTransactionAnnotation();
		FTransactionObjectEvent Result{.TransactionId = Context.TransactionId, .OperationId = Context.OperationId,
			.Type = Type, .Operation = Operation,
			.Change = FTransactionObjectChange(CaptureTransactionObjectIdentity(*Object), Object->GetObjectFlags()),
			.Annotation = CurrentAnnotation};
		if (Count) Result.Change.ChangedProperties.push_back(StructPath.empty() ? MemberName : StructPath.front()->NamePrivate);
		std::vector<std::pair<FPersistentObjectRef, FTransactionObjectEvent>> Events;
		Events.emplace_back(Target, std::move(Result));
		if (CurrentAnnotation)
			for (auto& [AdditionalObject, Change] : CurrentAnnotation->ComputeAdditionalObjectChanges(Annotation.get()))
			{
				if (!IsValid(AdditionalObject)) return std::unexpected("An array annotation reported an unavailable event target.");
				if (Change.HasChanges()) Events.emplace_back(FPersistentObjectRef(AdditionalObject), FTransactionObjectEvent{
					Context.TransactionId, Context.OperationId, Type, Operation, std::move(Change), {}, ETransactionObjectChangeCreatedBy::TransactionAnnotation});
			}
		return Events;
	}
}
