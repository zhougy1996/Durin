#pragma once

#include "DObject/Property.h"
#include "Editor/ReflectedPropertyEditing.h"

namespace Durin
{
	// Owns one detached, fully constructed snapshot-root value. Leaf addresses
	// resolved from this draft never point into the live edited object.
	class FPropertyValueDraft
	{
	public:
		explicit FPropertyValueDraft(const FReflectedPropertyEditTarget& Target, std::string* OutError)
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
			Alignment = std::max<size_t>(Property->GetValueAlignment(), __STDCPP_DEFAULT_NEW_ALIGNMENT__);
			Size = std::max<size_t>(1, static_cast<size_t>(Property->GetOffset())
				+ static_cast<size_t>(Property->GetElementSize()) * static_cast<size_t>(ArrayIndex)
				+ static_cast<size_t>(Property->GetValueSize()));
			Memory = ::operator new(Size, std::align_val_t(Alignment));
			std::memset(Memory, 0, Size);
			Value = Property->GetValuePtr(Memory, ArrayIndex);
			if (!Property->InitializeValue(Value))
			{
				Fail(OutError, "Unable to initialize reflected property draft storage.");
				return;
			}
			bInitialized = true;
			if (!RestorePropertyValue(Property, Memory, ArrayIndex, Current, OutError)) return;
			bValid = true;
		}

		~FPropertyValueDraft()
		{
			if (bInitialized) Property->DestroyValue(Value);
			if (Memory) ::operator delete(Memory, std::align_val_t(Alignment));
		}

		FPropertyValueDraft(const FPropertyValueDraft&) = delete;
		auto operator=(const FPropertyValueDraft&) -> FPropertyValueDraft& = delete;

		auto IsValid() const -> bool { return bValid; }

		auto Resolve(const FReflectedPropertyEditTarget& Source, FReflectedPropertyEditTarget& OutTarget,
			std::string* OutError) const -> bool
		{
			if (!bValid || Source.SnapshotProperty != Property || Source.SnapshotArrayIndex != ArrayIndex)
				return Fail(OutError, "The edit target does not match its reflected property draft root.");
			FReflectedPropertyEditTarget DraftTarget = Source;
			DraftTarget.SnapshotContainer = Memory;
			DraftTarget.LeafContainer = nullptr;
			return ResolveReflectedPropertyEditTarget(DraftTarget, OutTarget, OutError);
		}

		auto Capture(FPropertyValueSnapshot& OutSnapshot, std::string* OutError) const -> bool
		{
			return bValid && CapturePropertyValue(Property, Memory, ArrayIndex, OutSnapshot, OutError);
		}

	private:
		static auto Fail(std::string* OutError, std::string_view Message) -> bool
		{
			if (OutError) *OutError = Message;
			return false;
		}

		const FProperty* Property = nullptr;
		uint32 ArrayIndex = 0;
		void* Memory = nullptr;
		void* Value = nullptr;
		size_t Size = 0;
		size_t Alignment = __STDCPP_DEFAULT_NEW_ALIGNMENT__;
		bool bInitialized = false;
		bool bValid = false;
	};
}
