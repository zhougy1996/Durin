#include "TerrainAuthoringFeature.h"

#include "Asset/MountedSource.h"
#include "DObject/ObjectHandle.h"
#include "EncodedSourceSnapshot.h"
#include "Terrain/TerrainHeightmap.h"
#include "Terrain/TerrainHeightmapBuildOperations.h"
#include "Terrain/TerrainHeightmapDerivedData.h"
#include "Terrain/TerrainHeightmapPostLoad.h"
#include "TerrainHeightmapBuildAdapter.h"
#include "AssetForge/Builtins/TerrainHeightmapImport.h"
#include "Threading/Task.h"
#include "AssetForge/ImportService.h"

namespace Durin::AssetForge::Builtins
{
	using namespace Durin::Asset;
	namespace
	{
		constexpr size_t MaximumConcurrentTerrainLoads = 2;
		constexpr size_t MaximumTerrainLoadSubscribers = 64;

		struct FTerrainAuthoringLoadResult
		{
			std::shared_ptr<const FTerrainHeightmapPayload> Payload;
			FTerrainHeightmapSourceImportData Source;
			std::string DerivedDataKey;
			std::string Diagnostic;
			uint64 SourceFileSize = 0;
			int64 SourceLastWriteTime = 0;
			ETerrainHeightmapStatus FailureStatus = ETerrainHeightmapStatus::Failed;
			bool bLoadedFromDdc = false;
			bool bSucceeded = false;
		};

		struct FTerrainAuthoringLoadWork
		{
			std::mutex Mutex;
			FTerrainAuthoringLoadResult Result;
			FTaskHandle Worker;
			std::string CoalescingKey;
		};

		struct FTerrainAuthoringLoadPending
		{
			std::shared_ptr<FTerrainAuthoringLoadWork> Work;
			FTaskHandle Publisher;
			uint64 Generation = 0;
		};

	}

	struct FTerrainAuthoringState
	{
		std::mutex Mutex;
		std::unordered_map<std::string, std::weak_ptr<FTerrainAuthoringLoadWork>> LoadsByKey;
		std::unordered_map<uint64, FTerrainAuthoringLoadPending> PendingByObject;
		std::unordered_map<uint64, FImportHandle> PendingImportByObject;
		FAsyncOperationGroup OperationGroup;
	};

	namespace
	{

		auto ObjectKey(FObjectHandle Handle) -> uint64
		{
			return static_cast<uint64>(Handle.Generation) << 32 | Handle.Index;
		}

		auto BuildTerrainLoadResult(
			FTerrainHeightmapSourceImportData Source, std::string Key,
			const FTaskCancellationToken& Token) -> FTerrainAuthoringLoadResult
		{
			FTerrainAuthoringLoadResult Result;
			Result.Source = Source;
			Result.DerivedDataKey = Key;
			std::string DdcError;
			Asset::Build::FTerrainHeightmapDerivedDataLoadDiagnostics DdcDiagnostics;
			if (!Key.empty() && Asset::Build::LoadTerrainHeightmapDerivedData(
				Key, Result.Payload, DdcError, &DdcDiagnostics))
			{
				Result.Diagnostic = std::format(
					"Loaded terrain heightmap payload from DDC: query {} us, read {} us, decode {} us.",
					DdcDiagnostics.QueryNanoseconds / 1000, DdcDiagnostics.ReadNanoseconds / 1000,
					DdcDiagnostics.DecodeNanoseconds / 1000);
				Result.bLoadedFromDdc = true;
				Result.bSucceeded = true;
				return Result;
			}
			if (Token.IsCancellationRequested())
			{
				Result.Diagnostic = "Terrain heightmap authoring load was canceled.";
				return Result;
			}

			const auto CaptureStart = std::chrono::steady_clock::now();
			FEncodedSourceSnapshot Snapshot;
			std::string Error;
			if (!CaptureEncodedSource(Source.SourcePath, Snapshot, Error,
				MaximumTerrainHeightmapEncodedBytes))
			{
				Result.FailureStatus = ETerrainHeightmapStatus::SourceUnavailable;
				Result.Diagnostic = std::format(
					"Terrain heightmap DDC recovery failed ({}); source is unavailable: {}", DdcError, Error);
				return Result;
			}
			const uint64 CaptureNanoseconds = static_cast<uint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - CaptureStart).count());
			if (Token.IsCancellationRequested())
			{
				Result.Diagnostic = "Terrain heightmap authoring load was canceled after source capture.";
				return Result;
			}

			const auto DecodeStart = std::chrono::steady_clock::now();
			FTerrainHeightmapSourceData SourceData;
			if (!TranslateTerrainHeightmapSource(
				std::filesystem::path(Snapshot.SourcePath.Path).extension().generic_string(),
				Snapshot.GetBytes(), SourceData, Error))
			{
				Result.Diagnostic = std::format("Terrain heightmap source decode failed: {}", Error);
				return Result;
			}
			const uint64 DecodeNanoseconds = static_cast<uint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - DecodeStart).count());
			if (Token.IsCancellationRequested())
			{
				Result.Diagnostic = "Terrain heightmap authoring load was canceled after source decode.";
				return Result;
			}

			const auto BuildStart = std::chrono::steady_clock::now();
			Asset::Build::FTerrainHeightmapBuildProduct Product;
			if (!Asset::Build::BuildTerrainHeightmap({
				.Samples = std::move(SourceData.Samples),
				.Width = SourceData.Width,
				.Height = SourceData.Height,
				.SourceContentHashLow = Snapshot.ContentHash.HashLow,
				.SourceContentHashHigh = Snapshot.ContentHash.HashHigh,
				.DecoderId = SourceData.DecoderId,
				.DecoderVersion = SourceData.DecoderVersion,
				.SourceFormat = SourceData.SourceFormat,
				.SourceProfileVersion = SourceData.SourceProfileVersion,
				.bQueryDerivedData = false,
				.ShouldCancel = [&Token] { return Token.IsCancellationRequested(); }}, Product, Error))
			{
				Result.Diagnostic = std::format("Terrain heightmap source rebuild failed: {}", Error);
				return Result;
			}
			const uint64 BuildNanoseconds = static_cast<uint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - BuildStart).count());
			if (Token.IsCancellationRequested())
			{
				Result.Diagnostic = "Terrain heightmap authoring load was canceled after payload build.";
				return Result;
			}
			Result.Payload = std::move(Product.Payload);
			Result.DerivedDataKey = std::move(Product.DerivedDataKey);
			Result.Source = {
				.SourcePath = Snapshot.SourcePath,
				.SourceContentHashLow = Snapshot.ContentHash.HashLow,
				.SourceContentHashHigh = Snapshot.ContentHash.HashHigh,
				.DecoderId = SourceData.DecoderId,
				.DecoderVersion = SourceData.DecoderVersion,
				.SourceFormat = SourceData.SourceFormat,
				.SourceProfileVersion = SourceData.SourceProfileVersion};
			Result.SourceFileSize = Snapshot.FileSize;
			Result.SourceLastWriteTime = Snapshot.LastWriteTime;
			Result.Diagnostic = std::format(
				"Rebuilt terrain heightmap after DDC miss/corruption: capture {} us, decode {} us, build/store {} us.",
				CaptureNanoseconds / 1000, DecodeNanoseconds / 1000, BuildNanoseconds / 1000);
			Result.bSucceeded = true;
			return Result;
		}

		auto PublishTerrainLoad(
			FTerrainAuthoringState& State,
			FObjectHandle HeightmapHandle, uint64 Generation,
			const std::shared_ptr<FTerrainAuthoringLoadWork>& Work,
			std::string* OutError = nullptr) -> bool
		{
			auto* Heightmap = Cast<DTerrainHeightmap>(ResolveObjectHandle(HeightmapHandle));
			if (!IsValid(Heightmap) || !Heightmap->IsAuthoringLoadCurrent(Generation)) return false;
			FTerrainAuthoringLoadResult Result;
			{
				std::lock_guard Lock(Work->Mutex);
				Result = Work->Result;
			}
			bool bPublished = false;
			if (Result.bSucceeded && Result.Payload)
			{
				Heightmap->PublishAuthoringCandidate(
					std::move(Result.Source), Result.SourceFileSize, Result.SourceLastWriteTime,
					std::move(Result.Payload), std::move(Result.DerivedDataKey),
					std::move(Result.Diagnostic), false, false, Result.bLoadedFromDdc);
				bPublished = true;
			}
			else
			{
				if (OutError) *OutError = Result.Diagnostic;
				(void)Heightmap->FailAuthoringLoad(
					Generation, Result.FailureStatus, std::move(Result.Diagnostic));
			}
			std::lock_guard Lock(State.Mutex);
			State.PendingByObject.erase(ObjectKey(HeightmapHandle));
			if (auto Found = State.LoadsByKey.find(Work->CoalescingKey);
				Found != State.LoadsByKey.end() && Found->second.lock() == Work)
				State.LoadsByKey.erase(Found);
			return bPublished;
		}

		auto StartAsyncTerrainLoad(
			FTerrainAuthoringState& State,
			DTerrainHeightmap& Heightmap, std::string Key, std::string& OutError) -> bool
		{
			const FObjectHandle Handle = MakeObjectHandle(&Heightmap);
			if (IsObjectHandleNull(Handle)) return false;
			const std::string CoalescingKey = Key.empty()
				? std::format("source:{}", Heightmap.GetSourceImportData().SourcePath.Path) : Key;
			const uint64 Generation = Heightmap.BeginAuthoringLoad(
				Key.empty(), Key.empty()
					? "Terrain heightmap payload is rebuilding asynchronously from source."
					: "Terrain heightmap payload is loading asynchronously.");
			std::shared_ptr<FTerrainAuthoringLoadWork> Work;
			{
				std::lock_guard Lock(State.Mutex);
				std::erase_if(State.LoadsByKey, [](const auto& Entry) { return Entry.second.expired(); });
				if (auto Found = State.LoadsByKey.find(CoalescingKey); Found != State.LoadsByKey.end())
					Work = Found->second.lock();
				if (!Work)
				{
					if (State.LoadsByKey.size() >= MaximumConcurrentTerrainLoads)
					{
						OutError = "Terrain heightmap load admission reached its two-request byte bound.";
						(void)Heightmap.FailAuthoringLoad(Generation, ETerrainHeightmapStatus::Failed, OutError);
						return false;
					}
					Work = std::make_shared<FTerrainAuthoringLoadWork>();
					Work->CoalescingKey = CoalescingKey;
					const FTerrainHeightmapSourceImportData Source = Heightmap.GetSourceImportData();
					FTaskLaunchOptions Options;
					Options.CancellationToken = State.OperationGroup.GetCancellationToken();
					Options.Scope = State.OperationGroup.GetTaskScope();
					static const FTaskAttribution Attribution =
						RegisterTaskAttribution("TerrainHeightmap", "LoadPayload");
					Options.Attribution = Attribution;
					Work->Worker = LaunchCancelableTask("TerrainHeightmap.LoadPayload",
						[Work, Source, Key](const FTaskCancellationToken& Token) {
							FTerrainAuthoringLoadResult Result = BuildTerrainLoadResult(Source, Key, Token);
							std::lock_guard ResultLock(Work->Mutex);
							Work->Result = std::move(Result);
						}, Options);
					if (!Work->Worker.IsValid())
					{
						OutError = "The CPU task scheduler rejected Terrain heightmap loading.";
						(void)Heightmap.FailAuthoringLoad(Generation, ETerrainHeightmapStatus::Failed, OutError);
						return false;
					}
					State.LoadsByKey[CoalescingKey] = Work;
				}
				if (State.PendingByObject.size() >= MaximumTerrainLoadSubscribers)
				{
					OutError = "Terrain heightmap load subscriber bound was reached.";
					(void)Heightmap.FailAuthoringLoad(Generation, ETerrainHeightmapStatus::Failed, OutError);
					return false;
				}
				State.PendingByObject[ObjectKey(Handle)] = {.Work = Work, .Generation = Generation};
			}

			FTaskContinuationOptions PublishOptions;
			PublishOptions.Target = ETaskTarget::GameThreadDeferred;
			PublishOptions.CancellationToken = State.OperationGroup.GetCancellationToken();
			PublishOptions.Scope = State.OperationGroup.GetTaskScope();
			PublishOptions.EstimatedPayloadBytes = sizeof(FObjectHandle) + sizeof(uint64) + sizeof(std::shared_ptr<void>);
			FTerrainAuthoringLoadPending* Pending = nullptr;
			{
				std::lock_guard Lock(State.Mutex);
				Pending = &State.PendingByObject.at(ObjectKey(Handle));
				Pending->Publisher = ThenOutcome(Work->Worker, "TerrainHeightmap.PublishPayload",
					[&State, Handle, Generation, Work](FTaskOutcome<void>) {
						(void)PublishTerrainLoad(State, Handle, Generation, Work);
					}, PublishOptions);
				if (!Pending->Publisher.IsValid())
				{
					State.PendingByObject.erase(ObjectKey(Handle));
					OutError = "The GameThread executor rejected Terrain heightmap publication.";
					(void)Heightmap.FailAuthoringLoad(Generation, ETerrainHeightmapStatus::Failed, OutError);
					return false;
				}
			}
			OutError.clear();
			return true;
		}

		auto PostLoadTerrainHeightmap(
			FTerrainAuthoringState& State,
			DTerrainHeightmap& Heightmap, std::string& OutError) -> bool
		{
			std::string Key = Asset::Build::MakeTerrainHeightmapDerivedDataKey(Heightmap, OutError);
			const FGameThreadDeferredWorkQueueDiagnostics Deferred =
				GetGameThreadDeferredWorkQueueDiagnostics();
			if (State.OperationGroup.IsValid()
				&& IsTaskSchedulerRunning() && Deferred.bInstalled && Deferred.bAccepting)
				return StartAsyncTerrainLoad(State, Heightmap, std::move(Key), OutError);

			if (!Key.empty())
			{
				std::shared_ptr<const FTerrainHeightmapPayload> Payload;
				if (Asset::Build::LoadTerrainHeightmapDerivedData(Key, Payload, OutError))
				{
					const auto& Source = Heightmap.GetSourceImportData();
					Heightmap.PublishAuthoringCandidate(Source, 0, 0, std::move(Payload),
						std::move(Key), "Loaded terrain heightmap payload from DDC.",
						false, false, true);
					return true;
				}
			}
			FAssetPath Destination;
			if (!Heightmap.GetPackage()
				|| !FAssetPath::TryCreate(Heightmap.GetPackage()->GetPackagePath(),
					Destination, &OutError)) return false;
			FMountedSourceResolution SourceResolution;
			if (!ResolveMountedSourceReference(
				Destination.ToString(), Heightmap.GetSourceImportData().SourcePath.Path,
				EMountedSourceExistencePolicy::AllowMissing,
				SourceResolution, OutError))
			{
				const uint64 Generation = Heightmap.BeginAuthoringLoad(
					true, "Validating terrain heightmap recovery source.");
				(void)Heightmap.FailAuthoringLoad(
					Generation, ETerrainHeightmapStatus::Failed, OutError);
				return false;
			}
			if (!SourceResolution.bExists)
			{
				OutError = std::format(
					"Terrain heightmap recovery source '{}' is unavailable.",
					Heightmap.GetSourceImportData().SourcePath.Path);
				const uint64 Generation = Heightmap.BeginAuthoringLoad(
					true, "Validating terrain heightmap recovery source.");
				(void)Heightmap.FailAuthoringLoad(
					Generation, ETerrainHeightmapStatus::SourceUnavailable, OutError);
				return false;
			}
			FImportProvenance Existing;
			std::optional<FImportProvenance> Provenance;
			if (InspectTerrainHeightmapImportProvenance(Heightmap, Existing, OutError))
				Provenance = std::move(Existing);
			else OutError.clear();
			FImportRequest Request;
			if (!MakeTerrainHeightmapImportRequest(
				Heightmap.GetSourceImportData().SourcePath, Destination,
				EImportMode::Recover,
				{.OwnerId = std::format("TerrainHeightmap.Recovery:{}", Destination.ToString()),
					.ConflictIdentities = {Destination.ToString()}},
				std::move(Provenance), Request, OutError)) return false;
			Request.Lifetime = EImportOperationLifetime::SessionCritical;
			FImportHandle Import = GetImportService().SubmitImport(
				std::move(Request),
				std::format("Recover TerrainHeightmap {}", Destination.GetAssetName()));
			if (!Import)
			{
				OutError = "TerrainHeightmap AssetForge recovery could not be submitted.";
				return false;
			}
			{
				std::lock_guard Lock(State.Mutex);
				State.PendingImportByObject[ObjectKey(MakeObjectHandle(&Heightmap))] =
					std::move(Import);
			}
			OutError.clear();
			return true;
		}

		auto WaitForTerrainLoad(
			FTerrainAuthoringState& State,
			DTerrainHeightmap& Heightmap, std::string& OutError) -> bool
		{
			const FObjectHandle Handle = MakeObjectHandle(&Heightmap);
			FImportHandle Import;
			{
				std::lock_guard Lock(State.Mutex);
				if (const auto Found = State.PendingImportByObject.find(ObjectKey(Handle));
					Found != State.PendingImportByObject.end())
					Import = Found->second;
			}
			if (Import)
			{
				while (!Import.GetOperationHandle().GetSnapshot().IsTerminal())
				{
					GetImportService().PumpImportOperations();
					std::this_thread::yield();
				}
				FImportResult Result;
				const bool bHasResult = Import.TryGetResult(Result);
				{
					std::lock_guard Lock(State.Mutex);
					State.PendingImportByObject.erase(ObjectKey(Handle));
				}
				if (bHasResult && Result.Outcome.State == EImportOperationState::Succeeded)
				{
					OutError.clear();
					return true;
				}
				OutError = bHasResult ? Result.Outcome.Diagnostic
					: "TerrainHeightmap AssetForge recovery produced no result.";
				return false;
			}
			FTerrainAuthoringLoadPending Pending;
			{
				std::lock_guard Lock(State.Mutex);
				const auto Found = State.PendingByObject.find(ObjectKey(Handle));
				if (Found == State.PendingByObject.end())
				{
					if (Heightmap.GetStatus() == ETerrainHeightmapStatus::Ready) return true;
					OutError = Heightmap.GetLastDiagnostic();
					return false;
				}
				Pending = Found->second;
			}
			if (WaitTask(Pending.Work->Worker) != ETaskState::Succeeded)
			{
				OutError = "Terrain heightmap worker did not complete successfully.";
				return false;
			}
			const bool bPublished = PublishTerrainLoad(
				State, Handle, Pending.Generation, Pending.Work, &OutError);
			(void)CancelTask(Pending.Publisher);
			return bPublished;
		}
	}

	FTerrainAuthoringFeature::FTerrainAuthoringFeature()
		: State(std::make_unique<FTerrainAuthoringState>())
	{
	}

	FTerrainAuthoringFeature::~FTerrainAuthoringFeature()
	{
		Shutdown();
	}

	auto FTerrainAuthoringFeature::SetOperationGroup(FAsyncOperationGroup Group) -> bool
	{
		if (!Group.IsValid()) return false;
		std::lock_guard Lock(State->Mutex);
		if (State->OperationGroup.IsValid() || !State->LoadsByKey.empty()
			|| !State->PendingByObject.empty()) return false;
		State->OperationGroup = std::move(Group);
		return true;
	}

	auto FTerrainAuthoringFeature::Shutdown() -> void
	{
		std::vector<std::shared_ptr<FTerrainAuthoringLoadWork>> Works;
		std::vector<FTaskHandle> Publishers;
		std::vector<FImportOperationHandle> ImportOperations;
		{
			std::lock_guard Lock(State->Mutex);
			for (auto& [Key, Weak] : State->LoadsByKey)
				if (auto Work = Weak.lock()) Works.push_back(std::move(Work));
			for (auto& [Key, Pending] : State->PendingByObject)
				Publishers.push_back(Pending.Publisher);
			for (auto& [Key, Pending] : State->PendingImportByObject)
				ImportOperations.push_back(Pending.GetOperationHandle());
			State->PendingByObject.clear();
			State->PendingImportByObject.clear();
			State->LoadsByKey.clear();
		}
		for (const auto& Work : Works)
			(void)CancelTask(Work->Worker);
		for (const FTaskHandle& Publisher : Publishers) (void)CancelTask(Publisher);
		for (const FImportOperationHandle& Operation : ImportOperations)
			GetImportService().CancelAndDrainImportOperation(Operation);
	}

	auto FTerrainAuthoringFeature::PostLoadUncooked(
		DTerrainHeightmap& Heightmap, std::string& OutError) -> bool
	{
		return PostLoadTerrainHeightmap(*State, Heightmap, OutError);
	}

	auto FTerrainAuthoringFeature::WaitForAuthoringLoad(
		DTerrainHeightmap& Heightmap, std::string& OutError) -> bool
	{
		return WaitForTerrainLoad(*State, Heightmap, OutError);
	}

	auto FTerrainAuthoringFeature::ChangeSourceReference(
		DTerrainHeightmap& Heightmap,
		std::string_view SourceVirtualPath,
		std::string& OutError) -> bool
	{
		const FObjectHandle Handle = MakeObjectHandle(&Heightmap);
		std::shared_ptr<FTerrainAuthoringLoadWork> Work;
		FTaskHandle Publisher;
		FImportOperationHandle ImportOperation;
		bool bWorkHasOtherSubscribers = false;
		{
			std::lock_guard Lock(State->Mutex);
			if (const auto Found = State->PendingByObject.find(ObjectKey(Handle));
				Found != State->PendingByObject.end())
			{
				Work = Found->second.Work;
				Publisher = Found->second.Publisher;
				State->PendingByObject.erase(Found);
				if (auto KeyFound = State->LoadsByKey.find(Work->CoalescingKey);
					KeyFound != State->LoadsByKey.end() && KeyFound->second.lock() == Work)
				{
					bWorkHasOtherSubscribers = std::ranges::any_of(
						State->PendingByObject, [&](const auto& Entry) {
							return Entry.second.Work == Work;
						});
					if (!bWorkHasOtherSubscribers) State->LoadsByKey.erase(KeyFound);
				}
			}
			if (const auto Found = State->PendingImportByObject.find(ObjectKey(Handle));
				Found != State->PendingImportByObject.end())
			{
				ImportOperation = Found->second.GetOperationHandle();
				State->PendingImportByObject.erase(Found);
			}
		}
		if (Work && !bWorkHasOtherSubscribers) (void)CancelTask(Work->Worker);
		if (Publisher.IsValid()) (void)CancelTask(Publisher);
		if (ImportOperation.IsValid())
			GetImportService().CancelAndDrainImportOperation(ImportOperation);
		return ChangeTerrainHeightmapSourceReference(Heightmap, SourceVirtualPath, OutError);
	}
}
