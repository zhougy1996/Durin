#include "AssetTools/AssetToolsModule.h"

#include "AssetRegistry/Catalog.h"
#include "Asset/Load.h"
#include "DObject/Class.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"
#include "Factories/Factory.h"
#include "Editor/EditorEngine.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	IMPLEMENT_MODULE(FAssetToolsModule, AssetTools)

	auto MakeRejectedAssetOperation(
		EAssetOperationKind Kind, std::string Message) -> FAssetOperationResult;
	auto DuplicateAssetWithEditorPolicy(const FAssetDuplicateRequest& Request)
		-> FAssetOperationResult;
	auto SaveAssetsWithEditorPolicy(const FAssetSaveRequest& Request)
		-> FAssetOperationResult;
	auto RelocateAssetsWithEditorPolicy(const FAssetRelocationRequest& Request)
		-> FAssetOperationResult;
	auto FixUpRedirectorsWithEditorPolicy(
		const FAssetRedirectorFixupRequest& Request) -> FAssetOperationResult;
	auto PrepareAssetDeletionWithEditorPolicy(
		const FAssetDeletionRequest& Request,
		FAssetDeletionOperation& OutOperation) -> FAssetOperationResult;

	namespace
	{
		auto ValidateFactory(
			const DFactory* Factory,
			const DClass* AssetClass,
			std::string& OutError) -> bool
		{
			if (!Factory)
			{
				OutError = "No factory supports the requested asset.";
				return false;
			}
			const DClass* SupportedClass = Factory->GetSupportedClass();
			if (!SupportedClass || !AssetClass->IsChildOf(SupportedClass))
			{
				OutError = "The selected factory does not support the requested asset class.";
				return false;
			}
			return true;
		}
	}

	class FAssetTools final : public IAssetTools
	{
	public:
		auto CreateAsset(
			const FTopLevelAssetPath& AssetPath,
			DClass* AssetClass,
			const DFactory* Factory,
			DObject* Context,
			EObjectFlags Flags) -> FAssetToolsResult override
		{
			return CreateWithFactory(
				AssetPath, AssetClass, Factory, {}, Context, Flags, false);
		}

		auto ImportAsset(
			const FTopLevelAssetPath& AssetPath,
			DClass* AssetClass,
			std::string_view Filename,
			const DFactory* Factory,
			DObject* Context,
			EObjectFlags Flags) -> FAssetToolsResult override
		{
			return CreateWithFactory(
				AssetPath, AssetClass, Factory, Filename, Context, Flags, true);
		}

		auto DiscardPackage(DPackage* Package) -> bool override
		{
			checkf(IsInGameThread(), "Asset package discard must run on the game thread.");
			if (!Package || !Package->IsAssetPackage()) return false;
			FPackagePath Path;
			if (!FPackagePath::TryCreate(Package->GetPackagePath(), Path)) return false;
			return UnloadPackage(
				Package, EAssetPackageUnloadPolicy::DiscardUnsaved).Succeeded();
		}

		auto DuplicateAsset(const FAssetDuplicateRequest& Request)
			-> FAssetOperationResult override
		{
			return DuplicateAssetWithEditorPolicy(Request);
		}
		auto SaveAssets(const FAssetSaveRequest& Request)
			-> FAssetOperationResult override
		{
			return SaveAssetsWithEditorPolicy(Request);
		}
		auto RelocateAssets(const FAssetRelocationRequest& Request)
			-> FAssetOperationResult override
		{
			return RelocateAssetsWithEditorPolicy(Request);
		}
		auto FixUpRedirectors(const FAssetRedirectorFixupRequest& Request)
			-> FAssetOperationResult override
		{
			return FixUpRedirectorsWithEditorPolicy(Request);
		}
		auto PrepareDeletion(
			const FAssetDeletionRequest& Request,
			FAssetDeletionOperation& OutOperation)
			-> FAssetOperationResult override
		{
			return PrepareAssetDeletionWithEditorPolicy(Request, OutOperation);
		}

	private:
		auto CreateWithFactory(
			const FTopLevelAssetPath& AssetPath,
			DClass* AssetClass,
			const DFactory* RequestedFactory,
			std::string_view Filename,
			DObject* Context,
			EObjectFlags Flags,
			bool bFromFile) -> FAssetToolsResult
		{
			const EAssetOperationKind Kind = bFromFile
				? EAssetOperationKind::Import : EAssetOperationKind::Create;
			checkf(IsInGameThread(), "Asset tools creation must run on the game thread.");
			if (!AssetPath.IsValid())
				return MakeRejectedAssetOperation(Kind, "The destination asset path is invalid.");
			if (!AssetClass || !AssetClass->IsChildOf(DObject::StaticClass())
				|| AssetClass->HasAnyClassFlags(EClassFlags::Abstract))
				return MakeRejectedAssetOperation(
					Kind, "The requested asset class cannot be constructed.");
			if (FindPackage(AssetPath.GetPackagePath().GetView())
				|| FindTopLevelAssetExact(AssetPath))
				return MakeRejectedAssetOperation(Kind, std::format(
					"Asset {} already exists.", AssetPath.ToString()));
			if (bFromFile && Filename.empty())
				return MakeRejectedAssetOperation(
					Kind, "A source filename is required for import.");

			const DFactory* Factory = RequestedFactory;
			if (!Factory && bFromFile)
			{
				const std::string Extension =
					std::filesystem::path(Filename).extension().generic_string();
				std::vector<const DFactory*> Candidates =
					DFactory::FindFactories(AssetClass, Extension);
				if (Candidates.size() > 1)
					return MakeRejectedAssetOperation(Kind, std::format(
						"Multiple factories support {} for the requested asset class.", Extension));
				if (!Candidates.empty()) Factory = Candidates.front();
			}
			if (!Factory) Factory = DFactory::FindFactory(AssetClass);
			std::string Error;
			if ((Factory || bFromFile)
				&& !ValidateFactory(Factory, AssetClass, Error))
				return MakeRejectedAssetOperation(Kind, std::move(Error));

			DPackage* Package = CreatePackage(AssetPath.GetPackagePath());
			if (!Package)
				return MakeRejectedAssetOperation(
					Kind, "The destination package could not be created.");

			FFactoryDiagnostics Diagnostics;
			const FName AssetName(AssetPath.GetAssetName());
			DObject* Asset = nullptr;
			if (bFromFile)
				Asset = Factory->FactoryCreateFromFile(
					AssetClass, Package, AssetName, Flags, Filename, Context, &Diagnostics);
			else if (Factory)
				Asset = Factory->FactoryCreateNew(
					AssetClass, Package, AssetName, Flags, Context, &Diagnostics);
			else
			{
				FStaticConstructObjectParameters Parameters{
					AssetClass, Package, AssetName, AssetClass->PropertiesSize, Flags};
				Asset = StaticConstructObject(Parameters);
				DObjectForceRegistration(Asset);
			}
			if (!Asset)
			{
				DiscardPackage(Package);
				std::string Message = Diagnostics.ToString();
				if (Message.empty()) Message = bFromFile
					? "The factory could not import the asset."
					: "The factory could not create the asset.";
				return MakeRejectedAssetOperation(Kind, std::move(Message));
			}
			if (!Asset->IsA(AssetClass) || Asset->GetOuter() != Package
				|| Package->FindTopLevelAsset(Asset->GetFName()) != Asset)
			{
				DiscardPackage(Package);
				return MakeRejectedAssetOperation(
					Kind, "The factory returned an invalid top-level asset.");
			}
			Package->MarkDirty();
			Package->MarkAsNewlyCreated();
			return {
				.Kind = Kind,
				.Persistence = EAssetOperationPersistenceState::Dirty,
				.AffectedAssets = {AssetPath.GetPackagePath()},
				.Message = Diagnostics.ToString(),
				.Asset = Asset,
				.Package = Package};
		}
	};

	auto FAssetToolsModule::StartupModule() -> void
	{
		check(!AssetTools);
		AssetTools = std::make_unique<FAssetTools>();
	}

	auto FAssetToolsModule::ShutdownModule() -> void
	{
		if (GEditor)
			checkf(GEditor->GetTransactor()
				->DiscardCustomChangesByModule("AssetTools"),
				"AssetTools cannot retire while one of its custom changes is active");
		AssetTools.reset();
	}

	auto FAssetToolsModule::Get() -> IAssetTools&
	{
		check(AssetTools);
		return *AssetTools;
	}

	auto IAssetTools::Get() -> IAssetTools&
	{
		return FAssetToolsModule::GetModule().Get();
	}
}
