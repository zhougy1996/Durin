#pragma once

#include "DObject/Property.h"
#include "Editor/PropertyEditing.h"
#include "Misc/Failure.h"

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
		FResolvedPropertyValue& OutValue, std::string* OutError = nullptr) -> bool;

	// Owns one detached, fully constructed snapshot-root value. Leaf addresses
	// resolved from this draft never point into the live edited object.
	class FPropertyValueDraft
	{
	public:
		explicit FPropertyValueDraft(const FPropertyEditTarget& Target, std::string* OutError)
			: Property(Target.SnapshotProperty)
			, ArrayIndex(Target.SnapshotArrayIndex)
		{
			if (!Property || !Target.SnapshotContainer)
			{
				Fail(OutError, "The reflected property draft root is unavailable.");
				return;
			}
			if (Property->HasValueAccessors())
			{
				Fail(OutError, "Properties with custom value accessors cannot be used as draft roots.");
				return;
			}
			if (!Property->HasValueLifecycle() || Property->GetValueSize() == 0 || Property->GetValueAlignment() == 0)
			{
				Fail(OutError, "The reflected property lacks generated draft-value lifecycle metadata.");
				return;
			}

			FPropertyValueSnapshot Current;
			if (!CapturePropertyValue(Property, Target.SnapshotContainer, ArrayIndex, Current, OutError)) return;
			if (!Storage.DefaultConstruct(Property, ArrayIndex, OutError)) return;
			Memory = Storage.GetContainer();
			if (!RestorePropertyValue(Property, Memory, ArrayIndex, Current, OutError)) return;
			bValid = true;
		}

		FPropertyValueDraft(const FPropertyValueDraft&) = delete;
		auto operator=(const FPropertyValueDraft&) -> FPropertyValueDraft& = delete;

		auto IsValid() const -> bool { return bValid; }
		auto GetRootProperty() const -> const FProperty* { return Property; }
		auto GetRootContainer() const -> void* { return Memory; }
		auto GetRootArrayIndex() const -> uint32 { return ArrayIndex; }

		auto Restore(const FPropertyValueSnapshot& Snapshot, std::string* OutError) -> bool
		{
			return bValid && RestorePropertyValue(Property, Memory, ArrayIndex, Snapshot, OutError);
		}

		auto Resolve(const FPropertyEditTarget& Source, const FProperty*& OutProperty,
			void*& OutContainer, uint32& OutArrayIndex, std::string* OutError) const -> bool
		{
			if (!bValid || Source.SnapshotProperty != Property || Source.SnapshotArrayIndex != ArrayIndex)
				return Fail(OutError, "The edit target does not match its reflected property draft root.");
			FPropertyEditTarget DraftTarget = Source;
			DraftTarget.SnapshotContainer = Memory;
			FResolvedPropertyValue Resolved;
			if (!ResolveReflectedPropertyValue(DraftTarget, Resolved, OutError)) return false;
			OutProperty = Resolved.Property;
			OutContainer = Resolved.Container;
			OutArrayIndex = Resolved.ArrayIndex;
			return true;
		}

		auto Capture(FPropertyValueSnapshot& OutSnapshot, std::string* OutError) const -> bool
		{
			return bValid && CapturePropertyValue(Property, Memory, ArrayIndex, OutSnapshot, OutError);
		}

	private:
		const FProperty* Property = nullptr;
		uint32 ArrayIndex = 0;
		FReflectedValueStorage Storage;
		void* Memory = nullptr;
		bool bValid = false;
	};
}
