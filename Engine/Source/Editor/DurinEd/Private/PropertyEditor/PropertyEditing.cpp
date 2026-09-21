#include "PropertyEditor/PropertyEditing.h"

#include "PropertyEditor/PropertyValueDraft.h"

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
			std::optional<FPropertySnapshotError> Error;
		};

		auto ResolveMapEntry(void* RawContext, const void* Key, void* Value) -> bool
		{
			auto& Context = *static_cast<FResolveMapEntryContext*>(RawContext);
			FPropertyValueSnapshotPayload StoredKey;
			if (const auto Result = CapturePropertyValuePayload(Context.KeyProperty, Key, 0, StoredKey); !Result)
			{
				Context.Error = Result.error();
				return false;
			}
			if (StoredKey == *Context.TargetKey)
			{
				Context.Key = Key;
				Context.Value = Value;
				return false;
			}
			return true;
		}

		auto CaptureTargetValue(const FPropertyEditTarget& Target,
			FPropertyValueSnapshotPayload& OutSnapshot) -> std::expected<void, FPropertySnapshotError>
		{
			return CapturePropertyValuePayload(Target.SnapshotProperty, Target.SnapshotContainer,
				Target.SnapshotArrayIndex, OutSnapshot);
		}

		auto RestoreTargetValue(const FPropertyEditTarget& Target,
			const FPropertyValueSnapshotPayload& Snapshot) -> std::expected<void, FPropertySnapshotError>
		{
			return RestorePropertyValuePayload(Target.SnapshotProperty, Target.SnapshotContainer,
				Target.SnapshotArrayIndex, Snapshot);
		}

		auto RejectMutation(const FPropertyEditTarget& Target, EPropertyChangePhase Phase,
			EPropertyChangeOrigin Origin, EPropertyMutationError Code) -> FPropertyMutationResult
		{
			return {{.Code = Code, .Owner = FObjectKey(Target.Object),
				.Member = Target.MemberProperty ? Target.MemberProperty->NamePrivate.ToString() : std::string{},
				.Phase = Phase, .Origin = Origin, .Kind = Target.Kind}};
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
			FDeferredMutation* OutDeferred
		) -> FPropertyMutationResult
		{
			auto Reject = [&](EPropertyMutationError Code) { return RejectMutation(Target, Phase, Origin, Code); };
			auto RejectDraft = [&](const FPropertyValueDraftError& Cause) {
				auto Result = Reject(EPropertyMutationError::Draft);
				Result.Error.DraftCause = Cause;
				return Result;
			};
			if (std::ranges::any_of(GActiveGenericMutations, [&](const auto* Active) { return Active->IsSameMutationTarget(Target); }))
				return Reject(EPropertyMutationError::RecursiveEdit);
			FGenericMutationScope Scope(Target);

			FPropertyValueSnapshotPayload Before;
			if (const auto Capture = CaptureTargetValue(Target, Before); !Capture)
			{
				auto Result = Reject(EPropertyMutationError::CaptureBefore);
				Result.Error.SnapshotCause = Capture.error();
				return Result;
			}
			FPropertyValueDraft Draft(Target);
			if (const auto Result = Draft.Restore(ProposedValue); !Result)
				return RejectDraft(Result.Error);

			FResolvedPropertyValue DraftLeaf;
			const auto LeafResult = Draft.Resolve(Target, DraftLeaf.Property, DraftLeaf.Container, DraftLeaf.ArrayIndex);
			const bool bResolvedLeaf = static_cast<bool>(LeafResult);
			if (!bResolvedLeaf && Target.Kind != EPropertyChangeKind::MapKeyRename
				&& Target.Kind != EPropertyChangeKind::MapRemove)
				return RejectDraft(LeafResult.Error);

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
			for (const FPropertyEditExtension& Extension : SnapshotPropertyEditExtensions())
			{
				if (!Extension.PreEdit) continue;
				if (const auto Validation = Extension.PreEdit(*Target.Object, Proposal); !Validation)
				{
					auto Result = Reject(EPropertyMutationError::ExtensionValidation);
					Result.Error.ValidationCause = Validation.error();
					return Result;
				}
			}
			if (const auto Validation = Target.Object->PreEditChangeProperty(Proposal); !Validation)
			{
				auto Result = Reject(EPropertyMutationError::ObjectValidation);
				Result.Error.ValidationCause = Validation.error();
				return Result;
			}

			FPropertyValueSnapshotPayload Normalized;
			if (const auto Result = Draft.Capture(Normalized); !Result)
				return RejectDraft(Result.Error);
			if (Proposal.DeferredAction)
			{
				if (!OutDeferred)
					return Reject(EPropertyMutationError::DeferredUnavailable);
				OutDeferred->ProposedValue = std::move(Normalized);
				OutDeferred->Action = std::move(Proposal.DeferredAction);
				if (OutAppliedValue) *OutAppliedValue = std::move(Before);
				return {};
			}
			auto Recover = [&](EPropertyMutationError Code, const FPropertySnapshotError& Cause) {
				auto Result = Reject(Code);
				Result.Error.SnapshotCause = Cause;
				const auto Rollback = RestoreTargetValue(Target, Before);
				if (!Rollback) Result.Error.RollbackCause = Rollback.error();
				if (OutAppliedValue)
				{
					if (Rollback) *OutAppliedValue = Before;
					else if (const auto Capture = CaptureTargetValue(Target, *OutAppliedValue); !Capture)
						Result.Error.RecoveryCaptureCause = Capture.error();
				}
				return Result;
			};
			if (const auto Publication = RestoreTargetValue(Target, Normalized); !Publication)
				return Recover(EPropertyMutationError::Publication, Publication.error());

			FPropertyValueSnapshotPayload Applied;
			if (const auto Capture = CaptureTargetValue(Target, Applied); !Capture)
				return Recover(EPropertyMutationError::CaptureAfter, Capture.error());
			if (OutAppliedValue) *OutAppliedValue = std::move(Applied);
			return {};
		}

		auto RejectPath(const FPropertyEditTarget& Target, EPropertyEditPathError Code,
			uint64 Index = 0, uint64 Actual = 0, uint64 Expected = 0,
			std::optional<EContainerOpResult> ContainerCause = {},
			std::optional<FPropertySnapshotError> SnapshotCause = {}) -> FPropertyEditPathResult
		{
			FPropertyEditPathError Error{.Code = Code, .Owner = FObjectKey(Target.Object),
				.Member = Target.MemberProperty ? Target.MemberProperty->NamePrivate.ToString() : std::string{},
				.Leaf = Target.LeafProperty ? Target.LeafProperty->NamePrivate.ToString() : std::string{},
				.Snapshot = Target.SnapshotProperty ? Target.SnapshotProperty->NamePrivate.ToString() : std::string{},
				.HasSnapshotContainer = Target.SnapshotContainer != nullptr, .SnapshotArrayIndex = Target.SnapshotArrayIndex,
				.PathIndex = Index, .Actual = Actual, .Expected = Expected,
				.ContainerCause = ContainerCause, .SnapshotCause = std::move(SnapshotCause)};
			for (const auto& Segment : Target.Path)
				Error.Path.push_back({Segment.Property ? Segment.Property->NamePrivate.ToString() : std::string{},
					Segment.Selector, Segment.Index, Segment.MapKeyData, Segment.MapKey});
			return {.Error = std::move(Error)};
		}

		auto ValidateTarget(const FPropertyEditTarget& Target) -> FPropertyEditPathResult
		{
			if (!Target.Object) return RejectPath(Target, EPropertyEditPathError::MissingOwner);
			if (!Target.MemberProperty || !Target.LeafProperty || !Target.SnapshotProperty || !Target.SnapshotContainer)
				return RejectPath(Target, EPropertyEditPathError::IncompleteTarget);
			if (Target.SnapshotArrayIndex >= Target.SnapshotProperty->GetArrayDim())
				return RejectPath(Target, EPropertyEditPathError::SnapshotIndex, 0,
					Target.SnapshotArrayIndex, Target.SnapshotProperty->GetArrayDim());
			if (Target.Path.empty() || Target.Path.front().Property != Target.MemberProperty || Target.Path.back().Property != Target.LeafProperty)
				return RejectPath(Target, EPropertyEditPathError::Endpoints);
			for (size_t Index = 0; Index < Target.Path.size(); ++Index)
			{
				const auto& Segment = Target.Path[Index];
				if (!Segment.Property) return RejectPath(Target, EPropertyEditPathError::EmptySegment, Index);
				if (Segment.Selector != EPropertyPathSelector::MapKey && !Segment.MapKeyData.empty())
					return RejectPath(Target, EPropertyEditPathError::UnexpectedKeyData, Index, Segment.MapKeyData.size());
			}
			return {};
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
			FPropertyMutationError Error;
			explicit operator bool() const { return Error.Code == EPropertyMutationError::None; }
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
			EPropertyChangeOrigin Origin) -> FMutationExecutionResult
		{
			FMutationExecutionResult Result;
			if (const auto Publication = RestoreTargetValue(Target, ProposedValue); !Publication)
			{
				Result.Error = RejectMutation(Target, Phase, Origin, EPropertyMutationError::Publication).Error;
				Result.Error.SnapshotCause = Publication.error();
				return Result;
			}
			if (const auto Capture = CaptureTargetValue(Target, Result.AppliedValue); !Capture)
			{
				Result.Error = RejectMutation(Target, Phase, Origin, EPropertyMutationError::CaptureAfter).Error;
				Result.Error.SnapshotCause = Capture.error();
				return Result;
			}
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
			EPropertyChangeOrigin Origin
		) -> FMutationExecutionResult
		{
			FMutationExecutionResult Result;
			if (Operation == EMutationOperation::NotifyOnly)
			{
				NotifyMutation(Target, Phase, Origin);
				return Result;
			}
			if (!Value)
			{
				Result.Error = RejectMutation(Target, Phase, Origin, EPropertyMutationError::MissingValue).Error;
				return Result;
			}

			if (const auto Mutation = ApplyGenericMutation(
				Target, *Value, Phase, Origin, &Result.AppliedValue, &Result.Deferred); !Mutation)
			{
				Result.Error = Mutation.Error;
				return Result;
			}

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

	auto FormatPropertyValueDraftError(const FPropertyValueDraftError& Error) -> std::string
	{
		if (Error.SnapshotCause) return ToString(*Error.SnapshotCause);
		if (Error.ValueCause) return ToString(*Error.ValueCause);
		if (Error.PathCause) return FormatPropertyEditPathError(*Error.PathCause);
		switch (Error.Code)
		{
		case EPropertyValueDraftError::None: return {};
		case EPropertyValueDraftError::MissingRoot: return "The reflected property draft root is unavailable.";
		case EPropertyValueDraftError::Accessors: return "Properties with custom value accessors cannot be used as draft roots.";
		case EPropertyValueDraftError::Lifecycle: return "The reflected property lacks generated draft-value lifecycle metadata.";
		case EPropertyValueDraftError::RootMismatch: return "The edit target does not match its reflected property draft root.";
		default: return "The reflected property draft operation failed.";
		}
	}

	auto FormatPropertyMutationError(const FPropertyMutationError& Error) -> std::string
	{
		std::string Message;
		if (Error.DraftCause) Message = FormatPropertyValueDraftError(*Error.DraftCause);
		else if (Error.SnapshotCause) Message = ToString(*Error.SnapshotCause);
		else if (Error.ValidationCause) Message = ToString(*Error.ValidationCause);
		else switch (Error.Code)
		{
		case EPropertyMutationError::None: break;
		case EPropertyMutationError::RecursiveEdit: Message = "A reflected property hook cannot start a nested edit of the same target."; break;
		case EPropertyMutationError::MissingValue: Message = "The reflected-property mutation value is unavailable."; break;
		case EPropertyMutationError::DeferredUnavailable: Message = "The reflected-property caller cannot retain deferred validation."; break;
		default: Message = "The reflected-property mutation failed."; break;
		}
		if (Error.RollbackCause) Message += std::format(" Rollback also failed: {}", ToString(*Error.RollbackCause));
		if (Error.RecoveryCaptureCause) Message += std::format(" Recovery capture also failed: {}", ToString(*Error.RecoveryCaptureCause));
		return Message;
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

	auto FPropertyEditTarget::Validate() const -> FPropertyEditPathResult { return ValidateTarget(*this); }

	auto FormatPropertyEditPathError(const FPropertyEditPathError& Error) -> std::string
	{
		if (Error.SnapshotCause) return ToString(*Error.SnapshotCause);
		switch (Error.Code)
		{
		case EPropertyEditPathError::None: return {};
		case EPropertyEditPathError::MissingOwner: return "The edit target has no owning object.";
		case EPropertyEditPathError::IncompleteTarget: return "The edit target is incomplete.";
		case EPropertyEditPathError::SnapshotIndex: return "The snapshot property array index is out of range.";
		case EPropertyEditPathError::Endpoints: return "The property path must run from the member property to the leaf property.";
		case EPropertyEditPathError::EmptySegment: return "The property path contains an empty segment.";
		case EPropertyEditPathError::UnexpectedKeyData: return "Only map-key path segments may contain serialized key data.";
		case EPropertyEditPathError::SnapshotRoot: return "The property path does not begin at the snapshot root.";
		case EPropertyEditPathError::ArrayProperty: return "The reflected array path property is not an array.";
		case EPropertyEditPathError::ArrayCount: return "The reflected array path count is unavailable.";
		case EPropertyEditPathError::ArrayIndex: return "The reflected array path index is unavailable.";
		case EPropertyEditPathError::ArrayAccess: return "The reflected array path requires mutable random access.";
		case EPropertyEditPathError::MapSnapshot: return "The reflected map path lacks a stable key snapshot.";
		case EPropertyEditPathError::MapTraversal: return "The reflected map path requires mutable mapped traversal.";
		case EPropertyEditPathError::MapCapture: return "The reflected map key snapshot could not be captured.";
		case EPropertyEditPathError::MapMissing: return "The reflected map key is unavailable.";
		case EPropertyEditPathError::MapSelection: return "The reflected map path does not select its key or value property.";
		case EPropertyEditPathError::Selector: return "The reflected property path selector is unsupported.";
		case EPropertyEditPathError::Unresolved: return "The reflected property path could not be resolved.";
		case EPropertyEditPathError::Empty: return "The reflected property path is empty.";
		}
		return "Unknown reflected property path failure.";
	}

	auto ResolveReflectedPropertyValue(
		const FPropertyEditTarget& Target,
		FResolvedPropertyValue& OutValue
	) -> FPropertyEditPathResult
	{
		if (const auto Valid = ValidateTarget(Target); !Valid) return Valid;
		if (Target.Path.front().Property != Target.SnapshotProperty)
			return RejectPath(Target, EPropertyEditPathError::SnapshotRoot);

		void* Container = Target.SnapshotContainer;
		uint32 CurrentArrayIndex = Target.SnapshotArrayIndex;
		for (size_t PathIndex = 0; PathIndex < Target.Path.size(); ++PathIndex)
		{
			const FPropertyEditPathSegment& Segment = Target.Path[PathIndex];
			auto* CurrentProperty = const_cast<FProperty*>(Segment.Property);
			if (PathIndex + 1 == Target.Path.size())
			{
				OutValue = {CurrentProperty, Container, CurrentArrayIndex};
				return {};
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
				if (!ArrayProperty) return RejectPath(Target, EPropertyEditPathError::ArrayProperty, PathIndex,
					static_cast<uint64>(CurrentProperty->GetKind()), static_cast<uint64>(DurinCodeGen::EPropertyGenFlags::Array));
				uint64 Num = 0;
				const auto Counted = ArrayProperty->GetNum(Container, Num, CurrentArrayIndex);
				if (Counted != EContainerOpResult::Success)
					return RejectPath(Target, EPropertyEditPathError::ArrayCount, PathIndex, 0, 0, Counted);
				if (Segment.Index >= Num)
					return RejectPath(Target, EPropertyEditPathError::ArrayIndex, PathIndex, Segment.Index, Num);
				void* Element = nullptr;
				const auto Accessed = ArrayProperty->GetMutableElement(Container, Segment.Index, &Element, CurrentArrayIndex);
				if (Accessed != EContainerOpResult::Success)
					return RejectPath(Target, EPropertyEditPathError::ArrayAccess, PathIndex, Segment.Index, Num, Accessed);
				Container = Element;
				CurrentArrayIndex = 0;
				break;
			}
			case EPropertyPathSelector::MapKey:
			{
				auto* MapProperty = CurrentProperty->GetKind() == DurinCodeGen::EPropertyGenFlags::Map
					? static_cast<FMapProperty*>(CurrentProperty) : nullptr;
				if (!MapProperty || !Segment.MapKey.IsValid())
					return RejectPath(Target, EPropertyEditPathError::MapSnapshot, PathIndex,
						static_cast<uint64>(CurrentProperty->GetKind()), static_cast<uint64>(DurinCodeGen::EPropertyGenFlags::Map));
				FResolveMapEntryContext ResolveContext{MapProperty->GetKeyProp(), &Segment.MapKey};
				const EContainerOpResult VisitResult = MapProperty->VisitMutableEntries(
					Container, &ResolveMapEntry, &ResolveContext, CurrentArrayIndex);
				if (VisitResult != EContainerOpResult::Success)
					return RejectPath(Target, EPropertyEditPathError::MapTraversal, PathIndex, 0, 0, VisitResult);
				if (ResolveContext.Error) return RejectPath(Target, EPropertyEditPathError::MapCapture, PathIndex, 0, 0, {}, std::move(ResolveContext.Error));
				if (!ResolveContext.Key) return RejectPath(Target, EPropertyEditPathError::MapMissing, PathIndex);
				if (NextProperty == MapProperty->GetKeyProp())
					Container = const_cast<void*>(ResolveContext.Key);
				else if (NextProperty == MapProperty->GetValueProp())
					Container = ResolveContext.Value;
				else
					return RejectPath(Target, EPropertyEditPathError::MapSelection, PathIndex);
				CurrentArrayIndex = 0;
				break;
			}
			default:
				return RejectPath(Target, EPropertyEditPathError::Selector, PathIndex);
			}
			if (!Container || !NextProperty) return RejectPath(Target, EPropertyEditPathError::Unresolved, PathIndex);
		}
		return RejectPath(Target, EPropertyEditPathError::Empty);
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

	auto FPropertyEditTarget::ForMapEntry(const FProperty* EntryProperty, FByteBuffer SerializedKey) const -> FPropertyEditTarget
	{
		return ForMapEntry(EntryProperty, FPropertyValueSnapshotPayload{}, std::move(SerializedKey));
	}

	auto FPropertyEditTarget::ForMapEntry(const FProperty* EntryProperty,
		FPropertyValueSnapshotPayload KeySnapshot, FByteBuffer SerializedKey) const -> FPropertyEditTarget
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
		FByteBuffer SerializedKey) const -> FPropertyEditTarget
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

	auto FormatTransactionObjectRecordError(const FTransactionObjectRecordError& Error) -> std::string
	{
		if (Error.MemberCause) return FormatTransactionSnapshotError(*Error.MemberCause);
		if (Error.PathCause) return FormatPropertyEditPathError(*Error.PathCause);
		if (Error.DraftCause) return FormatPropertyValueDraftError(*Error.DraftCause);
		if (Error.MutationCause) return FormatPropertyMutationError(*Error.MutationCause);
		switch (Error.Code)
		{
		case ETransactionObjectRecordError::None: return {};
		case ETransactionObjectRecordError::SnapshotRoot: return "Transaction object records require an object-owned top-level snapshot member.";
		case ETransactionObjectRecordError::Payload: return "Transaction object record payloads do not match the snapshot member.";
		case ETransactionObjectRecordError::Path: return "Transaction property path no longer matches its reflected member.";
		case ETransactionObjectRecordError::Deferred: return "Deferred property validation is unavailable during P2 history restore.";
		default: return "Transaction object record operation failed.";
		}
	}

	auto FTransactionObjectRecord::Reject(ETransactionObjectRecordError Code) const -> FTransactionObjectRecordResult
	{
		return {{.Code = Code, .Owner = Target.GetKey(),
			.Member = SnapshotMember.GetMemberName().ToString(),
			.Snapshot = SnapshotMember.GetMemberName().ToString(),
			.Leaf = LeafProperty ? LeafProperty->NamePrivate.ToString() : std::string{},
			.ArrayIndex = SnapshotMember.GetArrayIndex(), .PathLength = Path.size(),
			.PathFirst = !Path.empty() && Path.front().Property ? Path.front().Property->NamePrivate.ToString() : std::string{},
			.PathLast = !Path.empty() && Path.back().Property ? Path.back().Property->NamePrivate.ToString() : std::string{},
			.ObjectOwnedStorage = true,
			.BeforeValid = Before.IsValid(), .AfterValid = After.IsValid()}};
	}

	auto FTransactionObjectRecord::Capture(
		const FPropertyEditTarget& InTarget,
		FPropertyValueSnapshotPayload InBefore,
		FPropertyValueSnapshotPayload InAfter,
		FTransactionObjectRecord& OutRecord) -> FTransactionObjectRecordResult
	{
		FTransactionObjectRecordError Error{
			.Owner = FObjectKey(InTarget.Object),
			.Member = InTarget.MemberProperty ? InTarget.MemberProperty->NamePrivate.ToString() : std::string{},
			.Snapshot = InTarget.SnapshotProperty ? InTarget.SnapshotProperty->NamePrivate.ToString() : std::string{},
			.Leaf = InTarget.LeafProperty ? InTarget.LeafProperty->NamePrivate.ToString() : std::string{},
			.ArrayIndex = InTarget.SnapshotArrayIndex,
			.ObjectOwnedStorage = InTarget.SnapshotContainer == InTarget.Object,
			.BeforeValid = InBefore.IsValid(), .AfterValid = InAfter.IsValid(),
			.ExpectedKind = InTarget.SnapshotProperty ? InTarget.SnapshotProperty->GetKind() : DurinCodeGen::EPropertyGenFlags::None,
			.BeforeKind = InBefore.GetProperty() ? InBefore.GetProperty()->GetKind() : DurinCodeGen::EPropertyGenFlags::None,
			.AfterKind = InAfter.GetProperty() ? InAfter.GetProperty()->GetKind() : DurinCodeGen::EPropertyGenFlags::None};
		if (const auto Validation = ValidateTarget(InTarget); !Validation)
		{
			Error.Code = ETransactionObjectRecordError::Target;
			Error.PathCause = std::make_shared<FPropertyEditPathError>(Validation.Error);
			return {Error};
		}
		if (InTarget.SnapshotContainer != InTarget.Object
			|| InTarget.SnapshotProperty != InTarget.MemberProperty)
			{ Error.Code = ETransactionObjectRecordError::SnapshotRoot; return {Error}; }
		if (!InBefore.IsValid() || !InAfter.IsValid()
			|| !ArePropertySnapshotTypesCompatible(InBefore.GetProperty(), InTarget.SnapshotProperty)
			|| !ArePropertySnapshotTypesCompatible(InAfter.GetProperty(), InTarget.SnapshotProperty))
			{ Error.Code = ETransactionObjectRecordError::Payload; return {Error}; }

		FTransactionObjectRecord Record;
		Record.Target = FPersistentObjectRef(InTarget.Object);
		if (const auto Result = FTransactionMemberLocator::Capture(
			InTarget.SnapshotProperty, InTarget.SnapshotArrayIndex, Record.SnapshotMember); !Result)
		{
			Error.Code = ETransactionObjectRecordError::Member;
			Error.MemberCause = Result.Error;
			return {Error};
		}
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
		return {};
	}

	auto FTransactionObjectRecord::BuildTarget(
		FPropertyEditTarget& OutTarget) const -> FTransactionObjectRecordResult
	{
		DObject* Object = Target.Resolve();
		const auto Resolved = SnapshotMember.Resolve(Object);
		if (!Resolved)
		{
			auto Result = Reject(ETransactionObjectRecordError::Member);
			Result.Error.MemberCause = Resolved.Error;
			Result.Error.MemberCause->Owner = Target.GetKey();
			return Result;
		}
		FProperty* Member = Resolved.Property;
		if (Path.empty() || Path.front().Property != Member
			|| Path.back().Property != LeafProperty)
			return Reject(ETransactionObjectRecordError::Path);
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
		if (const auto Validation = ValidateTarget(Result); !Validation)
		{
			auto Failure = Reject(ETransactionObjectRecordError::Target);
			Failure.Error.PathCause = std::make_shared<FPropertyEditPathError>(Validation.Error);
			return Failure;
		}
		OutTarget = std::move(Result);
		return {};
	}

	auto FTransactionObjectRecord::Validate() const -> FTransactionObjectRecordResult
	{
		FPropertyEditTarget TargetValue;
		if (const auto Result = BuildTarget(TargetValue); !Result) return Result;
		for (const FPropertyValueSnapshotPayload* Payload : {&Before, &After})
		{
			FPropertyValueDraft Draft(TargetValue);
			if (const auto Result = Draft.Restore(*Payload); !Result)
			{
				auto Failure = Reject(ETransactionObjectRecordError::Draft);
				Failure.Error.Before = Payload == &Before;
				Failure.Error.DraftCause = std::make_shared<FPropertyValueDraftError>(Result.Error);
				return Failure;
			}
			const FProperty* ResolvedProperty = nullptr;
			void* ResolvedContainer = nullptr;
			uint32 ResolvedArrayIndex = 0;
			if (const auto Result = Draft.Resolve(TargetValue, ResolvedProperty, ResolvedContainer,
				ResolvedArrayIndex); !Result)
			{
				auto Failure = Reject(ETransactionObjectRecordError::Draft);
				Failure.Error.Before = Payload == &Before;
				Failure.Error.DraftCause = std::make_shared<FPropertyValueDraftError>(Result.Error);
				return Failure;
			}
		}
		return {};
	}

	auto FTransactionObjectRecord::Apply(
		bool bBefore,
		EPropertyChangeOrigin Origin) const -> FTransactionObjectRecordResult
	{
		FPropertyEditTarget TargetValue;
		if (auto Result = BuildTarget(TargetValue); !Result)
		{
			Result.Error.Before = bBefore;
			return Result;
		}
		const FPropertyValueSnapshotPayload& Value = bBefore ? Before : After;
		const FMutationExecutionResult Result = ExecuteMutation(
			TargetValue, &Value, nullptr, EMutationOperation::Apply,
			EPropertyChangePhase::Committed, Origin);
		if (!Result)
		{
			auto Failure = Reject(ETransactionObjectRecordError::Mutation);
			Failure.Error.Before = bBefore;
			Failure.Error.MutationCause = std::make_shared<FPropertyMutationError>(Result.Error);
			return Failure;
		}
		if (Result.bDeferred)
		{
			auto Failure = Reject(ETransactionObjectRecordError::Deferred);
			Failure.Error.Before = bBefore;
			return Failure;
		}
		TargetValue.Object->MarkPackageDirty();
		return {};
	}

	auto FTransactionObjectRecord::AddReferencedObjects(FReferenceCollector& Collector) const -> void
	{
		Target.AddReferencedObjects(Collector);
		auto AddPayload = [&](const FPropertyValueSnapshotPayload& Payload) {
			for (const FObjectKey Handle : Payload.GetReferencedObjectKeys())
				FPersistentObjectRef::FromKey(Handle).AddReferencedObjects(Collector);
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

	auto FPropertyEditSession::Reject(std::string Message) -> FPropertyEditOperationResult
	{
		return {.Status = EPropertyEditResult::Failed, .Message = std::move(Message)};
	}

	FPropertyEditSession::~FPropertyEditSession()
	{
		// An applied preview must never be abandoned merely because its UI owner is
		// destroyed. Explicit Commit/Cancel remains preferable because it can surface errors.
		if (bActive)
		{
			if (const auto Result = Cancel(); !Result)
			{
				DURIN_FATAL("Unable to restore an unfinished reflected-property preview: {}", Result.Message);
				check(false);
			}
		}
		Reset();
	}

	auto FPropertyEditSession::Begin(
		const FPropertyEditTarget& InTarget,
		std::string_view InDescription,
		DTransactor* InTransactor
	) -> FPropertyEditOperationResult
	{
		if (bActive) return Reject("A reflected-property edit session is already active.");
		Target = InTarget;
		if (const auto Validation = ValidateTarget(Target); !Validation)
		{
			auto Result = Reject(FormatPropertyEditPathError(Validation.Error));
			Reset();
			return Result;
		}
		TargetObject = TStrongObjectPtr<DObject>(FObjectKey(InTarget.Object));
		if (!TargetObject)
		{
			auto Result = Reject("The reflected-property edit target is no longer live.");
			Reset();
			return Result;
		}
		Target.Object = TargetObject.Get();
		Transactor = InTransactor;
		Description = InDescription.empty()
			? std::format("Edit {}", Target.MemberProperty->NamePrivate)
			: InDescription;
		if (const auto Capture = CaptureTargetValue(Target, OriginalValue); !Capture)
		{
			auto Result = Reject(ToString(Capture.error()));
			Reset();
			return Result;
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
				auto Result = Reject("The reflected-property transactor rejected the edit scope.");
				Reset();
				return Result;
			}
			FTransactionObjectRecord Record;
			if (const auto Capture = FTransactionObjectRecord::Capture(
				Target, OriginalValue, CurrentValue, Record); !Capture)
			{
				auto Result = Reject(FormatTransactionObjectRecordError(Capture.Error));
				const auto Cleanup = TransactionScope->Cancel();
				if (Cleanup.Code == ETransactorResultCode::Rejected
					|| Cleanup.Code == ETransactorResultCode::Failed
					|| Cleanup.Code == ETransactorResultCode::RecoveryRequired)
					Result.Message += " Cleanup also failed: " + FormatTransactorResult(Cleanup);
				Reset();
				return Result;
			}
			const FTransactorResult RecordResult = TransactionScope->Record(std::move(Record));
			if (!RecordResult)
			{
				auto Result = Reject(FormatTransactorResult(RecordResult));
				const auto Cleanup = TransactionScope->Cancel();
				if (Cleanup.Code == ETransactorResultCode::Rejected
					|| Cleanup.Code == ETransactorResultCode::Failed
					|| Cleanup.Code == ETransactorResultCode::RecoveryRequired)
					Result.Message += " Cleanup also failed: " + FormatTransactorResult(Cleanup);
				Reset();
				return Result;
			}
			TransactionRecordId = RecordResult.RecordId;
		}
		bActive = true;
		return {};
	}

	auto FPropertyEditSession::Apply(
		const FPropertyValueSnapshot& ProposedValue) -> FPropertyEditOperationResult
	{
		return Apply(ProposedValue.GetPayload());
	}

	auto FPropertyEditSession::Apply(const FPropertyValueSnapshotPayload& ProposedValue) -> FPropertyEditOperationResult
	{
		if (!bActive) return Reject("No reflected-property edit session is active.");
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
		if (ProposedValue == CurrentValue) return {};
		FMutationExecutionResult Result = ExecuteMutation(
			Target, &ProposedValue, &CurrentValue, EMutationOperation::Apply,
			EPropertyChangePhase::Interactive, EPropertyChangeOrigin::Edit);
		if (!Result)
		{
			auto Failure = Reject(FormatPropertyMutationError(Result.Error));
			if (Result.AppliedValue.IsValid()) CurrentValue = std::move(Result.AppliedValue);
			return Failure;
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
					std::expected<void, FObjectValidationError> Validation) mutable {
					FPropertyEditSession* Owner = nullptr;
					{
						std::lock_guard Lock(OwnerState->Mutex);
						Owner = OwnerState->Owner;
					}
					if (Owner)
						Owner->CompleteDeferredEdit(
							std::move(Validation), std::move(DeferredValue));
				});
			if (bDeferredPending && DeferredOwnerState == OwnerState)
				CancelDeferredEdit = std::move(Cancel);
			return {.Status = EPropertyEditResult::Pending};
		}
		const FPropertyValueSnapshotPayload PreviousValue = CurrentValue;
		CurrentValue = std::move(Result.AppliedValue);
		if (auto Update = UpdateTransactorRecord(); !Update)
		{
			FMutationExecutionResult Rollback = ExecuteMutation(
				Target, &PreviousValue, nullptr, EMutationOperation::Apply,
				EPropertyChangePhase::Interactive, EPropertyChangeOrigin::Edit);
			if (Rollback && !Rollback.bDeferred)
				CurrentValue = std::move(Rollback.AppliedValue);
			if (!Rollback) Update.Message += " Rollback also failed: " + FormatPropertyMutationError(Rollback.Error);
			if (Rollback.bDeferred) Update.Message += " Rollback requires deferred validation.";
			return Update;
		}
		return {.Status = Result.bChanged ? EPropertyEditResult::Changed : EPropertyEditResult::NoChange};
	}

	auto FPropertyEditSession::CompleteDeferredEdit(
		std::expected<void, FObjectValidationError> Validation,
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
		if (!Validation)
		{
			DURIN_ERROR("Deferred reflected-property edit failed: {}", ToString(Validation.error()));
			Reset();
			return;
		}
		FMutationExecutionResult Result = ApplyDeferredMutation(
			Target,
			ProposedValue,
			EPropertyChangePhase::Interactive,
			EPropertyChangeOrigin::Edit);
		if (!Result)
		{
			DURIN_ERROR("Deferred reflected-property publication failed: {}", FormatPropertyMutationError(Result.Error));
			Reset();
			return;
		}
		CurrentValue = std::move(Result.AppliedValue);
		if (const auto Update = UpdateTransactorRecord(); !Update)
		{
			DURIN_ERROR("Deferred reflected-property record update failed: {}", Update.Message);
			Reset();
			return;
		}
		if (const auto CommitResult = Commit(); !CommitResult)
			DURIN_ERROR("Deferred reflected-property transaction failed: {}", CommitResult.Message);
	}

	auto FPropertyEditSession::MatchesTarget(const FPropertyEditTarget& Other) const -> bool
	{
		return bActive && Target.MatchesContinuousEdit(Other);
	}

	auto FPropertyEditSession::UpdateTransactorRecord() -> FPropertyEditOperationResult
	{
		if (!Transactor || !TransactionScope || !TransactionScope->IsActive()) return {};
		FTransactionObjectRecord Record;
		if (const auto Capture = FTransactionObjectRecord::Capture(
			Target, OriginalValue, CurrentValue, Record); !Capture)
		{
			auto Result = Reject(FormatTransactionObjectRecordError(Capture.Error));
			return Result;
		}
		const FTransactorResult Result =
			TransactionScope->UpdateRecord(TransactionRecordId, std::move(Record));
		if (Result) return {};
		auto Failure = Reject(FormatTransactorResult(Result));
		return Failure;
	}

	auto FPropertyEditSession::Commit() -> FPropertyEditOperationResult
	{
		if (!bActive) return Reject("No reflected-property edit session is active.");
		const bool bChanged = HasChanges();
		if (const auto Mutation = ExecuteMutation(Target, nullptr, nullptr, EMutationOperation::NotifyOnly,
			EPropertyChangePhase::Committed, EPropertyChangeOrigin::Edit); !Mutation)
		{
			auto Result = Reject(FormatPropertyMutationError(Mutation.Error));
			return Result;
		}
		if (bChanged)
		{
			Target.Object->MarkPackageDirty();
			FTransactorResult TransactorResult{
				.Code = ETransactorResultCode::NoOp};
			if (TransactionScope && TransactionScope->IsActive())
			{
				if (const auto Update = UpdateTransactorRecord(); !Update) return Update;
				TransactorResult = TransactionScope->End();
				if (TransactorResult.Code == ETransactorResultCode::Rejected
					|| TransactorResult.Code == ETransactorResultCode::Failed
					|| TransactorResult.Code == ETransactorResultCode::RecoveryRequired)
				{
					auto Failure = Reject(FormatTransactorResult(TransactorResult));
					return Failure;
				}
			}
		}
		else if (TransactionScope && TransactionScope->IsActive())
		{
			const FTransactorResult Result = TransactionScope->Cancel();
			if (Result.Code == ETransactorResultCode::Rejected
				|| Result.Code == ETransactorResultCode::Failed
				|| Result.Code == ETransactorResultCode::RecoveryRequired)
			{
				auto Failure = Reject(FormatTransactorResult(Result));
				return Failure;
			}
		}
		Reset();
		return {.Status = bChanged ? EPropertyEditResult::Changed : EPropertyEditResult::NoChange};
	}

	auto FPropertyEditSession::Cancel() -> FPropertyEditOperationResult
	{
		if (!bActive) return Reject("No reflected-property edit session is active.");
		const bool bChanged = HasChanges();
		FMutationExecutionResult Result = ExecuteMutation(
			Target, bChanged ? &OriginalValue : nullptr, nullptr,
			bChanged ? EMutationOperation::Apply : EMutationOperation::NotifyOnly,
			EPropertyChangePhase::Cancelled, EPropertyChangeOrigin::Edit);
		if (!Result)
		{
			auto Failure = Reject(FormatPropertyMutationError(Result.Error));
			if (Result.AppliedValue.IsValid()) CurrentValue = std::move(Result.AppliedValue);
			return Failure;
		}
		if (TransactionScope && TransactionScope->IsActive())
		{
			const FTransactorResult CancelResult = TransactionScope->Cancel();
			if (CancelResult.Code == ETransactorResultCode::Rejected
				|| CancelResult.Code == ETransactorResultCode::Failed
				|| CancelResult.Code == ETransactorResultCode::RecoveryRequired)
			{
				auto Failure = Reject(FormatTransactorResult(CancelResult));
				return Failure;
			}
		}
		Reset();
		return {.Status = bChanged ? EPropertyEditResult::Changed : EPropertyEditResult::NoChange};
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
