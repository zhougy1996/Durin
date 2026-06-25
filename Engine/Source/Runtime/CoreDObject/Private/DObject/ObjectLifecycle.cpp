#include "DObject/ObjectLifecycle.h"

#include "DObject/Class.h"
#include "DObject/DObjectArray.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/Object.h"

namespace Durin
{
	namespace
	{
		auto IsPermanentObject(DObject* Object) -> bool
		{
			if (!Object)
			{
				return true;
			}
			if (EnumHasAnyFlags(Object->GetObjectFlags(), EObjectFlags::Intrinsic))
			{
				return true;
			}
			DClass* ObjectClass = Object->GetClass();
			return ObjectClass && DType::StaticClass() && Object->IsA(DType::StaticClass());
		}

		class FMarkReferenceCollector : public FReferenceCollector
		{
		public:
			auto AddReferencedObject(DObject*& Object) -> void override
			{
				MarkObject(Object);
			}

			auto MarkObject(DObject* Object) -> void
			{
				if (!Object || Object->HasAnyInternalFlags(EObjectInternalFlags::Reachable))
				{
					return;
				}

				Object->SetInternalFlags(EObjectInternalFlags::Reachable);
				Object->AddReferencedObjects(*this);

				for (DObject* InnerObject : Object->GetInnerObjects())
				{
					MarkObject(InnerObject);
				}
			}
		};
	}

	FScopedObjectRoot::FScopedObjectRoot(DObject* InObject)
		: Object(InObject)
	{
		AddToRoot(Object);
	}

	FScopedObjectRoot::~FScopedObjectRoot()
	{
		RemoveFromRoot(Object);
	}

	auto AddToRoot(DObject* Object) -> void
	{
		if (Object)
		{
			Object->SetInternalFlags(EObjectInternalFlags::RootSet);
		}
	}

	auto RemoveFromRoot(DObject* Object) -> void
	{
		if (Object)
		{
			Object->ClearInternalFlags(EObjectInternalFlags::RootSet);
		}
	}

	auto ForEachObjectReference(DObject* Object, FReferenceCollector& Collector) -> void
	{
		if (!Object || !Object->GetClass())
		{
			return;
		}

		Object->GetClass()->ForEachProperty(
			[&](FProperty* Property)
			{
				if (!Property || Property->GetKind() != DurinCodeGen::EPropertyGenFlags::Object)
				{
					return;
				}

				auto* ObjectProperty = static_cast<FObjectProperty*>(Property);
				for (uint32 Index = 0; Index < ObjectProperty->GetArrayDim(); ++Index)
				{
					DObject* ReferencedObject = ObjectProperty->GetObjectPropertyValue(Object, Index);
					Collector.AddReferencedObject(ReferencedObject);
				}
			},
			true
		);
	}

	auto DestroyObject(DObject* Object) -> void
	{
		if (!Object || IsPermanentObject(Object))
		{
			return;
		}
		if (Object->HasAnyInternalFlags(EObjectInternalFlags::BeginDestroyed))
		{
			return;
		}

		Object->SetInternalFlags(EObjectInternalFlags::BeginDestroyed);

		std::vector<DObject*> InnerObjects = Object->GetInnerObjects();
		for (DObject* InnerObject : InnerObjects)
		{
			DestroyObject(InnerObject);
		}

		Object->BeginDestroy();
		Object->SetOuterPrivate(nullptr);
		GDObjectArray.Remove(Object);
		Object->FinishDestroy();
		delete Object;
	}

	auto CollectGarbage() -> void
	{
		for (DObject* Object : GDObjectArray.GetAll())
		{
			if (Object)
			{
				Object->ClearInternalFlags(EObjectInternalFlags::Reachable | EObjectInternalFlags::Garbage);
			}
		}

		FMarkReferenceCollector Marker;
		for (DObject* Object : GDObjectArray.GetAll())
		{
			if (!Object)
			{
				continue;
			}
			if (Object->HasAnyInternalFlags(EObjectInternalFlags::RootSet) || IsPermanentObject(Object))
			{
				Marker.MarkObject(Object);
			}
		}

		std::vector<DObject*> Snapshot = GDObjectArray.Snapshot();
		for (DObject* Object : Snapshot)
		{
			if (!Object || IsPermanentObject(Object))
			{
				continue;
			}
			if (!Object->HasAnyInternalFlags(EObjectInternalFlags::Reachable))
			{
				Object->SetInternalFlags(EObjectInternalFlags::Garbage);
				DestroyObject(Object);
			}
		}

		GDObjectArray.Compact();
	}
}
