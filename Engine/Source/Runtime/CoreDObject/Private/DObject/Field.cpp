// Created by zhougy on 2025/10/30.
#include "DObject/Field.h"

#include "DObject/Object.h"
#include "DObject/StructBase.h"

namespace Durin
{
	FFieldClass::FFieldClass(const char* InCPPName, uint64 InId, uint64 InCastFlags, FFieldClass* InSuperClass, FFieldConstructFuncType InConstructFunc)
		: Id(InId)
		, CastFlags(InCastFlags)
		, ClassFlags(EClassFlags::None)
		, SuperClass(InSuperClass)
		, ConstructFunc(InConstructFunc)
	{
		check(InCPPName);
		// Skip the "F" prefix
		check(InCPPName[0] == 'F');
		Name = ++InCPPName;

		GetAllFieldClasses().push_back(this);
		GetNameToFieldClassMap().emplace(Name, this);
	}

	auto FFieldClass::GetAllFieldClasses() -> std::vector<FFieldClass*>&
	{
		static std::vector<FFieldClass*> AllFieldClasses;
		return AllFieldClasses;
	}

	auto FFieldClass::GetNameToFieldClassMap() -> std::unordered_map<FName, FFieldClass*>&
	{
		static std::unordered_map<FName, FFieldClass*> NameToFieldClassMap;
		return NameToFieldClassMap;
	}

	auto FField::StaticClass() -> FFieldClass*
	{
		static FFieldClass StaticFieldClass(STR("FField"), FField::StaticClassCastFlagsPrivate(), FField::StaticClassCastFlags(), nullptr, nullptr);
		return &StaticFieldClass;
	}
	FField* FField::Construct(const FFieldVariant& InOwner, const FName& InName, EObjectFlags InFlags /*= EObjectFlags::NoFlags*/)
	{
		DURIN_ERROR("FField::Construct should not be called directly.");
		check(false);
		return nullptr;
	}

	auto FFieldVariant::IsA(const DClass* InClass) const -> bool
	{
		return IsDObject() && ToDObjectUnSafe()->IsA(InClass);
	}

	FField::FField(FFieldVariant InOwner, FName InName, EObjectFlags InFlags)
		: Owner(InOwner)
		, Next(nullptr)
		, NamePrivate(InName)
		, FlagsPrivate(InFlags)
		, MetaDataMap(nullptr)
	{
	}
	FField::~FField()
	{
		delete MetaDataMap;
	}

	auto FField::CheckMutable() const -> void
	{
		const FField* Field = this;
		uint32 Depth = 0;
		while (Field)
		{
			require(++Depth <= 128);
			const auto Owner = Field->GetOwnerVariant();
			if (auto* Object = Owner.ToDObject())
			{
				if (auto* Scope = Cast<DStructBase>(Object)) Scope->CheckPropertiesMutable();
				return;
			}
			Field = Owner.ToField();
		}
	}

	auto FField::SetNext(FField* InNext) -> void
	{
		CheckMutable();
		for (auto* Field = InNext; Field; Field = Field->GetNext()) require(Field != this);
		Next = InNext;
	}

	auto FField::SetOwner(FFieldVariant InOwner) -> void
	{
		CheckMutable();
		requiref(!Owner.IsValid() || (Owner.ToDObject() == InOwner.ToDObject() && Owner.ToField() == InOwner.ToField()),
			"Field '{}' already belongs to another metadata scope.", NamePrivate.ToString());
		for (auto* Field = InOwner.ToField(); Field; Field = Field->GetOwnerVariant().ToField())
			requiref(Field != this, "Field ownership cycle.");
		if (auto* Field = InOwner.ToField()) Field->CheckMutable();
		else if (auto* Scope = Cast<DStructBase>(InOwner.ToDObject())) Scope->CheckPropertiesMutable();
		Owner = InOwner;
	}

	auto FField::SetMetaData(const FName& InKey, const std::string& InValue) -> void
	{
		CheckMutable();
		if (!MetaDataMap)
		{
			MetaDataMap = new std::unordered_map<FName, std::string>();
		}
		(*MetaDataMap)[InKey] = InValue;
	}

	auto FField::GetMetaData(const FName& InKey) const -> const std::string&
	{
		static const std::string EmptyString;

		if (InKey.IsNone() || !MetaDataMap)
		{
			return EmptyString;
		}

		auto It = MetaDataMap->find(InKey);
		if (It != MetaDataMap->end())
		{
			return It->second;
		}
		return EmptyString;
	}
}
