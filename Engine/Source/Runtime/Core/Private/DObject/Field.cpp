// Created by zhougy on 2025/10/30.
#include "DObject/Field.h"

#include "DObject/Object.h"

namespace Doge
{
	FFieldClass::FFieldClass(const CharT* InCPPName, uint64 InId, uint64 InCastFlags, FFieldClass* InSuperClass, FFieldConstructFuncType InConstructFunc)
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

	auto FFieldClass::GetNameToFieldClassMap() -> std::unordered_map<FName, FFieldClass*>
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
		DOGE_ERROR("FField::Construct should not be called directly.");
		check(false);
		return nullptr;
	}

	auto FFieldVariant::IsA(const DClass* InClass) const -> bool
	{
		return false;
		// TODO: implement IsA
		// return IsDObject() && ToDObjectUnSafe()->IsA(InClass);
	}

	FField::FField(FFieldVariant InOwner, FName InName, EObjectFlags InFlags)
		: Owner(InOwner)
		, Next(nullptr)
		, NamePrivate(InName)
		, FlagsPrivate(InFlags)
		, MetaDataMap(nullptr)
	{
	}
	auto FField::SetMetaData(const FName& InKey, const FString& InValue) -> void
	{
		if (!MetaDataMap)
		{
			MetaDataMap = new std::unordered_map<FName, FString>();
		}
		(*MetaDataMap)[InKey] = InValue;
	}

	auto FField::GetMetaData(const FName& InKey) const -> const FString&
	{
		static const FString EmptyString;

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
