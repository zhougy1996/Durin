#include "DObject/NativeEnum.h"

#include <typeindex>

namespace Durin
{
	namespace
	{
		struct FNativeEnumEntry
		{
			const DurinCodeGen::FNativeEnumRegistration* Registration;
			DEnum* (*Resolver)();
		};
		auto NativeEnums() -> std::unordered_map<std::type_index, FNativeEnumEntry>&
		{
			static std::unordered_map<std::type_index, FNativeEnumEntry> Entries;
			return Entries;
		}
	}

	DurinCodeGen::FNativeEnumRegistration::FNativeEnumRegistration(const std::type_info& InType, DEnum* (*Resolver)())
		: Type(InType)
	{
		require(Resolver);
		const bool Inserted = NativeEnums().emplace(std::type_index(Type), FNativeEnumEntry{this, Resolver}).second;
		require(Inserted);
	}

	DurinCodeGen::FNativeEnumRegistration::~FNativeEnumRegistration()
	{
		auto& Entries = NativeEnums();
		const auto It = Entries.find(std::type_index(Type));
		require(It != Entries.end() && It->second.Registration == this);
		Entries.erase(It);
	}

	auto Private::ResolveNativeEnum(const std::type_info& Type) -> DEnum*
	{
		const auto It = NativeEnums().find(std::type_index(Type));
		return It == NativeEnums().end() ? nullptr : It->second.Resolver();
	}
}
