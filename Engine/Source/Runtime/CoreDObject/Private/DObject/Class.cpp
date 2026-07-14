#include "DObject/Class.h"

#include "DObject/Property.h"
#include "DObject/DObjectArray.h"

namespace
{
	auto MakeDefaultObjectName(std::string_view ShortName) -> std::string
	{
		const size_t Separator = ShortName.rfind("::");
		if (Separator != std::string_view::npos) ShortName.remove_prefix(Separator + 2);
		if (ShortName.size() >= 2 && (ShortName.front() == 'A' || ShortName.front() == 'D')
			&& std::isupper(static_cast<unsigned char>(ShortName[1])))
		{
			ShortName.remove_prefix(1);
		}
		return std::string(ShortName);
	}

	auto HumanizeTypeName(std::string_view Name) -> std::string
	{
		std::string Result;
		Result.reserve(Name.size() + 8);
		for (size_t Index = 0; Index < Name.size(); ++Index)
		{
			const unsigned char Current = static_cast<unsigned char>(Name[Index]);
			const bool bCurrentUpper = std::isupper(Current) != 0;
			const bool bPreviousLowerOrDigit = Index > 0 && (std::islower(static_cast<unsigned char>(Name[Index - 1])) || std::isdigit(static_cast<unsigned char>(Name[Index - 1])));
			const bool bAcronymBoundary = Index > 0 && Index + 1 < Name.size() && bCurrentUpper
				&& std::isupper(static_cast<unsigned char>(Name[Index - 1])) && std::islower(static_cast<unsigned char>(Name[Index + 1]));
			if (bCurrentUpper && (bPreviousLowerOrDigit || bAcronymBoundary)) Result.push_back(' ');
			Result.push_back(Name[Index]);
		}
		return Result;
	}
}

namespace Durin
{
	COREDOBJECT_API DClass* Z_Construct_DClass_DObject();
	COREDOBJECT_API DClass* Z_Construct_DClass_DType();
	COREDOBJECT_API DClass* Z_Construct_DClass_DStructBase();
	COREDOBJECT_API DClass* Z_Construct_DClass_DClass();
	COREDOBJECT_API DClass* Z_Construct_DClass_DStruct();
	COREDOBJECT_API DClass* Z_Construct_DClass_DEnum();
}

namespace Durin
{
	IMPLEMENT_INTRINSIC_CLASS(DType, COREDOBJECT_API, DObject, COREDOBJECT_API, {})

	IMPLEMENT_INTRINSIC_CLASS(DStructBase, COREDOBJECT_API, DType, COREDOBJECT_API, {})

	IMPLEMENT_INTRINSIC_CLASS(DClass, COREDOBJECT_API, DStructBase, COREDOBJECT_API, {})

	IMPLEMENT_INTRINSIC_CLASS(DStruct, COREDOBJECT_API, DStructBase, COREDOBJECT_API, {})

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

	auto DClass::IsChildOf(const DClass* InClass) const -> bool
	{
		for (const DClass* Class = this; Class; Class = Class->GetSuperClass())
		{
			if (Class == InClass) return true;
		}
		return false;
	}

	auto DClass::SetTypeNames(std::string_view InShortName, std::string_view InDisplayName, std::string_view InDefaultObjectName) -> void
	{
		ShortName = InShortName;
		DefaultObjectName = InDefaultObjectName.empty() ? MakeDefaultObjectName(ShortName) : std::string(InDefaultObjectName);
		DisplayName = InDisplayName.empty() ? HumanizeTypeName(DefaultObjectName) : std::string(InDisplayName);
	}

	auto GetDerivedClasses(const DClass* BaseClass, bool bIncludeBase) -> std::vector<DClass*>
	{
		std::vector<DClass*> Classes;
		if (!BaseClass) return Classes;
		for (DObject* Object : GDObjectArray.GetAll())
		{
			auto* Class = Cast<DClass>(Object);
			if (Class && Class->IsChildOf(BaseClass) && (bIncludeBase || Class != BaseClass)) Classes.push_back(Class);
		}
		std::ranges::sort(Classes, [](const DClass* Left, const DClass* Right) {
			return Left->GetQualifiedName().ToString() < Right->GetQualifiedName().ToString();
		});
		return Classes;
	}

	auto FindStructByQualifiedName(std::string_view QualifiedName) -> DStruct*
	{
		for (DObject* Object : GDObjectArray.GetAll())
		{
			auto* Struct = Cast<DStruct>(Object);
			if (Struct && Struct->GetQualifiedName().ToString() == QualifiedName) return Struct;
		}
		return nullptr;
	}

	template<typename T>
	static auto FindTypeByPath(std::string_view ObjectPath) -> T*
	{
		for (DObject* Object : GDObjectArray.GetAll())
		{
			auto* Type = Cast<T>(Object);
			if (Type && Type->GetObjectPath() == ObjectPath) return Type;
		}
		return nullptr;
	}

	auto FindClassByPath(std::string_view ObjectPath) -> DClass* { return FindTypeByPath<DClass>(ObjectPath); }
	auto FindStructByPath(std::string_view ObjectPath) -> DStruct* { return FindTypeByPath<DStruct>(ObjectPath); }
	auto FindEnumByPath(std::string_view ObjectPath) -> DEnum* { return FindTypeByPath<DEnum>(ObjectPath); }
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

COREDOBJECT_API Durin::DClass* Z_Construct_DClass_Durin_DStruct()
{
	return Durin::Z_Construct_DClass_DStruct();
}

COREDOBJECT_API Durin::DClass* Z_Construct_DClass_Durin_DEnum()
{
	return Durin::Z_Construct_DClass_DEnum();
}
