#include "DObject/PropertyValueIterator.h"

#include "DObject/Class.h"
#include "DObject/DurinPropertyTypes.h"

namespace Durin
{
	FPropertyValueIterator::FPropertyValueIterator(const DStructBase* Type, const void* Container,
		FPropertyValueIteratorOptions InOptions) : Options(InOptions)
	{
		FFrame Frame;
		AddStruct(Frame, Type, Container);
		if (Status == EContainerOpResult::Success && !Frame.Entries.empty()) Stack.push_back(std::move(Frame));
	}

	FPropertyValueIterator::FPropertyValueIterator(const FProperty* Property, const void* Container,
		FPropertyValueIteratorOptions InOptions) : Options(InOptions)
	{
		FFrame Frame;
		AddProperty(Frame, Property, Container);
		if (Status == EContainerOpResult::Success && !Frame.Entries.empty()) Stack.push_back(std::move(Frame));
	}

	auto FPropertyValueIterator::AddProperty(FFrame& Frame, const FProperty* Property, const void* Container) -> void
	{
		if (!Property || !Container)
		{
			Status = EContainerOpResult::InvalidInput;
			return;
		}
		if (!Options.bIncludeDeprecated && Property->IsDeprecated()) return;
		for (uint32 Index = 0; Index < Property->GetArrayDim(); ++Index)
			Frame.Entries.push_back({Property, Container, Index});
	}

	auto FPropertyValueIterator::AddStruct(FFrame& Frame, const DStructBase* Type, const void* Container) -> void
	{
		if (!Type || !Container)
		{
			Status = EContainerOpResult::InvalidInput;
			return;
		}
		// Match ForEachProperty's base-first order without recursive superclass calls.
		std::vector<const DStructBase*> Types;
		for (auto* Current = Type; Current; Current = Options.bIncludeSuper ? Current->GetSuperStructBase() : nullptr)
			Types.push_back(Current);
		for (auto It = Types.rbegin(); It != Types.rend(); ++It)
			for (auto* Field = (*It)->ChildProperties; Field; Field = Field->Next)
				AddProperty(Frame, static_cast<const FProperty*>(Field), Container);
	}

	auto FPropertyValueIterator::Key() const -> const FProperty*
	{
		return *this ? Stack.back().Entries[Stack.back().Index].Property : nullptr;
	}

	auto FPropertyValueIterator::Value() const -> const void*
	{
		if (!*this) return nullptr;
		const auto& Entry = Stack.back().Entries[Stack.back().Index];
		return Entry.Property->GetValuePtr(Entry.Container, Entry.ArrayIndex);
	}

	auto FPropertyValueIterator::GetArrayIndex() const -> uint32
	{
		return *this ? Stack.back().Entries[Stack.back().Index].ArrayIndex : 0;
	}

	auto FPropertyValueIterator::GetPropertyChain() const -> std::vector<const FProperty*>
	{
		std::vector<const FProperty*> Chain;
		if (*this)
			for (const auto& Frame : Stack) Chain.push_back(Frame.Entries[Frame.Index].Property);
		return Chain;
	}

	auto FPropertyValueIterator::Descend(FFrame& Frame, const FEntry& Entry) -> void
	{
		using K = DurinCodeGen::EPropertyGenFlags;
		if (Entry.Property->GetKind() == K::Struct)
		{
			AddStruct(Frame, static_cast<const FStructProperty*>(Entry.Property)->GetStruct(), Value());
		}
		else if (Entry.Property->GetKind() == K::Array)
		{
			const auto* Array = static_cast<const FArrayProperty*>(Entry.Property);
			if (!Array->GetInner()) { Status = EContainerOpResult::InvalidInput; return; }
			if (!Array->HasArrayOps() || !Array->GetOps().VisitConst)
			{ Status = EContainerOpResult::Unsupported; return; }
			struct FContext { FPropertyValueIterator& Iterator; FFrame& Frame; const FProperty* Inner; } Context{*this, Frame, Array->GetInner()};
			const auto Result = Array->VisitElements(Entry.Container, [](void* Raw, uint64, const void* Element) {
				auto& C = *static_cast<FContext*>(Raw);
				C.Iterator.AddProperty(C.Frame, C.Inner, Element);
				return C.Iterator.Status == EContainerOpResult::Success;
			}, &Context, Entry.ArrayIndex);
			if (Status == EContainerOpResult::Success) Status = Result;
		}
		else if (Entry.Property->GetKind() == K::Map)
		{
			const auto* Map = static_cast<const FMapProperty*>(Entry.Property);
			if (!Map->GetKeyProp() || !Map->GetValueProp()) { Status = EContainerOpResult::InvalidInput; return; }
			if (!Map->HasMapOps() || !Map->GetOps().VisitConst)
			{ Status = EContainerOpResult::Unsupported; return; }
			struct FContext { FPropertyValueIterator& Iterator; FFrame& Frame; const FMapProperty* Map; } Context{*this, Frame, Map};
			const auto Result = Map->VisitEntries(Entry.Container, [](void* Raw, const void* Key, const void* Value) {
				auto& C = *static_cast<FContext*>(Raw);
				C.Iterator.AddProperty(C.Frame, C.Map->GetKeyProp(), Key);
				C.Iterator.AddProperty(C.Frame, C.Map->GetValueProp(), Value);
				return C.Iterator.Status == EContainerOpResult::Success;
			}, &Context, Entry.ArrayIndex);
			if (Status == EContainerOpResult::Success) Status = Result;
		}
	}

	auto FPropertyValueIterator::operator++() -> FPropertyValueIterator&
	{
		if (!*this) return *this;
		const bool bDescend = Options.bRecursive && !bSkipRecursionOnce;
		bSkipRecursionOnce = false;
		if (bDescend)
		{
			FFrame Children;
			Descend(Children, Stack.back().Entries[Stack.back().Index]);
			if (Status != EContainerOpResult::Success) { Stack.clear(); return *this; }
			if (!Children.Entries.empty()) { Stack.push_back(std::move(Children)); return *this; }
		}
		while (!Stack.empty())
		{
			if (++Stack.back().Index < Stack.back().Entries.size()) break;
			Stack.pop_back();
		}
		return *this;
	}
}
