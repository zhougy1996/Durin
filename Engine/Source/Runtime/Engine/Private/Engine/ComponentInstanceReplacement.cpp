#include "Engine/ComponentInstanceDataCache.h"

#include "DObject/ObjectGraphReplacement.h"
#include "DObject/ObjectLifecycle.h"
#include "Engine/Actor.h"
#include <typeinfo>

namespace Durin
{
	namespace
	{
		class FPreviousReferences final : public FReferenceCollector
		{
		public:
			explicit FPreviousReferences(const FObjectReplacementMap& InMap) : Map(InMap) {}
			auto AddReferencedObject(DObject*& Object) -> void override { bOld |= Map.Find(Object) != nullptr; }
			const FObjectReplacementMap& Map;
			bool bOld = false;
		};
		template<typename T>
		auto HasOldReferences(const T& Value, const FObjectReplacementMap& Map) -> bool
		{
			FPreviousReferences Check(Map);
			Value.AddReferencedObjects(Check);
			return Check.bOld;
		}

		auto ReplaceReference(FPersistentObjectRef& Reference, const FObjectReplacementMap& Map) -> FTransactionCaptureResult
		{
			auto Key = Map.RemapObjectKey(Reference.GetKey());
			if (!Key) return std::unexpected(ToString(Key.error()));
			Reference = FPersistentObjectRef::FromKey(*Key);
			return {};
		}
	}

	auto FActorComponentInstanceSourceInfo::FindAndReplaceInstances(const FObjectReplacementMap& Map) -> FTransactionCaptureResult
	{
		if (auto Valid = Validate(); !Valid) return Valid;
		auto Result = *this;
		auto* Previous = Resolve();
		for (auto* Reference : {&Result.Exact, &Result.GeneratedOwner, &Result.Class, &Result.Owner})
			if (auto Valid = ReplaceReference(*Reference, Map); !Valid) return Valid;
		if (auto Valid = Result.Validate(); !Valid) return Valid;
		if (Previous)
		{
			const auto* Entry = Map.Find(Previous);
			// Logical construction identity must agree with the same graph map.
			// A matching name alone cannot authorize a different owner or source.
			if (Entry && (!Entry->Replacement || Result.Resolve() != Entry->Replacement))
				return std::unexpected("Component instance matching disagrees with the replacement graph.");
			if (!Entry && Result.Owner != Owner)
				return std::unexpected("Component instance owner changed without a complete component mapping.");
		}
		*this = std::move(Result);
		return {};
	}

	auto FComponentInstanceLifecycleState::FindAndReplaceInstances(const FObjectReplacementMap& Map) -> FTransactionCaptureResult
	{
		auto Result = *this;
		if (auto Valid = Result.Target.FindAndReplaceInstances(Map); !Valid) return Valid;
		if (auto Valid = ReplaceReference(Result.Owner, Map); !Valid) return Valid;
		if (auto Valid = Result.ValidateReferences(); !Valid) return Valid;
		*this = std::move(Result);
		return {};
	}

	auto FActorComponentInstanceData::FindAndReplaceInstances(const FObjectReplacementMap& Map) -> FTransactionCaptureResult
	{ return Source.FindAndReplaceInstances(Map); }

	auto FActorComponentInstanceData::CreateReplacement(const FObjectReplacementMap& Map) const
		-> std::expected<std::shared_ptr<FActorComponentInstanceData>, std::string>
	{
		if (typeid(*this) != typeid(FActorComponentInstanceData))
			return std::unexpected("Derived component instance data must preserve its dynamic type during reinstancing.");
		auto Result = std::make_shared<FActorComponentInstanceData>(*this);
		if (auto Valid = Result->FindAndReplaceInstances(Map); !Valid) return std::unexpected(Valid.error());
		return Result;
	}

	auto FSceneComponentInstanceData::CreateReplacement(const FObjectReplacementMap& Map) const
		-> std::expected<std::shared_ptr<FActorComponentInstanceData>, std::string>
	{
		if (typeid(*this) != typeid(FSceneComponentInstanceData))
			return std::unexpected("Derived scene instance data must preserve its dynamic type during reinstancing.");
		auto Result = std::make_shared<FSceneComponentInstanceData>(*this);
		if (auto Valid = Result->FindAndReplaceInstances(Map); !Valid) return std::unexpected(Valid.error());
		return Result;
	}

	auto FActorRootComponentReconstructionData::FindAndReplaceInstances(const FObjectReplacementMap& Map) -> FTransactionCaptureResult
	{
		auto Result = *this;
		if (auto Valid = ReplaceReference(Result.SourceActor, Map); !Valid) return Valid;
		for (auto* Source : {&Result.Root, &Result.RootParent})
			if (auto Valid = Source->FindAndReplaceInstances(Map); !Valid) return Valid;
		for (auto& Attachment : Result.Attached)
			for (auto* Source : {&Attachment.Target, &Attachment.Parent})
				if (auto Valid = Source->FindAndReplaceInstances(Map); !Valid) return Valid;
		if (auto Valid = Result.Validate(); !Valid) return Valid;
		if (HasOldReferences(Result, Map)) return std::unexpected("Component instance data still retains old-generation references.");
		*this = std::move(Result);
		return {};
	}

	auto FComponentInstanceDataCache::FindAndReplaceInstances(const FObjectReplacementMap& Map) -> FTransactionCaptureResult
	{
		auto Result = *this;
		if (auto Valid = ReplaceReference(Result.SourceActor, Map); !Valid) return Valid;
		for (auto& State : Result.ComponentLifecycle)
			if (auto Valid = State.FindAndReplaceInstances(Map); !Valid) return Valid;
		for (auto& Entry : Result.ComponentsInstanceData)
		{
			if (!Entry) return std::unexpected("Null component instance data cannot be reinstanced.");
			auto Replacement = Entry->CreateReplacement(Map);
			if (!Replacement) return std::unexpected(Replacement.error());
			if (!*Replacement || Replacement->get() == Entry.get() || typeid(**Replacement) != typeid(*Entry))
				return std::unexpected("Component instance reinstancing must return independent data of the same dynamic type.");
			Entry = std::move(*Replacement);
		}
		if (auto Valid = Result.Validate(); !Valid) return Valid;
		if (HasOldReferences(Result, Map)) return std::unexpected("Component instance cache still retains old-generation references.");
		*this = std::move(Result);
		return {};
	}
}
