#pragma once

#include "DObject/Property.h"
#include "PropertyEditor/PropertyEditing.h"

namespace Durin::Editor
{
	// Resolves a reflected edit path to its current property storage.
	struct FResolvedPropertyValue
	{
		const FProperty* Property = nullptr;
		void* Container = nullptr;
		uint32 ArrayIndex = 0;
	};

	auto ResolveReflectedPropertyValue(const FPropertyEditTarget& Target,
		FResolvedPropertyValue& OutValue) -> FPropertyEditPathResult;

	// Owns detached snapshot-root storage and retains typed initialization failures.
	class FPropertyValueDraft
	{
	public:
		explicit FPropertyValueDraft(const FPropertyEditTarget& Target)
			: Property(Target.SnapshotProperty), ArrayIndex(Target.SnapshotArrayIndex)
		{
			Context.Root = Property ? Property->NamePrivate.ToString() : std::string{};
			Context.ArrayIndex = ArrayIndex;
			Context.HasContainer = Target.SnapshotContainer != nullptr;
			Context.HasLifecycle = Property && Property->HasValueLifecycle();
			Context.ValueSize = Property ? Property->GetValueSize() : 0;
			Context.ValueAlignment = Property ? Property->GetValueAlignment() : 0;
			if (!Property || !Target.SnapshotContainer)
				InitializationError = Reject(EPropertyValueDraftError::MissingRoot).Error;
			else if (Property->HasValueAccessors())
				InitializationError = Reject(EPropertyValueDraftError::Accessors).Error;
			else if (!Context.HasLifecycle || !Context.ValueSize || !Context.ValueAlignment)
				InitializationError = Reject(EPropertyValueDraftError::Lifecycle).Error;
			if (!IsValid()) return;

			FPropertyValueSnapshotPayload Current;
			if (const auto Result = CapturePropertyValuePayload(Property, Target.SnapshotContainer, ArrayIndex, Current); !Result)
			{
				InitializationError = SnapshotFailure(EPropertyValueDraftError::Capture, Result.error()).Error;
				return;
			}
			if (const auto Result = Storage.DefaultConstruct(Property, ArrayIndex); !Result)
			{
				InitializationError = Reject(EPropertyValueDraftError::Storage).Error;
				InitializationError.ValueCause = Result.error();
				return;
			}
			Memory = Storage.GetContainer();
			InitializationError = Restore(Current).Error;
		}

		FPropertyValueDraft(const FPropertyValueDraft&) = delete;
		auto operator=(const FPropertyValueDraft&) -> FPropertyValueDraft& = delete;
		auto IsValid() const -> bool { return InitializationError.Code == EPropertyValueDraftError::None; }
		auto GetError() const -> const FPropertyValueDraftError& { return InitializationError; }
		auto GetRootProperty() const -> const FProperty* { return Property; }
		auto GetRootContainer() const -> void* { return Memory; }
		auto GetRootArrayIndex() const -> uint32 { return ArrayIndex; }

		auto Restore(const FPropertyValueSnapshotPayload& Snapshot) -> FPropertyValueDraftResult
		{
			if (!IsValid()) return {InitializationError};
			const auto Result = RestorePropertyValuePayload(Property, Memory, ArrayIndex, Snapshot);
			return Result ? FPropertyValueDraftResult{} : SnapshotFailure(EPropertyValueDraftError::Restore, Result.error());
		}

		auto Resolve(const FPropertyEditTarget& Source, const FProperty*& OutProperty,
			void*& OutContainer, uint32& OutArrayIndex) const -> FPropertyValueDraftResult
		{
			if (!IsValid()) return {InitializationError};
			if (Source.SnapshotProperty != Property || Source.SnapshotArrayIndex != ArrayIndex)
			{
				auto Result = Reject(EPropertyValueDraftError::RootMismatch);
				Result.Error.RequestedRoot = Source.SnapshotProperty ? Source.SnapshotProperty->NamePrivate.ToString() : std::string{};
				Result.Error.RequestedArrayIndex = Source.SnapshotArrayIndex;
				return Result;
			}
			FPropertyEditTarget DraftTarget = Source;
			DraftTarget.SnapshotContainer = Memory;
			FResolvedPropertyValue Resolved;
			if (const auto Path = ResolveReflectedPropertyValue(DraftTarget, Resolved); !Path)
			{
				auto Result = Reject(EPropertyValueDraftError::Path);
				Result.Error.PathCause = Path.Error;
				return Result;
			}
			OutProperty = Resolved.Property;
			OutContainer = Resolved.Container;
			OutArrayIndex = Resolved.ArrayIndex;
			return {};
		}

		auto Capture(FPropertyValueSnapshotPayload& OutSnapshot) const -> FPropertyValueDraftResult
		{
			if (!IsValid()) return {InitializationError};
			const auto Result = CapturePropertyValuePayload(Property, Memory, ArrayIndex, OutSnapshot);
			return Result ? FPropertyValueDraftResult{} : SnapshotFailure(EPropertyValueDraftError::Capture, Result.error());
		}

		auto Capture(FPropertyValueSnapshot& OutSnapshot) const -> FPropertyValueDraftResult
		{
			if (!IsValid()) return {InitializationError};
			const auto Result = CapturePropertyValue(Property, Memory, ArrayIndex, OutSnapshot);
			return Result ? FPropertyValueDraftResult{} : SnapshotFailure(EPropertyValueDraftError::Capture, Result.error());
		}

	private:
		auto Reject(EPropertyValueDraftError Code) const -> FPropertyValueDraftResult
		{
			auto Error = Context;
			Error.Code = Code;
			return {std::move(Error)};
		}
		auto SnapshotFailure(EPropertyValueDraftError Code, const FPropertySnapshotError& Cause) const -> FPropertyValueDraftResult
		{
			auto Result = Reject(Code);
			Result.Error.SnapshotCause = Cause;
			return Result;
		}
		const FProperty* Property = nullptr;
		uint32 ArrayIndex = 0;
		FReflectedValueStorage Storage;
		void* Memory = nullptr;
		FPropertyValueDraftError Context;
		FPropertyValueDraftError InitializationError;
	};
}
