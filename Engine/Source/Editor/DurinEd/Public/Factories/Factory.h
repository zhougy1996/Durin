#pragma once

#include "DurinEdAPI.h"
#include "DObject/Object.h"

#include "Factory.gen.h"

namespace Durin
{
	// Collects bounded, per-operation diagnostics without shared mutable state.
	class FFactoryDiagnostics
	{
	public:
		static constexpr size_t MaximumMessageCount = 8;
		static constexpr size_t MaximumMessageLength = 1024;

		DURINED_API auto Report(std::string_view Message) -> void;
		auto GetMessages() const -> const std::vector<std::string>& { return Messages; }
		DURINED_API auto ToString() const -> std::string;

	private:
		std::vector<std::string> Messages;
	};

	// Defines the editor extension point for constructing managed objects from new or file-backed inputs.
	DCLASS(Abstract)
	class DFactory : public DObject
	{
		GENERATED_BODY()

	public:
		auto GetSupportedClass() const -> DClass* { return SupportedClass; }
		auto GetFormats() const -> const std::vector<std::string>& { return Formats; }

		// Discovery returns immutable class-default objects. Per-operation settings
		// belong on a transient factory instance supplied explicitly by the caller.
		DURINED_API static auto GetAvailableFactories()
			-> std::vector<const DFactory*>;
		DURINED_API static auto FindFactory(const DClass* SupportedClass)
			-> const DFactory*;
		DURINED_API static auto FindFactories(const DClass* SupportedClass)
			-> std::vector<const DFactory*>;
		DURINED_API static auto FindFactoryByExtension(
			std::string_view Extension) -> const DFactory*;
		// Returns every extension candidate so ambiguity can be presented rather
		// than silently resolved. Class-plus-extension lookup additionally filters
		// candidates by the requested class.
		DURINED_API static auto FindFactoriesByExtension(
			std::string_view Extension) -> std::vector<const DFactory*>;
		DURINED_API static auto FindFactories(
			const DClass* SupportedClass,
			std::string_view Extension) -> std::vector<const DFactory*>;
		DURINED_API static auto InvalidateFactoryCache() -> void;

		DURINED_API virtual auto FactoryCreateNew(
			DClass* InClass,
			DObject* InParent,
			FName InName,
			EObjectFlags Flags,
			DObject* Context,
			FFactoryDiagnostics* Diagnostics) const -> DObject*;

		DURINED_API virtual auto FactoryCreateFromFile(
			DClass* InClass,
			DObject* InParent,
			FName InName,
			EObjectFlags Flags,
			std::string_view Filename,
			DObject* Context,
			FFactoryDiagnostics* Diagnostics) const -> DObject*;

	protected:
		DURINED_API explicit DFactory(const FObjectInitializer& ObjectInitializer);

		DClass* SupportedClass = nullptr;
		std::vector<std::string> Formats;
	};
}
