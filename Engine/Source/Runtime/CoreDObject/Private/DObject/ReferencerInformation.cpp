#include "DObject/ReferencerInformation.h"
#include "DObject/Class.h"
#include "DObject/DObjectArray.h"
#include "DObject/Function.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"
#include "DObject/Property.h"
#include "DObject/StrongObjectPtr.h"
#include "GCReferenceSchema.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	auto FindObjectReferencers(DObject* Object, std::span<DObject* const> DeletionRoots,
		FReferencerInformationList& Out, std::span<const FStrongObjectPtr* const> RetainedByModel) -> bool
	{
		CheckGameThread();
		Out = {};
		auto Inside = [](DObject* Value, DObject* Root) {
			for (; Value; Value = Value->GetOuter()) if (Value == Root) return true;
			return false;
		};
		class FCollector final : public FReferenceCollector
		{
		public:
			explicit FCollector(DObject* InTarget) : Target(InTarget) {}
			auto AddReferencedObject(DObject*& Value) -> void override
			{
				if (IncludePackageOwner && Cast<DPackage>(Value) && Target->GetPackage() == Value) { ++Count; return; }
				for (auto* Current = Value; Current; Current = Current->GetOuter())
					if (Current == Target) { ++Count; break; }
			}
			DObject* Target;
			int32 Count = 0;
			bool IncludePackageOwner = false;
		} Collector(Object);
		bool Native = false;
		const auto Objects = GDObjectArray.GetAll(EObjectQueryScope::LiveOnly);
		for (auto* Owner : Objects)
		{
			if (!IsValid(Owner)) continue;
			if (Inside(Owner, Object) || (Cast<DPackage>(Owner) && Inside(Object, Owner)))
			{
				uint32 Excluded = 0;
				std::unordered_set<const FStrongObjectPtr*> Seen;
				for (const auto* Root : RetainedByModel)
					if (Root && Seen.insert(Root).second && Root->Get() == Owner) ++Excluded;
				Native |= Private::GetStrongObjectReferenceCount(FObjectKey(Owner)) > Excluded;
			}
			Collector.Count = 0;
			// A package's export table is ownership, not a referencer to its exports.
			if (Cast<DPackage>(Owner)) ForEachObjectReference(Owner, Collector);
			else Owner->AddReferencedObjects(Collector);
			if (!Collector.Count) continue;
			FReferencerInformation Info{.Referencer = FWeakObjectPtr(Owner), .TotalReferences = Collector.Count};
			Owner->GetClass()->ForEachProperty([&](FProperty* Property) {
				Collector.Count = 0;
				for (uint32 Index = 0; Index < Property->GetArrayDim(); ++Index)
					Private::FGCReferenceSchemaRegistry::VisitProperty(Property, Owner, Index, Collector);
				if (Collector.Count) Info.ReferencingProperties.push_back(Property);
			});
			const bool Internal = std::ranges::any_of(DeletionRoots, [&](auto* Root) { return Inside(Owner, Root); });
			(Internal ? Out.InternalReferences : Out.ExternalReferences).push_back(std::move(Info));
		}
		Collector.Count = 0;
		Collector.IncludePackageOwner = true;
		Private::AddFunctionParameterReferences(Collector);
		return Native || Collector.Count != 0;
	}
}
