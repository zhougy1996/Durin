#include "Authoring/AuthoringBuildService.h"

#include "DObject/DObjectGlobals.h"
#include "Threading/RunnableThread.h"

namespace Durin::AssetBuild
{
	namespace
	{
		struct FAuthoringBuildServiceState
		{
			std::unique_ptr<FTexture2DBuildCoordinator> Texture2D;
			bool bAcceptingRequests = true;
		};

		std::mutex GAuthoringBuildServiceMutex;
		std::unique_ptr<FAuthoringBuildServiceState> GAuthoringBuildService;
	}

	auto InitializeAuthoringBuildService(
		const FAuthoringBuildServiceConfig& Config) -> bool
	{
		CheckGameThread();
		std::lock_guard Lock(GAuthoringBuildServiceMutex);
		if (GAuthoringBuildService) return GAuthoringBuildService->bAcceptingRequests;
		auto State = std::make_unique<FAuthoringBuildServiceState>();
		State->Texture2D = std::make_unique<FTexture2DBuildCoordinator>(Config.Texture2D);
		GAuthoringBuildService = std::move(State);
		return true;
	}

	auto GetTexture2DBuildCoordinator() -> FTexture2DBuildCoordinator*
	{
		std::lock_guard Lock(GAuthoringBuildServiceMutex);
		return GAuthoringBuildService && GAuthoringBuildService->bAcceptingRequests
			? GAuthoringBuildService->Texture2D.get() : nullptr;
	}

	auto GetAuthoringBuildServiceSnapshot() -> FAuthoringBuildServiceSnapshot
	{
		std::lock_guard Lock(GAuthoringBuildServiceMutex);
		FAuthoringBuildServiceSnapshot Result;
		if (!GAuthoringBuildService || !GAuthoringBuildService->Texture2D) return Result;
		Result.QueuedRequestCount = GAuthoringBuildService->Texture2D->GetQueuedCount();
		Result.RunningRequestCount = GAuthoringBuildService->Texture2D->GetRunningCount();
		Result.InFlightEstimatedBytes =
			GAuthoringBuildService->Texture2D->GetInFlightEstimatedBytes();
		Result.bAcceptingRequests = GAuthoringBuildService->bAcceptingRequests;
		return Result;
	}

	auto PumpAuthoringBuildCompletions(uint32 MaximumCount) -> uint32
	{
		CheckGameThread();
		FTexture2DBuildCoordinator* Coordinator = GetTexture2DBuildCoordinator();
		return Coordinator ? Coordinator->PumpCompletions(MaximumCount) : 0;
	}

	auto WaitForAuthoringBuildService(double TimeoutSeconds) -> bool
	{
		CheckGameThread();
		const auto Deadline = std::chrono::steady_clock::now()
			+ std::chrono::duration_cast<std::chrono::steady_clock::duration>(
				std::chrono::duration<double>(std::max(TimeoutSeconds, 0.0)));
		for (;;)
		{
			PumpAuthoringBuildCompletions(std::numeric_limits<uint32>::max());
			const FAuthoringBuildServiceSnapshot Snapshot =
				GetAuthoringBuildServiceSnapshot();
			if (Snapshot.QueuedRequestCount == 0 && Snapshot.RunningRequestCount == 0)
			{
				PumpAuthoringBuildCompletions(std::numeric_limits<uint32>::max());
				return true;
			}
			if (std::chrono::steady_clock::now() >= Deadline) return false;
			std::this_thread::yield();
		}
	}

	auto ShutdownAuthoringBuildService() -> void
	{
		CheckGameThread();
		std::unique_ptr<FAuthoringBuildServiceState> State;
		{
			std::lock_guard Lock(GAuthoringBuildServiceMutex);
			if (!GAuthoringBuildService) return;
			GAuthoringBuildService->bAcceptingRequests = false;
			State = std::move(GAuthoringBuildService);
		}
		if (State->Texture2D) State->Texture2D->Shutdown();
	}
}
