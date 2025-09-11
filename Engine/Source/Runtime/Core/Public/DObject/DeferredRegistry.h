#pragma once

#include "DObject/Class.h"

template<typename T>
class TDeferredRegistry
{
public:
	using TInfo = T;
	using TType = T::TType;
	using TRegisterFunc = TType* (*)();

	struct FRegistrant
	{
		FName Name;
		TRegisterFunc OuterRegister;
		TRegisterFunc InnerRegister;
		FClassRegistrationInfo* Info;
	};

	static auto Get() -> TDeferredRegistry&
	{
		static TDeferredRegistry Registry;
		return Registry;
	}

	auto AddRegistration(TRegisterFunc InOuterRegister, TRegisterFunc InInnerRegister, const UTF8Char* InName, TInfo& InInfo) -> void
	{
		FRegistrant NewRegistrant;

		NewRegistrant.Name = FName(InName);
		NewRegistrant.OuterRegister = InOuterRegister;
		NewRegistrant.InnerRegister = InInnerRegister;
		NewRegistrant.Info = &InInfo;

		Registrations.push_back(NewRegistrant);
	}

	auto GetRegistrations() -> std::vector<FRegistrant>& { return Registrations; }

	auto ClearRegistrations() -> void { std::vector<FRegistrant>().swap(Registrations); }

private:
	std::vector<FRegistrant> Registrations;
};


using FClassDeferredRegistry = TDeferredRegistry<FClassRegistrationInfo>;

