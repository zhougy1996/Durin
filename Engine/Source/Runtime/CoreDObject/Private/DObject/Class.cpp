#include "DObject/Class.h"

#include "DObject/Property.h"
#include "DObject/DObjectArray.h"

namespace Durin
{
	COREDOBJECT_API DClass* Z_Construct_DClass_DObject();
	COREDOBJECT_API DClass* Z_Construct_DClass_DType();
	COREDOBJECT_API DClass* Z_Construct_DClass_DStructBase();
	COREDOBJECT_API DClass* Z_Construct_DClass_DClass();
	COREDOBJECT_API DClass* Z_Construct_DClass_DEnum();
}

namespace Durin
{
	IMPLEMENT_INTRINSIC_CLASS(DType, COREDOBJECT_API, DObject, COREDOBJECT_API, {})

	IMPLEMENT_INTRINSIC_CLASS(DStructBase, COREDOBJECT_API, DType, COREDOBJECT_API, {})

	IMPLEMENT_INTRINSIC_CLASS(DClass, COREDOBJECT_API, DStructBase, COREDOBJECT_API, {})

	IMPLEMENT_INTRINSIC_CLASS(DEnum, COREDOBJECT_API, DType, COREDOBJECT_API, {})

	auto DStructBase::RegisterDependencies() -> void
	{
		if (SuperStructBase)
		{
			SuperStructBase->RegisterDependencies();
		}
	}

	auto DStructBase::ForEachProperty(const std::function<void(FProperty*)>& Visitor, bool bIncludeSuper) const -> void
	{
		if (bIncludeSuper && SuperStructBase)
		{
			SuperStructBase->ForEachProperty(Visitor, true);
		}

		for (FField* Field = ChildProperties; Field; Field = Field->Next)
		{
			Visitor(static_cast<FProperty*>(Field));
		}
	}

	auto DStructBase::FindPropertyByName(FName InName, bool bIncludeSuper) const -> FProperty*
	{
		FProperty* FoundProperty = nullptr;
		ForEachProperty(
			[&](FProperty* Property)
			{
				if (!FoundProperty && Property->NamePrivate == InName)
				{
					FoundProperty = Property;
				}
			},
			bIncludeSuper
		);
		return FoundProperty;
	}

	auto DEnum::FindValueByName(FName InName, int64& OutValue) const -> bool
	{
		for (const FEnumValue& Value : Values)
		{
			if (Value.Name == InName)
			{
				OutValue = Value.Value;
				return true;
			}
		}
		return false;
	}

	auto DEnum::FindNameByValue(int64 InValue, FName& OutName) const -> bool
	{
		for (const FEnumValue& Value : Values)
		{
			if (Value.Value == InValue)
			{
				OutName = Value.Name;
				return true;
			}
		}
		return false;
	}

	auto DEnum::ForEachValue(const std::function<void(const FEnumValue&)>& Visitor) const -> void
	{
		for (const FEnumValue& Value : Values)
		{
			Visitor(Value);
		}
	}

	auto GetPrivateStaticClassBody(
		const char* PackageName,
		const char* Name,
		DClass*& ReturnClass,
		void(*RegisterNativeFunc)(),
		uint32 InSize,
		uint32 InAlignment,
		EClassFlags InClassFlags,
		DClass::ClassConstructorType InClassConstructor,
		DClass::StaticClassFunctionType InSuperClassFn
	) -> DClass*
	{
		auto* Class = new DClass(
			EC_StaticConstructor,
			FName(Name),
			InSize,
			InAlignment,
			EObjectFlags::Intrinsic,
			InClassFlags,
			EClassCastFlags::DClass,
			InClassConstructor
		);

		ReturnClass = Class; // assign before setting superclass to handle circular dependencies

		DClass* SuperClass = InSuperClassFn ? InSuperClassFn() : nullptr;
		Class->SetSuperStructBase(SuperClass);

		Class->Register(DClass::StaticClass, PackageName, Name);

		return Class;
	}

	auto FindClassByQualifiedName(std::string_view QualifiedName) -> DClass*
	{
		for (DObject* Object : GDObjectArray.GetAll())
		{
			auto* Class = Cast<DClass>(Object);
			if (Class && Class->GetQualifiedName().ToString() == QualifiedName) return Class;
		}
		return nullptr;
	}
}

COREDOBJECT_API Durin::DClass* Z_Construct_DClass_Durin_DObject()
{
	return Durin::Z_Construct_DClass_DObject();
}

COREDOBJECT_API Durin::DClass* Z_Construct_DClass_Durin_DType()
{
	return Durin::Z_Construct_DClass_DType();
}

COREDOBJECT_API Durin::DClass* Z_Construct_DClass_Durin_DStructBase()
{
	return Durin::Z_Construct_DClass_DStructBase();
}

COREDOBJECT_API Durin::DClass* Z_Construct_DClass_Durin_DClass()
{
	return Durin::Z_Construct_DClass_DClass();
}

COREDOBJECT_API Durin::DClass* Z_Construct_DClass_Durin_DEnum()
{
	return Durin::Z_Construct_DClass_DEnum();
}
