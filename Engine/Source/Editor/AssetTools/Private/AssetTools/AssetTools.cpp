#include "AssetTools/AssetToolsModule.h"
#include "Asset/PackageUnloading.h"

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

	auto DuplicateAssetWithEditorPolicy(const FAssetDuplicateRequest&, FFeedbackContext*) -> DObject*;
	auto SaveAssetsWithEditorPolicy(const FAssetSaveRequest&, FFeedbackContext*) -> bool;
	auto RelocateAssetsWithEditorPolicy(const FAssetRelocationRequest&, FFeedbackContext*) -> bool;
	auto FixUpRedirectorsWithEditorPolicy(const FAssetRedirectorFixupRequest&, FFeedbackContext*) -> bool;
	auto PrepareAssetDeletionWithEditorPolicy(const FAssetDeletionRequest&, FAssetDeletionOperation&, FFeedbackContext*) -> bool;

	namespace
	{
		auto Feedback(FFeedbackContext* Warn) -> FFeedbackContext& { return Warn ? *Warn : FFeedbackContext::Get(); }
		auto Reject(FFeedbackContext* Warn, std::string_view Message) -> DObject*
		{
			Feedback(Warn).Error(Message);
			return nullptr;
		}
		auto ValidateClass(DClass* Class) -> bool
		{
			return Class && Class->IsChildOf(DObject::StaticClass()) && !Class->HasAnyClassFlags(EClassFlags::Abstract);
		}
		auto FindImportFactory(const FAssetImportRequest& Item, FFeedbackContext* Warn) -> const DFactory*
		{
			if (!Item.AssetPath.IsValid()) { Reject(Warn, "The destination asset path is invalid."); return nullptr; }
			if (!ValidateClass(Item.AssetClass)) { Reject(Warn, "The requested asset class cannot be constructed."); return nullptr; }
			if (Item.Filename.empty()) { Reject(Warn, "A source filename is required for import."); return nullptr; }
			const auto Destination = Editor::InspectAssetDestination(Item.AssetPath.GetPackagePath().GetView());
			if (!Destination) { Reject(Warn, Editor::FormatAssetDestinationValidation(Destination)); return nullptr; }
			std::error_code Error;
			const bool bExists = std::filesystem::exists(Destination.PhysicalPath, Error);
			if (Error) { Reject(Warn, "The destination package could not be inspected: " + Error.message()); return nullptr; }
			if (bExists) { Reject(Warn, "A package file already occupies the destination."); return nullptr; }
			const DFactory* Factory = Item.Factory;
			if (!Factory)
			{
				const auto Extension = std::filesystem::path(Item.Filename).extension().generic_string();
				const auto Candidates = DFactory::FindFactories(Item.AssetClass, Extension);
				if (Candidates.size() > 1)
				{
					Reject(Warn, std::format("Multiple factories support {} for the requested asset class.", Extension));
					return nullptr;
				}
				if (!Candidates.empty()) Factory = Candidates.front();
			}
			if (!Factory) Factory = DFactory::FindFactory(Item.AssetClass);
			if (!Factory) { Reject(Warn, "No factory supports the requested asset."); return nullptr; }
			const DClass* Supported = Factory->GetSupportedClass();
			if (!Supported || !Item.AssetClass->IsChildOf(Supported))
			{
				Reject(Warn, "The selected factory does not support the requested asset class.");
				return nullptr;
			}
			return Factory;
		}
	}

	class FAssetTools final : public IAssetTools
	{
	public:
		auto CreateAsset(const FTopLevelAssetPath& Path, DClass* Class, const DFactory* Factory,
			DObject* Context, EObjectFlags Flags, FFeedbackContext* Warn) -> DObject* override
		{
			return CreateWithFactory(Path, Class, Factory, {}, Context, Flags, Warn);
		}
		auto ImportAsset(const FTopLevelAssetPath& Path, DClass* Class, std::string_view Filename,
			const DFactory* RequestedFactory, DObject* Context, EObjectFlags Flags, FFeedbackContext* Warn) -> DObject* override
		{
			checkf(IsInGameThread(), "Asset import must run on the game thread.");
			const auto* Factory = FindImportFactory({Path, Class, std::string(Filename), RequestedFactory, Context, Flags}, Warn);
			return Factory ? CreateWithFactory(Path, Class, Factory, Filename, Context, Flags, Warn) : nullptr;
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

		auto DuplicateAsset(const FAssetDuplicateRequest& Request, FFeedbackContext* Warn) -> DObject* override
		{ return DuplicateAssetWithEditorPolicy(Request, Warn); }
		auto SaveAssets(const FAssetSaveRequest& Request, FFeedbackContext* Warn) -> bool override
		{ return SaveAssetsWithEditorPolicy(Request, Warn); }
		auto RelocateAssets(const FAssetRelocationRequest& Request, FFeedbackContext* Warn) -> bool override
		{ return RelocateAssetsWithEditorPolicy(Request, Warn); }
		auto FixUpRedirectors(const FAssetRedirectorFixupRequest& Request, FFeedbackContext* Warn) -> bool override
		{ return FixUpRedirectorsWithEditorPolicy(Request, Warn); }
		auto PrepareDeletion(const FAssetDeletionRequest& Request, FAssetDeletionOperation& Operation, FFeedbackContext* Warn) -> bool override
		{ return PrepareAssetDeletionWithEditorPolicy(Request, Operation, Warn); }

	private:
		auto CreateWithFactory(const FTopLevelAssetPath& Path, DClass* Class, const DFactory* RequestedFactory,
			std::string_view Filename, DObject* Context, EObjectFlags Flags, FFeedbackContext* Warn) -> DObject*
		{
			checkf(IsInGameThread(), "Asset tools creation must run on the game thread.");
			if (!Path.IsValid()) return Reject(Warn, "The destination asset path is invalid.");
			if (!ValidateClass(Class)) return Reject(Warn, "The requested asset class cannot be constructed.");
			if (FindPackage(Path.GetPackagePath().GetView()) || FindAssetExact(Path))
				return Reject(Warn, std::format("Asset {} already exists.", Path.ToString()));
			const DFactory* Factory = RequestedFactory ? RequestedFactory : DFactory::FindFactory(Class);
			if (Factory && (!Factory->GetSupportedClass() || !Class->IsChildOf(Factory->GetSupportedClass())))
				return Reject(Warn, "The selected factory does not support the requested asset class.");
			auto* Package = CreatePackage(Path.GetPackagePath());
			if (!Package) return Reject(Warn, "The destination package could not be created.");
			FFactoryDiagnostics Diagnostics;
			const FName Name(Path.GetAssetName());
			DObject* Asset = nullptr;
			if (!Filename.empty()) Asset = Factory->FactoryCreateFromFile(Class, Package, Name, Flags, Filename, Context, &Diagnostics);
			else if (Factory) Asset = Factory->FactoryCreateNew(Class, Package, Name, Flags, Context, &Diagnostics);
			else
			{
				FStaticConstructObjectParameters Parameters{Class, Package, Name, Class->GetStructureSize(), Flags};
				Asset = StaticConstructObject(Parameters);
				DObjectForceRegistration(Asset);
			}
			if (!Asset)
			{
				const auto Message = Diagnostics.ToString();
				Reject(Warn, Message.empty() ? "The factory could not construct the asset." : Message);
				DiscardPackage(Package);
				return nullptr;
			}
			if (!Asset->IsA(Class) || Asset->GetOuter() != Package || Asset->GetFName() != Name
				|| Package->FindTopLevelAsset(Asset->GetFName()) != Asset)
			{
				const auto Message = std::format("Invalid factory product {} (class {}); expected {} (class {}) registered in its destination package.",
					Asset->GetObjectPath(), Asset->GetClass()->GetName(), Path.ToString(), Class->GetName());
				DiscardPackage(Package);
				return Reject(Warn, Message);
			}
			Package->MarkDirty();
			Package->MarkAsNewlyCreated();
			if (!Diagnostics.GetEntries().empty()) Feedback(Warn).Warning(Diagnostics.ToString());
			return Asset;
		}
	};

	auto IAssetTools::ImportAssets(const FAssetImportBatchRequest& Request, FFeedbackContext* Warn) -> std::vector<DObject*>
	{
		checkf(IsInGameThread(), "Asset batch import must run on the game thread.");
		const auto Batch = Request;
		std::vector<FStrongObjectPtr> Retained;
		std::unordered_map<FPackagePath, size_t> Counts;
		for (const auto& Item : Batch.Items)
		{
			Retained.emplace_back(Item.AssetClass);
			Retained.emplace_back(const_cast<DFactory*>(Item.Factory));
			Retained.emplace_back(Item.Context);
			if (Item.AssetPath.IsValid()) ++Counts[Item.AssetPath.GetPackagePath()];
		}
		std::vector<DObject*> Assets(Batch.Items.size(), nullptr);
		for (size_t Index = 0; Index < Batch.Items.size(); ++Index)
		{
			if (Batch.ShouldCancel && Batch.ShouldCancel()) break;
			const auto& Item = Batch.Items[Index];
			if (Item.AssetPath.IsValid() && Counts[Item.AssetPath.GetPackagePath()] > 1)
				Reject(Warn, std::format("Multiple imports target package {}.", Item.AssetPath.GetPackagePath().ToString()));
			else Assets[Index] = ImportAsset(Item.AssetPath, Item.AssetClass, Item.Filename, Item.Factory, Item.Context, Item.Flags, Warn);
			if (Assets[Index]) Retained.emplace_back(Assets[Index]);
			if (!Assets[Index] && Batch.bStopOnFailure) break;
		}
		return Assets;
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
