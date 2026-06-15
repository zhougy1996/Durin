#include "DObject/DObjectGlobals.h"

#include "Modules/ModuleManager.h"
#include "DObject/Class.h"
#include "DObject/DObjectArray.h"
#include "DObject/Property.h"

namespace Durin
{
	namespace
	{
		auto ConstructGeneratedProperty(
			const FFieldVariant& Owner,
			const DurinCodeGen::FPropertyParamsBase* PropertyParams,
			DClass* ReferencedClass
		) -> FProperty*
		{
			switch (PropertyParams->Kind)
			{
			case DurinCodeGen::EPropertyGenFlags::Bool:
				return new FBoolProperty(
					Owner,
					FName(PropertyParams->NameUTF8),
					EObjectFlags::NoFlags,
					PropertyParams->Flags,
					PropertyParams->ArrayDim,
					PropertyParams->Offset,
					PropertyParams->ElementSize,
					PropertyParams->Kind,
					ReferencedClass
				);
			case DurinCodeGen::EPropertyGenFlags::String:
				return new FStringProperty(
					Owner,
					FName(PropertyParams->NameUTF8),
					EObjectFlags::NoFlags,
					PropertyParams->Flags,
					PropertyParams->ArrayDim,
					PropertyParams->Offset,
					PropertyParams->ElementSize,
					PropertyParams->Kind,
					ReferencedClass
				);
			case DurinCodeGen::EPropertyGenFlags::Enum:
				return new FEnumProperty(
					Owner,
					FName(PropertyParams->NameUTF8),
					EObjectFlags::NoFlags,
					PropertyParams->Flags,
					PropertyParams->ArrayDim,
					PropertyParams->Offset,
					PropertyParams->ElementSize,
					PropertyParams->Kind,
					ReferencedClass
				);
			case DurinCodeGen::EPropertyGenFlags::Object:
				return new FObjectProperty(
					Owner,
					FName(PropertyParams->NameUTF8),
					EObjectFlags::NoFlags,
					PropertyParams->Flags,
					PropertyParams->ArrayDim,
					PropertyParams->Offset,
					PropertyParams->ElementSize,
					PropertyParams->Kind,
					ReferencedClass
				);
			case DurinCodeGen::EPropertyGenFlags::Int8:
			case DurinCodeGen::EPropertyGenFlags::Int16:
			case DurinCodeGen::EPropertyGenFlags::Int32:
			case DurinCodeGen::EPropertyGenFlags::Int64:
			case DurinCodeGen::EPropertyGenFlags::UInt8:
			case DurinCodeGen::EPropertyGenFlags::UInt16:
			case DurinCodeGen::EPropertyGenFlags::UInt32:
			case DurinCodeGen::EPropertyGenFlags::UInt64:
			case DurinCodeGen::EPropertyGenFlags::Float:
			case DurinCodeGen::EPropertyGenFlags::Double:
				return new FNumericProperty(
					Owner,
					FName(PropertyParams->NameUTF8),
					EObjectFlags::NoFlags,
					PropertyParams->Flags,
					PropertyParams->ArrayDim,
					PropertyParams->Offset,
					PropertyParams->ElementSize,
					PropertyParams->Kind,
					ReferencedClass
				);
			case DurinCodeGen::EPropertyGenFlags::None:
			default:
				return new FProperty(
					Owner,
					FName(PropertyParams->NameUTF8),
					EObjectFlags::NoFlags,
					PropertyParams->Flags,
					PropertyParams->ArrayDim,
					PropertyParams->Offset,
					PropertyParams->ElementSize,
					PropertyParams->Kind,
					ReferencedClass
				);
			}
		}
	}

	auto FObjectInitializer::Get() -> const FObjectInitializer&
	{
		static thread_local FObjectInitializer Instance;
		return Instance;
	}

	auto DObjectInit() -> void
	{
		ProcessNewlyLoadedDObjects();
		DObjectProcessRegistrants();

		FModuleManager::Get().SetProcessLoadedObjectsCallback(ProcessNewlyLoadedDObjects);
		FModuleManager::Get().StartProcessingNewlyLoadedObjects();

		auto& array = GDObjectArray;
	}

	auto StaticAllocateObject(DClass* Class, DObject* Outer, FName Name, size_t Size) -> DObject*
	{
		// Allocate memory and zero it out
		DObject* Obj = nullptr;
		Obj = static_cast<DObject*>(std::malloc(Size));
		assert(Obj && "Memory allocation failed");
		new (Obj) DObject(Class, Outer, Name);

		return Obj;
	}

	auto StaticConstructObject(const FStaticConstructObjectParameters& Params) -> DObject*
	{
		DObject* Obj = StaticAllocateObject(Params.Class, Params.Outer, Params.Name, Params.Size);

		DClass* InClass = Params.Class;

		assert(InClass && InClass->ClassConstructor);

		FObjectInitializer ObjectInitializer;
		ObjectInitializer.Obj = Obj;
		ObjectInitializer.Class = InClass;
		ObjectInitializer.Outer = Params.Outer;
		ObjectInitializer.Name = Params.Name;

		InClass->ClassConstructor(ObjectInitializer);

		return Obj;
	}


	auto DurinCodeGen::ConstructDClass(const FClassParams& Params) -> DClass*
	{
		DClass* Class = Params.ClassNoRegisterFunc();

		DObjectForceRegistration(Class);

		if (!Class->ChildProperties && Params.PropertyParams && Params.NumProperties > 0)
		{
			FField* LastProperty = nullptr;
			for (size_t Index = 0; Index < Params.NumProperties; ++Index)
			{
				const FPropertyParamsBase* PropertyParams = Params.PropertyParams[Index];
				DClass* ReferencedClass = PropertyParams->ReferencedClassFunc ? PropertyParams->ReferencedClassFunc() : nullptr;
				FProperty* Property = ConstructGeneratedProperty(FFieldVariant(Class), PropertyParams, ReferencedClass);

				if (LastProperty)
				{
					LastProperty->Next = Property;
				}
				else
				{
					Class->ChildProperties = Property;
				}
				LastProperty = Property;
			}
		}
		return Class;
	}
}
