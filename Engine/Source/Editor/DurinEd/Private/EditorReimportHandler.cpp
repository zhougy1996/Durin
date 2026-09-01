#include "EditorReimportHandler.h"

#include "Asset/AssetOperations.h"
#include "DObject/Object.h"
#include "DObject/Package.h"
#include "Factories/Factory.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	namespace
	{
		struct FReimportHandlerRegistry
		{
			std::vector<FReimportHandler*> Handlers;
			bool bNeedsSorting = false;
		};

		auto GetReimportHandlerRegistry() -> FReimportHandlerRegistry&
		{
			// Handler destructors may run during static teardown, so registry storage
			// intentionally remains valid until process termination.
			static FReimportHandlerRegistry* Registry = new FReimportHandlerRegistry;
			return *Registry;
		}

		auto IsClassDefaultHandler(const FReimportHandler* Handler) -> bool
		{
			const auto* Object = dynamic_cast<const DObject*>(Handler);
			return Object && Object->IsClassDefaultObject();
		}

		auto GetSortedHandlers() -> const std::vector<FReimportHandler*>&
		{
			// Materialize reflected factory CDOs so their handler bases can register.
			(void)DFactory::GetAvailableFactories();
			FReimportHandlerRegistry& Registry = GetReimportHandlerRegistry();
			if (Registry.bNeedsSorting)
			{
				std::stable_sort(Registry.Handlers.begin(), Registry.Handlers.end(),
					[](const FReimportHandler* Left, const FReimportHandler* Right) {
						if (Left->GetPriority() != Right->GetPriority())
							return Left->GetPriority() > Right->GetPriority();
						return IsClassDefaultHandler(Left)
							&& !IsClassDefaultHandler(Right);
					});
				Registry.bNeedsSorting = false;
			}
			return Registry.Handlers;
		}

		auto SelectReimportHandler(const DObject& Object, bool bFromFiles,
			FReimportCapabilities& OutCapabilities) -> const FReimportHandler*
		{
			for (const FReimportHandler* Handler : GetSortedHandlers())
			{
				FReimportCapabilities Capabilities =
					Handler->GetReimportCapabilities(Object);
				if (bFromFiles ? Capabilities.bCanReimportFromFile
					: Capabilities.bCanReimport)
				{
					OutCapabilities = std::move(Capabilities);
					return Handler;
				}
			}
			OutCapabilities.Diagnostic =
				"No registered handler supports reimport for the selected object.";
			return nullptr;
		}

		auto WrapPersistence(DObject& Object, const FReimportOptions& Options,
			FReimportCompletion Completion) -> FReimportCompletion
		{
			return [&Object, Options, Completion = std::move(Completion)](
				FReimportResult Result) mutable {
				if (Result.Succeeded() && Options.bSave)
				{
					DPackage* Package = Object.GetPackage();
					const FAssetResult Saved = Package
						? SavePackage(Package)
						: FAssetResult{EAssetError::InvalidPath,
							"Only packaged assets can be persisted after reimport."};
					if (!Saved)
						Result = {EReimportStatus::PersistenceFailure, Saved.Message};
				}
				if (Completion) Completion(std::move(Result));
			};
		}
	}

	FReimportHandler::FReimportHandler()
	{
		FReimportManager::RegisterHandler(*this);
	}

	FReimportHandler::~FReimportHandler()
	{
		FReimportManager::UnregisterHandler(*this);
	}

	auto FReimportManager::RegisterHandler(FReimportHandler& Handler) -> void
	{
		FReimportHandlerRegistry& Registry = GetReimportHandlerRegistry();
		if (std::ranges::find(Registry.Handlers, &Handler) != Registry.Handlers.end())
			return;
		Registry.Handlers.push_back(&Handler);
		Registry.bNeedsSorting = true;
	}

	auto FReimportManager::UnregisterHandler(FReimportHandler& Handler) -> void
	{
		FReimportHandlerRegistry& Registry = GetReimportHandlerRegistry();
		std::erase(Registry.Handlers, &Handler);
	}

	auto FReimportManager::GetCapabilities(const DObject& Object)
		-> FReimportCapabilities
	{
		checkf(IsInGameThread(), "Reimport capability queries must run on the game thread.");
		FReimportCapabilities RetainedCapabilities;
		if (SelectReimportHandler(Object, false, RetainedCapabilities))
			return RetainedCapabilities;

		FReimportCapabilities FileCapabilities;
		if (SelectReimportHandler(Object, true, FileCapabilities))
			return FileCapabilities;
		return RetainedCapabilities.Diagnostic.empty()
			? std::move(FileCapabilities)
			: std::move(RetainedCapabilities);
	}

	auto FReimportManager::Reimport(DObject& Object,
		const FReimportOptions& Options, FReimportCompletion Completion) -> void
	{
		checkf(IsInGameThread(), "Reimport requests must run on the game thread.");
		FReimportCapabilities Capabilities;
		const FReimportHandler* Handler =
			SelectReimportHandler(Object, false, Capabilities);
		if (!Handler)
		{
			FReimportCapabilities FileCapabilities;
			const bool bCanReimportFromFile =
				SelectReimportHandler(Object, true, FileCapabilities) != nullptr;
			if (Completion) Completion({bCanReimportFromFile
				? EReimportStatus::MissingSource : EReimportStatus::Unsupported,
				bCanReimportFromFile
					? std::move(FileCapabilities.Diagnostic)
					: std::move(Capabilities.Diagnostic)});
			return;
		}
		Handler->Reimport(
			Object, WrapPersistence(Object, Options, std::move(Completion)));
	}

	auto FReimportManager::ReimportFromFiles(DObject& Object,
		std::span<const std::string> Filenames, const FReimportOptions& Options,
		FReimportCompletion Completion) -> void
	{
		checkf(IsInGameThread(), "Reimport requests must run on the game thread.");
		FReimportCapabilities Capabilities;
		const FReimportHandler* Handler =
			SelectReimportHandler(Object, true, Capabilities);
		if (!Handler)
		{
			if (Completion) Completion({EReimportStatus::Unsupported,
				std::move(Capabilities.Diagnostic)});
			return;
		}
		Handler->ReimportFromFiles(Object, Filenames,
			WrapPersistence(Object, Options, std::move(Completion)));
	}
}
