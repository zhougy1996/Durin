#pragma once

#include "DObject/ContainerOps.h"
#include "DObject/Property.h"

namespace Durin
{
	class DStructBase;
	class FProperty;
	class FArrayProperty;

	struct FPropertyValueIteratorOptions
	{
		bool bRecursive = true;
		bool bIncludeSuper = true;
		bool bIncludeDeprecated = true;
		// Filters yielded values only; unmatched containers are still traversed.
		std::optional<DurinCodeGen::EPropertyGenFlags> Kind;
	};

	enum class EPropertyValuePathKind : uint8 { Member, ArrayElement, MapKey, MapValue };
	struct FPropertyValuePathEntry
	{
		const FProperty* Property = nullptr;
		uint32 StaticArrayIndex = 0;
		EPropertyValuePathKind Kind = EPropertyValuePathKind::Member;
		// Array index or Map iteration ordinal (not a stable key identity).
		uint64 ContainerIndex = 0;
	};

	// Read-only, pre-order depth-first traversal of live reflected storage.
	// Object references are leaves. Metadata and storage must outlive the iterator;
	// do not resize/reorder containers or replace their storage during iteration.
	// Random-access arrays advance lazily. Maps and traversal-only arrays collect
	// child addresses on descent, so their pending siblings consume auxiliary storage.
	class FPropertyValueIterator
	{
	public:
		FPropertyValueIterator() = default;
		// Visits fields (including each fixed-array element), not the root struct itself.
		COREDOBJECT_API FPropertyValueIterator(const DStructBase* Type, const void* Container,
			FPropertyValueIteratorOptions InOptions = {});
		// Container is the owning storage, not an already-offset property value.
		COREDOBJECT_API FPropertyValueIterator(const FProperty* Property, const void* Container,
			FPropertyValueIteratorOptions InOptions = {});

		explicit operator bool() const { return !Stack.empty() && Status == EContainerOpResult::Success; }
		COREDOBJECT_API auto Key() const -> const FProperty*;
		COREDOBJECT_API auto Value() const -> const void*;
		// Index within the property's fixed array, not a dynamic Array/Map index.
		COREDOBJECT_API auto GetStaticArrayIndex() const -> uint32;
		COREDOBJECT_API auto operator++() -> FPropertyValueIterator&;
		auto SkipRecursiveProperty() -> void { bSkipRecursionOnce = true; }
		// Root-to-current order, including struct/container properties. Empty at end/error.
		COREDOBJECT_API auto GetPropertyChain() const -> std::vector<const FProperty*>;
		COREDOBJECT_API auto GetPropertyChain(std::vector<const FProperty*>& OutChain) const -> void;
		COREDOBJECT_API auto GetPropertyPath(std::vector<FPropertyValuePathEntry>& OutPath) const -> void;
		// Diagnostic only: Map ordinals depend on the container's iteration order.
		COREDOBJECT_API auto GetPropertyPathDebugString() const -> std::string;
		// Success after normal exhaustion; container failures terminate traversal.
		auto GetStatus() const -> EContainerOpResult { return Status; }

	private:
		struct FEntry
		{
			const FProperty* Property;
			const void* Container;
			uint32 ArrayIndex;
			EPropertyValuePathKind PathKind = EPropertyValuePathKind::Member;
			uint64 ContainerIndex = 0;
		};
		struct FFrame
		{
			std::vector<FEntry> Entries;
			size_t Index = 0;
			const FArrayProperty* Array = nullptr;
			const void* ArrayContainer = nullptr;
			uint32 ArrayStaticIndex = 0;
			uint64 NextElement = 0;
			uint64 ElementCount = 0;
		};
		auto AddProperty(FFrame& Frame, const FProperty* Property, const void* Container,
			EPropertyValuePathKind PathKind = EPropertyValuePathKind::Member, uint64 ContainerIndex = 0) -> void;
		auto AddStruct(FFrame& Frame, const DStructBase* Type, const void* Container) -> void;
		auto Descend(FFrame& Frame, const FEntry& Entry) -> void;
		auto LoadNextArrayElement(FFrame& Frame) -> bool;
		auto Advance() -> void;
		auto SeekMatch() -> void;
		std::vector<FFrame> Stack;
		FPropertyValueIteratorOptions Options;
		EContainerOpResult Status = EContainerOpResult::Success;
		bool bSkipRecursionOnce = false;
	};
}
