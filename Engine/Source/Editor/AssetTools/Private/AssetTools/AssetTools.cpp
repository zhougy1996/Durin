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
#include "AssetTools/AssetImportAdapter.h"
#include "Import/AssetImportTaskAccess.h"
#include "TickableEditorObject.h"
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

	}

	namespace
	{
		struct FAdapter { uint64 Id; std::function<bool(const DAssetImportTask&, bool)> Accepts;
			std::function<std::shared_ptr<IAssetImportTaskOperation>(const DAssetImportTask&)> Create; size_t Dispatches = 0; };
		struct FActiveImport { uint64 Adapter; TStrongObjectPtr<DAssetImportTask> Task, Inputs; };
		auto& Adapters = *new std::vector<std::shared_ptr<FAdapter>>;
		auto& ActiveImports = *new std::vector<FActiveImport>;
		uint64 NextAdapterId = 0;
		auto& BrowserSync = *new std::function<void(std::span<DObject* const>)>;
	}
	FAssetImportAdapterRegistration::FAssetImportAdapterRegistration(
		std::function<bool(const DAssetImportTask&, bool)> Accepts,
		std::function<std::shared_ptr<IAssetImportTaskOperation>(const DAssetImportTask&)> Create)
	{
		check(IsInGameThread()); Id = ++NextAdapterId;
		Adapters.push_back(std::make_shared<FAdapter>(FAdapter{Id, std::move(Accepts), std::move(Create)}));
	}
	FAssetImportAdapterRegistration::~FAssetImportAdapterRegistration() { Reset(); }
	auto FAssetImportAdapterRegistration::Reset() -> void
	{
		if (!Id) return;
		check(IsInGameThread());
		for (const auto& Adapter : Adapters) if (Adapter->Id == Id)
			requiref(Adapter->Dispatches == 0, "Cannot retire an import adapter from its dispatch callback.");
		std::erase_if(Adapters, [&](const auto& Adapter) { return Adapter->Id == Id; });
		const auto Snapshot = ActiveImports;
		for (const auto& Active : Snapshot) if (Active.Adapter == Id)
			if (auto* Task = Active.Task.Get()) FAssetImportTaskAccess::CancelAndDrain(*Task);
		std::erase_if(ActiveImports, [&](const auto& Active) { return Active.Adapter == Id; });
		Id = 0;
	}
	auto SetAssetImportBrowserSync(std::function<void(std::span<DObject* const>)> Callback) -> void
	{ check(IsInGameThread()); BrowserSync = std::move(Callback); }

	class FAssetTools final : public IAssetTools, public FTickableEditorObject
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

		auto GetTickableEditor() const -> DEditorEngine* override { return GEditor; }
		auto Tick(float) noexcept -> void override
		{
			const auto Snapshot = ActiveImports;
			for (const auto& Active : Snapshot)
				if (auto* Task = Active.Task.Get()) FAssetImportTaskAccess::Tick(*Task);
			std::erase_if(ActiveImports, [](const auto& Active) { return !Active.Task || Active.Task->IsAsyncImportComplete(); });
		}
		auto ImportAssetTasks(std::span<DAssetImportTask* const> Tasks) -> void override { SubmitTasks(Tasks); }
		auto ImportAssets(std::span<const std::string> Files, std::string_view Destination, DFactory* Factory,
			bool Sync, std::vector<std::pair<std::string, std::string>>* Mappings, bool Async, bool Scene) const -> std::vector<DObject*> override
		{
			check(IsInGameThread());
			const std::vector<std::string> Filenames(Files.begin(), Files.end());
			const auto Destinations = Mappings ? *Mappings : std::vector<std::pair<std::string, std::string>>{};
			TStrongObjectPtr<DFactory> FactoryOwner(Factory);
			std::vector<TStrongObjectPtr<DAssetImportTask>> Tasks;
			std::vector<DAssetImportTask*> Raw;
			for (const auto& File : Filenames)
			{
				TStrongObjectPtr<DAssetImportTask> Task(NewObject<DAssetImportTask>(nullptr, "FileImportTask", EObjectFlags::Transient));
				Task->Filename = File; Task->DestinationPath = Destination; Task->Factory = Factory; Task->bAsync = Async;
				const auto Mapping = std::ranges::find(Destinations, File, &std::pair<std::string, std::string>::first);
				if (Mapping != Destinations.end()) Task->DestinationPath = Mapping->second;
				Raw.push_back(Task.Get()); Tasks.push_back(std::move(Task));
			}
			SubmitTasks(Raw, Scene);
			// This API returns concrete successful outputs, so even async-capable adapters
			// must finish before the borrowed result list is returned. Tasks expose polling.
			std::vector<DObject*> Objects;
			std::vector<FStrongObjectPtr> OutputOwners;
			for (auto& Task : Tasks)
			{
				for (auto* Object : Task->GetObjects()) { Objects.push_back(Object); OutputOwners.emplace_back(Object); }
				MarkAsGarbage(Task.Get()); Task.Reset();
			}
			if (Sync && BrowserSync && !Objects.empty()) { const auto Callback = BrowserSync; Callback(Objects); }
			return Objects;
		}
		auto SubmitTasks(std::span<DAssetImportTask* const> Tasks, bool Scene = false) const -> void
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
				if (!FAssetImportTaskAccess::Begin(*Task)) { Reject(nullptr, "The import task is already running."); continue; }
				struct FCompletion
				{
					DAssetImportTask& Task;
					FStrongObjectPtr Result;
					~FCompletion() { const std::array Objects{Result.Get()}; FAssetImportTaskAccess::CompleteSynchronous(Task, Objects); }
				} Completion{*Task};
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
				const auto ProviderSnapshot = Adapters;
				std::vector<std::shared_ptr<FAdapter>> Candidates;
				for (const auto& Adapter : ProviderSnapshot)
				{
					++Adapter->Dispatches;
					struct FDispatch { size_t& Count; ~FDispatch() { --Count; } } Dispatch{Adapter->Dispatches};
					if (Adapter->Accepts(*Inputs, Scene)) Candidates.push_back(Adapter);
				}
				if (!Candidates.empty())
				{
					if (Candidates.size() != 1) { Reject(nullptr, "Multiple adapters support this import task."); continue; }
					++Candidates.front()->Dispatches;
					struct FDispatch { size_t& Count; ~FDispatch() { --Count; } } Dispatch{Candidates.front()->Dispatches};
					auto Operation = Candidates.front()->Create(*Inputs);
					if (!Operation) continue;
					FAssetImportTaskAccess::Attach(*Task, std::move(Operation), [Task] {
						std::erase_if(ActiveImports, [Task](const auto& Active) { return Active.Task.Get() == Task; });
					});
					ActiveImports.push_back({Candidates.front()->Id, Owner, Inputs});
					FAssetImportTaskAccess::Tick(*Task);
					if (!Inputs->bAsync) (void)Task->GetObjects();
					std::erase_if(ActiveImports, [](const auto& Active) { return !Active.Task || Active.Task->IsAsyncImportComplete(); });
					continue;
				}
				if (Scene && !Factory) { Reject(nullptr, "No scene import adapter supports this source and factory."); continue; }
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
				Completion.Result = FStrongObjectPtr(Object);
				if (Inputs->bSave)
				{
					auto* Package = Object->GetPackage();
					DEditorLoadingAndSavingUtils::SavePackages(std::span{&Package, 1}, true);
				}
			}
		}
		auto RenameAssets(std::span<const FAssetRenameData> Requests) -> bool override
		{ return RenameAssetsWithEditorPolicy(Requests, PostRename); }
		auto OnAssetPostRename() -> TMulticastDelegate<void(std::span<const FAssetRenameData>)>& override { return PostRename; }
		auto FixupReferencers(std::span<DAssetRedirector* const> Objects, bool, ERedirectFixupMode Mode) const -> void override
		{ FixupReferencersWithEditorPolicy(Objects, Mode); }
		auto IsFixupReferencersInProgress() const -> bool override { return IsEditorFixupInProgress(); }

	private:
		TMulticastDelegate<void(std::span<const FAssetRenameData>)> PostRename;
		mutable std::unordered_set<DFactory*> ActiveFactories;
		auto CreateWithFactory(const FTopLevelAssetPath& Path, DClass* Class, DFactory* RequestedFactory,
			std::string_view Filename, DObject* Context, EObjectFlags Flags, FFeedbackContext* Warn,
			FName CallingContext = {}, bool Overwrite = false, const DAssetImportTask* ImportTask = nullptr) const -> DObject*
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
			const bool bWasDirty = Current && Current->IsDirty();
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
			if (FindPackage(Path.GetPackagePath().GetView()) != Current || (Current && Current->IsDirty() != bWasDirty))
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
		const auto Snapshot = ActiveImports;
		for (const auto& Active : Snapshot)
			if (auto* Task = Active.Task.Get()) FAssetImportTaskAccess::CancelAndDrain(*Task);
		ActiveImports.clear(); BrowserSync = {};
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
