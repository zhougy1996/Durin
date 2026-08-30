#include "Editor/PropertyEditing.h"

#include "Editor/PropertyValueDraft.h"

#include "DObject/Class.h"
#include "DObject/DObjectArray.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/Object.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Property.h"
#include "DObject/WeakObjectPtr.h"
#include "Logging/LogMacros.h"
#include "Misc/AssertionMacros.h"

namespace Durin::Editor
{
	namespace
	{
		std::mutex GPropertyEditExtensionsMutex;
		std::unordered_map<FPropertyEditExtensionHandle, FPropertyEditExtension>
			GPropertyEditExtensions;
		FPropertyEditExtensionHandle GNextPropertyEditExtensionHandle = 1;

		auto SnapshotPropertyEditExtensions() -> std::vector<FPropertyEditExtension>
		{
			std::lock_guard Lock(GPropertyEditExtensionsMutex);
			std::vector<FPropertyEditExtension> Extensions;
			Extensions.reserve(GPropertyEditExtensions.size());
			for (const auto& [Handle, Extension] : GPropertyEditExtensions)
				Extensions.push_back(Extension);
			return Extensions;
		}

		struct FResolveMapEntryContext
		{
			FProperty* KeyProperty = nullptr;
			const FPropertyValueSnapshotPayload* TargetKey = nullptr;
			const void* Key = nullptr;
			void* Value = nullptr;
			std::string Error;
		};

		auto ResolveMapEntry(void* RawContext, const void* Key, void* Value) -> bool
		{
			auto& Context = *static_cast<FResolveMapEntryContext*>(RawContext);
			FPropertyValueSnapshotPayload StoredKey;
			if (!CapturePropertyValuePayload(
				Context.KeyProperty, Key, 0, StoredKey, &Context.Error)) return false;
			if (StoredKey == *Context.TargetKey)
			{
				Context.Key = Key;
				Context.Value = Value;
				return false;
			}
			return true;
		}

		auto CaptureTargetValue(const FPropertyEditTarget& Target,
			FPropertyValueSnapshotPayload& OutSnapshot, std::string* OutError) -> bool
		{
			return CapturePropertyValuePayload(
				Target.SnapshotProperty, Target.SnapshotContainer, Target.SnapshotArrayIndex, OutSnapshot, OutError);
		}

		auto RestoreTargetValue(const FPropertyEditTarget& Target,
			const FPropertyValueSnapshotPayload& Snapshot, std::string* OutError) -> bool
		{
			return RestorePropertyValuePayload(
				Target.SnapshotProperty, Target.SnapshotContainer, Target.SnapshotArrayIndex, Snapshot, OutError);
		}

		auto MakeEventPath(const FPropertyEditTarget& Target) -> std::vector<FPropertyPathSegment>
		{
			std::vector<FPropertyPathSegment> Result;
			Result.reserve(Target.Path.size());
			for (const FPropertyEditPathSegment& Segment : Target.Path)
				Result.push_back({Segment.Property, Segment.Selector, Segment.Index, Segment.MapKeyData});
			return Result;
		}

		thread_local std::vector<const FPropertyEditTarget*> GActiveGenericMutations;

		// Balances pre/post mutation callbacks even when a container edit fails.
		class FGenericMutationScope
		{
		public:
			explicit FGenericMutationScope(const FPropertyEditTarget& Target)
				: Target(&Target) { GActiveGenericMutations.push_back(this->Target); }
			~FGenericMutationScope() { GActiveGenericMutations.pop_back(); }
		private:
			const FPropertyEditTarget* Target;
		};

		struct FDeferredMutation
		{
			FPropertyValueSnapshotPayload ProposedValue;
			FPropertyEditDeferredAction Action;
		};

		auto ApplyGenericMutation(
			const FPropertyEditTarget& Target,
			const FPropertyValueSnapshotPayload& ProposedValue,
			EPropertyChangePhase Phase,
			EPropertyChangeOrigin Origin,
			FPropertyValueSnapshotPayload* OutAppliedValue,
			FDeferredMutation* OutDeferred,
			std::string* OutError
		) -> bool
		{
			if (std::ranges::any_of(GActiveGenericMutations, [&](const auto* Active) { return Active->IsSameMutationTarget(Target); }))
				return Fail("A reflected property hook cannot start a nested edit of the same target.", OutError);
			FGenericMutationScope Scope(Target);

			FPropertyValueSnapshotPayload Before;
			if (!CaptureTargetValue(Target, Before, OutError)) return false;
			FPropertyValueDraft Draft(Target, OutError);
			if (!Draft.IsValid() || !Draft.Restore(ProposedValue, OutError)) return false;

			FResolvedPropertyValue DraftLeaf;
			const bool bResolvedLeaf = Draft.Resolve(Target, DraftLeaf.Property, DraftLeaf.Container, DraftLeaf.ArrayIndex, nullptr);
			if (!bResolvedLeaf && Target.Kind != EPropertyChangeKind::MapKeyRename
				&& Target.Kind != EPropertyChangeKind::MapRemove)
				return Fail("The detached property proposal leaf could not be resolved.", OutError);

			std::vector<FPropertyPathSegment> EventPath = MakeEventPath(Target);
			FPropertyEditProposal Proposal{
				Target.MemberProperty,
				Target.LeafProperty,
				EventPath,
				Phase,
				Target.Kind,
				Origin,
				Draft.GetRootProperty(),
				Draft.GetRootContainer(),
				Draft.GetRootArrayIndex(),
				bResolvedLeaf ? DraftLeaf.Container : nullptr,
				bResolvedLeaf ? DraftLeaf.ArrayIndex : 0
			};
			std::string HookError;
			for (const FPropertyEditExtension& Extension : SnapshotPropertyEditExtensions())
			{
				if (Extension.PreEdit && !Extension.PreEdit(*Target.Object, Proposal, HookError))
				{
					if (OutError) *OutError = HookError.empty()
						? "A property edit extension rejected the reflected property proposal."
						: HookError;
					return false;
				}
			}
			if (!Target.Object->PreEditChangeProperty(Proposal, HookError))
			{
				if (OutError) *OutError = HookError.empty() ? "The object rejected the reflected property proposal." : HookError;
				return false;
			}

			FPropertyValueSnapshotPayload Normalized;
			if (!Draft.Capture(Normalized, OutError)) return false;
			if (Proposal.DeferredAction)
			{
				if (!OutDeferred)
					return Fail("The reflected-property caller cannot retain deferred validation.", OutError);
				OutDeferred->ProposedValue = std::move(Normalized);
				OutDeferred->Action = std::move(Proposal.DeferredAction);
				if (OutAppliedValue) *OutAppliedValue = std::move(Before);
				return true;
			}
			std::string ApplyError;
			if (!RestoreTargetValue(Target, Normalized, &ApplyError))
			{
				std::string RollbackError;
				const bool bRolledBack = RestoreTargetValue(Target, Before, &RollbackError);
				if (!bRolledBack)
					ApplyError += std::format(" Rollback also failed: {}", RollbackError);
				if (OutAppliedValue)
				{
					if (bRolledBack) *OutAppliedValue = Before;
					else CaptureTargetValue(Target, *OutAppliedValue, nullptr);
				}
				if (OutError) *OutError = ApplyError;
				return false;
			}

			FPropertyValueSnapshotPayload Applied;
			std::string CaptureError;
			if (!CaptureTargetValue(Target, Applied, &CaptureError))
			{
				std::string RollbackError;
				const bool bRolledBack = RestoreTargetValue(Target, Before, &RollbackError);
				if (!bRolledBack)
					CaptureError += std::format(" Rollback also failed: {}", RollbackError);
				if (OutAppliedValue)
				{
					if (bRolledBack) *OutAppliedValue = Before;
					else CaptureTargetValue(Target, *OutAppliedValue, nullptr);
				}
				if (OutError) *OutError = CaptureError;
				return false;
			}
			if (OutAppliedValue) *OutAppliedValue = std::move(Applied);
			return true;
		}

		auto ValidateTarget(const FPropertyEditTarget& Target, std::string* OutError) -> bool
		{
			if (!Target.Object) return Fail("The edit target has no owning object.", OutError);
			if (!Target.MemberProperty || !Target.LeafProperty
				|| !Target.SnapshotProperty || !Target.SnapshotContainer) return Fail("The edit target is incomplete.", OutError);
			if (Target.SnapshotArrayIndex >= Target.SnapshotProperty->GetArrayDim()) return Fail("The snapshot property array index is out of range.", OutError);
			if (Target.Path.empty() || Target.Path.front().Property != Target.MemberProperty || Target.Path.back().Property != Target.LeafProperty)
			{
				return Fail("The property path must run from the member property to the leaf property.", OutError);
			}
			for (const FPropertyEditPathSegment& Segment : Target.Path)
			{
				if (!Segment.Property) return Fail("The property path contains an empty segment.", OutError);
				if (Segment.Selector != EPropertyPathSelector::MapKey && !Segment.MapKeyData.empty())
				{
					return Fail("Only map-key path segments may contain serialized key data.", OutError);
				}
			}
			return true;
		}

		// Identifies the container mutation applied while resolving an edit path.
		enum class EMutationOperation : uint8
		{
			Apply,
			NotifyOnly,
		};

		// Reports whether a container mutation changed storage and its resulting index.
		struct FMutationExecutionResult
		{
			FPropertyValueSnapshotPayload AppliedValue;
			FDeferredMutation Deferred;
			bool bSucceeded = false;
			bool bChanged = false;
			bool bDeferred = false;
		};

		auto NotifyMutation(
			const FPropertyEditTarget& Target,
			EPropertyChangePhase Phase,
			EPropertyChangeOrigin Origin
		) -> void
		{
			std::vector<FPropertyPathSegment> EventPath = MakeEventPath(Target);
			Target.Object->PostEditChangeProperty({
				Target.MemberProperty,
				Target.LeafProperty,
				EventPath,
				Phase,
				Target.Kind,
				Origin
			});
			const FPropertyChangedEvent Event{
				Target.MemberProperty,
				Target.LeafProperty,
				std::move(EventPath),
				Phase,
				Target.Kind,
				Origin};
			for (const FPropertyEditExtension& Extension : SnapshotPropertyEditExtensions())
				if (Extension.PostEdit) Extension.PostEdit(*Target.Object, Event);
		}

		auto ApplyDeferredMutation(
			const FPropertyEditTarget& Target,
			const FPropertyValueSnapshotPayload& ProposedValue,
			EPropertyChangePhase Phase,
			EPropertyChangeOrigin Origin,
			std::string* OutError) -> FMutationExecutionResult
		{
			FMutationExecutionResult Result;
			if (!RestoreTargetValue(Target, ProposedValue, OutError)) return Result;
			if (!CaptureTargetValue(Target, Result.AppliedValue, OutError)) return Result;
			Result.bSucceeded = true;
			Result.bChanged = true;
			NotifyMutation(Target, Phase, Origin);
			return Result;
		}

		auto ExecuteMutation(
			const FPropertyEditTarget& Target,
			const FPropertyValueSnapshotPayload* Value,
			const FPropertyValueSnapshotPayload* PreviousValue,
			EMutationOperation Operation,
			EPropertyChangePhase Phase,
			EPropertyChangeOrigin Origin,
			std::string* OutError
		) -> FMutationExecutionResult
		{
			FMutationExecutionResult Result;
			if (Operation == EMutationOperation::NotifyOnly)
			{
				NotifyMutation(Target, Phase, Origin);
				Result.bSucceeded = true;
				return Result;
			}
			if (!Value)
			{
				Fail("The reflected-property mutation value is unavailable.", OutError);
				return Result;
			}

			if (!ApplyGenericMutation(
				Target, *Value, Phase, Origin, &Result.AppliedValue, &Result.Deferred, OutError))
				return Result;

			Result.bSucceeded = true;
			if (Result.Deferred.Action)
			{
				Result.bDeferred = true;
				return Result;
			}
			Result.bChanged = !PreviousValue || !(Result.AppliedValue == *PreviousValue);
			if (Phase != EPropertyChangePhase::Interactive || Result.bChanged) NotifyMutation(Target, Phase, Origin);
			return Result;
		}
	}

	auto RegisterPropertyEditExtension(FPropertyEditExtension Extension)
		-> FPropertyEditExtensionHandle
	{
		if (!Extension.PreEdit && !Extension.PostEdit) return 0;
		std::lock_guard Lock(GPropertyEditExtensionsMutex);
		const FPropertyEditExtensionHandle Handle = GNextPropertyEditExtensionHandle++;
		GPropertyEditExtensions.emplace(Handle, std::move(Extension));
		return Handle;
	}

	auto UnregisterPropertyEditExtension(FPropertyEditExtensionHandle Handle) -> void
	{
		if (Handle == 0) return;
		std::lock_guard Lock(GPropertyEditExtensionsMutex);
		GPropertyEditExtensions.erase(Handle);
	}

	struct FPropertyEditSession::FDeferredOwnerState
	{
		std::mutex Mutex;
		FPropertyEditSession* Owner = nullptr;
	};

	auto ResolveReflectedPropertyValue(
		const FPropertyEditTarget& Target,
		FResolvedPropertyValue& OutValue,
		std::string* OutError
	) -> bool
	{
		if (!ValidateTarget(Target, OutError)) return false;
		if (Target.Path.front().Property != Target.SnapshotProperty)
			return Fail("The property path does not begin at the snapshot root.", OutError);

		void* Container = Target.SnapshotContainer;
		uint32 CurrentArrayIndex = Target.SnapshotArrayIndex;
		for (size_t PathIndex = 0; PathIndex < Target.Path.size(); ++PathIndex)
		{
			const FPropertyEditPathSegment& Segment = Target.Path[PathIndex];
			auto* CurrentProperty = const_cast<FProperty*>(Segment.Property);
			if (PathIndex + 1 == Target.Path.size())
			{
				OutValue = {CurrentProperty, Container, CurrentArrayIndex};
				return true;
			}

			FProperty* NextProperty = const_cast<FProperty*>(Target.Path[PathIndex + 1].Property);
			switch (Segment.Selector)
			{
			case EPropertyPathSelector::None:
			case EPropertyPathSelector::StaticArrayIndex:
				Container = CurrentProperty->GetValuePtr(Container, CurrentArrayIndex);
				CurrentArrayIndex = Target.Path[PathIndex + 1].Selector == EPropertyPathSelector::StaticArrayIndex
					? static_cast<uint32>(Target.Path[PathIndex + 1].Index) : 0;
				break;
			case EPropertyPathSelector::ArrayIndex:
			{
				auto* ArrayProperty = CurrentProperty->GetKind() == DurinCodeGen::EPropertyGenFlags::Array
					? static_cast<FArrayProperty*>(CurrentProperty) : nullptr;
				uint64 Num = 0;
				if (!ArrayProperty || ArrayProperty->GetNum(Container, Num, CurrentArrayIndex) != EContainerOpResult::Success
					|| Segment.Index >= Num)
					return Fail("The reflected array path index is unavailable.", OutError);
				void* Element = nullptr;
				if (ArrayProperty->GetMutableElement(Container, Segment.Index, &Element, CurrentArrayIndex)
					!= EContainerOpResult::Success)
					return Fail("The reflected array path requires mutable random access.", OutError);
				Container = Element;
				CurrentArrayIndex = 0;
				break;
			}
			case EPropertyPathSelector::MapKey:
			{
				auto* MapProperty = CurrentProperty->GetKind() == DurinCodeGen::EPropertyGenFlags::Map
					? static_cast<FMapProperty*>(CurrentProperty) : nullptr;
				if (!MapProperty || !Segment.MapKey.IsValid())
					return Fail("The reflected map path lacks a stable key snapshot.", OutError);
				FResolveMapEntryContext ResolveContext{MapProperty->GetKeyProp(), &Segment.MapKey};
				const EContainerOpResult VisitResult = MapProperty->VisitMutableEntries(
					Container, &ResolveMapEntry, &ResolveContext, CurrentArrayIndex);
				if (VisitResult != EContainerOpResult::Success)
					return Fail("The reflected map path requires mutable mapped traversal.", OutError);
				if (!ResolveContext.Error.empty()) return Fail(ResolveContext.Error, OutError);
				if (!ResolveContext.Key) return Fail("The reflected map key is unavailable.", OutError);
				if (NextProperty == MapProperty->GetKeyProp())
					Container = const_cast<void*>(ResolveContext.Key);
				else if (NextProperty == MapProperty->GetValueProp())
					Container = ResolveContext.Value;
				else
					return Fail("The reflected map path does not select its key or value property.", OutError);
				CurrentArrayIndex = 0;
				break;
			}
			default:
				return Fail("The reflected property path selector is unsupported.", OutError);
			}
			if (!Container || !NextProperty) return Fail("The reflected property path could not be resolved.", OutError);
		}
		return Fail("The reflected property path is empty.", OutError);
	}

	auto FPropertyEditTarget::ForMember(DObject* Object, const FProperty* Property, uint32 ArrayIndex) -> FPropertyEditTarget
	{
		FPropertyEditTarget Target;
		Target.Object = Object;
		Target.MemberProperty = Property;
		Target.LeafProperty = Property;
		Target.SnapshotProperty = Property;
		Target.SnapshotContainer = Object;
		Target.SnapshotArrayIndex = ArrayIndex;
		Target.Path.push_back({
			Property,
			Property && Property->GetArrayDim() > 1 ? EPropertyPathSelector::StaticArrayIndex : EPropertyPathSelector::None,
			ArrayIndex
		});
		return Target;
	}

	auto FPropertyEditTarget::ForStructMember(const FProperty* Property, uint32 ArrayIndex) const -> FPropertyEditTarget
	{
		FPropertyEditTarget Target = *this;
		Target.LeafProperty = Property;
		Target.Path.push_back({
			Property,
			Property && Property->GetArrayDim() > 1 ? EPropertyPathSelector::StaticArrayIndex : EPropertyPathSelector::None,
			ArrayIndex
		});
		Target.Kind = EPropertyChangeKind::ValueSet;
		return Target;
	}

	auto FPropertyEditTarget::ForArrayElement(const FProperty* ElementProperty, uint64 ElementIndex) const -> FPropertyEditTarget
	{
		FPropertyEditTarget Target = *this;
		Target.LeafProperty = ElementProperty;
		if (!Target.Path.empty())
		{
			Target.Path.back().Selector = EPropertyPathSelector::ArrayIndex;
			Target.Path.back().Index = ElementIndex;
		}
		Target.Path.push_back({ElementProperty});
		Target.Kind = EPropertyChangeKind::ValueSet;
		return Target;
	}

	auto FPropertyEditTarget::ForMapEntry(const FProperty* EntryProperty, std::vector<std::byte> SerializedKey) const -> FPropertyEditTarget
	{
		return ForMapEntry(EntryProperty, FPropertyValueSnapshotPayload{}, std::move(SerializedKey));
	}

	auto FPropertyEditTarget::ForMapEntry(const FProperty* EntryProperty,
		FPropertyValueSnapshotPayload KeySnapshot, std::vector<std::byte> SerializedKey) const -> FPropertyEditTarget
	{
		FPropertyEditTarget Target = *this;
		Target.LeafProperty = EntryProperty;
		if (!Target.Path.empty())
		{
			Target.Path.back().Selector = EPropertyPathSelector::MapKey;
			Target.Path.back().MapKeyData = std::move(SerializedKey);
			Target.Path.back().MapKey = std::move(KeySnapshot);
		}
		Target.Path.push_back({EntryProperty});
		Target.Kind = EPropertyChangeKind::ValueSet;
		return Target;
	}

	auto FPropertyEditTarget::ForMapEntry(
		const FProperty* EntryProperty,
		const FPropertyValueSnapshot& KeySnapshot,
		std::vector<std::byte> SerializedKey) const -> FPropertyEditTarget
	{
		return ForMapEntry(
			EntryProperty, KeySnapshot.GetPayload(), std::move(SerializedKey));
	}

	auto FPropertyEditTarget::IsSameMutationTarget(const FPropertyEditTarget& Other) const -> bool
	{
		if (Object != Other.Object || MemberProperty != Other.MemberProperty
			|| LeafProperty != Other.LeafProperty || SnapshotProperty != Other.SnapshotProperty
			|| SnapshotContainer != Other.SnapshotContainer || SnapshotArrayIndex != Other.SnapshotArrayIndex
			|| LogicalIdentity != Other.LogicalIdentity || Path.size() != Other.Path.size()) return false;
		for (size_t Index = 0; Index < Path.size(); ++Index)
		{
			const FPropertyEditPathSegment& Left = Path[Index];
			const FPropertyEditPathSegment& Right = Other.Path[Index];
			if (Left.Property != Right.Property || Left.Selector != Right.Selector || Left.Index != Right.Index
				|| Left.MapKeyData != Right.MapKeyData || Left.MapKey != Right.MapKey) return false;
		}
		return true;
	}

	auto FPropertyEditTarget::IsSameStableTarget(const FPropertyEditTarget& Other) const -> bool
	{
		if (Object != Other.Object || MemberProperty != Other.MemberProperty
			|| LeafProperty != Other.LeafProperty || SnapshotProperty != Other.SnapshotProperty
			|| SnapshotArrayIndex != Other.SnapshotArrayIndex
			|| LogicalIdentity != Other.LogicalIdentity || Path.size() != Other.Path.size()) return false;
		for (size_t Index = 0; Index < Path.size(); ++Index)
		{
			const FPropertyEditPathSegment& Left = Path[Index];
			const FPropertyEditPathSegment& Right = Other.Path[Index];
			if (Left.Property != Right.Property || Left.Selector != Right.Selector || Left.Index != Right.Index
				|| Left.MapKeyData != Right.MapKeyData || Left.MapKey != Right.MapKey) return false;
		}
		return true;
	}

	auto FPropertyEditTarget::MatchesContinuousEdit(const FPropertyEditTarget& Other) const -> bool
	{
		if (Object != Other.Object || MemberProperty != Other.MemberProperty
			|| LeafProperty != Other.LeafProperty || SnapshotProperty != Other.SnapshotProperty
			|| SnapshotContainer != Other.SnapshotContainer || SnapshotArrayIndex != Other.SnapshotArrayIndex
			|| Kind != Other.Kind || LogicalIdentity != Other.LogicalIdentity
			|| Path.size() != Other.Path.size()) return false;
		for (size_t Index = 0; Index < Path.size(); ++Index)
		{
			const FPropertyEditPathSegment& Left = Path[Index];
			const FPropertyEditPathSegment& Right = Other.Path[Index];
			if (Left.Property != Right.Property || Left.Selector != Right.Selector || Left.Index != Right.Index) return false;
			const bool bContinuousKeyRename = Kind == EPropertyChangeKind::MapKeyRename
				&& Left.Selector == EPropertyPathSelector::MapKey;
			if (!bContinuousKeyRename && (Left.MapKeyData != Right.MapKeyData || Left.MapKey != Right.MapKey)) return false;
		}
		return true;
	}

	auto FTransactionPropertyPathSegment::TryGetAllocatedSize(size_t& OutBytes) const -> bool
	{
		size_t PayloadBytes = 0;
		if (!MapKey.TryGetAllocatedSize(PayloadBytes)
			|| MapKeyData.capacity() > std::numeric_limits<size_t>::max() - PayloadBytes)
			return false;
		OutBytes = MapKeyData.capacity() + PayloadBytes;
		return true;
	}

	auto FTransactionObjectRecord::Capture(
		const FPropertyEditTarget& InTarget,
		FPropertyValueSnapshotPayload InBefore,
		FPropertyValueSnapshotPayload InAfter,
		FTransactionObjectRecord& OutRecord,
		std::string* OutError) -> bool
	{
		if (!ValidateTarget(InTarget, OutError)) return false;
		if (InTarget.SnapshotContainer != InTarget.Object
			|| InTarget.SnapshotProperty != InTarget.MemberProperty)
			return Fail("Transaction object records require an object-owned top-level snapshot member.", OutError);
		if (!InBefore.IsValid() || !InAfter.IsValid()
			|| !ArePropertySnapshotTypesCompatible(InBefore.GetProperty(), InTarget.SnapshotProperty)
			|| !ArePropertySnapshotTypesCompatible(InAfter.GetProperty(), InTarget.SnapshotProperty))
			return Fail("Transaction object record payloads do not match the snapshot member.", OutError);

		FTransactionObjectRecord Record;
		Record.Target = FPersistentObjectRef(InTarget.Object);
		if (!FTransactionMemberLocator::Capture(
			InTarget.SnapshotProperty, InTarget.SnapshotArrayIndex,
			Record.SnapshotMember, OutError)) return false;
		Record.LeafProperty = InTarget.LeafProperty;
		Record.Path.reserve(InTarget.Path.size());
		for (const FPropertyEditPathSegment& Segment : InTarget.Path)
		{
			Record.Path.push_back({Segment.Property, Segment.Selector, Segment.Index,
				Segment.MapKeyData, Segment.MapKey});
		}
		Record.LogicalIdentity = InTarget.LogicalIdentity;
		Record.Kind = InTarget.Kind;
		Record.Before = std::move(InBefore);
		Record.After = std::move(InAfter);
		OutRecord = std::move(Record);
		return true;
	}

	auto FTransactionObjectRecord::BuildTarget(
		FPropertyEditTarget& OutTarget,
		std::string* OutError) const -> bool
	{
		DObject* Object = Target.Resolve();
		FProperty* Member = SnapshotMember.Resolve(Object, OutError);
		if (!Member || Path.empty() || Path.front().Property != Member
			|| Path.back().Property != LeafProperty)
			return Fail("Transaction property path no longer matches its reflected member.", OutError);
		FPropertyEditTarget Result;
		Result.Object = Object;
		Result.MemberProperty = Member;
		Result.LeafProperty = LeafProperty;
		Result.SnapshotProperty = Member;
		Result.SnapshotContainer = Object;
		Result.SnapshotArrayIndex = SnapshotMember.GetArrayIndex();
		Result.Path.reserve(Path.size());
		for (const FTransactionPropertyPathSegment& Segment : Path)
		{
			Result.Path.push_back({Segment.Property, Segment.Selector, Segment.Index,
				Segment.MapKeyData, Segment.MapKey});
		}
		Result.LogicalIdentity = LogicalIdentity;
		Result.Kind = Kind;
		if (!ValidateTarget(Result, OutError)) return false;
		OutTarget = std::move(Result);
		return true;
	}

	auto FTransactionObjectRecord::Validate(std::string* OutError) const -> bool
	{
		FPropertyEditTarget TargetValue;
		if (!BuildTarget(TargetValue, OutError)) return false;
		for (const FPropertyValueSnapshotPayload* Payload : {&Before, &After})
		{
			FPropertyValueDraft Draft(TargetValue, OutError);
			if (!Draft.IsValid() || !Draft.Restore(*Payload, OutError)) return false;
			const FProperty* ResolvedProperty = nullptr;
			void* ResolvedContainer = nullptr;
			uint32 ResolvedArrayIndex = 0;
			if (!Draft.Resolve(TargetValue, ResolvedProperty, ResolvedContainer,
				ResolvedArrayIndex, OutError)) return false;
		}
		return true;
	}

	auto FTransactionObjectRecord::Apply(
		bool bBefore,
		EPropertyChangeOrigin Origin,
		std::string* OutError) const -> bool
	{
		FPropertyEditTarget TargetValue;
		if (!BuildTarget(TargetValue, OutError)) return false;
		const FPropertyValueSnapshotPayload& Value = bBefore ? Before : After;
		const FMutationExecutionResult Result = ExecuteMutation(
			TargetValue, &Value, nullptr, EMutationOperation::Apply,
			EPropertyChangePhase::Committed, Origin, OutError);
		if (!Result.bSucceeded) return false;
		if (Result.bDeferred)
			return Fail("Deferred property validation is unavailable during P2 history restore.", OutError);
		TargetValue.Object->MarkPackageDirty();
		return true;
	}

	auto FTransactionObjectRecord::AddReferencedObjects(FReferenceCollector& Collector) const -> void
	{
		Target.AddReferencedObjects(Collector);
		auto AddPayload = [&](const FPropertyValueSnapshotPayload& Payload) {
			for (const FObjectHandle Handle : Payload.GetReferencedObjectHandles())
				FPersistentObjectRef::FromHandle(Handle).AddReferencedObjects(Collector);
		};
		AddPayload(Before);
		AddPayload(After);
		for (const FTransactionPropertyPathSegment& Segment : Path)
			AddPayload(Segment.MapKey);
	}

	auto FTransactionObjectRecord::TryGetAllocatedSize(size_t& OutBytes) const -> bool
	{
		size_t Total = 0;
		auto Add = [&](size_t Value) {
			if (Total > std::numeric_limits<size_t>::max() - Value) return false;
			Total += Value;
			return true;
		};
		if (Path.capacity() > std::numeric_limits<size_t>::max()
			/ sizeof(FTransactionPropertyPathSegment)
			|| !Add(Path.capacity() * sizeof(FTransactionPropertyPathSegment))
			|| !Add(LogicalIdentity.capacity())) return false;
		for (const FTransactionPropertyPathSegment& Segment : Path)
		{
			size_t SegmentBytes = 0;
			if (!Segment.TryGetAllocatedSize(SegmentBytes) || !Add(SegmentBytes)) return false;
		}
		for (const FPropertyValueSnapshotPayload* Payload : {&Before, &After})
		{
			size_t PayloadBytes = 0;
			if (!Payload->TryGetAllocatedSize(PayloadBytes) || !Add(PayloadBytes)) return false;
		}
		OutBytes = Total;
		return true;
	}

	FPropertyEditSession::~FPropertyEditSession()
	{
		// An applied preview must never be abandoned merely because its UI owner is
		// destroyed. Explicit Commit/Cancel remains preferable because it can surface errors.
		if (bActive)
		{
			std::string Error;
			if (Cancel(&Error) == EPropertyEditResult::Failed)
			{
				DURIN_FATAL("Unable to restore an unfinished reflected-property preview: {}", Error);
				check(false);
			}
		}
		Reset();
	}

	auto FPropertyEditSession::Begin(
		const FPropertyEditTarget& InTarget,
		std::string_view InDescription,
		std::string* OutError,
		DTransactor* InTransactor
	) -> bool
	{
		if (bActive) return Fail("A reflected-property edit session is already active.", OutError);
		Target = InTarget;
		if (!ValidateTarget(Target, OutError))
		{
			Reset();
			return false;
		}
		TargetObject = TStrongObjectPtr<DObject>(MakeObjectHandle(InTarget.Object));
		if (!TargetObject)
		{
			Reset();
			return Fail("The reflected-property edit target is no longer live.", OutError);
		}
		Target.Object = TargetObject.Get();
		Transactor = InTransactor;
		Description = InDescription.empty()
			? std::format("Edit {}", Target.MemberProperty->NamePrivate.ToString())
			: InDescription;
		if (!CaptureTargetValue(Target, OriginalValue, OutError))
		{
			Reset();
			return false;
		}
		CurrentValue = OriginalValue;
		if (Transactor)
		{
			TransactionScope.emplace(Transactor, FTransactionContext{
				.Name = "ReflectedProperty",
				.Description = Description,
				.PrimaryObject = FPersistentObjectRef(Target.Object),
			});
			if (!TransactionScope->IsActive())
			{
				Reset();
				return Fail("The reflected-property transactor rejected the edit scope.", OutError);
			}
			FTransactionObjectRecord Record;
			if (!FTransactionObjectRecord::Capture(
				Target, OriginalValue, CurrentValue, Record, OutError))
			{
				(void)TransactionScope->Cancel();
				Reset();
				return false;
			}
			const FTransactorResult RecordResult = Transactor->Record(std::move(Record));
			if (!RecordResult)
			{
				(void)TransactionScope->Cancel();
				Reset();
				return Fail(RecordResult.Message.empty()
					? "The reflected-property transactor rejected the initial record."
					: RecordResult.Message, OutError);
			}
			TransactionRecordId = RecordResult.RecordId;
		}
		bActive = true;
		return true;
	}

	auto FPropertyEditSession::Apply(
		const FPropertyValueSnapshot& ProposedValue,
		std::string* OutError) -> EPropertyEditResult
	{
		return Apply(ProposedValue.GetPayload(), OutError);
	}

	auto FPropertyEditSession::Apply(const FPropertyValueSnapshotPayload& ProposedValue, std::string* OutError) -> EPropertyEditResult
	{
		if (!bActive) { Fail("No reflected-property edit session is active.", OutError); return EPropertyEditResult::Failed; }
		if (bDeferredPending)
		{
			if (DeferredOwnerState)
			{
				std::lock_guard Lock(DeferredOwnerState->Mutex);
				DeferredOwnerState->Owner = nullptr;
			}
			if (CancelDeferredEdit) CancelDeferredEdit();
			CancelDeferredEdit = {};
			DeferredOwnerState.reset();
			bDeferredPending = false;
		}
		if (ProposedValue == CurrentValue) return EPropertyEditResult::NoChange;
		FMutationExecutionResult Result = ExecuteMutation(
			Target, &ProposedValue, &CurrentValue, EMutationOperation::Apply,
			EPropertyChangePhase::Interactive, EPropertyChangeOrigin::Edit, OutError);
		if (!Result.bSucceeded)
		{
			if (Result.AppliedValue.IsValid()) CurrentValue = std::move(Result.AppliedValue);
			return EPropertyEditResult::Failed;
		}
		if (Result.bDeferred)
		{
			bDeferredPending = true;
			DeferredOwnerState = std::make_shared<FDeferredOwnerState>();
			DeferredOwnerState->Owner = this;
			const std::shared_ptr<FDeferredOwnerState> OwnerState = DeferredOwnerState;
			FPropertyValueSnapshotPayload DeferredValue = std::move(Result.Deferred.ProposedValue);
			FPropertyEditDeferredCancel Cancel = Result.Deferred.Action(
				[OwnerState, DeferredValue = std::move(DeferredValue)](
					bool bSucceeded, std::string Error) mutable {
					FPropertyEditSession* Owner = nullptr;
					{
						std::lock_guard Lock(OwnerState->Mutex);
						Owner = OwnerState->Owner;
					}
					if (Owner)
						Owner->CompleteDeferredEdit(
							bSucceeded, std::move(Error), std::move(DeferredValue));
				});
			if (bDeferredPending && DeferredOwnerState == OwnerState)
				CancelDeferredEdit = std::move(Cancel);
			return EPropertyEditResult::Pending;
		}
		const FPropertyValueSnapshotPayload PreviousValue = CurrentValue;
		CurrentValue = std::move(Result.AppliedValue);
		if (!UpdateTransactorRecord(OutError))
		{
			FMutationExecutionResult Rollback = ExecuteMutation(
				Target, &PreviousValue, nullptr, EMutationOperation::Apply,
				EPropertyChangePhase::Interactive, EPropertyChangeOrigin::Edit, nullptr);
			if (Rollback.bSucceeded && !Rollback.bDeferred)
				CurrentValue = std::move(Rollback.AppliedValue);
			return EPropertyEditResult::Failed;
		}
		return Result.bChanged ? EPropertyEditResult::Changed : EPropertyEditResult::NoChange;
	}

	auto FPropertyEditSession::CompleteDeferredEdit(
		bool bSucceeded,
		std::string Error,
		FPropertyValueSnapshotPayload ProposedValue) -> void
	{
		if (!bActive || !bDeferredPending) return;
		if (DeferredOwnerState)
		{
			std::lock_guard Lock(DeferredOwnerState->Mutex);
			DeferredOwnerState->Owner = nullptr;
		}
		CancelDeferredEdit = {};
		DeferredOwnerState.reset();
		bDeferredPending = false;
		if (!bSucceeded)
		{
			if (!Error.empty()) DURIN_ERROR("Deferred reflected-property edit failed: {}", Error);
			Reset();
			return;
		}
		FMutationExecutionResult Result = ApplyDeferredMutation(
			Target,
			ProposedValue,
			EPropertyChangePhase::Interactive,
			EPropertyChangeOrigin::Edit,
			&Error);
		if (!Result.bSucceeded)
		{
			DURIN_ERROR("Deferred reflected-property publication failed: {}", Error);
			Reset();
			return;
		}
		CurrentValue = std::move(Result.AppliedValue);
		if (!UpdateTransactorRecord(&Error))
		{
			DURIN_ERROR("Deferred reflected-property record update failed: {}", Error);
			Reset();
			return;
		}
		if (Commit(&Error) == EPropertyEditResult::Failed)
			DURIN_ERROR("Deferred reflected-property transaction failed: {}", Error);
	}

	auto FPropertyEditSession::MatchesTarget(const FPropertyEditTarget& Other) const -> bool
	{
		return bActive && Target.MatchesContinuousEdit(Other);
	}

	auto FPropertyEditSession::UpdateTransactorRecord(std::string* OutError) -> bool
	{
		if (!Transactor || !TransactionScope || !TransactionScope->IsActive()) return true;
		FTransactionObjectRecord Record;
		if (!FTransactionObjectRecord::Capture(
			Target, OriginalValue, CurrentValue, Record, OutError)) return false;
		const FTransactorResult Result =
			Transactor->UpdateRecord(TransactionRecordId, std::move(Record));
		if (Result) return true;
		return Fail(Result.Message.empty()
			? "The reflected-property transactor rejected the record update."
			: Result.Message, OutError);
	}

	auto FPropertyEditSession::Commit(std::string* OutError) -> EPropertyEditResult
	{
		if (!bActive) { Fail("No reflected-property edit session is active.", OutError); return EPropertyEditResult::Failed; }
		const bool bChanged = HasChanges();
		if (!ExecuteMutation(Target, nullptr, nullptr, EMutationOperation::NotifyOnly,
			EPropertyChangePhase::Committed, EPropertyChangeOrigin::Edit, OutError).bSucceeded)
			return EPropertyEditResult::Failed;
		if (bChanged)
		{
			Target.Object->MarkPackageDirty();
			FTransactorResult TransactorResult{
				.Code = ETransactorResultCode::NoOp};
			if (TransactionScope && TransactionScope->IsActive())
			{
				if (!UpdateTransactorRecord(OutError)) return EPropertyEditResult::Failed;
				TransactorResult = TransactionScope->End();
				if (TransactorResult.Code == ETransactorResultCode::Rejected
					|| TransactorResult.Code == ETransactorResultCode::Failed)
				{
					if (OutError) *OutError = TransactorResult.Message;
					return EPropertyEditResult::Failed;
				}
			}
		}
		else if (TransactionScope && TransactionScope->IsActive())
		{
			const FTransactorResult Result = TransactionScope->Cancel();
			if (Result.Code == ETransactorResultCode::Rejected
				|| Result.Code == ETransactorResultCode::Failed)
			{
				if (OutError) *OutError = Result.Message;
				return EPropertyEditResult::Failed;
			}
		}
		Reset();
		return bChanged ? EPropertyEditResult::Changed : EPropertyEditResult::NoChange;
	}

	auto FPropertyEditSession::Cancel(std::string* OutError) -> EPropertyEditResult
	{
		if (!bActive) { Fail("No reflected-property edit session is active.", OutError); return EPropertyEditResult::Failed; }
		const bool bChanged = HasChanges();
		FMutationExecutionResult Result = ExecuteMutation(
			Target, bChanged ? &OriginalValue : nullptr, nullptr,
			bChanged ? EMutationOperation::Apply : EMutationOperation::NotifyOnly,
			EPropertyChangePhase::Cancelled, EPropertyChangeOrigin::Edit, OutError);
		if (!Result.bSucceeded)
		{
			if (Result.AppliedValue.IsValid()) CurrentValue = std::move(Result.AppliedValue);
			return EPropertyEditResult::Failed;
		}
		if (TransactionScope && TransactionScope->IsActive())
		{
			const FTransactorResult CancelResult = TransactionScope->Cancel();
			if (CancelResult.Code == ETransactorResultCode::Rejected
				|| CancelResult.Code == ETransactorResultCode::Failed)
			{
				if (OutError) *OutError = CancelResult.Message;
				return EPropertyEditResult::Failed;
			}
		}
		Reset();
		return bChanged ? EPropertyEditResult::Changed : EPropertyEditResult::NoChange;
	}

	auto FPropertyEditSession::Reset() -> void
	{
		if (DeferredOwnerState)
		{
			std::lock_guard Lock(DeferredOwnerState->Mutex);
			DeferredOwnerState->Owner = nullptr;
		}
		if (CancelDeferredEdit) CancelDeferredEdit();
		CancelDeferredEdit = {};
		DeferredOwnerState.reset();
		bDeferredPending = false;
		if (TransactionScope && TransactionScope->IsActive())
			(void)TransactionScope->Cancel();
		TransactionScope.reset();
		TransactionRecordId = 0;
		bActive = false;
		Target = {};
		TargetObject.Reset();
		OriginalValue = {};
		CurrentValue = {};
		Description.clear();
		Transactor = nullptr;
	}
}
