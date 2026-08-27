#include "AssetTools/ReimportManager.h"

#include "Asset/AssetOperations.h"
#include "DObject/Package.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	namespace
	{
		auto SelectReimportFactory(const DObject& Object, std::string& OutError)
			-> const DFactory*
		{
			const std::vector<const DFactory*> Factories =
				DFactory::FindFactories(Object.GetClass());
			if (Factories.empty())
			{
				OutError = "No factory supports reimport for the selected asset class.";
				return nullptr;
			}
			if (Factories.size() > 1)
			{
				OutError = "Multiple factories support reimport for the selected asset class.";
				return nullptr;
			}
			return Factories.front();
		}

		auto WrapPersistence(DObject& Object, const FReimportOptions& Options,
			FReimportCompletion Completion) -> FReimportCompletion
		{
			return [&Object, Options, Completion = std::move(Completion)](
				FReimportResult Result) mutable {
				if (Result.Succeeded() && Options.bSave)
				{
					DPackage* Package = Object.GetPackage();
					const Asset::FAssetResult Saved = Package
						? Asset::SavePackage(Package)
						: Asset::FAssetResult{Asset::EAssetError::InvalidPath,
							"Only packaged assets can be persisted after reimport."};
					if (!Saved)
						Result = {EReimportStatus::PersistenceFailure, Saved.Message};
				}
				if (Completion) Completion(std::move(Result));
			};
		}
	}

	auto FReimportManager::GetCapabilities(const DObject& Object)
		-> FReimportCapabilities
	{
		checkf(IsInGameThread(), "Reimport capability queries must run on the game thread.");
		std::string Error;
		const DFactory* Factory = SelectReimportFactory(Object, Error);
		if (!Factory) return {.Diagnostic = std::move(Error)};
		return Factory->GetReimportCapabilities(Object);
	}

	auto FReimportManager::Reimport(DObject& Object,
		const FReimportOptions& Options, FReimportCompletion Completion) -> void
	{
		checkf(IsInGameThread(), "Reimport requests must run on the game thread.");
		std::string Error;
		const DFactory* Factory = SelectReimportFactory(Object, Error);
		if (!Factory)
		{
			if (Completion) Completion({EReimportStatus::Unsupported, std::move(Error)});
			return;
		}
		const FReimportCapabilities Capabilities =
			Factory->GetReimportCapabilities(Object);
		if (!Capabilities.bCanReimport)
		{
			if (Completion) Completion({Capabilities.bCanReimportFromFile
				? EReimportStatus::MissingSource : EReimportStatus::Unsupported,
				Capabilities.Diagnostic.empty()
					? "The selected asset cannot be reimported from retained sources."
					: Capabilities.Diagnostic});
			return;
		}
		Factory->FactoryReimport(
			Object, WrapPersistence(Object, Options, std::move(Completion)));
	}

	auto FReimportManager::ReimportFromFiles(DObject& Object,
		std::span<const std::string> Filenames, const FReimportOptions& Options,
		FReimportCompletion Completion) -> void
	{
		checkf(IsInGameThread(), "Reimport requests must run on the game thread.");
		std::string Error;
		const DFactory* Factory = SelectReimportFactory(Object, Error);
		if (!Factory)
		{
			if (Completion) Completion({EReimportStatus::Unsupported, std::move(Error)});
			return;
		}
		const FReimportCapabilities Capabilities =
			Factory->GetReimportCapabilities(Object);
		if (!Capabilities.bCanReimportFromFile)
		{
			if (Completion) Completion({EReimportStatus::Unsupported,
				Capabilities.Diagnostic.empty()
					? "The selected asset cannot be reimported from files."
					: Capabilities.Diagnostic});
			return;
		}
		Factory->FactoryReimportFromFiles(Object, Filenames,
			WrapPersistence(Object, Options, std::move(Completion)));
	}
}
