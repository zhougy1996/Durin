#pragma once

#include "AssetToolsAPI.h"
#include "DObject/Object.h"

#include "Factory.gen.h"

namespace Durin
{
	// Describes which mutation requests one factory can service for a loaded object.
	struct FReimportCapabilities
	{
		bool bCanReimport = false;
		bool bCanReimportFromFile = false;
		std::string Diagnostic;
	};

	// Identifies one terminal reimport outcome independently from persistence.
	enum class EReimportStatus : uint8
	{
		Unsupported,
		MissingSource,
		SourceOrBuildFailure,
		Succeeded,
		PersistenceFailure,
	};

	struct FReimportResult
	{
		EReimportStatus Status = EReimportStatus::Unsupported;
		std::string Message;

		auto Succeeded() const -> bool
		{
			return Status == EReimportStatus::Succeeded;
		}
		explicit operator bool() const { return Succeeded(); }
	};

	// Accepted requests invoke their completion exactly once on the game thread.
	using FReimportCompletion = std::function<void(FReimportResult)>;

	// Collects bounded, per-operation diagnostics without shared mutable state.
	class FFactoryDiagnostics
	{
	public:
		static constexpr size_t MaximumMessageCount = 8;
		static constexpr size_t MaximumMessageLength = 1024;

		ASSETTOOLS_API auto Report(std::string_view Message) -> void;
		auto GetMessages() const -> const std::vector<std::string>& { return Messages; }
		ASSETTOOLS_API auto ToString() const -> std::string;

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
		ASSETTOOLS_API static auto GetAvailableFactories()
			-> std::vector<const DFactory*>;
		ASSETTOOLS_API static auto FindFactory(const DClass* SupportedClass)
			-> const DFactory*;
		ASSETTOOLS_API static auto FindFactories(const DClass* SupportedClass)
			-> std::vector<const DFactory*>;
		ASSETTOOLS_API static auto FindFactoryByExtension(
			std::string_view Extension) -> const DFactory*;
		// Returns every extension candidate so ambiguity can be presented rather
		// than silently resolved. Class-plus-extension lookup additionally filters
		// candidates by the requested class.
		ASSETTOOLS_API static auto FindFactoriesByExtension(
			std::string_view Extension) -> std::vector<const DFactory*>;
		ASSETTOOLS_API static auto FindFactories(
			const DClass* SupportedClass,
			std::string_view Extension) -> std::vector<const DFactory*>;
		ASSETTOOLS_API static auto InvalidateFactoryCache() -> void;

		ASSETTOOLS_API virtual auto FactoryCreateNew(
			DClass* InClass,
			DObject* InParent,
			FName InName,
			EObjectFlags Flags,
			DObject* Context,
			FFactoryDiagnostics* Diagnostics = nullptr) const -> DObject*;

		ASSETTOOLS_API virtual auto FactoryCreateFromFile(
			DClass* InClass,
			DObject* InParent,
			FName InName,
			EObjectFlags Flags,
			std::string_view Filename,
			DObject* Context,
			FFactoryDiagnostics* Diagnostics = nullptr) const -> DObject*;

		ASSETTOOLS_API virtual auto GetReimportCapabilities(
			const DObject& Object) const -> FReimportCapabilities;
		ASSETTOOLS_API virtual auto FactoryReimport(
			DObject& Object,
			FReimportCompletion Completion) const -> void;
		ASSETTOOLS_API virtual auto FactoryReimportFromFiles(
			DObject& Object,
			std::span<const std::string> Filenames,
			FReimportCompletion Completion) const -> void;

	protected:
		ASSETTOOLS_API explicit DFactory(const FObjectInitializer& ObjectInitializer);

		DClass* SupportedClass = nullptr;
		std::vector<std::string> Formats;
	};
}
