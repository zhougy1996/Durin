#pragma once

#include "DurinEdAPI.h"
#include "DObject/Object.h"

#include "Factory.gen.h"

namespace Durin
{
	// Defines the editor extension point for constructing managed objects from new or file-backed inputs.
	DCLASS(Abstract)
	class DFactory : public DObject
	{
		GENERATED_BODY()

	public:
		auto GetSupportedClass() const -> DClass* { return SupportedClass; }
		auto GetFormats() const -> const std::vector<std::string>& { return Formats; }

		// Discovers concrete reflected factories through their immutable
		// class-default objects. Factory discovery and cache access are confined
		// to the editor game thread.
		DURINED_API static auto GetAvailableFactories()
			-> std::vector<const DFactory*>;
		// Finds the one factory whose SupportedClass exactly matches the input.
		// An absent or ambiguous mapping returns null.
		DURINED_API static auto FindFactory(const DClass* SupportedClass)
			-> const DFactory*;
		// Finds one factory by a case-insensitive extension with optional leading
		// dots. An absent or ambiguous mapping returns null.
		DURINED_API static auto FindFactoryByExtension(
			std::string_view Extension) -> const DFactory*;
		// Drops the current snapshot so the next query rediscovers reflected
		// factories.
		DURINED_API static auto InvalidateFactoryCache() -> void;

		DURINED_API virtual auto FactoryCreateNew(
			DClass* InClass,
			DObject* InParent,
			FName InName,
			EObjectFlags Flags,
			DObject* Context) -> DObject*;

		DURINED_API virtual auto FactoryCreateFromFile(
			DClass* InClass,
			DObject* InParent,
			FName InName,
			EObjectFlags Flags,
			std::string_view Filename,
			DObject* Context) -> DObject*;

	protected:
		DURINED_API explicit DFactory(const FObjectInitializer& ObjectInitializer);

		DClass* SupportedClass = nullptr;
		std::vector<std::string> Formats;
	};
}
