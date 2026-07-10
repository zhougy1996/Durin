#include "DObject/DObjectGlobals.h"

#include "Modules/ModuleManager.h"
#include "DObject/Class.h"
#include "DObject/DObjectArray.h"
#include "DObject/DurinPropertyTypes.h"

namespace Durin
{
	namespace
	{
		auto ConstructGeneratedProperty(
			const FFieldVariant& Owner,
			const DurinCodeGen::FPropertyParamsBase* PropertyParams
		) -> FProperty*
		{
			DClass* ReferencedClass = PropertyParams->ReferencedClassFunc ? PropertyParams->ReferencedClassFunc() : nullptr;
			DEnum* ReferencedEnum = PropertyParams->ReferencedEnumFunc ? PropertyParams->ReferencedEnumFunc() : nullptr;
			DStruct* ReferencedStruct = PropertyParams->ReferencedStructFunc ? PropertyParams->ReferencedStructFunc() : nullptr;
			FProperty* Property = nullptr;

			switch (PropertyParams->Kind)
			{
			case DurinCodeGen::EPropertyGenFlags::Bool:
				Property = new FBoolProperty(
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
				break;
			case DurinCodeGen::EPropertyGenFlags::String:
				Property = new FStringProperty(
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
				break;
			case DurinCodeGen::EPropertyGenFlags::Enum:
				Property = new FEnumProperty(
					Owner,
					FName(PropertyParams->NameUTF8),
					EObjectFlags::NoFlags,
					PropertyParams->Flags,
					PropertyParams->ArrayDim,
					PropertyParams->Offset,
					PropertyParams->ElementSize,
					PropertyParams->Kind,
					ReferencedClass,
					ReferencedEnum
				);
				break;
			case DurinCodeGen::EPropertyGenFlags::Object:
				Property = new FObjectProperty(
					Owner,
					FName(PropertyParams->NameUTF8),
					EObjectFlags::NoFlags,
					PropertyParams->Flags,
					PropertyParams->ArrayDim,
					PropertyParams->Offset,
					PropertyParams->ElementSize,
					PropertyParams->Kind,
					ReferencedClass,
					PropertyParams->bIsObjectPtrWrapper
				);
				break;
			case DurinCodeGen::EPropertyGenFlags::Struct:
				Property = new FStructProperty(
					Owner,
					FName(PropertyParams->NameUTF8),
					EObjectFlags::NoFlags,
					PropertyParams->Flags,
					PropertyParams->ArrayDim,
					PropertyParams->Offset,
					PropertyParams->ElementSize,
					PropertyParams->Kind,
					ReferencedStruct
				);
				break;
			case DurinCodeGen::EPropertyGenFlags::Array:
				Property = new FArrayProperty(
					Owner,
					FName(PropertyParams->NameUTF8),
					EObjectFlags::NoFlags,
					PropertyParams->Flags,
					PropertyParams->ArrayDim,
					PropertyParams->Offset,
					PropertyParams->ElementSize,
					PropertyParams->Kind,
					ReferencedClass,
					PropertyParams->ArrayHelper
				);
				if (PropertyParams->Inner)
				{
					static_cast<FArrayProperty*>(Property)->SetInner(ConstructGeneratedProperty(FFieldVariant(Property), PropertyParams->Inner));
				}
				break;
			case DurinCodeGen::EPropertyGenFlags::Map:
				Property = new FMapProperty(
					Owner,
					FName(PropertyParams->NameUTF8),
					EObjectFlags::NoFlags,
					PropertyParams->Flags,
					PropertyParams->ArrayDim,
					PropertyParams->Offset,
					PropertyParams->ElementSize,
					PropertyParams->Kind,
					ReferencedClass,
					PropertyParams->MapHelper
				);
				if (PropertyParams->Key)
				{
					static_cast<FMapProperty*>(Property)->SetKeyProp(ConstructGeneratedProperty(FFieldVariant(Property), PropertyParams->Key));
				}
				if (PropertyParams->Value)
				{
					static_cast<FMapProperty*>(Property)->SetValueProp(ConstructGeneratedProperty(FFieldVariant(Property), PropertyParams->Value));
				}
				break;
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
				Property = new FNumericProperty(
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
				break;
			case DurinCodeGen::EPropertyGenFlags::None:
			default:
				Property = new FProperty(
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
				break;
			}

			Property->SetValueAccessors(PropertyParams->MutableValueAccessor, PropertyParams->ConstValueAccessor);
			return Property;
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
		Obj = static_cast<DObject*>(::operator new(Size));
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

		Obj->SetOuterPrivate(Params.Outer);
		Obj->AddObject(Params.Name);

		return Obj;
	}


	auto DurinCodeGen::ConstructDClass(const FClassParams& Params) -> DClass*
	{
		DClass* Class = Params.ClassNoRegisterFunc();

		DObjectForceRegistration(Class);
		Class->SetQualifiedName(FName(Params.QualifiedClassName));

		if (!Class->ChildProperties && Params.PropertyParams && Params.NumProperties > 0)
		{
			FField* LastProperty = nullptr;
			for (size_t Index = 0; Index < Params.NumProperties; ++Index)
			{
				const FPropertyParamsBase* PropertyParams = Params.PropertyParams[Index];
				FProperty* Property = ConstructGeneratedProperty(FFieldVariant(Class), PropertyParams);

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

	auto DurinCodeGen::ConstructDEnum(const FEnumParams& Params) -> DEnum*
	{
		DEnum* Enum = Params.EnumNoRegisterFunc();

		DObjectForceRegistration(Enum);

		return Enum;
	}

	auto DurinCodeGen::ConstructDStruct(const FStructParams& Params) -> DStruct*
	{
		DStruct* Struct = Params.StructNoRegisterFunc();
		DObjectForceRegistration(Struct);
		Struct->SetCppOps(Params.Initialize, Params.Destroy, Params.Copy);
		if (!Struct->ChildProperties && Params.PropertyParams && Params.NumProperties > 0)
		{
			FField* LastProperty = nullptr;
			for (size_t Index = 0; Index < Params.NumProperties; ++Index)
			{
				FProperty* Property = ConstructGeneratedProperty(FFieldVariant(Struct), Params.PropertyParams[Index]);
				if (LastProperty) LastProperty->Next = Property;
				else Struct->ChildProperties = Property;
				LastProperty = Property;
			}
		}
		return Struct;
	}
}
