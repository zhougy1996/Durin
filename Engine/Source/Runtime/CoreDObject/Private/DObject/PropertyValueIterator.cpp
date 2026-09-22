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
		SeekMatch();
	}

	FPropertyValueIterator::FPropertyValueIterator(const FProperty* Property, const void* Container,
		FPropertyValueIteratorOptions InOptions) : Options(InOptions)
	{
		FFrame Frame;
		AddProperty(Frame, Property, Container);
		if (Status == EContainerOpResult::Success && !Frame.Entries.empty()) Stack.push_back(std::move(Frame));
		SeekMatch();
	}

	auto FPropertyValueIterator::AddProperty(FFrame& Frame, const FProperty* Property, const void* Container,
		EPropertyValuePathKind PathKind, uint64 ContainerIndex) -> void
	{
		if (!Property || !Container)
		{
			Status = EContainerOpResult::InvalidInput;
			return;
		}
		if (!Options.bIncludeDeprecated && Property->IsDeprecated()) return;
		for (uint32 Index = 0; Index < Property->GetArrayDim(); ++Index)
			Frame.Entries.push_back({Property, Container, Index, PathKind, ContainerIndex});
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

	auto FPropertyValueIterator::GetStaticArrayIndex() const -> uint32
	{
		return *this ? Stack.back().Entries[Stack.back().Index].ArrayIndex : 0;
	}

	auto FPropertyValueIterator::GetPropertyChain() const -> std::vector<const FProperty*>
	{
		std::vector<const FProperty*> Chain;
		GetPropertyChain(Chain);
		return Chain;
	}

	auto FPropertyValueIterator::GetPropertyChain(std::vector<const FProperty*>& OutChain) const -> void
	{
		OutChain.clear();
		if (*this)
			for (const auto& Frame : Stack) OutChain.push_back(Frame.Entries[Frame.Index].Property);
	}

	auto FPropertyValueIterator::GetPropertyPath(std::vector<FPropertyValuePathEntry>& OutPath) const -> void
	{
		OutPath.clear();
		if (*this)
			for (const auto& Frame : Stack)
			{
				const auto& Entry = Frame.Entries[Frame.Index];
				OutPath.push_back({Entry.Property, Entry.ArrayIndex, Entry.PathKind, Entry.ContainerIndex});
			}
	}

	auto FPropertyValueIterator::GetPropertyPathDebugString() const -> std::string
	{
		std::string Path;
		if (!*this) return Path;
		for (const auto& Frame : Stack)
		{
			const auto& Entry = Frame.Entries[Frame.Index];
			switch (Entry.PathKind)
			{
			case EPropertyValuePathKind::Member:
				if (!Path.empty()) Path += '.';
				Path += Entry.Property->NamePrivate.ToString();
				break;
			case EPropertyValuePathKind::ArrayElement: Path += std::format("[{}]", Entry.ContainerIndex); break;
			case EPropertyValuePathKind::MapKey: Path += std::format("{{{}}}.Key", Entry.ContainerIndex); break;
			case EPropertyValuePathKind::MapValue: Path += std::format("{{{}}}.Value", Entry.ContainerIndex); break;
			}
			if (Entry.Property->GetArrayDim() > 1) Path += std::format("[{}]", Entry.ArrayIndex);
		}
		return Path;
	}

	auto FPropertyValueIterator::LoadNextArrayElement(FFrame& Frame) -> bool
	{
		Frame.Entries.clear();
		Frame.Index = 0;
		while (Frame.NextElement < Frame.ElementCount)
		{
			const uint64 Index = Frame.NextElement++;
			const void* Element = nullptr;
			Status = Frame.Array->GetElement(Frame.ArrayContainer, Index, &Element, Frame.ArrayStaticIndex);
			if (Status != EContainerOpResult::Success) return false;
			AddProperty(Frame, Frame.Array->GetInner(), Element, EPropertyValuePathKind::ArrayElement, Index);
			if (Status != EContainerOpResult::Success) return false;
			if (!Frame.Entries.empty()) return true;
		}
		return false;
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
			if (Array->HasArrayOps() && Array->HasCapability(EArrayOpsFlags::Count | EArrayOpsFlags::RandomAccess))
			{
				Frame.Array = Array;
				Frame.ArrayContainer = Entry.Container;
				Frame.ArrayStaticIndex = Entry.ArrayIndex;
				Status = Array->GetNum(Entry.Container, Frame.ElementCount, Entry.ArrayIndex);
				if (Status == EContainerOpResult::Success) LoadNextArrayElement(Frame);
				return;
			}
			if (!Array->HasArrayOps() || !Array->GetOps().VisitConst)
			{ Status = EContainerOpResult::Unsupported; return; }
			struct FContext { FPropertyValueIterator& Iterator; FFrame& Frame; const FProperty* Inner; } Context{*this, Frame, Array->GetInner()};
			const auto Result = Array->VisitElements(Entry.Container, [](void* Raw, uint64 Index, const void* Element) {
				auto& C = *static_cast<FContext*>(Raw);
				C.Iterator.AddProperty(C.Frame, C.Inner, Element, EPropertyValuePathKind::ArrayElement, Index);
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
			struct FContext { FPropertyValueIterator& Iterator; FFrame& Frame; const FMapProperty* Map; uint64 Index = 0; } Context{*this, Frame, Map};
			const auto Result = Map->VisitEntries(Entry.Container, [](void* Raw, const void* Key, const void* Value) {
				auto& C = *static_cast<FContext*>(Raw);
				C.Iterator.AddProperty(C.Frame, C.Map->GetKeyProp(), Key, EPropertyValuePathKind::MapKey, C.Index);
				C.Iterator.AddProperty(C.Frame, C.Map->GetValueProp(), Value, EPropertyValuePathKind::MapValue, C.Index++);
				return C.Iterator.Status == EContainerOpResult::Success;
			}, &Context, Entry.ArrayIndex);
			if (Status == EContainerOpResult::Success) Status = Result;
		}
	}

	auto FPropertyValueIterator::Advance() -> void
	{
		if (!*this) return;
		const bool bDescend = Options.bRecursive && !bSkipRecursionOnce;
		bSkipRecursionOnce = false;
		if (bDescend)
		{
			FFrame Children;
			Descend(Children, Stack.back().Entries[Stack.back().Index]);
			if (Status != EContainerOpResult::Success) { Stack.clear(); return; }
			if (!Children.Entries.empty()) { Stack.push_back(std::move(Children)); return; }
		}
		while (!Stack.empty())
		{
			if (++Stack.back().Index < Stack.back().Entries.size()) break;
			if (Stack.back().Array && LoadNextArrayElement(Stack.back())) break;
			if (Status != EContainerOpResult::Success) { Stack.clear(); return; }
			Stack.pop_back();
		}
	}

	auto FPropertyValueIterator::SeekMatch() -> void
	{
		while (*this && Options.Kind && Key()->GetKind() != *Options.Kind) Advance();
	}

	auto FPropertyValueIterator::operator++() -> FPropertyValueIterator&
	{
		Advance();
		SeekMatch();
		return *this;
	}
}
