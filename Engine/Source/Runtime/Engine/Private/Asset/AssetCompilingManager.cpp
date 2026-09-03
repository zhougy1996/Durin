#include "Asset/AssetCompilingManager.h"

#include "CoreGlobals.h"
#include "DObject/Class.h"
#include "DObject/DObjectGlobals.h"
#include "Logging/LogMacros.h"
#include "Materials/Material.h"
#include "Threading/RunnableThread.h"
#include "Texture/Texture2D.h"
#include "Texture/TextureCompilingManager.h"

#include <algorithm>
#include <limits>
#include <mutex>
#include <unordered_map>

namespace Durin
{
	namespace
	{
		struct FRegisteredAssetCompiler
		{
			std::vector<DClass*> AssetClasses;
			std::shared_ptr<IAssetCompilingManager> Manager;
			uint64 Generation = 0;
		};

		struct FDispatchEntry
		{
			std::shared_ptr<IAssetCompilingManager> Manager;
			FName CompilerName;
		};

		struct FRoutedBatch
		{
			FDispatchEntry Entry;
			std::vector<DObject*> Objects;
		};

		std::mutex GAssetCompilingMutex;
		std::unordered_map<FName, FRegisteredAssetCompiler> GCompilers;
		std::unordered_map<DClass*, FName> GClassRoutes;
		std::vector<FName> GOrderedCompilers;
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

		auto IsCanonicalCompilerName(const FName& Name) -> bool
		{
			const std::string Value = Name.ToString();
			return !Value.empty() && std::ranges::all_of(Value, [](char Character) {
				return Character >= 'a' && Character <= 'z'
					|| Character >= 'A' && Character <= 'Z'
					|| Character >= '0' && Character <= '9'
					|| Character == '.' || Character == '_' || Character == '-';
			});
		}

		auto HasValidUniqueClasses(std::span<DClass* const> Classes) -> bool
		{
			if (Classes.empty()) return false;
			for (size_t Index = 0; Index < Classes.size(); ++Index)
			{
				if (!Classes[Index]) return false;
				if (std::ranges::find(Classes.first(Index), Classes[Index])
					!= Classes.first(Index).end()) return false;
			}
			return true;
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

		auto RebuildCompilerOrder() -> void
		{
			GOrderedCompilers.clear();
			GOrderedCompilers.reserve(GCompilers.size());
			for (const auto& [Name, Entry] : GCompilers) GOrderedCompilers.push_back(Name);
			std::ranges::sort(GOrderedCompilers, {},
				[](const FName& Name) { return Name.ToString(); });
		}

		auto MakeSnapshot(bool bReverse = false) -> std::vector<FDispatchEntry>
		{
			std::vector<FDispatchEntry> Result;
			std::lock_guard Lock(GAssetCompilingMutex);
			Result.reserve(GOrderedCompilers.size());
			for (const FName& Name : GOrderedCompilers)
			{
				const auto It = GCompilers.find(Name);
				if (It == GCompilers.end()) continue;
				Result.push_back({It->second.Manager, Name});
			}
			if (bReverse) std::ranges::reverse(Result);
			return Result;
		}

		auto MakeRoutedBatches(std::span<DObject* const> Objects)
			-> std::vector<FRoutedBatch>
		{
			std::vector<FRoutedBatch> Result;
			std::lock_guard Lock(GAssetCompilingMutex);
			for (DObject* Object : Objects)
			{
				if (!IsValid(Object)) continue;
				FName CompilerName;
				for (DClass* Class = Object->GetClass(); Class; Class = Class->GetSuperClass())
				{
					const auto Route = GClassRoutes.find(Class);
					if (Route != GClassRoutes.end())
					{
						CompilerName = Route->second;
						break;
					}
				}
				if (CompilerName.IsNone()) continue;
				auto Batch = std::ranges::find(Result, CompilerName,
					[](const FRoutedBatch& Item) { return Item.Entry.CompilerName; });
				if (Batch == Result.end())
				{
					const auto Registered = GCompilers.find(CompilerName);
					if (Registered == GCompilers.end()) continue;
					Result.push_back({{Registered->second.Manager, CompilerName}, {}});
					Batch = std::prev(Result.end());
				}
				Batch->Objects.push_back(Object);
			}
			std::ranges::sort(Result, {}, [](const FRoutedBatch& Batch) {
				return Batch.Entry.CompilerName.ToString();
			});
			return Result;
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

		auto Publish(const FName& CompilerName, const FAssetCompileProcessResult& Result) -> void
		{
			if (Result.SuccessfullyCompiledAssets.empty()) return;
			FAssetCompileProcessResult Coalesced;
			CoalesceSuccessfulAssets(Coalesced, Result);
			GPostCompileEvent.Broadcast({CompilerName,
				std::move(Coalesced.SuccessfullyCompiledAssets)});
		}

		auto ValidateRegistrationLocked(
			const FAssetCompilingManagerRegistration& Registration,
			std::string* OutError) -> bool
		{
			if (!GRunning || GShutdown)
				return SetError(OutError,
					"Asset compiling manager is not accepting registrations.");
			if (GCompilers.contains(Registration.Name))
				return SetError(OutError, "Asset compiler name is already registered.");
			for (DClass* Class : Registration.AssetClasses)
				if (GClassRoutes.contains(Class))
					return SetError(OutError, "Asset class already has an exact compiler route.");
			return true;
		}
	}

	FAssetCompilerRegistrationHandle::~FAssetCompilerRegistrationHandle() { Reset(); }
	FAssetCompilerRegistrationHandle::FAssetCompilerRegistrationHandle(
		FAssetCompilerRegistrationHandle&& Other) noexcept
		: CompilerName(Other.CompilerName), Generation(std::exchange(Other.Generation, 0)) {}
	auto FAssetCompilerRegistrationHandle::operator=(
		FAssetCompilerRegistrationHandle&& Other) noexcept
		-> FAssetCompilerRegistrationHandle&
	{
		if (this != &Other)
		{
			Reset();
			CompilerName = Other.CompilerName;
			Generation = std::exchange(Other.Generation, 0);
		}
		return *this;
	}
	auto FAssetCompilerRegistrationHandle::Reset() -> void
	{
		if (Generation == 0) return;
		FAssetCompilingManager::Get().Unregister(CompilerName, Generation);
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
			"Asset compiling manager must start on GameThread.");
		std::lock_guard Lock(GAssetCompilingMutex);
		if (GRunning) return true;
		if (GShutdown) return SetError(OutError,
			"Asset compiling manager cannot restart after terminal shutdown.");
		GRunning = true;
		if (OutError) OutError->clear();
		return true;
	}

	auto FAssetCompilingManager::RegisterCompiler(
		FAssetCompilingManagerRegistration Registration,
		std::string* OutError)
		-> FAssetCompilerRegistrationHandle
	{
		if (!Registration.Manager || !IsCanonicalCompilerName(Registration.Name)
			|| !HasValidUniqueClasses(Registration.AssetClasses))
		{
			SetError(OutError, "Asset compiler registration is invalid.");
			return {};
		}
		if (!IsSupportedThread())
		{
			SetError(OutError, "Asset compiler registration requires GameThread.");
			return {};
		}

		{
			std::lock_guard Lock(GAssetCompilingMutex);
			if (!ValidateRegistrationLocked(Registration, OutError)) return {};
		}
		if (!Registration.Manager->Start(OutError)) return {};
		FAssetCompilerRegistrationHandle Result;
		bool bRollback = false;
		{
			std::lock_guard Lock(GAssetCompilingMutex);
			if (!ValidateRegistrationLocked(Registration, OutError)) bRollback = true;
			else
			{
				const uint64 Generation = GNextGeneration++;
				for (DClass* Class : Registration.AssetClasses)
					GClassRoutes.emplace(Class, Registration.Name);
				GCompilers.emplace(Registration.Name, FRegisteredAssetCompiler{
					std::move(Registration.AssetClasses),
					Registration.Manager, Generation});
				RebuildCompilerOrder();
				Result.CompilerName = Registration.Name;
				Result.Generation = Generation;
			}
		}
		if (bRollback)
		{
			Registration.Manager->StopAdmission();
			Registration.Manager->Shutdown();
			return {};
		}
		if (OutError) OutError->clear();
		return Result;
	}

	auto FAssetCompilingManager::Unregister(FName CompilerName, uint64 Generation) -> void
	{
		requiref(IsSupportedThread(), "Asset compiler unregistration requires GameThread.");
		FRegisteredAssetCompiler Removed;
		{
			std::lock_guard Lock(GAssetCompilingMutex);
			const auto It = GCompilers.find(CompilerName);
			if (It == GCompilers.end() || It->second.Generation != Generation) return;
			Removed = std::move(It->second);
			for (DClass* Class : Removed.AssetClasses) GClassRoutes.erase(Class);
			GCompilers.erase(It);
			RebuildCompilerOrder();
		}
		Removed.Manager->StopAdmission();
		FAssetCompileProcessResult Result = Removed.Manager->FinishAllCompilation();
		Removed.Manager->Shutdown();
		Publish(CompilerName, Result);
	}

	auto FAssetCompilingManager::ProcessAsyncTasks(const FAssetCompileProcessParams& Params)
		-> FAssetCompileProcessResult
	{
		FAssetCompileProcessResult Aggregate;
		if (!IsSupportedThread() || Params.MaximumCompletions == 0) return Aggregate;
		auto Entries = MakeSnapshot();
		if (Entries.empty()) return Aggregate;
		std::unordered_map<FName, FAssetCompileProcessResult> CompilerResults;
		auto Accumulate = [&](const FName& CompilerName, FAssetCompileProcessResult Item) {
			CoalesceSuccessfulAssets(CompilerResults[CompilerName], Item);
			CoalesceSuccessfulAssets(Aggregate, std::move(Item));
		};
		const uint64 Cursor = GRoundRobinCursor++;
		for (size_t Offset = 0; Offset < Entries.size(); ++Offset)
		{
			if (Aggregate.ProcessedCompletionCount >= Params.MaximumCompletions) break;
			if (Params.Deadline && std::chrono::steady_clock::now() >= *Params.Deadline) break;
			FDispatchEntry& Entry = Entries[(Offset + Cursor) % Entries.size()];
			const uint32 Remaining = Params.MaximumCompletions
				- Aggregate.ProcessedCompletionCount;
			const uint32 RemainingCompilers = static_cast<uint32>(Entries.size() - Offset);
			FAssetCompileProcessResult Item = Entry.Manager->ProcessAsyncTasks({
				std::max(1u, Remaining / RemainingCompilers), Params.Deadline});
			Accumulate(Entry.CompilerName, std::move(Item));
		}
		while (Aggregate.ProcessedCompletionCount < Params.MaximumCompletions)
		{
			bool bMadeProgress = false;
			for (auto& Entry : Entries)
			{
				if (Aggregate.ProcessedCompletionCount >= Params.MaximumCompletions) break;
				if (Params.Deadline && std::chrono::steady_clock::now() >= *Params.Deadline) break;
				FAssetCompileProcessResult Item = Entry.Manager->ProcessAsyncTasks({
					Params.MaximumCompletions - Aggregate.ProcessedCompletionCount,
					Params.Deadline});
				bMadeProgress |= Item.ProcessedCompletionCount != 0;
				Accumulate(Entry.CompilerName, std::move(Item));
			}
			if (!bMadeProgress) break;
		}
		for (const auto& Entry : Entries)
			if (const auto It = CompilerResults.find(Entry.CompilerName);
				It != CompilerResults.end()) Publish(Entry.CompilerName, It->second);
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
			Count = SaturatingAdd(Count, Entry.Manager->GetNumRemainingAssets());
		return Count;
	}

	auto FAssetCompilingManager::FinishCompilationForObjects(
		std::span<DObject* const> Objects) -> FAssetCompileProcessResult
	{
		FAssetCompileProcessResult Aggregate;
		if (!IsSupportedThread()) return Aggregate;
		for (FRoutedBatch& Batch : MakeRoutedBatches(Objects))
		{
			FAssetCompileProcessResult Item = Batch.Entry.Manager->FinishCompilationForObjects(Batch.Objects);
			Publish(Batch.Entry.CompilerName, Item);
			CoalesceSuccessfulAssets(Aggregate, std::move(Item));
		}
		return Aggregate;
	}

	auto FAssetCompilingManager::FinishCompilationForObject(DObject& Object)
		-> FAssetCompileProcessResult
	{
		DObject* ObjectPointer = &Object;
		return FinishCompilationForObjects(std::span<DObject* const>(&ObjectPointer, 1));
	}

	auto FAssetCompilingManager::MarkCompilationAsCanceled(
		std::span<DObject* const> Objects) -> void
	{
		if (!IsSupportedThread()) return;
		for (FRoutedBatch& Batch : MakeRoutedBatches(Objects))
			Batch.Entry.Manager->MarkCompilationAsCanceled(Batch.Objects);
	}

	auto FAssetCompilingManager::MarkCompilationAsCanceled(DObject& Object) -> void
	{
		DObject* ObjectPointer = &Object;
		MarkCompilationAsCanceled(std::span<DObject* const>(&ObjectPointer, 1));
	}

	auto FAssetCompilingManager::FinishAllCompilation() -> FAssetCompileProcessResult
	{
		FAssetCompileProcessResult Aggregate;
		if (!IsSupportedThread()) return Aggregate;
		for (auto& Entry : MakeSnapshot())
		{
			FAssetCompileProcessResult Item = Entry.Manager->FinishAllCompilation();
			Publish(Entry.CompilerName, Item);
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
			Result.CompilerCount = static_cast<uint32>(GCompilers.size());
			Result.ProcessedCompletionCount = GProcessedCompletions;
			Result.bAcceptingRequests = GRunning && !GShutdown;
			Result.bShutdown = GShutdown;
			Result.Messages = GMessages;
		}
		for (auto& Entry : MakeSnapshot())
		{
			const uint64 Remaining = Entry.Manager->GetNumRemainingAssets();
			Result.Compilers.push_back({Entry.CompilerName, Remaining});
			Result.RemainingAssetCount = SaturatingAdd(Result.RemainingAssetCount, Remaining);
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
		{
			std::lock_guard Lock(GAssetCompilingMutex);
			if (GShutdown) return;
			GRunning = false;
			GClassRoutes.clear();
		}
		auto Entries = MakeSnapshot(true);
		for (auto& Entry : Entries)
			Entry.Manager->StopAdmission();
		for (auto& Entry : Entries)
		{
			FAssetCompileProcessResult Item = Entry.Manager->FinishAllCompilation();
			Entry.Manager->Shutdown();
			Publish(Entry.CompilerName, Item);
		}
		std::lock_guard Lock(GAssetCompilingMutex);
		GCompilers.clear();
		GOrderedCompilers.clear();
		GShutdown = true;
	}

	auto FAssetCompilingManager::IsAcceptingRequests() const -> bool
	{
		std::lock_guard Lock(GAssetCompilingMutex);
		return GRunning && !GShutdown;
	}

	extern auto CreateMaterialCompilingManager()
		-> std::shared_ptr<IAssetCompilingManager>;

	auto InitializeAssetCompilingManager() -> bool
	{
		auto& Aggregate = FAssetCompilingManager::Get();
		std::string Error;
		if (!Aggregate.Start(&Error)) return false;
		auto MaterialRegistration = Aggregate.RegisterCompiler({
			.Name = FName("Durin.Material"),
			.AssetClasses = {DMaterial::StaticClass()},
			.Manager = CreateMaterialCompilingManager()}, &Error);
		if (!MaterialRegistration.IsValid())
		{
			DURIN_ERROR("Material compiling manager failed to register: {}", Error);
			Aggregate.Shutdown();
			return false;
		}
		auto TextureRegistration = Aggregate.RegisterCompiler({
			.Name = FName("Durin.Texture"),
			.AssetClasses = {DTexture2D::StaticClass()},
			.Manager = AssetPrivate::CreateTextureCompilingManager()}, &Error);
		if (!TextureRegistration.IsValid())
		{
			DURIN_ERROR("Texture compiling manager failed to register: {}", Error);
			Aggregate.Shutdown();
			return false;
		}
		MaterialRegistration.Generation = 0;
		TextureRegistration.Generation = 0;
		return true;
	}

	auto ShutdownAssetCompilingManager() -> void
	{
		FAssetCompilingManager::Get().Shutdown();
	}
}
