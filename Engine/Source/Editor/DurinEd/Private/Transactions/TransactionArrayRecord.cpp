#include "Transactions/Transactor.h"

#include "DObject/Class.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/ObjectGraphReplacement.h"
#include "DObject/TransactionArray.h"
#include "DObject/Package.h"
#include "DObject/TransactionObjectAnnotation.h"

namespace Durin
{
	auto FTransaction::FArrayData::HasReplacementReferences(const FObjectReplacementMap& Map) const -> bool
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
		if (Check.bMapped || Map.Find(Target.Get()) || Map.Find(Class.Resolve())) return true;
		for (const auto* Range : {&Data, Inverse ? &*Inverse : nullptr})
			if (Range) for (const auto& Value : *Range)
			{
				if (Value.Payload.HasReplacementReferences(Map)) return true;
				for (const auto& Reference : Value.HardReferences) if (Map.Find(Reference.Resolve())) return true;
			}
		return false;
	}

	auto FTransaction::FArrayData::Reinstance(const FObjectReplacementMap& Map) -> FTransactionCaptureResult
	{
		if (Map.Find(Class.Resolve())) return std::unexpected("Array history cannot migrate reflected class descriptors.");
		auto Rebound = Target.CreateReplacement(Map);
		if (!Rebound) return std::unexpected(Rebound.error());
		Target = std::move(*Rebound);
		for (auto* Range : {&Data, Inverse ? &*Inverse : nullptr})
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
		for (auto* Value : {&Annotation, &InverseAnnotation, &CompletionAnnotation})
		{
			auto Replaced = ReinstanceTransactionAnnotation(*Value, Map);
			if (!Replaced) return std::unexpected(Replaced.error());
		}
		if (auto Valid = Validate(); !Valid) return std::unexpected(Valid.error());
		return {};
	}

	auto FTransaction::FArrayData::Capture(const FTransactionArrayCapture& Request)
		-> std::expected<FTransaction::FArrayData, std::string>
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
		FTransaction::FArrayData Result;
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

	auto FTransaction::FArrayData::HasExpired() const noexcept -> bool
	{
		return Annotation && Annotation->HasExpired();
	}

	auto FTransaction::FArrayData::Validate() const -> FTransactionCaptureResult
	{
		auto* Object = Target.Get();
		if (!Object || (!Object->IsGarbage() && !CanTransactObject(*Object)) || Object->GetClass() != Class.Resolve()
			|| !ResolveContainer())
			return std::unexpected("Array transaction owner or reflected schema is unavailable.");
		for (const auto* Range : {&Data, Inverse ? &*Inverse : nullptr})
			if (Range) for (const auto& Element : *Range)
				for (const auto& Reference : Element.HardReferences)
					if (!Reference.IsNull() && !Reference.Get()) return std::unexpected("Array transaction reference is unavailable.");
		try
		{
			for (const auto& Value : {Annotation, InverseAnnotation, CompletionAnnotation})
				if (Value) if (auto Valid = Value->Validate(); !Valid) return Valid;
		}
		catch (const std::exception& Error) { return std::unexpected(std::format("Array annotation validation failed: {}", Error.what())); }
		catch (...) { return std::unexpected("Array annotation validation failed."); }
		return {};
	}

	auto FTransaction::FArrayData::ResolveContainer() const -> void*
	{
		auto* Object = Target.Get();
		if (!Object || Object->GetClass() != Class.Resolve()) return nullptr;
		return ResolveTransactionArray({.Object = Object, .Member = Member, .MemberIndex = MemberIndex, .StructPath = StructPath});
	}

	auto FTransaction::FArrayData::ValidateSourceRange(bool bFirstLoad) const -> FTransactionCaptureResult
	{
		// An insertion reads no source elements; earlier loads can create its insertion point.
		if (Oper == -1 && !bFirstLoad) return {};
		const auto* Container = ResolveContainer();
		if (!Container) return std::unexpected("Array replay container is unavailable.");
		const auto Size = Member->GetOps().Num(Container);
		if (Index > Size || (Oper != -1 && Count > Size - Index))
			return std::unexpected("Array replay range is out of bounds.");
		return {};
	}

	auto FTransaction::FArrayData::CaptureRange(size_t Num) const -> std::expected<FRange, std::string>
	{
		if (Num == 0) return FRange{};
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

	auto FTransaction::FArrayData::CaptureInverse() -> void
	{
		if (auto Valid = Validate(); !Valid) throw std::runtime_error(Valid.error());
		auto Captured = CaptureRange(Oper == -1 ? 0 : Count);
		if (!Captured) throw std::runtime_error(Captured.error());
		Inverse = std::move(*Captured);
		if (Annotation) ReplayChange.emplace(CaptureTransactionObjectIdentity(*Target.Get()), Target.Get()->GetObjectFlags());
		InverseAnnotation = Annotation ? Annotation->CreateSnapshot(*Target.Get()) : Target.Get()->FindOrCreateTransactionAnnotation();
	}

	auto FTransaction::FArrayData::Apply(EPropertyChangeOrigin Origin) -> void
	{
		if (auto Valid = Validate(); !Valid) throw std::runtime_error(Valid.error());
		const auto& Ops = Member->GetOps();
		const size_t RemoveCount = Oper == -1 ? 0 : Count;
		FDetachedContainerStorage Detached;
		if (Detached.Create(Ops) != EContainerOpResult::Success) throw std::runtime_error("Array replay range allocation failed.");
		if (Ops.Resize(Detached.Get(), Data.size()) != EContainerOpResult::Success)
			throw std::runtime_error("Array replay range construction failed.");
		for (size_t Offset = 0; Offset < Data.size(); ++Offset)
		{
			void* Element = nullptr;
			if (Ops.GetMutableAt(Detached.Get(), Offset, &Element) != EContainerOpResult::Success)
				throw std::runtime_error("Array replay element lookup failed.");
			if (Oper == 0)
			{
				const void* Current = nullptr;
				if (Ops.GetConstAt(ResolveContainer(), Index + Offset, &Current) != EContainerOpResult::Success)
					throw std::runtime_error("Array replacement baseline is unavailable.");
				if (auto Copied = Member->GetInner()->CopyAssignValue(Element, Current); !Copied)
					throw std::runtime_error(ToString(Copied.error()));
			}
			const auto& Stored = Data[Offset];
			auto Resolved = Stored.Payload.WithResolvedReferences(Stored.HardReferences);
			if (!Resolved) throw std::runtime_error(Resolved.error());
			if (auto Restored = RestorePropertyValuePayload(Member->GetInner(), Element, 0, *Resolved, Oper == 0); !Restored)
				throw std::runtime_error(ToString(Restored.error()));
		}
		if (Ops.Splice(ResolveContainer(), Index, RemoveCount, Detached.Get()) != EContainerOpResult::Success)
			throw std::runtime_error("Array replay splice rejected the range.");
		CompletionAnnotation = Annotation;
		auto* Object = Target.Get();
		const FProperty* Root = StructPath.empty() ? Member : StructPath.front();
		const FPropertyPathSegment Path{Root};
		Object->PostEditChangeProperty({.MemberProperty = Root, .LeafProperty = Root, .Path = {&Path, 1},
			.Phase = EPropertyChangePhase::Committed, .Kind = Oper == 0 ? EPropertyChangeKind::ValueSet
				: Oper == 1 ? EPropertyChangeKind::ArrayRemove : EPropertyChangeKind::ArrayAdd, .Origin = Origin});
		Object->MarkPackageDirty();
		Data = std::move(*Inverse);
		Annotation = std::move(InverseAnnotation);
		Oper = -Oper;
		Inverse.reset();
	}

	auto FTransaction::FArrayData::AddReferencedObjects(FReferenceCollector& Collector) const -> void
	{
		Target.AddReferencedObjects(Collector); Class.AddReferencedObjects(Collector);
		for (const auto* Range : {&Data, Inverse ? &*Inverse : nullptr})
			if (Range) for (const auto& Element : *Range)
				for (const auto& Reference : Element.HardReferences) Reference.AddReferencedObjects(Collector);
		for (const auto& Value : {Annotation, InverseAnnotation, CompletionAnnotation})
			if (Value) Value->AddReferencedObjects(Collector);
	}

	auto FTransaction::FArrayData::TryGetAllocatedSize(size_t& OutBytes) const -> bool
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

	auto FTransaction::FArrayData::GetAffectedPackages() const -> std::vector<DPackage*>
	{
		if (auto* Object = Target.Get(); Object && Object->GetPackage()) return {Object->GetPackage()};
		return {};
	}

	auto FTransaction::FArrayData::BuildObjectEvents(const FTransactionContext& Context,
		ETransactionObjectEventType Type) const
		-> std::expected<std::vector<std::pair<FPersistentObjectRef, FTransactionObjectEvent>>, std::string>
	{
		if (!ReplayChange) return std::unexpected("Array replay event identity is unavailable.");
		const auto& CurrentAnnotation = CompletionAnnotation;
		FTransactionObjectEvent Result{.TransactionId = Context.TransactionId, .OperationId = Context.OperationId,
			.Type = Type,
			.Change = *ReplayChange,
			.Annotation = CurrentAnnotation};
		std::vector<std::pair<FPersistentObjectRef, FTransactionObjectEvent>> Events;
		Events.emplace_back(Target, std::move(Result));
		if (CurrentAnnotation)
			for (auto& [AdditionalObject, Change] : CurrentAnnotation->ComputeAdditionalObjectChanges(Annotation.get()))
			{
				if (!IsValid(AdditionalObject)) return std::unexpected("An array annotation reported an unavailable event target.");
				if (Change.HasChanges()) Events.emplace_back(FPersistentObjectRef(AdditionalObject), FTransactionObjectEvent{
					Context.TransactionId, Context.OperationId, Type, std::move(Change), {}, ETransactionObjectChangeCreatedBy::TransactionAnnotation});
			}
		return Events;
	}
}
