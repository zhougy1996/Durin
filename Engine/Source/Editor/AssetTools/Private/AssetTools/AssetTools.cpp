#include "AssetTools/AssetToolsModule.h"
#include "ObjectTools.h"
#include "DObject/Archive.h"
#include "DObject/ObjectGraphReplacement.h"

#include "AssetRegistry/Catalog.h"
#include "Asset/Load.h"
#include "DObject/Class.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"
#include "DObject/StrongObjectPtr.h"
#include "Factories/Factory.h"
#include "AssetImportTask.h"
#include "FileHelpers.h"
#include "Misc/StringConvert.h"
#include "Editor/EditorEngine.h"
#include "Import/AssetDestinationValidation.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	IMPLEMENT_MODULE(FAssetToolsModule, AssetTools)

	auto DuplicateAssetWithEditorPolicy(std::string_view, std::string_view, DObject*) -> DObject*;
	auto MakeUniqueAssetName(std::string_view, std::string_view, std::string&, std::string&) -> void;
	auto RenameAssetsWithEditorPolicy(std::span<const FAssetRenameData>, TMulticastDelegate<void(std::span<const FAssetRenameData>)>&) -> bool;
	auto FixupReferencersWithEditorPolicy(std::span<DAssetRedirector* const>, ERedirectFixupMode) -> void;
	auto IsEditorFixupInProgress() -> bool;
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
		auto FindImportFactory(const FAssetImportRequest& Item, FFeedbackContext* Warn) -> DFactory*
		{
			if (!Item.AssetPath.IsValid()) { Reject(Warn, "The destination asset path is invalid."); return nullptr; }
			if (!ValidateClass(Item.AssetClass)) { Reject(Warn, "The requested asset class cannot be constructed."); return nullptr; }
			if (Item.Filename.empty()) { Reject(Warn, "A source filename is required for import."); return nullptr; }
			const auto Destination = InspectAssetDestination(Item.AssetPath.GetPackagePath().GetView());
			if (!Destination) { Reject(Warn, FormatAssetDestinationValidation(Destination)); return nullptr; }
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
			return Item.Factory ? Item.Factory : Cast<DFactory>(NewObject(Factory->GetClass(), nullptr, FName("ImportFactoryInvocation"), EObjectFlags::Transient));
		}
	}

	class FAssetTools final : public IAssetTools
	{
	public:
		auto CreateAsset(std::string_view Name, std::string_view Directory, DClass* Class, DFactory* Factory,
			FName CallingContext, bool Overwrite) -> DObject* override
		{
			FPackagePath PackagePath;
			FTopLevelAssetPath Path;
			std::string PackageName(Directory);
			if (!PackageName.ends_with('/')) PackageName += '/';
			if (!FPackagePath::TryCreate(PackageName + std::string(Name), PackagePath)
				|| !FTopLevelAssetPath::TryCreate(PackagePath, Name, Path)) return Reject(nullptr, "The destination identity is invalid.");
			return CreateWithFactory(Path, Class, Factory, {}, nullptr,
				EObjectFlags::Public | EObjectFlags::Standalone | EObjectFlags::Transactional,
				&FFeedbackContext::Get(), CallingContext, Overwrite);
		}
		auto DuplicateAsset(std::string_view Name, std::string_view Directory, DObject* Source) -> DObject* override
		{ return DuplicateAssetWithEditorPolicy(Name, Directory, Source); }
		auto CreateUniqueAssetName(std::string_view Base, std::string_view Suffix, std::string& Package, std::string& Name) -> void override
		{ MakeUniqueAssetName(Base, Suffix, Package, Name); }

		auto ImportAssetTasks(std::span<DAssetImportTask* const> Tasks) -> void override
		{
			check(IsInGameThread());
			// Snapshot and retain the entire selection before callbacks can collect or
			// modify the caller's array. Each invocation also snapshots its inputs.
			std::vector<TStrongObjectPtr<DAssetImportTask>> Owners;
			for (auto* Task : Tasks) Owners.emplace_back(Task);
			for (const auto& Owner : Owners)
			{
				auto* Task = Owner.Get();
				if (!Task) { Feedback(nullptr).Warning("ImportAssetTasks supplied an empty task."); continue; }
				if (Task->bRunning) { Reject(nullptr, "The import task is already running."); continue; }
				Task->bRunning = true;
				struct FCompletion { bool& Running; ~FCompletion() { Running = false; } } Completion{Task->bRunning};
				Task->Results.clear(); Task->ObjectView.clear(); Task->ImportedObjectPaths.clear();
				TStrongObjectPtr<DAssetImportTask> Inputs(NewObject<DAssetImportTask>(nullptr, "ImportTaskInputs", EObjectFlags::Transient));
				Inputs->Filename = Task->Filename;
				Inputs->DestinationPath = Task->DestinationPath;
				Inputs->DestinationName = Task->DestinationName;
				Inputs->bReplaceExisting = Task->bReplaceExisting;
				Inputs->bReplaceExistingSettings = Task->bReplaceExistingSettings;
				Inputs->bAutomated = Task->bAutomated;
				Inputs->bSave = Task->bSave;
				Inputs->bAsync = Task->bAsync;
				Inputs->Factory = Task->Factory;
				Inputs->Options = Task->Options;
				const auto& Filename = Inputs->Filename;
				const auto& Directory = Inputs->DestinationPath;
				const std::string Name = Inputs->DestinationName.empty()
					? StringUtils::SanitizeFileName(std::filesystem::path(Filename).stem().generic_string(), "Asset") : Inputs->DestinationName;
				TStrongObjectPtr<DFactory> Factory(Inputs->Factory.Get());
				if (Filename.empty()) { Reject(nullptr, "A source filename is required for import."); continue; }
				if (!Factory)
				{
					const auto Candidates = DFactory::FindFactoriesByExtension(std::filesystem::path(Filename).extension().generic_string());
					if (Candidates.size() != 1) { Reject(nullptr, "Import requires one unambiguous factory for this source extension."); continue; }
					Factory = Cast<DFactory>(NewObject(Candidates.front()->GetClass(), nullptr, FName("ImportTaskFactory"), EObjectFlags::Transient));
				}
				if (Factory && Factory->IsClassDefaultObject())
					Factory = Cast<DFactory>(NewObject(Factory->GetClass(), nullptr, FName("ImportTaskFactory"), EObjectFlags::Transient));
				if (!Factory) { Reject(nullptr, "The import factory could not be created."); continue; }
				FPackagePath PackagePath;
				FTopLevelAssetPath Path;
				std::string Prefix = Directory;
				if (!Prefix.ends_with('/')) Prefix += '/';
				if (!FPackagePath::TryCreate(Prefix + Name, PackagePath) || !FTopLevelAssetPath::TryCreate(PackagePath, Name, Path))
				{ Reject(nullptr, "The destination identity is invalid."); continue; }
				auto* Object = CreateWithFactory(Path, Factory->GetSupportedClass(), Factory.Get(), Filename, nullptr,
					EObjectFlags::Public | EObjectFlags::Standalone | EObjectFlags::Transactional,
						&FFeedbackContext::Get(), {}, Inputs->bReplaceExisting, Inputs.Get());
				if (!Object) continue;
				Task->Results.emplace_back(Object);
				Task->ImportedObjectPaths.push_back(Path.ToString());
				if (Inputs->bSave)
				{
					auto* Package = Object->GetPackage();
					DEditorLoadingAndSavingUtils::SavePackages(std::span{&Package, 1}, true);
				}
			}
		}

		auto ImportAsset(const FTopLevelAssetPath& Path, DClass* Class, std::string_view Filename,
			DFactory* RequestedFactory, DObject* Context, EObjectFlags Flags, FFeedbackContext* Warn) -> DObject* override
		{
			checkf(IsInGameThread(), "Asset import must run on the game thread.");
			auto* Factory = FindImportFactory({Path, Class, std::string(Filename), RequestedFactory, Context, Flags}, Warn);
			return Factory ? CreateWithFactory(Path, Class, Factory, Filename, Context, Flags, Warn) : nullptr;
		}
		auto RenameAssets(std::span<const FAssetRenameData> Requests) -> bool override
		{ return RenameAssetsWithEditorPolicy(Requests, PostRename); }
		auto OnAssetPostRename() -> TMulticastDelegate<void(std::span<const FAssetRenameData>)>& override { return PostRename; }
		auto FixupReferencers(std::span<DAssetRedirector* const> Objects, bool, ERedirectFixupMode Mode) const -> void override
		{ FixupReferencersWithEditorPolicy(Objects, Mode); }
		auto IsFixupReferencersInProgress() const -> bool override { return IsEditorFixupInProgress(); }
		auto PrepareDeletion(const FAssetDeletionRequest& Request, FAssetDeletionOperation& Operation, FFeedbackContext* Warn) -> bool override
		{ return PrepareAssetDeletionWithEditorPolicy(Request, Operation, Warn); }

	private:
		TMulticastDelegate<void(std::span<const FAssetRenameData>)> PostRename;
		std::unordered_set<DFactory*> ActiveFactories;
		auto CreateWithFactory(const FTopLevelAssetPath& Path, DClass* Class, DFactory* RequestedFactory,
			std::string_view Filename, DObject* Context, EObjectFlags Flags, FFeedbackContext* Warn,
			FName CallingContext = {}, bool Overwrite = false, const DAssetImportTask* ImportTask = nullptr) -> DObject*
		{
			checkf(IsInGameThread(), "Asset tools creation must run on the game thread.");
			if (!Path.IsValid()) return Reject(Warn, "The destination asset path is invalid.");
			if (!Class && RequestedFactory) Class = RequestedFactory->GetSupportedClass();
			if (!ValidateClass(Class)) return Reject(Warn, "The requested asset class cannot be constructed.");
			const auto* Prototype = RequestedFactory ? RequestedFactory : DFactory::FindFactory(Class);
			if (Prototype && (!Prototype->GetSupportedClass() || !Class->IsChildOf(Prototype->GetSupportedClass())))
				return Reject(Warn, "The selected factory does not support the requested asset class.");
			TStrongObjectPtr<DFactory> Invocation;
			if (Prototype)
			{
				auto* Factory = Prototype->IsClassDefaultObject()
					? Cast<DFactory>(NewObject(Prototype->GetClass(), nullptr, FName("AssetFactoryInvocation"), EObjectFlags::Transient))
					: RequestedFactory;
				if (!Factory) return Reject(Warn, "The factory invocation could not be created.");
				Invocation = TStrongObjectPtr<DFactory>(Factory);
			}
			if (Invocation && !ActiveFactories.insert(Invocation.Get()).second)
				return Reject(Warn, "The factory is already executing another invocation.");
			struct FFactoryScope
			{
				std::unordered_set<DFactory*>& Active;
				DFactory* Factory;
				~FFactoryScope() { if (Factory) Active.erase(Factory); }
			} FactoryScope{ActiveFactories, Invocation.Get()};
			const auto Destination = InspectAssetDestination(Path.GetPackagePath().GetView());
			if (!Destination && !Destination.AssetExists()) return Reject(Warn, FormatAssetDestinationValidation(Destination));
			std::error_code Error;
			const bool Exists = std::filesystem::exists(Destination.PhysicalPath, Error);
			if (Error == std::errc::not_a_directory) Error.clear(); // The live asset may outlast a temporarily invalid save directory.
			if (Error) return Reject(Warn, "The destination file could not be inspected: " + Error.message());
			DPackage* Current = FindPackage(Path.GetPackagePath().GetView());
			if ((Current || Destination.AssetExists() || Exists) && !Overwrite) return Reject(Warn, "The destination is already occupied.");
			if (Overwrite && !Current && (Destination.AssetExists() || Exists))
			{
				std::optional<UnifiedError::FError> Cause;
				Current = LoadPackage(Path.GetPackagePath(), nullptr, &Cause);
				if (!Current) return Reject(Warn, Cause ? Cause->ToString() : "The existing package could not be loaded.");
			}
			if (Current)
			{
				auto* Existing = Current->FindTopLevelAsset(FName(Path.GetAssetName()));
				if (!Existing || Existing->GetClass() != Class) return Reject(Warn, "Replacement requires an existing asset of the same class.");
			}
			FStrongObjectPtr PreviousOwner(Current);
			const uint64 PreviousRevision = Current ? Current->GetEditRevision() : 0;
			struct FImportConfiguration
			{
				std::function<void()> Restore;
				~FImportConfiguration() { if (Restore) Restore(); }
			} Configuration;
			if (ImportTask && !Invocation->ConfigureImportTask(*ImportTask,
				Current ? Current->FindTopLevelAsset(FName(Path.GetAssetName())) : nullptr, Warn, Configuration.Restore)) return nullptr;
			auto* Package = NewObject<DPackage>(nullptr, FName(Path.GetPackagePath().GetPackageName()));
			if (!Package || !Package->InitializePreparedAssetPackage(Path.GetPackagePath())) return Reject(Warn, "The destination package could not be prepared.");
			const TStrongObjectPtr<DPackage> CandidateOwner(Package);
			auto Abandon = [&] { MarkObjectHierarchyAsGarbage(Package); RequestGarbageCollection(); };
			if (Current)
				for (auto* Sibling : Current->GetTopLevelAssets())
					if (Sibling->GetFName() != FName(Path.GetAssetName()) && !DuplicateObject(Sibling, Package, Sibling->GetFName()))
					{ Abandon(); return Reject(Warn, "A sibling asset could not be preserved."); }
			const FName Name(Path.GetAssetName());
			DObject* Asset = nullptr;
			bool ReportedFailure = false;
			FFeedbackContext FactoryFeedback({}, [&](ELogLevel Level, std::string_view Message) {
				ReportedFailure |= Level >= ELogLevel::Error;
				Feedback(Warn).Serialize(Level, Message);
			});
			if (!Filename.empty())
			{
				bool Canceled = false;
				Asset = Invocation.Get()->FactoryCreateFile(Class, Package, Name, Flags, Filename, {}, &FactoryFeedback, Canceled);
				if (Canceled) Asset = nullptr;
			}
			else if (Invocation.Get()) Asset = Invocation.Get()->FactoryCreateNew(Class, Package, Name, Flags, Context, &FactoryFeedback, CallingContext);
			else Asset = NewObject(Class, Package, Name, Flags);
			if (!Asset) { Abandon(); if (!ReportedFailure) Reject(Warn, "The factory did not create an asset."); return nullptr; }
			if (!Asset || Asset->GetClass() != Class || Asset->GetOuter() != Package || Asset->GetFName() != Name
				|| Package->FindTopLevelAsset(Name) != Asset)
			{ Abandon(); return Reject(Warn, "The factory did not produce the requested registered asset."); }
			if (FindPackage(Path.GetPackagePath().GetView()) != Current || (Current && Current->GetEditRevision() != PreviousRevision))
			{ Abandon(); return Reject(Warn, "The destination changed while the factory was executing."); }
			if (!Overwrite)
			{
				Error.clear();
				const bool Occupied = std::filesystem::exists(Destination.PhysicalPath, Error);
				if (Error == std::errc::not_a_directory) Error.clear();
				if (Occupied || Error) { Abandon(); return Reject(Warn, "The destination file changed while the factory was executing."); }
			}
			PreviousOwner.Reset();
			FObjectGraphReplacement Publication;
			const FObjectReplacementPackagePair Pair{Current, Package};
			auto Applied = Publication.Prepare(std::span{&Pair, 1});
			if (!Applied) { Abandon(); return Reject(Warn, ToString(Applied.error())); }
			Applied = Publication.TryCommit();
			if (!Applied) { Publication.Abort(); return Reject(Warn, ToString(Applied.error())); }
			Package->MarkDirty();
			if (!Current || Current->IsNewlyCreated()) Package->MarkAsNewlyCreated();
			require(Publication.Retire());
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
			Retained.emplace_back(Item.Factory);
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
