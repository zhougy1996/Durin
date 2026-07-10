#include "DObject/ObjectLifecycle.h"

#include "DObject/Class.h"
#include "DObject/DObjectArray.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/Object.h"

namespace Durin
{
	namespace
	{
		auto ForEachPropertyReference(FProperty* Property, void* Container, uint32 ArrayIndex, FReferenceCollector& Collector) -> void
		{
			if (!Property)
			{
				return;
			}

			if (Property->GetKind() == DurinCodeGen::EPropertyGenFlags::Object)
			{
				auto* ObjectProperty = static_cast<FObjectProperty*>(Property);
				if (!ObjectProperty->IsObjectPtrWrapper())
				{
					return;
				}

				DObject* ReferencedObject = ObjectProperty->GetObjectPropertyValue(Container, ArrayIndex);
				Collector.AddReferencedObject(ReferencedObject);
				return;
			}

			if (Property->GetKind() == DurinCodeGen::EPropertyGenFlags::Array)
			{
				auto* ArrayProperty = static_cast<FArrayProperty*>(Property);
				FProperty* Inner = ArrayProperty->GetInner();
				if (!Inner || !ArrayProperty->HasArrayHelper())
				{
					return;
				}

				const uint64 Num = ArrayProperty->Num(Container, ArrayIndex);
				for (uint64 Index = 0; Index < Num; ++Index)
				{
					void* Element = ArrayProperty->GetMutableElementPtr(Container, Index, ArrayIndex);
					ForEachPropertyReference(Inner, Element, 0, Collector);
				}
				return;
			}

			if (Property->GetKind() == DurinCodeGen::EPropertyGenFlags::Struct)
			{
				auto* StructProperty = static_cast<FStructProperty*>(Property);
				DStruct* Struct = StructProperty->GetStruct();
				if (!Struct) return;
				void* StructValue = Property->GetValuePtr(Container, ArrayIndex);
				Struct->ForEachProperty([&](FProperty* Field) {
					for (uint32 Index = 0; Field && Index < Field->GetArrayDim(); ++Index)
					{
						ForEachPropertyReference(Field, StructValue, Index, Collector);
					}
				}, false);
				return;
			}

			if (Property->GetKind() == DurinCodeGen::EPropertyGenFlags::Map)
			{
				auto* MapProperty = static_cast<FMapProperty*>(Property);
				if (!MapProperty->HasMapHelper()) return;
				const uint64 Num = MapProperty->Num(Container, ArrayIndex);
				for (uint64 Index = 0; Index < Num; ++Index)
				{
					ForEachPropertyReference(MapProperty->GetKeyProp(), const_cast<void*>(MapProperty->GetKeyPtr(Container, Index, ArrayIndex)), 0, Collector);
					ForEachPropertyReference(MapProperty->GetValueProp(), const_cast<void*>(MapProperty->GetMappedValuePtr(Container, Index, ArrayIndex)), 0, Collector);
				}
			}
		}

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

	COREDOBJECT_API auto ConditionallyMarkAsReachable(DObject* Object) -> void
	{
		(void)Object;
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
				if (!Property)
				{
					return;
				}

				for (uint32 Index = 0; Index < Property->GetArrayDim(); ++Index)
				{
					ForEachPropertyReference(Property, Object, Index, Collector);
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
		Object->BeginDestroy();

		std::vector<DObject*> InnerObjects = Object->GetInnerObjects();
		for (DObject* InnerObject : InnerObjects)
		{
			DestroyObject(InnerObject);
		}

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
