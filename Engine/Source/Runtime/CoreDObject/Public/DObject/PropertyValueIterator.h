#pragma once

#include "DObject/ContainerOps.h"

namespace Durin
{
	class DStructBase;
	class FProperty;

	struct FPropertyValueIteratorOptions
	{
		bool bRecursive = true;
		bool bIncludeSuper = true;
		bool bIncludeDeprecated = true;
	};

	// Read-only, pre-order depth-first traversal of live reflected storage.
	// Object references are leaves. Metadata and storage must outlive the iterator;
	// do not resize/reorder containers or replace their storage during iteration.
	// Container callbacks are synchronous: child addresses are collected on descent,
	// so auxiliary storage includes pending siblings, not just nesting depth.
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
		COREDOBJECT_API auto GetArrayIndex() const -> uint32;
		COREDOBJECT_API auto operator++() -> FPropertyValueIterator&;
		auto SkipRecursiveProperty() -> void { bSkipRecursionOnce = true; }
		// Root-to-current order, including struct/container properties. Empty at end/error.
		COREDOBJECT_API auto GetPropertyChain() const -> std::vector<const FProperty*>;
		// Success after normal exhaustion; container failures terminate traversal.
		auto GetStatus() const -> EContainerOpResult { return Status; }

	private:
		struct FEntry
		{
			const FProperty* Property;
			const void* Container;
			uint32 ArrayIndex;
		};
		struct FFrame
		{
			std::vector<FEntry> Entries;
			size_t Index = 0;
		};
		auto AddProperty(FFrame& Frame, const FProperty* Property, const void* Container) -> void;
		auto AddStruct(FFrame& Frame, const DStructBase* Type, const void* Container) -> void;
		auto Descend(FFrame& Frame, const FEntry& Entry) -> void;
		std::vector<FFrame> Stack;
		FPropertyValueIteratorOptions Options;
		EContainerOpResult Status = EContainerOpResult::Success;
		bool bSkipRecursionOnce = false;
	};
}
