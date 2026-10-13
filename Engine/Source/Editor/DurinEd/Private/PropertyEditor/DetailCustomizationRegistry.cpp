#include "PropertyEditor/DetailCustomizationRegistry.h"
#include "DObject/Class.h"

namespace Durin::Editor
{
	auto FDetailCustomizationRegistry::Get() -> FDetailCustomizationRegistry&
	{
		static FDetailCustomizationRegistry Registry;
		return Registry;
	}

	auto FDetailCustomizationRegistry::RegisterDetailCustomization(DClass* Class,
		std::shared_ptr<IDetailCustomization> Customization) -> FDetailCustomizationHandle
	{
		if (!Class || !Customization || Entries.contains(Class)) return {};
		const uint64 Id = NextId++;
		Entries.emplace(Class, FEntry{Id, std::move(Customization)});
		return {Id};
	}

	auto FDetailCustomizationRegistry::Unregister(FDetailCustomizationHandle Handle) -> bool
	{
		if (!Handle) return false;
		const auto It = std::ranges::find_if(Entries, [Handle](const auto& Pair) { return Pair.second.Id == Handle.Id; });
		if (It == Entries.end()) return false;
		Entries.erase(It);
		return true;
	}

	auto FDetailCustomizationRegistry::FindDetailCustomization(const DClass* Class) const -> std::shared_ptr<IDetailCustomization>
	{
		for (const DClass* Current = Class; Current; Current = Current->GetSuperClass())
			if (const auto It = Entries.find(const_cast<DClass*>(Current)); It != Entries.end()) return It->second.Customization;
		return nullptr;
	}

	auto FDetailCustomizationRegistry::FindDetailCustomizations(const DClass* Class) const -> std::vector<std::shared_ptr<IDetailCustomization>>
	{
		std::vector<std::shared_ptr<IDetailCustomization>> Result;
		for (const DClass* Current = Class; Current; Current = Current->GetSuperClass())
			if (const auto It = Entries.find(const_cast<DClass*>(Current)); It != Entries.end()) Result.push_back(It->second.Customization);
		std::ranges::reverse(Result);
		return Result;
	}
}
