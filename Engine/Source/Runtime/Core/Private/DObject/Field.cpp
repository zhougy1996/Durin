// Created by zhougy on 2025/10/30.
#include "DObject/Field.h"

#include "DObject/Object.h"

FFieldClass::FFieldClass(const CharT* InCPPName, uint64 InId, uint64 InCastFlags, FFieldClass* InSuperClass, FFieldConstructFunc InConstructFunc)
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
{
}
