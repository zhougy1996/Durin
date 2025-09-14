#pragma once

#include "DObject/ObjectMacros.h"

using FClassRegisterFunc = DClass* (*)();

template<typename T>
struct FRegistrationInfo
{
	using TType = T;

	TType* InnerSingleton = nullptr;
	TType* OuterSingleton = nullptr;
};

using FClassRegistrationInfo = FRegistrationInfo<DClass>;

struct FClassRegisterCompiledInInfo
{
	DClass* (*OuterRegister)();
	DClass* (*InnerRegister)();
	const UTF8Char* Name;
	FClassRegistrationInfo* Info;
};

class DObject
{
	DECLARE_CLASS(DObject, DObject, GetPrivateStaticClass)

public:
	virtual ~DObject() = default;

	auto SetName(FName InName) -> void { NamePrivate = InName; }

	auto GetFName() const -> FName { return NamePrivate; }

	auto GetName() const -> FString { return NamePrivate.ToString(); }

	auto GetClass() const -> DClass* { return ClassPrivate; }

protected:
	CORE_API auto Register(FName InName) -> void;

private:
	static auto GetPrivateStaticClass() -> DClass*;

	// Add a newly created object to the name hash tables and the object array
	CORE_API auto AddObject(FName InName) -> void;

	FName NamePrivate;

	DClass* ClassPrivate;

	friend CORE_API auto DObjectForceRegistration(DObject* Object) -> void;

	friend CORE_API auto Z_Construct_DClass_DObject_NoRegister() -> DClass*;
};

CORE_API auto DObjectForceRegistration(DObject* Object) -> void;

CORE_API auto RegisterCompiledInInfo(FClassRegisterFunc InOuterRegister, FClassRegisterFunc InInnerRegister, const UTF8Char* InName, FClassRegistrationInfo& InInfo) -> void;

CORE_API auto RegisterCompiledInInfo(const FClassRegisterCompiledInInfo* ClassInfo, size_t NumClassInfo) -> void;

CORE_API auto ProcessNewlyLoadedDObjects() -> void;

struct FRegisterCompiledInInfo
{
	template<typename... ArgTypes>
	FRegisterCompiledInInfo(ArgTypes&&... Args)
	{
		RegisterCompiledInInfo(std::forward<ArgTypes>(Args)...);
	}
};
