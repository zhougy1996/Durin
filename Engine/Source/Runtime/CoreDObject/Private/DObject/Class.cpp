#include "DObject/Class.h"

#include "DObject/Property.h"
#include "DObject/DObjectArray.h"
#include "Misc/StringHelper.h"
#include "QualifiedTypeRegistry.h"

namespace
{
	struct FQualifiedTypeRegistry
	{
		std::unordered_map<Durin::FName, Durin::DClass*> Classes;
		std::unordered_map<Durin::FName, Durin::DStruct*> Structs;
		std::unordered_map<Durin::FName, Durin::DEnum*> Enums;
	};

	auto GetQualifiedTypeRegistry() -> FQualifiedTypeRegistry&
	{
		// Reflected types are process-lifetime objects. Function-local storage avoids
		// constructing FNames from global initializers before FNameInit establishes None.
		static FQualifiedTypeRegistry Registry;
		return Registry;
	}

	template<typename T>
	auto RegisterQualifiedType(std::unordered_map<Durin::FName, T*>& Types, Durin::FName QualifiedName, T* Type) -> void
	{
		check(Type);
		check(!QualifiedName.IsNone());
		auto [It, bInserted] = Types.emplace(QualifiedName, Type);
		check((bInserted || It->second == Type) && "Reflected qualified names must be unique.");
	}

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

	auto MakeDefaultDisplayName(std::string_view ShortName, std::string_view ConventionalPrefixes) -> std::string
	{
		const size_t Separator = ShortName.rfind("::");
		if (Separator != std::string_view::npos) ShortName.remove_prefix(Separator + 2);
		if (ShortName.size() >= 2 && ConventionalPrefixes.find(ShortName.front()) != std::string_view::npos
			&& std::isupper(static_cast<unsigned char>(ShortName[1])))
		{
			ShortName.remove_prefix(1);
		}
		return Durin::StringUtils::HumanizeName(ShortName);
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

	DEnum::DEnum(
		EStaticConstructor,
		FName InName,
		FName InQualifiedName,
		FName InShortName,
		std::string_view InDisplayName,
		bool bInIsScoped,
		DurinCodeGen::EEnumUnderlyingType InUnderlyingType,
		uint16 InUnderlyingSize,
		std::vector<FEnumValue> InValues,
		EObjectFlags InFlags
	)
		: DType(EC_StaticConstructor, InFlags)
		, QualifiedName(InQualifiedName)
		, ShortName(InShortName)
		, DisplayName(InDisplayName.empty() ? MakeDefaultDisplayName(InShortName.ToString(), "E") : InDisplayName)
		, bIsScoped(bInIsScoped)
		, UnderlyingType(InUnderlyingType)
		, UnderlyingSize(InUnderlyingSize)
		, Values(std::move(InValues))
	{
		(void)InName;
		for (FEnumValue& Value : Values)
		{
			if (Value.DisplayName.empty()) Value.DisplayName = MakeDefaultDisplayName(Value.Name.ToString(), "");
		}
	}

	auto DEnum::FindValueRecordByName(FName InName) const -> const FEnumValue*
	{
		for (const FEnumValue& Value : Values)
		{
			if (Value.Name == InName) return &Value;
		}
		return nullptr;
	}

	auto DEnum::FindValueRecordByValue(uint64 InValue) const -> const FEnumValue*
	{
		for (const FEnumValue& Value : Values)
		{
			if (Value.Value == InValue) return &Value;
		}
		return nullptr;
	}

	auto DEnum::FindValueByName(FName InName, uint64& OutValue) const -> bool
	{
		const FEnumValue* Value = FindValueRecordByName(InName);
		if (!Value) return false;
		OutValue = Value->Value;
		return true;
	}

	auto DEnum::FindNameByValue(uint64 InValue, FName& OutName) const -> bool
	{
		const FEnumValue* Value = FindValueRecordByValue(InValue);
		if (!Value) return false;
		OutName = Value->Name;
		return true;
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

	namespace Private
	{
		auto UpdateQualifiedClassName(DClass* Class, FName PreviousName) -> void
		{
			auto& Classes = GetQualifiedTypeRegistry().Classes;
			if (!PreviousName.IsNone() && PreviousName != Class->GetQualifiedName())
			{
				auto Previous = Classes.find(PreviousName);
				if (Previous != Classes.end() && Previous->second == Class) Classes.erase(Previous);
			}
			RegisterQualifiedType(Classes, Class->GetQualifiedName(), Class);
		}

		auto RegisterQualifiedStruct(DStruct* Struct) -> void
		{
			RegisterQualifiedType(GetQualifiedTypeRegistry().Structs, Struct->GetQualifiedName(), Struct);
		}

		auto RegisterQualifiedEnum(DEnum* Enum) -> void
		{
			RegisterQualifiedType(GetQualifiedTypeRegistry().Enums, Enum->GetQualifiedName(), Enum);
		}
	}

	auto DClass::SetQualifiedName(FName InQualifiedName) -> void
	{
		FName PreviousName = QualifiedName;
		QualifiedName = InQualifiedName;
		Private::UpdateQualifiedClassName(this, PreviousName);
	}

	auto FindClassByQualifiedName(FName QualifiedName) -> DClass*
	{
		auto& Classes = GetQualifiedTypeRegistry().Classes;
		auto It = Classes.find(QualifiedName);
		return It != Classes.end() ? It->second : nullptr;
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
		DisplayName = InDisplayName.empty() ? MakeDefaultDisplayName(DefaultObjectName, "") : std::string(InDisplayName);
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

	auto FindStructByQualifiedName(FName QualifiedName) -> DStruct*
	{
		auto& Structs = GetQualifiedTypeRegistry().Structs;
		auto It = Structs.find(QualifiedName);
		return It != Structs.end() ? It->second : nullptr;
	}

	auto FindEnumByQualifiedName(FName QualifiedName) -> DEnum*
	{
		auto& Enums = GetQualifiedTypeRegistry().Enums;
		auto It = Enums.find(QualifiedName);
		return It != Enums.end() ? It->second : nullptr;
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
