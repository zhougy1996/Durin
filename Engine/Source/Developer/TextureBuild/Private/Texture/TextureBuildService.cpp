#include "Texture/TextureBuildService.h"

#include "AssetBuild/BuildHost.h"
#include "DObject/DObjectGlobals.h"
#include "Threading/RunnableThread.h"

namespace Durin::Asset::Build
{
	namespace
	{
		struct FTextureBuildServiceState
		{
			std::shared_ptr<FTexture2DBuildCoordinator> Coordinator;
			FBuildServiceRegistration Registration;
		};

		std::mutex GTextureBuildServiceMutex;
		std::unique_ptr<FTextureBuildServiceState> GTextureBuildService;
	}

	auto InitializeTextureBuildService(
		FModuleOwnedCallbackGate OwnerGate,
		const FTexture2DBuildCoordinatorConfig& Config) -> bool
	{
		CheckGameThread();
		std::lock_guard Lock(GTextureBuildServiceMutex);
		if (GTextureBuildService) return true;
		auto State = std::make_unique<FTextureBuildServiceState>();
		State->Coordinator = std::make_shared<FTexture2DBuildCoordinator>(Config);
		const std::shared_ptr<FTexture2DBuildCoordinator> Coordinator = State->Coordinator;
		std::string Error;
		State->Registration = RegisterBuildServiceContribution({
			.Identity = "Durin.TextureBuild.Coordinator",
			.DrainOrder = 100,
			.OwnerGate = std::move(OwnerGate),
			.Start = [Coordinator] { return Coordinator->Start(); },
			.StopAdmission = [Coordinator] { Coordinator->Shutdown(); },
			.PumpCompletions = [Coordinator](uint32 MaximumCount) {
				return Coordinator->PumpCompletions(MaximumCount);
			},
			.Wait = [Coordinator](double TimeoutSeconds) {
				const auto Deadline = std::chrono::steady_clock::now()
					+ std::chrono::duration<double>(std::max(TimeoutSeconds, 0.0));
				while (Coordinator->GetQueuedCount() != 0
					|| Coordinator->GetRunningCount() != 0)
				{
					Coordinator->PumpCompletions(std::numeric_limits<uint32>::max());
					if (std::chrono::steady_clock::now() >= Deadline) return false;
					std::this_thread::yield();
				}
				Coordinator->PumpCompletions(std::numeric_limits<uint32>::max());
				return true;
			},
			.Drain = [Coordinator] { Coordinator->Shutdown(); },
			.Snapshot = [Coordinator] {
				return std::tuple{
					Coordinator->GetQueuedCount(), Coordinator->GetRunningCount(),
					Coordinator->GetInFlightEstimatedBytes()};
			}}, &Error);
		if (!State->Registration.IsValid()) return false;
		GTextureBuildService = std::move(State);
		return true;
	}

	auto GetTexture2DBuildCoordinator() -> FTexture2DBuildCoordinator*
	{
		std::lock_guard Lock(GTextureBuildServiceMutex);
		return GTextureBuildService ? GTextureBuildService->Coordinator.get() : nullptr;
	}

	auto ShutdownTextureBuildService() -> void
	{
		CheckGameThread();
		std::unique_ptr<FTextureBuildServiceState> State;
		{
			std::lock_guard Lock(GTextureBuildServiceMutex);
			State = std::move(GTextureBuildService);
		}
		if (!State) return;
		State->Registration.Reset();
		State->Coordinator->Shutdown();
	}
}
