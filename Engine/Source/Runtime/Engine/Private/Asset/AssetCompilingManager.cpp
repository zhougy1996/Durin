#include "Asset/AssetCompilingManager.h"

#include "CoreGlobals.h"
#include "DObject/DObjectGlobals.h"
#include "Logging/LogMacros.h"
#include "Threading/RunnableThread.h"

#include <algorithm>
#include <limits>
#include <mutex>
#include <unordered_map>

namespace Durin
{
	namespace
	{
		struct FRegisteredAssetCompilingManager
		{
			std::shared_ptr<IAssetCompilingManager> Manager;
			std::vector<FName> Dependencies;
			FModuleOwnedCallbackGate Gate;
			FModuleOwnedResourceLease RegistryResource;
			uint64 Generation = 0;
			uint32 DependencyLevel = 0;
			bool bStarted = false;
			bool bBuiltIn = false;
		};

		struct FDispatchEntry
		{
			FName DomainName;
			std::shared_ptr<IAssetCompilingManager> Manager;
			FModuleOwnedCallbackGate Gate;
			FModuleOwnedResourceLease Resource;
			uint32 DependencyLevel = 0;
		};

		std::mutex GAssetCompilingMutex;
		std::unordered_map<FName, FRegisteredAssetCompilingManager> GManagers;
		std::vector<FName> GOrderedDomains;
		std::vector<std::string> GMessages;
		uint64 GNextGeneration = 1;
		uint64 GProcessedCompletions = 0;
		uint64 GRoundRobinCursor = 0;
		bool GRunning = false;
		bool GShutdown = false;
		FAssetPostCompileEvent GPostCompileEvent;

		auto SetError(std::string* OutError, std::string Message) -> bool
		{
			if (OutError) *OutError = std::move(Message);
			return false;
		}

		auto IsCanonicalDomainName(const FName& Name) -> bool
		{
			const std::string Value = Name.ToString();
			return !Value.empty() && std::ranges::all_of(Value, [](char Character) {
				return Character >= 'a' && Character <= 'z'
					|| Character >= 'A' && Character <= 'Z'
					|| Character >= '0' && Character <= '9'
					|| Character == '.' || Character == '_' || Character == '-';
			});
		}

		auto IsSupportedThread() -> bool
		{
			return !GIsGameThreadIdInitialized || IsInGameThread();
		}

		auto SaturatingAdd(uint64 Left, uint64 Right) -> uint64
		{
			return Right > std::numeric_limits<uint64>::max() - Left
				? std::numeric_limits<uint64>::max() : Left + Right;
		}

		auto BuildOrder(
			const std::unordered_map<FName, FRegisteredAssetCompilingManager>& Managers,
			std::vector<FName>& OutOrder,
			std::unordered_map<FName, uint32>& OutLevels,
			std::vector<std::string>* OutMissingDependencies,
			std::string* OutError) -> bool
		{
			std::unordered_map<FName, uint32> Indegrees;
			std::unordered_map<FName, std::vector<FName>> Dependents;
			for (const auto& [Name, Entry] : Managers) Indegrees.emplace(Name, 0);
			for (const auto& [Name, Entry] : Managers)
			{
				for (const FName& Dependency : Entry.Dependencies)
				{
					if (!Managers.contains(Dependency))
					{
						if (OutMissingDependencies && OutMissingDependencies->size() < 32)
							OutMissingDependencies->push_back(std::format(
								"Asset compilation domain '{}' has missing optional dependency '{}'.",
								Name.ToString(), Dependency.ToString()));
						continue;
					}
					++Indegrees[Name];
					Dependents[Dependency].push_back(Name);
				}
			}
			std::vector<FName> Ready;
			for (const auto& [Name, Degree] : Indegrees)
				if (Degree == 0) Ready.push_back(Name);
			auto SortNames = [](std::vector<FName>& Names) {
				std::ranges::sort(Names, {}, [](const FName& Name) { return Name.ToString(); });
			};
			SortNames(Ready);
			OutOrder.clear();
			OutLevels.clear();
			while (!Ready.empty())
			{
				const FName Name = Ready.front();
				Ready.erase(Ready.begin());
				OutOrder.push_back(Name);
				const uint32 Level = OutLevels[Name];
				for (const FName& Dependent : Dependents[Name])
				{
					OutLevels[Dependent] = std::max(OutLevels[Dependent], Level + 1);
					if (--Indegrees[Dependent] == 0) Ready.push_back(Dependent);
				}
				SortNames(Ready);
			}
			if (OutOrder.size() != Managers.size())
				return SetError(OutError, "Asset compilation domain dependency cycle detected.");
			return true;
		}

		auto MakeSnapshot(bool bReverse = false) -> std::vector<FDispatchEntry>
		{
			std::vector<FDispatchEntry> Result;
			std::lock_guard Lock(GAssetCompilingMutex);
			Result.reserve(GOrderedDomains.size());
			for (const FName& Name : GOrderedDomains)
			{
				const auto It = GManagers.find(Name);
				if (It == GManagers.end() || !It->second.bStarted) continue;
				auto Resource = It->second.Gate.IsValid()
					? It->second.Gate.RetainResource() : FModuleOwnedResourceLease{};
				if (It->second.Gate.IsValid() && !Resource) continue;
				Result.push_back({Name, It->second.Manager, It->second.Gate,
					std::move(Resource), It->second.DependencyLevel});
			}
			if (bReverse) std::ranges::reverse(Result);
			return Result;
		}

		template<typename F>
		auto Invoke(FDispatchEntry& Entry, F&& Callback) -> bool
		{
			if (!Entry.Gate.IsValid())
			{
				std::invoke(std::forward<F>(Callback), *Entry.Manager);
				return true;
			}
			auto Invocation = Entry.Gate.TryEnter();
			if (!Invocation) return false;
			std::invoke(std::forward<F>(Callback), *Entry.Manager);
			return true;
		}

		auto CoalesceSuccessfulAssets(FAssetCompileProcessResult& Destination,
			FAssetCompileProcessResult Source) -> void
		{
			Destination.ProcessedCompletionCount = static_cast<uint32>(std::min<uint64>(
				static_cast<uint64>(Destination.ProcessedCompletionCount)
					+ Source.ProcessedCompletionCount,
				std::numeric_limits<uint32>::max()));
			for (const FWeakObjectPtr& Candidate : Source.SuccessfullyCompiledAssets)
			{
				const FObjectHandle Handle = Candidate.GetHandle();
				if (IsObjectHandleNull(Handle)) continue;
				const bool bDuplicate = std::ranges::any_of(
					Destination.SuccessfullyCompiledAssets,
					[Handle](const FWeakObjectPtr& Existing) {
						const FObjectHandle Other = Existing.GetHandle();
						return Handle.Index == Other.Index && Handle.Generation == Other.Generation;
					});
				if (!bDuplicate) Destination.SuccessfullyCompiledAssets.push_back(Candidate);
			}
		}

		auto Publish(const FName& DomainName, const FAssetCompileProcessResult& Result) -> void
		{
			if (Result.SuccessfullyCompiledAssets.empty()) return;
			FAssetCompileProcessResult Coalesced;
			CoalesceSuccessfulAssets(Coalesced, Result);
			GPostCompileEvent.Broadcast({DomainName,
				std::move(Coalesced.SuccessfullyCompiledAssets)});
		}
	}

	FAssetCompilingManagerRegistration::~FAssetCompilingManagerRegistration() { Reset(); }
	FAssetCompilingManagerRegistration::FAssetCompilingManagerRegistration(
		FAssetCompilingManagerRegistration&& Other) noexcept
		: DomainName(Other.DomainName), Generation(std::exchange(Other.Generation, 0)) {}
	auto FAssetCompilingManagerRegistration::operator=(
		FAssetCompilingManagerRegistration&& Other) noexcept
		-> FAssetCompilingManagerRegistration&
	{
		if (this != &Other)
		{
			Reset();
			DomainName = Other.DomainName;
			Generation = std::exchange(Other.Generation, 0);
		}
		return *this;
	}
	auto FAssetCompilingManagerRegistration::Reset() -> void
	{
		if (Generation == 0) return;
		FAssetCompilingManager::Get().Unregister(DomainName, Generation);
		Generation = 0;
	}

	auto FAssetCompilingManager::Get() -> FAssetCompilingManager&
	{
		static FAssetCompilingManager Instance;
		return Instance;
	}

	auto FAssetCompilingManager::Start(std::string* OutError) -> bool
	{
		if (!IsSupportedThread()) return SetError(OutError,
			"Asset compilation manager must start on GameThread.");
		std::lock_guard Lock(GAssetCompilingMutex);
		if (GRunning) return true;
		if (GShutdown) return SetError(OutError,
			"Asset compilation manager cannot restart after terminal shutdown.");
		GRunning = true;
		if (OutError) OutError->clear();
		return true;
	}

	auto FAssetCompilingManager::RegisterManager(
		std::shared_ptr<IAssetCompilingManager> Manager,
		FModuleOwnedCallbackGate OwnerGate, std::string* OutError)
		-> FAssetCompilingManagerRegistration
	{
		if (!OwnerGate.IsValid())
			return SetError(OutError, "External asset compiling manager owner gate is invalid."),
				FAssetCompilingManagerRegistration{};
		if (!Manager)
			return SetError(OutError, "Asset compiling manager is invalid."),
				FAssetCompilingManagerRegistration{};
		if (!IsSupportedThread())
			return SetError(OutError, "Asset compiling manager registration requires GameThread."),
				FAssetCompilingManagerRegistration{};
		FModuleOwnedResourceLease Resource = OwnerGate.RetainResource();
		if (!Resource)
			return SetError(OutError, "Asset compilation provider module is retiring."),
				FAssetCompilingManagerRegistration{};
		FName DomainName;
		std::vector<FName> Dependencies;
		{
			auto Invocation = OwnerGate.TryEnter();
			if (!Invocation)
				return SetError(OutError, "Asset compilation provider module is retiring."),
					FAssetCompilingManagerRegistration{};
			DomainName = Manager->GetAssetTypeName();
			Dependencies = Manager->GetDependentTypeNames();
		}
		if (!IsCanonicalDomainName(DomainName))
			return SetError(OutError, "Asset compilation domain name is not canonical."),
				FAssetCompilingManagerRegistration{};
		{
			std::lock_guard Lock(GAssetCompilingMutex);
			if (!GRunning || GShutdown)
				return SetError(OutError, "Asset compilation manager is not accepting registrations."),
					FAssetCompilingManagerRegistration{};
			if (GManagers.contains(DomainName))
				return SetError(OutError, "Asset compilation domain is already registered."),
					FAssetCompilingManagerRegistration{};
		}
		{
			auto Invocation = OwnerGate.TryEnter();
			if (!Invocation || !Manager->Start(OutError))
				return FAssetCompilingManagerRegistration{};
		}
		FAssetCompilingManagerRegistration Registration;
		bool bRollback = false;
		{
			std::lock_guard Lock(GAssetCompilingMutex);
			if (!GRunning || GShutdown || GManagers.contains(DomainName))
			{
				bRollback = true;
			}
			else
			{
				const uint64 Generation = GNextGeneration++;
				GManagers.emplace(DomainName, FRegisteredAssetCompilingManager{
					Manager, Dependencies, OwnerGate, std::move(Resource), Generation, 0, true, false});
				std::vector<FName> NewOrder;
				std::unordered_map<FName, uint32> Levels;
				std::vector<std::string> Missing;
				if (!BuildOrder(GManagers, NewOrder, Levels, &Missing, OutError))
				{
					GManagers.erase(DomainName);
					bRollback = true;
				}
				else
				{
					for (auto& [Name, Entry] : GManagers) Entry.DependencyLevel = Levels[Name];
					GOrderedDomains = std::move(NewOrder);
					for (std::string& Message : Missing)
						if (GMessages.size() < 32) GMessages.push_back(std::move(Message));
					Registration.DomainName = DomainName;
					Registration.Generation = Generation;
				}
			}
		}
		if (bRollback)
		{
			auto Invocation = OwnerGate.TryEnter();
			if (Invocation) { Manager->StopAdmission(); Manager->Shutdown(); }
			if (OutError && OutError->empty())
				*OutError = "Asset compilation registration state changed during start.";
			return {};
		}
		if (OutError) OutError->clear();
		return Registration;
	}

	auto FAssetCompilingManager::RegisterBuiltInManager(
		std::shared_ptr<IAssetCompilingManager> Manager, std::string* OutError)
		-> FAssetCompilingManagerRegistration
	{
		if (!Manager) return {};
		const FName DomainName = Manager->GetAssetTypeName();
		const std::vector<FName> Dependencies = Manager->GetDependentTypeNames();
		if (!IsCanonicalDomainName(DomainName)) return {};
		if (!Manager->Start(OutError)) return {};
		FAssetCompilingManagerRegistration Result;
		bool bRollback = false;
		{
			std::lock_guard Lock(GAssetCompilingMutex);
			if (!GRunning || GManagers.contains(DomainName)) bRollback = true;
			else
			{
				const uint64 Generation = GNextGeneration++;
				GManagers.emplace(DomainName, FRegisteredAssetCompilingManager{
					Manager, Dependencies, {}, {}, Generation, 0, true, true});
				std::vector<FName> NewOrder;
				std::unordered_map<FName, uint32> Levels;
				if (!BuildOrder(GManagers, NewOrder, Levels, &GMessages, OutError))
				{
					GManagers.erase(DomainName);
					bRollback = true;
				}
				else
				{
					for (auto& [Name, Entry] : GManagers) Entry.DependencyLevel = Levels[Name];
					GOrderedDomains = std::move(NewOrder);
					Result.DomainName = DomainName;
					Result.Generation = Generation;
				}
			}
		}
		if (bRollback) { Manager->StopAdmission(); Manager->Shutdown(); return {}; }
		return Result;
	}

	auto FAssetCompilingManager::Unregister(FName DomainName, uint64 Generation) -> void
	{
		FRegisteredAssetCompilingManager Removed;
		{
			std::lock_guard Lock(GAssetCompilingMutex);
			const auto It = GManagers.find(DomainName);
			if (It == GManagers.end() || It->second.Generation != Generation) return;
			Removed = std::move(It->second);
			GManagers.erase(It);
			std::unordered_map<FName, uint32> Levels;
			BuildOrder(GManagers, GOrderedDomains, Levels, nullptr, nullptr);
			for (auto& [Name, Entry] : GManagers) Entry.DependencyLevel = Levels[Name];
		}
		FDispatchEntry Entry{DomainName, Removed.Manager, Removed.Gate,
			std::move(Removed.RegistryResource), Removed.DependencyLevel};
		Invoke(Entry, [](IAssetCompilingManager& Manager) { Manager.StopAdmission(); });
		FAssetCompileProcessResult Result;
		Invoke(Entry, [&](IAssetCompilingManager& Manager) {
			Result = Manager.FinishAllCompilation();
			Manager.Shutdown();
		});
		Publish(DomainName, Result);
	}

	auto FAssetCompilingManager::ProcessAsyncTasks(const FAssetCompileProcessParams& Params)
		-> FAssetCompileProcessResult
	{
		FAssetCompileProcessResult Aggregate;
		if (!IsSupportedThread() || Params.MaximumCompletions == 0) return Aggregate;
		auto Entries = MakeSnapshot();
		if (Entries.empty()) return Aggregate;
		std::unordered_map<FName, FAssetCompileProcessResult> DomainResults;
		auto Accumulate = [&](const FName& DomainName,
			FAssetCompileProcessResult Item) {
			CoalesceSuccessfulAssets(DomainResults[DomainName], Item);
			CoalesceSuccessfulAssets(Aggregate, std::move(Item));
		};
		uint32 RemainingManagers = static_cast<uint32>(Entries.size());
		const uint64 Cursor = GRoundRobinCursor++;
		for (size_t Begin = 0; Begin < Entries.size();)
		{
			size_t End = Begin + 1;
			while (End < Entries.size()
				&& Entries[End].DependencyLevel == Entries[Begin].DependencyLevel) ++End;
			const size_t Count = End - Begin;
			for (size_t Offset = 0; Offset < Count; ++Offset)
			{
				if (Aggregate.ProcessedCompletionCount >= Params.MaximumCompletions) break;
				if (Params.Deadline && std::chrono::steady_clock::now() >= *Params.Deadline) break;
				FDispatchEntry& Entry = Entries[Begin + (Offset + Cursor) % Count];
				const uint32 Remaining = Params.MaximumCompletions
					- Aggregate.ProcessedCompletionCount;
				const uint32 Share = std::max(1u, Remaining / std::max(1u, RemainingManagers));
				FAssetCompileProcessResult Item;
				Invoke(Entry, [&](IAssetCompilingManager& Manager) {
					Item = Manager.ProcessAsyncTasks({Share, Params.Deadline});
				});
				Accumulate(Entry.DomainName, std::move(Item));
				--RemainingManagers;
			}
			Begin = End;
		}
		// Reclaim quota left unused by idle domains. The first pass guarantees
		// every domain a bounded opportunity; later passes spend only the budget
		// that those domains did not consume.
		while (Aggregate.ProcessedCompletionCount < Params.MaximumCompletions)
		{
			bool bMadeProgress = false;
			for (auto& Entry : Entries)
			{
				if (Aggregate.ProcessedCompletionCount >= Params.MaximumCompletions) break;
				if (Params.Deadline && std::chrono::steady_clock::now() >= *Params.Deadline) break;
				FAssetCompileProcessResult Item;
				Invoke(Entry, [&](IAssetCompilingManager& Manager) {
					Item = Manager.ProcessAsyncTasks({
						Params.MaximumCompletions - Aggregate.ProcessedCompletionCount,
						Params.Deadline});
				});
				bMadeProgress |= Item.ProcessedCompletionCount != 0;
				Accumulate(Entry.DomainName, std::move(Item));
			}
			if (!bMadeProgress) break;
		}
		for (const auto& Entry : Entries)
			if (const auto It = DomainResults.find(Entry.DomainName);
				It != DomainResults.end()) Publish(Entry.DomainName, It->second);
		{
			std::lock_guard Lock(GAssetCompilingMutex);
			GProcessedCompletions = SaturatingAdd(
				GProcessedCompletions, Aggregate.ProcessedCompletionCount);
		}
		return Aggregate;
	}

	auto FAssetCompilingManager::GetNumRemainingAssets() const -> uint64
	{
		uint64 Count = 0;
		auto Entries = MakeSnapshot();
		for (auto& Entry : Entries)
			Invoke(Entry, [&](IAssetCompilingManager& Manager) {
				Count = SaturatingAdd(Count, Manager.GetNumRemainingAssets());
			});
		return Count;
	}

	auto FAssetCompilingManager::FinishCompilationForObjects(
		std::span<DObject* const> Objects) -> FAssetCompileProcessResult
	{
		FAssetCompileProcessResult Aggregate;
		if (!IsSupportedThread()) return Aggregate;
		auto Entries = MakeSnapshot();
		for (auto& Entry : Entries)
		{
			FAssetCompileProcessResult Item;
			Invoke(Entry, [&](IAssetCompilingManager& Manager) {
				Item = Manager.FinishCompilationForObjects(Objects);
			});
			Publish(Entry.DomainName, Item);
			CoalesceSuccessfulAssets(Aggregate, std::move(Item));
		}
		return Aggregate;
	}

	auto FAssetCompilingManager::MarkCompilationAsCanceled(
		std::span<DObject* const> Objects) -> bool
	{
		if (!IsSupportedThread()) return false;
		auto Entries = MakeSnapshot();
		for (auto& Entry : Entries)
			Invoke(Entry, [&](IAssetCompilingManager& Manager) {
				Manager.MarkCompilationAsCanceled(Objects);
			});
		return true;
	}

	auto FAssetCompilingManager::FinishAllCompilation() -> FAssetCompileProcessResult
	{
		FAssetCompileProcessResult Aggregate;
		if (!IsSupportedThread()) return Aggregate;
		auto Entries = MakeSnapshot();
		for (auto& Entry : Entries)
		{
			FAssetCompileProcessResult Item;
			Invoke(Entry, [&](IAssetCompilingManager& Manager) {
				Item = Manager.FinishAllCompilation();
			});
			Publish(Entry.DomainName, Item);
			CoalesceSuccessfulAssets(Aggregate, std::move(Item));
		}
		return Aggregate;
	}

	auto FAssetCompilingManager::GetDiagnostics() const
		-> FAssetCompilingManagerDiagnostics
	{
		FAssetCompilingManagerDiagnostics Result;
		{
			std::lock_guard Lock(GAssetCompilingMutex);
			Result.ManagerCount = static_cast<uint32>(GManagers.size());
			Result.ProcessedCompletionCount = GProcessedCompletions;
			Result.bAcceptingRequests = GRunning && !GShutdown;
			Result.bShutdown = GShutdown;
			Result.Messages = GMessages;
		}
		auto Entries = MakeSnapshot();
		for (auto& Entry : Entries)
		{
			uint64 Remaining = 0;
			Invoke(Entry, [&](IAssetCompilingManager& Manager) {
				Remaining = Manager.GetNumRemainingAssets();
			});
			Result.Domains.push_back({Entry.DomainName, Remaining});
			Result.RemainingAssetCount = SaturatingAdd(
				Result.RemainingAssetCount, Remaining);
		}
		return Result;
	}

	auto FAssetCompilingManager::OnAssetPostCompile() -> FAssetPostCompileEvent&
	{
		return GPostCompileEvent;
	}

	auto FAssetCompilingManager::Shutdown() -> void
	{
		if (!IsSupportedThread()) return;
		std::vector<FDispatchEntry> Entries;
		{
			std::lock_guard Lock(GAssetCompilingMutex);
			if (GShutdown) return;
			GRunning = false;
		}
		Entries = MakeSnapshot(true);
		for (auto& Entry : Entries)
			Invoke(Entry, [](IAssetCompilingManager& Manager) { Manager.StopAdmission(); });
		for (auto& Entry : Entries)
		{
			FAssetCompileProcessResult Item;
			Invoke(Entry, [&](IAssetCompilingManager& Manager) {
				Item = Manager.FinishAllCompilation();
				Manager.Shutdown();
			});
			Publish(Entry.DomainName, Item);
		}
		std::lock_guard Lock(GAssetCompilingMutex);
		GManagers.clear();
		GOrderedDomains.clear();
		GShutdown = true;
	}

	auto FAssetCompilingManager::IsAcceptingRequests() const -> bool
	{
		std::lock_guard Lock(GAssetCompilingMutex);
		return GRunning && !GShutdown;
	}

	// Material supplies the built-in manager from its private implementation.
	extern auto CreateMaterialCompilingManager() -> std::shared_ptr<IAssetCompilingManager>;

	auto InitializeAssetCompilingManager() -> bool
	{
		auto& Manager = FAssetCompilingManager::Get();
		std::string Error;
		if (!Manager.Start(&Error)) return false;
		auto Registration = Manager.RegisterBuiltInManager(
			CreateMaterialCompilingManager(), &Error);
		if (!Registration.IsValid())
		{
			DURIN_ERROR("Material compilation manager failed to register: {}", Error);
			Manager.Shutdown();
			return false;
		}
		// The aggregate owns built-in lifetime; disarm the scoped token.
		Registration.Generation = 0;
		return true;
	}

	auto ShutdownAssetCompilingManager() -> void
	{
		FAssetCompilingManager::Get().Shutdown();
	}
}
