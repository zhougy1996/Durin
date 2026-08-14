#include "TerrainHeightmapAuthoringPolicy.h"

#include "DObject/ObjectHandle.h"
#include "EncodedSourceSnapshot.h"
#include "Terrain/TerrainHeightmap.h"
#include "Terrain/TerrainHeightmapBuildOperations.h"
#include "Terrain/TerrainHeightmapDerivedData.h"
#include "Terrain/TerrainHeightmapPostLoad.h"
#include "TerrainHeightmapBuildAdapter.h"
#include "TerrainHeightmapSourceTranslation.h"
#include "Threading/Task.h"

namespace Durin::Asset::Import::Standard
{
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
			FTaskCancellationSource Cancellation;
			FTaskHandle Worker;
			std::string CoalescingKey;
		};

		struct FTerrainAuthoringLoadPending
		{
			std::shared_ptr<FTerrainAuthoringLoadWork> Work;
			FTaskHandle Publisher;
			uint64 Generation = 0;
		};

		std::mutex GTerrainLoadMutex;
		std::unordered_map<std::string, std::weak_ptr<FTerrainAuthoringLoadWork>> GTerrainLoadsByKey;
		std::unordered_map<uint64, FTerrainAuthoringLoadPending> GTerrainPendingByObject;
		bool GTerrainHeightmapAuthoringPolicyRegistered = false;

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
				.SourceProfileVersion = SourceData.SourceProfileVersion}, Product, Error))
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
			std::lock_guard Lock(GTerrainLoadMutex);
			GTerrainPendingByObject.erase(ObjectKey(HeightmapHandle));
			if (auto Found = GTerrainLoadsByKey.find(Work->CoalescingKey);
				Found != GTerrainLoadsByKey.end() && Found->second.lock() == Work)
				GTerrainLoadsByKey.erase(Found);
			return bPublished;
		}

		auto StartAsyncTerrainLoad(
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
				std::lock_guard Lock(GTerrainLoadMutex);
				std::erase_if(GTerrainLoadsByKey, [](const auto& Entry) { return Entry.second.expired(); });
				if (auto Found = GTerrainLoadsByKey.find(CoalescingKey); Found != GTerrainLoadsByKey.end())
					Work = Found->second.lock();
				if (!Work)
				{
					if (GTerrainLoadsByKey.size() >= MaximumConcurrentTerrainLoads)
					{
						OutError = "Terrain heightmap load admission reached its two-request byte bound.";
						(void)Heightmap.FailAuthoringLoad(Generation, ETerrainHeightmapStatus::Failed, OutError);
						return false;
					}
					Work = std::make_shared<FTerrainAuthoringLoadWork>();
					Work->CoalescingKey = CoalescingKey;
					const FTerrainHeightmapSourceImportData Source = Heightmap.GetSourceImportData();
					FTaskLaunchOptions Options;
					Options.CancellationToken = Work->Cancellation.GetToken();
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
					GTerrainLoadsByKey[CoalescingKey] = Work;
				}
				if (GTerrainPendingByObject.size() >= MaximumTerrainLoadSubscribers)
				{
					OutError = "Terrain heightmap load subscriber bound was reached.";
					(void)Heightmap.FailAuthoringLoad(Generation, ETerrainHeightmapStatus::Failed, OutError);
					return false;
				}
				GTerrainPendingByObject[ObjectKey(Handle)] = {.Work = Work, .Generation = Generation};
			}

			FTaskContinuationOptions PublishOptions;
			PublishOptions.Target = ETaskTarget::GameThreadDeferred;
			PublishOptions.EstimatedPayloadBytes = sizeof(FObjectHandle) + sizeof(uint64) + sizeof(std::shared_ptr<void>);
			FTerrainAuthoringLoadPending* Pending = nullptr;
			{
				std::lock_guard Lock(GTerrainLoadMutex);
				Pending = &GTerrainPendingByObject.at(ObjectKey(Handle));
				Pending->Publisher = ThenOutcome(Work->Worker, "TerrainHeightmap.PublishPayload",
					[Handle, Generation, Work](FTaskOutcome<void>) {
						(void)PublishTerrainLoad(Handle, Generation, Work);
					}, PublishOptions);
				if (!Pending->Publisher.IsValid())
				{
					GTerrainPendingByObject.erase(ObjectKey(Handle));
					OutError = "The GameThread executor rejected Terrain heightmap publication.";
					(void)Heightmap.FailAuthoringLoad(Generation, ETerrainHeightmapStatus::Failed, OutError);
					return false;
				}
			}
			OutError.clear();
			return true;
		}

		auto PostLoadTerrainHeightmap(DTerrainHeightmap& Heightmap, std::string& OutError) -> bool
		{
			std::string Key = Asset::Build::MakeTerrainHeightmapDerivedDataKey(Heightmap, OutError);
			const FGameThreadDeferredWorkQueueDiagnostics Deferred =
				GetGameThreadDeferredWorkQueueDiagnostics();
			if (IsTaskSchedulerRunning() && Deferred.bInstalled && Deferred.bAccepting)
				return StartAsyncTerrainLoad(Heightmap, std::move(Key), OutError);

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
			FEncodedSourceSnapshot Snapshot;
			if (!CaptureEncodedSource(Heightmap.GetSourceImportData().SourcePath,
				Snapshot, OutError, MaximumTerrainHeightmapEncodedBytes)) return false;
			FTerrainHeightmapSourceData SourceData;
			return TranslateTerrainHeightmapSource(
				std::filesystem::path(Snapshot.SourcePath.Path).extension().generic_string(),
				Snapshot.GetBytes(), SourceData, OutError)
				&& BuildTerrainHeightmapFromSource(Heightmap, std::move(SourceData), Snapshot,
					OutError, false, false);
		}

		auto WaitForTerrainLoad(DTerrainHeightmap& Heightmap, std::string& OutError) -> bool
		{
			const FObjectHandle Handle = MakeObjectHandle(&Heightmap);
			FTerrainAuthoringLoadPending Pending;
			{
				std::lock_guard Lock(GTerrainLoadMutex);
				const auto Found = GTerrainPendingByObject.find(ObjectKey(Handle));
				if (Found == GTerrainPendingByObject.end())
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
			return PublishTerrainLoad(Handle, Pending.Generation, Pending.Work, &OutError);
		}
	}

	auto RegisterTerrainHeightmapAuthoringPolicy() -> bool
	{
		if (GTerrainHeightmapAuthoringPolicyRegistered) return true;
		if (!RegisterTerrainHeightmapUncookedPostLoadHandler(PostLoadTerrainHeightmap)) return false;
		if (!RegisterTerrainHeightmapAuthoringLoadWaitHandler(WaitForTerrainLoad))
		{
			UnregisterTerrainHeightmapUncookedPostLoadHandler();
			return false;
		}
		if (!RegisterTerrainHeightmapSourceChangeHandler(ChangeTerrainHeightmapSourceReference))
		{
			UnregisterTerrainHeightmapAuthoringLoadWaitHandler();
			UnregisterTerrainHeightmapUncookedPostLoadHandler();
			return false;
		}
		GTerrainHeightmapAuthoringPolicyRegistered = true;
		return true;
	}

	auto UnregisterTerrainHeightmapAuthoringPolicy() -> void
	{
		if (!GTerrainHeightmapAuthoringPolicyRegistered) return;
		std::vector<std::shared_ptr<FTerrainAuthoringLoadWork>> Works;
		std::vector<FTaskHandle> Publishers;
		{
			std::lock_guard Lock(GTerrainLoadMutex);
			for (auto& [Key, Weak] : GTerrainLoadsByKey)
				if (auto Work = Weak.lock()) Works.push_back(std::move(Work));
			for (auto& [Key, Pending] : GTerrainPendingByObject)
				Publishers.push_back(Pending.Publisher);
			GTerrainPendingByObject.clear();
			GTerrainLoadsByKey.clear();
		}
		for (const auto& Work : Works)
		{
			Work->Cancellation.RequestCancellation();
			(void)CancelTask(Work->Worker);
		}
		for (const auto& Work : Works) (void)WaitTask(Work->Worker);
		for (const FTaskHandle& Publisher : Publishers) (void)CancelTask(Publisher);
		UnregisterTerrainHeightmapSourceChangeHandler();
		UnregisterTerrainHeightmapAuthoringLoadWaitHandler();
		UnregisterTerrainHeightmapUncookedPostLoadHandler();
		GTerrainHeightmapAuthoringPolicyRegistered = false;
	}
}
