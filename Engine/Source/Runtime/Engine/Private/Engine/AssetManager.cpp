#include "Engine/AssetManager.h"

#include "AssetRegistry/Catalog.h"
#include "AssetRegistry/References.h"
#include "DObject/Class.h"

namespace Durin
{
	struct FPrimaryAssetManagerState
	{
		struct FEntry
		{
			uint64 Generation = 0;
			std::vector<FName> CurrentBundles, PendingBundles;
			std::shared_ptr<FStreamableHandle> Current, Pending;
		};
		FStreamableManager Streamable;
		bool bActive = false;
		FAssetManagerSettings Settings;
		uint64 Revision = std::numeric_limits<uint64>::max();
		uint64 NextGeneration = 0;
		std::unordered_map<FPrimaryAssetId, FAssetData> Catalog;
		std::unordered_map<FPrimaryAssetId, FPrimaryAssetRules> Rules;
		std::unordered_set<FPrimaryAssetId> EditorOnly;
		std::unordered_map<FPrimaryAssetId, FEntry> Loaded;
		std::vector<std::string> Diagnostics;
	};

	namespace
	{
		auto Normalize(std::vector<FName>& Names) -> void
		{
			std::erase_if(Names, [](FName Name) { return Name.IsNone(); });
			std::ranges::sort(Names, {}, [](FName Name) { return Name.ToString(); });
			Names.erase(std::unique(Names.begin(), Names.end()), Names.end());
		}
	}

	DAssetManager::DAssetManager(const FObjectInitializer& Initializer)
		: Super(Initializer), State(std::make_shared<FPrimaryAssetManagerState>()) {}
	DAssetManager::~DAssetManager() { Shutdown(); }
	auto DAssetManager::StartInitialLoading(FAssetManagerSettings Settings) -> std::expected<void, std::string>
	{
		if (State->bActive) return std::unexpected("Asset manager is already initialized.");
		for (const auto& Type : Settings.PrimaryAssetTypesToScan)
		{
			if (!Type.PrimaryAssetType.IsValid() || Type.bHasBlueprintClasses)
				return std::unexpected("Primary type requires a valid native type; Blueprint discovery is unsupported.");
			if (!Type.AssetBaseClass.empty() && !FindClassByQualifiedName(FName(Type.AssetBaseClass)))
				return std::unexpected("Primary type base class is not registered: " + Type.AssetBaseClass);
		}
		State->Settings = std::move(Settings);
		State->bActive = true;
		State->Revision = std::numeric_limits<uint64>::max();
		RefreshCatalog();
		if (!State->Diagnostics.empty())
		{
			const auto Error = State->Diagnostics.front();
			Shutdown();
			return std::unexpected(Error);
		}
		return {};
	}
	auto DAssetManager::Shutdown() -> void
	{
		// Detach/cancel requests before dropping the manager's retained handles.
		State->bActive = false;
		State->Streamable.ClearRequests();
		State->Loaded.clear();
		State->Catalog.clear();
		State->Rules.clear();
		State->EditorOnly.clear();
	}
	auto DAssetManager::BeginDestroy() -> void { Shutdown(); Super::BeginDestroy(); }
	auto DAssetManager::GetStreamableManager() -> FStreamableManager& { return State->Streamable; }
	auto DAssetManager::Redirect(FPrimaryAssetId Id) const -> FPrimaryAssetId
	{
		std::unordered_set<FPrimaryAssetId> Seen;
		for (uint32 Depth = 0; Depth < 32; ++Depth)
		{
			if (!Id.IsValid() || !Seen.insert(Id).second) return {};
			auto Next = Id;
			bool bRedirected = false;
			if (auto It = State->Settings.PrimaryAssetTypeRedirects.find(Next.PrimaryAssetType);
				It != State->Settings.PrimaryAssetTypeRedirects.end()) { Next.PrimaryAssetType = It->second; bRedirected = true; }
			if (auto It = State->Settings.PrimaryAssetIdRedirects.find(Next);
				It != State->Settings.PrimaryAssetIdRedirects.end()) { Next = It->second; bRedirected = true; }
			if (Next == Id) return bRedirected ? FPrimaryAssetId{} : Id;
			Id = Next;
		}
		return {};
	}
	auto DAssetManager::RefreshCatalog() -> void
	{
		if (!State->bActive || State->Revision == GetAssetCatalogRevision()) return;
		// Persistent catalog only: neither live-object scanning nor asset construction.
		const auto Snapshot = CaptureAssetCatalogSnapshot();
		State->Revision = Snapshot.Revision;
		State->Catalog.clear(); State->Rules.clear(); State->EditorOnly.clear(); State->Diagnostics.clear();
		std::unordered_set<FPrimaryAssetId> Duplicates;
		std::vector<FAssetData> Assets;
		for (const auto& [Path, Package] : Snapshot.Assets)
			for (const auto& Asset : Package.TopLevelAssets) Assets.push_back(Asset);
		std::ranges::sort(Assets, {}, &FAssetData::AssetPath);
		for (const auto& Asset : Assets)
		{
			const auto Id = Asset.PrimaryAssetId;
			if (!Id.IsValid()) continue;
			FPrimaryAssetRules Rules = State->Settings.DefaultRules;
			bool bInclude = State->Settings.PrimaryAssetTypesToScan.empty();
			bool bEditorOnly = false;
			for (const auto& Type : State->Settings.PrimaryAssetTypesToScan)
			{
				if (Type.PrimaryAssetType != Id.PrimaryAssetType || (Type.bIsEditorOnly && !DURIN_WITH_EDITOR)) continue;
				if (!Type.AssetBaseClass.empty())
				{
					const auto* AssetClass = FindClassByQualifiedName(FName(Asset.AssetClassName));
					const auto* Base = FindClassByQualifiedName(FName(Type.AssetBaseClass));
					if (!AssetClass || !Base || !AssetClass->IsChildOf(Base)) continue;
				}
				bool bPath = Type.Directories.empty() && Type.SpecificAssets.empty();
				bPath |= std::ranges::contains(Type.SpecificAssets, Asset.AssetPath);
				for (const auto& Directory : Type.Directories)
					bPath |= Asset.AssetPath.GetPackagePath().ToString().starts_with(Directory + "/");
				if (!bPath) continue;
				bInclude = true;
				bEditorOnly |= Type.bIsEditorOnly;
				// UE type rules override only non-default fields.
				const FPrimaryAssetRules Defaults;
				if (Type.Rules.Priority != Defaults.Priority) Rules.Priority = Type.Rules.Priority;
				if (Type.Rules.ChunkId != Defaults.ChunkId) Rules.ChunkId = Type.Rules.ChunkId;
				if (Type.Rules.bApplyRecursively != Defaults.bApplyRecursively) Rules.bApplyRecursively = Type.Rules.bApplyRecursively;
				if (Type.Rules.CookRule != Defaults.CookRule) Rules.CookRule = Type.Rules.CookRule;
			}
			if (!bInclude) continue;
			if (auto Override = State->Settings.PrimaryAssetRules.find(Id); Override != State->Settings.PrimaryAssetRules.end()) Override->second.Apply(Rules);
			if (Duplicates.contains(Id)) continue;
			if (auto Prior = State->Catalog.find(Id); Prior != State->Catalog.end())
			{
				State->Diagnostics.push_back(std::format("Duplicate primary ID '{}': '{}' and '{}'.", Id.ToString(), Prior->second.AssetPath.ToString(), Asset.AssetPath.ToString()));
				State->Catalog.erase(Prior); State->Rules.erase(Id); Duplicates.insert(Id);
				continue;
			}
			State->Catalog.emplace(Id, Asset); State->Rules.emplace(Id, Rules);
			if (bEditorOnly) State->EditorOnly.insert(Id);
		}
		for (const auto& [Type, Target] : State->Settings.PrimaryAssetTypeRedirects)
			if (!Redirect({Type, FName("_RedirectValidation")}).IsValid()) State->Diagnostics.push_back("Invalid primary type redirect chain: " + Type.ToString());
		for (const auto& [Id, Target] : State->Settings.PrimaryAssetIdRedirects)
			if (!Redirect(Id).IsValid()) State->Diagnostics.push_back("Invalid primary redirect chain: " + Id.ToString());
	}
	auto DAssetManager::GetPrimaryAssetIdList(FPrimaryAssetType Type) -> std::vector<FPrimaryAssetId>
	{
		RefreshCatalog();
		std::vector<FPrimaryAssetId> Result;
		for (const auto& [Id, Asset] : State->Catalog) if (Id.PrimaryAssetType == Type) Result.push_back(Id);
		std::ranges::sort(Result, {}, &FPrimaryAssetId::ToString);
		return Result;
	}
	auto DAssetManager::GetPrimaryAssetPath(FPrimaryAssetId Id) -> FSoftObjectPath
	{
		RefreshCatalog();
		const auto It = State->Catalog.find(Redirect(Id));
		return It == State->Catalog.end() ? FSoftObjectPath{} : FSoftObjectPath(It->second.AssetPath);
	}
	auto DAssetManager::GetPrimaryAssetIdForPath(const FTopLevelAssetPath& Path) -> FPrimaryAssetId
	{
		RefreshCatalog();
		for (const auto& [Id, Asset] : State->Catalog) if (Asset.AssetPath == Path) return Id;
		return {};
	}
	auto DAssetManager::GetPrimaryAssetRules(FPrimaryAssetId Id) -> FPrimaryAssetRules
	{
		RefreshCatalog();
		const auto It = State->Rules.find(Redirect(Id));
		return It == State->Rules.end() ? FPrimaryAssetRules{} : It->second;
	}
	auto DAssetManager::GetPrimaryAssetObject(FPrimaryAssetId Id) -> DObject*
	{
		const auto It = State->Loaded.find(Redirect(Id));
		return It != State->Loaded.end() && It->second.Current ? It->second.Current->GetLoadedAsset() : nullptr;
	}
	auto DAssetManager::GetPrimaryAssetBundleState(FPrimaryAssetId Id, bool bPending) -> std::vector<FName>
	{
		const auto It = State->Loaded.find(Redirect(Id));
		if (It == State->Loaded.end()) return {};
		return bPending ? It->second.PendingBundles : It->second.CurrentBundles;
	}
	auto DAssetManager::GetDiagnostics() -> std::vector<std::string> { RefreshCatalog(); return State->Diagnostics; }
	auto DAssetManager::ModifyCookReferences(const FPackagePath& Package, std::vector<FPackagePath>& PackagesToCook) -> void
	{
		RefreshCatalog();
		for (const auto& [Id, Asset] : State->Catalog)
			if (Asset.AssetPath.GetPackagePath() == Package)
				for (const auto& Bundle : Asset.AssetBundleData.Bundles)
					for (const auto& Path : Bundle.BundleAssets) PackagesToCook.push_back(Path.GetPackagePath());
		std::ranges::sort(PackagesToCook);
		PackagesToCook.erase(std::unique(PackagesToCook.begin(), PackagesToCook.end()), PackagesToCook.end());
	}
	auto DAssetManager::ModifyCook(std::vector<FPackagePath>& PackagesToCook,
		std::vector<FPackagePath>& PackagesToNeverCook) -> std::expected<void, std::string>
	{
		RefreshCatalog();
		if (!State->bActive) return std::unexpected("Cook requires an initialized asset manager.");
		if (!State->Diagnostics.empty()) return std::unexpected(State->Diagnostics.front());
		const auto Validate = [](const FPrimaryAssetRules& Rules) -> std::expected<void, std::string> {
			if (Rules.ChunkId != -1) return std::unexpected("AssetManager ChunkId requires an unsupported physical chunk/install backend.");
			if (Rules.CookRule != EPrimaryAssetCookRule::Unknown && Rules.CookRule != EPrimaryAssetCookRule::AlwaysCook
				&& Rules.CookRule != EPrimaryAssetCookRule::NeverCook) return std::unexpected("Unsupported primary asset CookRule.");
			return {};
		};
		if (auto Result = Validate(State->Settings.DefaultRules); !Result) return Result;
		for (const auto& Type : State->Settings.PrimaryAssetTypesToScan)
			if (auto Result = Validate(Type.Rules); !Result) return Result;
		for (const auto& [Id, Override] : State->Settings.PrimaryAssetRules)
		{
			FPrimaryAssetRules Rules; Override.Apply(Rules);
			if (auto Result = Validate(Rules); !Result) return std::unexpected(Id.ToString() + ": " + Result.error());
		}
		const auto Snapshot = CaptureAssetRegistrySnapshot();
		struct FAssignment { FPrimaryAssetId Id; FPrimaryAssetRules Rules; };
		std::map<FPackagePath, std::vector<FAssignment>> Managers;
		std::vector<FPrimaryAssetId> Ids;
		for (const auto& [Id, Asset] : State->Catalog) Ids.push_back(Id);
		std::ranges::sort(Ids, {}, &FPrimaryAssetId::ToString);
		for (const auto Id : Ids)
		{
			auto Rules = State->Rules.at(Id);
			if (State->EditorOnly.contains(Id) && Rules.CookRule == EPrimaryAssetCookRule::AlwaysCook)
				Rules.CookRule = EPrimaryAssetCookRule::Unknown;
			const auto PrimaryPackage = State->Catalog.at(Id).AssetPath.GetPackagePath();
			std::vector<FPackagePath> Pending{PrimaryPackage};
			ModifyCookReferences(PrimaryPackage, Pending);
			std::unordered_set<FPackagePath> Visited;
			while (!Pending.empty())
			{
				auto Path = Pending.back(); Pending.pop_back();
				if (const auto Resolution = Snapshot.ResolvePackagePath(Path); Resolution) Path = Resolution.FinalPath;
				if (!Visited.insert(Path).second) continue;
				// A higher-priority primary takes ownership of its secondary closure.
				if (std::ranges::any_of(Ids, [&](const auto Other) {
					return State->Catalog.at(Other).AssetPath.GetPackagePath() == Path && State->Rules.at(Other).Priority > Rules.Priority;
				})) continue;
				Managers[Path].push_back({Id, Rules});
				if (!Rules.bApplyRecursively) continue;
				if (const auto* Package = Snapshot.Catalog.FindExact(Path))
					Pending.insert(Pending.end(), Package->Dependencies.begin(), Package->Dependencies.end());
				ModifyCookReferences(Path, Pending);
			}
		}
		std::vector<FPackagePath> Always, Never;
		for (const auto& [Path, Assignments] : Managers)
		{
			const auto Priority = std::ranges::max(Assignments, {}, [](const auto& Value) { return Value.Rules.Priority; }).Rules.Priority;
			const FAssignment* Selected = nullptr;
			for (const auto& Assignment : Assignments)
			{
				if (Assignment.Rules.Priority != Priority || Assignment.Rules.CookRule == EPrimaryAssetCookRule::Unknown) continue;
				if (Selected && Selected->Rules.CookRule != Assignment.Rules.CookRule)
					return std::unexpected(std::format("Conflicting Cook rules for '{}': '{}' and '{}' at priority {}.",
						Path.ToString(), Selected->Id.ToString(), Assignment.Id.ToString(), Priority));
				Selected = &Assignment;
			}
			if (!Selected) continue;
			(Selected->Rules.CookRule == EPrimaryAssetCookRule::AlwaysCook ? Always : Never).push_back(Path);
		}
		PackagesToCook.insert(PackagesToCook.end(), Always.begin(), Always.end());
		PackagesToNeverCook.insert(PackagesToNeverCook.end(), Never.begin(), Never.end());
		return {};
	}
	auto DAssetManager::LoadPrimaryAsset(FPrimaryAssetId Id, std::span<const FName> Bundles, FAssetManagerLoadParams Params) -> std::shared_ptr<FStreamableHandle>
	{ return LoadPrimaryAssets(std::span(&Id, 1), Bundles, std::move(Params)); }
	auto DAssetManager::LoadPrimaryAssets(std::span<const FPrimaryAssetId> Ids, std::span<const FName> Bundles, FAssetManagerLoadParams Params) -> std::shared_ptr<FStreamableHandle>
	{ return Transition(Ids, Bundles, {}, true, std::move(Params)); }
	auto DAssetManager::GetPrimaryAssetLoadSet(FPrimaryAssetId Id, std::span<const FName> Bundles) const -> std::expected<std::vector<FSoftObjectPath>, FAssetReadError>
	{
		std::vector<FSoftObjectPath> Targets;
		const auto Asset = State->Catalog.find(Id);
		if (!Id.IsValid()) return std::unexpected(FAssetReadError{EAssetReadError::InvalidPath, "Invalid primary asset ID."});
		if (Asset == State->Catalog.end()) return std::unexpected(FAssetReadError{EAssetReadError::NotFound,
			std::format("Primary asset ID '{}' was not found.", Id.ToString())});
		Targets.emplace_back(Asset->second.AssetPath);
		for (const auto& Bundle : Asset->second.AssetBundleData.Bundles)
			if (std::ranges::contains(Bundles, Bundle.BundleName))
				for (const auto& Path : Bundle.BundleAssets)
					if (!std::ranges::contains(Targets, FSoftObjectPath(Path))) Targets.emplace_back(Path);
		return Targets;
	}
	auto DAssetManager::PreloadPrimaryAssets(std::span<const FPrimaryAssetId> Ids, std::span<const FName> Bundles, FAssetManagerLoadParams Params) -> std::shared_ptr<FStreamableHandle>
	{
		if (!State->bActive) return FStreamableManager::CreateErrorHandle({EAssetReadError::ShuttingDown, "Asset manager is not initialized."}, std::move(Params.DebugName));
		RefreshCatalog();
		FStreamableAsyncLoadParams Load;
		for (const auto Id : Ids)
		{
			auto Targets = GetPrimaryAssetLoadSet(Redirect(Id), Bundles);
			if (!Targets) return FStreamableManager::CreateErrorHandle(std::move(Targets.error()), std::move(Params.DebugName));
			for (const auto& Target : *Targets)
				if (!std::ranges::contains(Load.TargetsToStream, Target)) Load.TargetsToStream.push_back(Target);
		}
		Load.OnComplete = std::move(Params.OnComplete);
		Load.OnCancel = std::move(Params.OnCancel);
		Load.Priority = Params.Priority;
		Load.DebugName = std::move(Params.DebugName);
		return State->Streamable.RequestAsyncLoad(std::move(Load));
	}
	auto DAssetManager::ChangeBundleStateForPrimaryAssets(std::span<const FPrimaryAssetId> Ids,
		std::span<const FName> Add, std::span<const FName> Remove, bool bRemoveAll, FAssetManagerLoadParams Params) -> std::shared_ptr<FStreamableHandle>
	{ return Transition(Ids, Add, Remove, bRemoveAll, std::move(Params)); }
	auto DAssetManager::Transition(std::span<const FPrimaryAssetId> Ids, std::span<const FName> Add,
		std::span<const FName> Remove, bool bRemoveAll, FAssetManagerLoadParams Params) -> std::shared_ptr<FStreamableHandle>
	{
		if (!State->bActive) return FStreamableManager::CreateErrorHandle({EAssetReadError::ShuttingDown, "Asset manager is not initialized."}, std::move(Params.DebugName));
		RefreshCatalog();
		std::vector<std::shared_ptr<FStreamableHandle>> Children;
		std::vector<std::pair<FPrimaryAssetId, uint64>> Generations;
		bool bAnyWork = false;
		std::unordered_set<FPrimaryAssetId> Seen;
		for (auto Requested : Ids)
		{
			const auto Id = Redirect(Requested);
			if (!Seen.insert(Id.IsValid() ? Id : Requested).second) continue;
			const auto Existing = State->Loaded.find(Id);
			auto Desired = bRemoveAll || Existing == State->Loaded.end() ? std::vector<FName>{}
				: (Existing->second.Pending ? Existing->second.PendingBundles : Existing->second.CurrentBundles);
			std::erase_if(Desired, [&](FName Name) { return std::ranges::contains(Remove, Name); });
			Desired.insert(Desired.end(), Add.begin(), Add.end()); Normalize(Desired);
			auto Targets = GetPrimaryAssetLoadSet(Id, Desired);
			if (!Targets)
			{
				Children.push_back(FStreamableManager::CreateErrorHandle(std::move(Targets.error()), Requested.ToString()));
				bAnyWork = true;
				continue;
			}
			auto& Entry = State->Loaded[Id];
			if (!Entry.Pending && Entry.Current && Desired == Entry.CurrentBundles && Entry.Current->GetRequestedAssets() == *Targets)
			{ Children.push_back(Entry.Current); continue; }
			bAnyWork = true;
			const auto Generation = Entry.Generation = ++State->NextGeneration;
			if (Entry.Pending) Entry.Pending->CancelHandle();
			Entry.Pending.reset();
			Entry.PendingBundles = std::move(Desired);
			std::weak_ptr<FPrimaryAssetManagerState> Weak = State;
			FStreamableAsyncLoadParams Load;
			Load.TargetsToStream = std::move(*Targets); Load.Priority = Params.Priority; Load.DebugName = Id.ToString();
			auto Pending = State->Streamable.RequestAsyncLoad(std::move(Load));
			Entry.Pending = Pending;
			Children.push_back(Pending); Generations.emplace_back(Id, Generation);
			Pending->SetLoadCompletedCallback([Weak, Id, Generation](FStreamableHandle& Handle) {
				const auto State = Weak.lock();
				if (!State || !State->bActive) return;
				const auto It = State->Loaded.find(Id);
				if (It == State->Loaded.end() || It->second.Generation != Generation) return;
				auto& Entry = It->second;
				if (!Handle.HasError() && !Handle.WasCanceled())
				{
					Entry.Current = Entry.Pending;
					Entry.CurrentBundles = Entry.PendingBundles;
				}
				Entry.Pending.reset(); Entry.PendingBundles.clear();
				if (!Entry.Current) State->Loaded.erase(It);
			});
		}
		if (!bAnyWork)
		{
			// UE returns null and calls completion immediately when nothing changes.
			if (Params.OnComplete)
			{
				auto Completed = Children.empty() ? State->Streamable.RequestSyncLoad(std::span<const FSoftObjectPath>{}) : Children.front();
				Params.OnComplete(*Completed);
			}
			return nullptr;
		}
		// The observer has its own roots. Releasing it cannot release a child retained by the manager.
		std::weak_ptr<FPrimaryAssetManagerState> Weak = State;
		auto OnCancel = [Weak, Generations = std::move(Generations), Callback = std::move(Params.OnCancel)](FStreamableHandle& Handle) {
			if (auto State = Weak.lock(); State && State->bActive)
				for (const auto& [Id, Generation] : Generations)
					if (auto It = State->Loaded.find(Id); It != State->Loaded.end() && It->second.Generation == Generation && It->second.Pending)
					{
						It->second.Generation = ++State->NextGeneration;
						It->second.Pending->CancelHandle();
						It->second.Pending.reset(); It->second.PendingBundles.clear();
						if (!It->second.Current) State->Loaded.erase(It);
					}
			if (Callback) Callback(Handle);
		};
		return State->Streamable.CreateCombinedHandle(Children, std::move(Params.OnComplete), std::move(Params.DebugName), false, std::move(OnCancel));
	}
	auto DAssetManager::UnloadPrimaryAsset(FPrimaryAssetId Requested) -> bool
	{
		const auto It = State->Loaded.find(Redirect(Requested));
		if (It == State->Loaded.end()) return false;
		++It->second.Generation;
		if (It->second.Pending) It->second.Pending->CancelHandle();
		State->Loaded.erase(It);
		return true;
	}
	auto DAssetManager::UnloadPrimaryAssets(std::span<const FPrimaryAssetId> Ids) -> size_t
	{ size_t Count = 0; for (const auto Id : Ids) Count += UnloadPrimaryAsset(Id); return Count; }
}
