#pragma once

#include <expected>

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
		FResolvedPropertyValue& OutValue) -> std::expected<void, FPropertyEditPathError>;

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
				InitializationError = Reject(EPropertyValueDraftError::MissingRoot).error();
			else if (Property->HasValueAccessors())
				InitializationError = Reject(EPropertyValueDraftError::Accessors).error();
			else if (!Context.HasLifecycle || !Context.ValueSize || !Context.ValueAlignment)
				InitializationError = Reject(EPropertyValueDraftError::Lifecycle).error();
			if (!IsValid()) return;

			FPropertyValueSnapshotPayload Current;
			if (const auto Result = CapturePropertyValuePayload(Property, Target.SnapshotContainer, ArrayIndex, Current); !Result)
			{
				InitializationError = SnapshotFailure(EPropertyValueDraftError::Capture, Result.error()).error();
				return;
			}
			if (const auto Result = Storage.DefaultConstruct(Property, ArrayIndex); !Result)
			{
				InitializationError = Reject(EPropertyValueDraftError::Storage).error();
				InitializationError.ValueCause = Result.error();
				return;
			}
			Memory = Storage.GetContainer();
			if (const auto Result = Restore(Current); !Result)
				InitializationError = Result.error();
		}

		FPropertyValueDraft(const FPropertyValueDraft&) = delete;
		auto operator=(const FPropertyValueDraft&) -> FPropertyValueDraft& = delete;
		auto IsValid() const -> bool { return InitializationError.Code == EPropertyValueDraftError::None; }
		auto GetError() const -> const FPropertyValueDraftError& { return InitializationError; }
		auto GetRootProperty() const -> const FProperty* { return Property; }
		auto GetRootContainer() const -> void* { return Memory; }
		auto GetRootArrayIndex() const -> uint32 { return ArrayIndex; }

		auto Restore(const FPropertyValueSnapshotPayload& Snapshot) -> std::expected<void, FPropertyValueDraftError>
		{
			if (!IsValid()) return std::unexpected(InitializationError);
			const auto Result = RestorePropertyValuePayload(Property, Memory, ArrayIndex, Snapshot);
			if (!Result) return SnapshotFailure(EPropertyValueDraftError::Restore, Result.error());
			return {};
		}

		auto Resolve(const FPropertyEditTarget& Source, const FProperty*& OutProperty,
			void*& OutContainer, uint32& OutArrayIndex) const -> std::expected<void, FPropertyValueDraftError>
		{
			if (!IsValid()) return std::unexpected(InitializationError);
			if (Source.SnapshotProperty != Property || Source.SnapshotArrayIndex != ArrayIndex)
			{
				auto Result = Reject(EPropertyValueDraftError::RootMismatch);
				Result.error().RequestedRoot = Source.SnapshotProperty ? Source.SnapshotProperty->NamePrivate.ToString() : std::string{};
				Result.error().RequestedArrayIndex = Source.SnapshotArrayIndex;
				return Result;
			}
			FPropertyEditTarget DraftTarget = Source;
			DraftTarget.SnapshotContainer = Memory;
			FResolvedPropertyValue Resolved;
			if (const auto Path = ResolveReflectedPropertyValue(DraftTarget, Resolved); !Path)
			{
				auto Result = Reject(EPropertyValueDraftError::Path);
				Result.error().PathCause = Path.error();
				return Result;
			}
			OutProperty = Resolved.Property;
			OutContainer = Resolved.Container;
			OutArrayIndex = Resolved.ArrayIndex;
			return {};
		}

		auto Capture(FPropertyValueSnapshotPayload& OutSnapshot) const -> std::expected<void, FPropertyValueDraftError>
		{
			if (!IsValid()) return std::unexpected(InitializationError);
			const auto Result = CapturePropertyValuePayload(Property, Memory, ArrayIndex, OutSnapshot);
			if (!Result) return SnapshotFailure(EPropertyValueDraftError::Capture, Result.error());
			return {};
		}

		auto Capture(FPropertyValueSnapshot& OutSnapshot) const -> std::expected<void, FPropertyValueDraftError>
		{
			if (!IsValid()) return std::unexpected(InitializationError);
			const auto Result = CapturePropertyValue(Property, Memory, ArrayIndex, OutSnapshot);
			if (!Result) return SnapshotFailure(EPropertyValueDraftError::Capture, Result.error());
			return {};
		}

	private:
		auto Reject(EPropertyValueDraftError Code) const -> std::expected<void, FPropertyValueDraftError>
		{
			auto Error = Context;
			Error.Code = Code;
			return std::unexpected(std::move(Error));
		}
		auto SnapshotFailure(EPropertyValueDraftError Code, const FPropertySnapshotError& Cause) const -> std::expected<void, FPropertyValueDraftError>
		{
			auto Result = Reject(Code);
			Result.error().SnapshotCause = Cause;
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
