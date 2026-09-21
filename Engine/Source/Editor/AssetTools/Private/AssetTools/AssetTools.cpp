#include "AssetTools/AssetToolsModule.h"

#include "AssetRegistry/Catalog.h"
#include "Asset/Load.h"
#include "DObject/Class.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"
#include "DObject/StrongObjectPtr.h"
#include "Factories/Factory.h"
#include "Editor/EditorEngine.h"
#include "Import/AssetDestinationValidation.h"
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
		auto RejectImportValidation(EAssetOperationKind Kind, FAssetImportValidation Validation) -> FAssetOperationResult
		{
			auto Result = MakeRejectedAssetOperation(Kind, FormatAssetImportValidation(Validation));
			Result.ImportCause = std::make_shared<FAssetImportValidation>(std::move(Validation));
			return Result;
		}

		auto ValidateFactory(const DFactory* Factory, const DClass* AssetClass) -> EAssetImportError
		{
			if (!Factory) return EAssetImportError::MissingFactory;
			const DClass* SupportedClass = Factory->GetSupportedClass();
			if (!SupportedClass || !AssetClass->IsChildOf(SupportedClass)) return EAssetImportError::UnsupportedFactory;
			return EAssetImportError::None;
		}

		auto InspectImport(const FAssetImportRequest& Item) -> FAssetImportValidation
		{
			auto Reject = [&](EAssetImportError Error) -> FAssetImportValidation {
				return {.Error = Error, .AssetPath = Item.AssetPath.ToString(), .Filename = Item.Filename,
					.ClassName = Item.AssetClass ? Item.AssetClass->GetName() : std::string{}};
			};
			if (!Item.AssetPath.IsValid())
				return Reject(EAssetImportError::Path);
			if (!Item.AssetClass || !Item.AssetClass->IsChildOf(DObject::StaticClass())
				|| Item.AssetClass->HasAnyClassFlags(EClassFlags::Abstract))
				return Reject(EAssetImportError::Class);
			if (Item.Filename.empty())
				return Reject(EAssetImportError::Filename);
			const auto Destination = Editor::InspectAssetDestination(
				Item.AssetPath.GetPackagePath().GetView());
			if (!Destination)
			{
				auto Result = Reject(EAssetImportError::Destination);
				Result.DestinationCause = std::make_shared<Editor::FAssetDestinationValidation>(Destination);
				return Result;
			}
			// Also reject files not yet projected into the catalog.
			std::error_code Error;
			const bool bExists = std::filesystem::exists(Destination.PhysicalPath, Error);
			if (Error) { auto Result = Reject(EAssetImportError::FileInspection); Result.FileCause = Error; return Result; }
			if (bExists) return Reject(EAssetImportError::FileExists);
			const DFactory* Factory = Item.Factory;
			if (!Factory)
			{
				const auto Extension = std::filesystem::path(Item.Filename).extension().generic_string();
				const auto Candidates = DFactory::FindFactories(Item.AssetClass, Extension);
				if (Candidates.size() > 1)
					{ auto Result = Reject(EAssetImportError::AmbiguousFactory); Result.Extension = Extension; Result.Count = Candidates.size(); return Result; }
				if (!Candidates.empty()) Factory = Candidates.front();
			}
			// Preserve class-based fallback for factories with custom source layouts.
			if (!Factory) Factory = DFactory::FindFactory(Item.AssetClass);
			if (auto Error = ValidateFactory(Factory, Item.AssetClass); Error != EAssetImportError::None) return Reject(Error);
			return {.Factory = Factory};
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
			checkf(IsInGameThread(), "Asset import must run on the game thread.");
			const auto Validation = InspectImport({
				AssetPath, AssetClass, std::string(Filename), Factory, Context, Flags});
			if (!Validation)
				return RejectImportValidation(EAssetOperationKind::Import, Validation);
			return CreateWithFactory(
				AssetPath, AssetClass, Validation.Factory, Filename, Context, Flags, true);
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

		auto RecoverPackages(const FPackageReloadRequest& Request)
			-> FPackageReloadOperation override
		{
			checkf(IsInGameThread(), "Asset package recovery must run on the game thread.");
			FPackageReloadRequest Coordinated = Request;
			if (GEditor && GEditor->GetTransactor())
			{
				auto Participant = CreateTransactorReloadParticipant(
					*GEditor->GetTransactor(), Coordinated.Packages);
				if (Participant) Coordinated.Participants.push_back(std::move(Participant));
			}
			return ReloadPackages(Coordinated);
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
			auto Reject = [&](EAssetCreationError Code, DObject* Actual = nullptr) -> FAssetOperationResult {
				FAssetCreationError Error{.Code = Code, .RequestedPath = AssetPath.ToString(),
					.RequestedClass = AssetClass ? AssetClass->GetName() : std::string{}, .Filename = std::string(Filename),
					.ActualPath = Actual ? Actual->GetObjectPath() : std::string{},
					.ActualClass = Actual ? Actual->GetClass()->GetName() : std::string{}};
				auto Result = MakeRejectedAssetOperation(Kind, FormatAssetCreationError(Error));
				Result.CreationCause = std::move(Error);
				return Result;
			};
			checkf(IsInGameThread(), "Asset tools creation must run on the game thread.");
			if (!AssetPath.IsValid())
				return Reject(EAssetCreationError::Path);
			if (!AssetClass || !AssetClass->IsChildOf(DObject::StaticClass())
				|| AssetClass->HasAnyClassFlags(EClassFlags::Abstract))
				return Reject(EAssetCreationError::Class);
			if (FindPackage(AssetPath.GetPackagePath().GetView())
				|| FindTopLevelAssetExact(AssetPath))
				return Reject(EAssetCreationError::Occupied);
			if (bFromFile && Filename.empty())
				return Reject(EAssetCreationError::Filename);

			const DFactory* Factory = RequestedFactory;
			if (!Factory) Factory = DFactory::FindFactory(AssetClass);
			if (Factory || bFromFile)
			{
				if (auto Error = ValidateFactory(Factory, AssetClass); Error != EAssetImportError::None)
					return RejectImportValidation(Kind, {.Error = Error, .AssetPath = AssetPath.ToString(),
						.Filename = std::string(Filename), .ClassName = AssetClass->GetName()});
			}

			DPackage* Package = CreatePackage(AssetPath.GetPackagePath());
			if (!Package)
				return Reject(EAssetCreationError::PackageCreation);

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
				auto Result = Reject(EAssetCreationError::FactoryRejected);
				Result.FactoryCause = std::make_shared<FFactoryDiagnostics>(std::move(Diagnostics));
				if (!Result.FactoryCause->GetEntries().empty()) Result.Message = Result.FactoryCause->ToString();
				DiscardPackage(Package);
				return Result;
			}
			if (!Asset->IsA(AssetClass) || Asset->GetOuter() != Package
				|| Asset->GetFName() != AssetName
				|| Package->FindTopLevelAsset(Asset->GetFName()) != Asset)
			{
				auto Result = Reject(!Asset->IsA(AssetClass) ? EAssetCreationError::ProductType
					: Asset->GetOuter() != Package ? EAssetCreationError::ProductOuter
					: Asset->GetFName() != AssetName ? EAssetCreationError::ProductName
					: EAssetCreationError::ProductRegistration, Asset);
				DiscardPackage(Package);
				return Result;
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

	auto FormatAssetCreationError(const FAssetCreationError& Error) -> std::string
	{
		switch (Error.Code)
		{
		case EAssetCreationError::None: return {};
		case EAssetCreationError::FactoryRejected: return "The factory could not construct the asset.";
		case EAssetCreationError::Path: return "The destination asset path is invalid.";
		case EAssetCreationError::Class: return "The requested asset class cannot be constructed.";
		case EAssetCreationError::Occupied: return std::format("Asset {} already exists.", Error.RequestedPath);
		case EAssetCreationError::Filename: return "A source filename is required for import.";
		case EAssetCreationError::PackageCreation: return "The destination package could not be created.";
		case EAssetCreationError::ProductType: return "The factory returned the wrong asset class.";
		case EAssetCreationError::ProductOuter: return "The factory returned an asset outside its package.";
		case EAssetCreationError::ProductName: return "The factory returned an asset with the wrong name.";
		case EAssetCreationError::ProductRegistration: return "The factory product is not registered as the top-level asset.";
		}
		return {};
	}

	auto FormatAssetImportValidation(const FAssetImportValidation& Result) -> std::string
	{
		switch (Result.Error)
		{
		case EAssetImportError::None: return {};
		case EAssetImportError::Path: return "The destination asset path is invalid.";
		case EAssetImportError::Class: return "The requested asset class cannot be constructed.";
		case EAssetImportError::Filename: return "A source filename is required for import.";
		case EAssetImportError::Destination: return Result.DestinationCause ? Editor::FormatAssetDestinationValidation(*Result.DestinationCause) : "The destination is unavailable.";
		case EAssetImportError::FileInspection: return "The destination package could not be inspected: " + Result.FileCause.message();
		case EAssetImportError::FileExists: return "A package file already occupies the destination.";
		case EAssetImportError::AmbiguousFactory: return std::format("Multiple factories support {} for the requested asset class.", Result.Extension);
		case EAssetImportError::MissingFactory: return "No factory supports the requested asset.";
		case EAssetImportError::UnsupportedFactory: return "The selected factory does not support the requested asset class.";
		case EAssetImportError::DuplicatePackage: return "Multiple imports target the same package.";
		}
		return {};
	}

	auto IAssetTools::InspectImports(std::span<const FAssetImportRequest> Items)
		-> std::vector<FAssetImportValidation>
	{
		checkf(IsInGameThread(), "Asset import inspection must run on the game thread.");
		std::unordered_map<FPackagePath, size_t> Counts;
		for (const auto& Item : Items)
			if (Item.AssetPath.IsValid()) ++Counts[Item.AssetPath.GetPackagePath()];
		std::vector<FAssetImportValidation> Results;
		Results.reserve(Items.size());
		for (const auto& Item : Items)
		{
			if (Item.AssetPath.IsValid() && Counts[Item.AssetPath.GetPackagePath()] > 1)
				Results.push_back({.Error = EAssetImportError::DuplicatePackage, .AssetPath = Item.AssetPath.ToString(),
					.Count = Counts[Item.AssetPath.GetPackagePath()]});
			else Results.push_back(InspectImport(Item));
		}
		return Results;
	}

	auto IAssetTools::ImportAssets(const FAssetImportBatchRequest& Request)
		-> FAssetImportBatchResult
	{
		checkf(IsInGameThread(), "Asset batch import must run on the game thread.");
		// Own request values and pin invocation objects across failed-package GC.
		const FAssetImportBatchRequest Batch = Request;
		std::vector<FStrongObjectPtr> Retained;
		for (const auto& Item : Batch.Items)
		{
			Retained.emplace_back(Item.AssetClass);
			Retained.emplace_back(const_cast<DFactory*>(Item.Factory));
			Retained.emplace_back(Item.Context);
		}
		const auto Validation = InspectImports(Batch.Items);
		for (const auto& Item : Validation)
			Retained.emplace_back(const_cast<DFactory*>(Item.Factory));
		FAssetImportBatchResult Result;
		Result.Items.resize(Batch.Items.size());
		for (size_t Index = 0; Index < Batch.Items.size(); ++Index)
		{
			if (Batch.ShouldCancel && Batch.ShouldCancel())
			{
				for (size_t Remaining = Index; Remaining < Result.Items.size(); ++Remaining)
					Result.Items[Remaining].State = EAssetImportItemState::Canceled;
				break;
			}
			const auto& Item = Batch.Items[Index];
			auto& Output = Result.Items[Index];
			Output.Operation = Validation[Index]
				? ImportAsset(Item.AssetPath, Item.AssetClass, Item.Filename,
					Validation[Index].Factory, Item.Context, Item.Flags)
				: RejectImportValidation(EAssetOperationKind::Import, Validation[Index]);
			Output.State = Output.Operation ? EAssetImportItemState::Accepted : EAssetImportItemState::Rejected;
			if (!Output.Operation && Batch.bStopOnFailure) break;
		}
		return Result;
	}

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
