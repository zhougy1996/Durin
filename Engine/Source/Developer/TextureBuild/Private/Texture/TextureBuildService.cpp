#include "Texture/TextureBuildService.h"

#include "DObject/DObjectGlobals.h"
#include "Threading/RunnableThread.h"

namespace Durin::Asset::Build
{
	namespace
	{
		struct FTextureBuildServiceState
		{
			std::shared_ptr<FTexture2DBuildCoordinator> Coordinator;
		};

		std::mutex GTextureBuildServiceMutex;
		std::unique_ptr<FTextureBuildServiceState> GTextureBuildService;
	}

	auto InitializeTextureBuildService(
		FModuleOwnedCallbackGate OwnerGate,
		const FTexture2DBuildCoordinatorConfig& Config) -> bool
	{
		CheckGameThread();
		{
			std::lock_guard Lock(GTextureBuildServiceMutex);
			if (GTextureBuildService) return true;
			auto State = std::make_unique<FTextureBuildServiceState>();
			State->Coordinator = std::make_shared<FTexture2DBuildCoordinator>(Config);
			GTextureBuildService = std::move(State);
		}
		if (Private::InitializeTextureCompilingManager(std::move(OwnerGate))) return true;
		{
			std::lock_guard Lock(GTextureBuildServiceMutex);
			GTextureBuildService.reset();
		}
		return false;
	}

	auto GetTexture2DBuildCoordinator() -> FTexture2DBuildCoordinator*
	{
		std::lock_guard Lock(GTextureBuildServiceMutex);
		return GTextureBuildService ? GTextureBuildService->Coordinator.get() : nullptr;
	}

	auto ShutdownTextureBuildService() -> void
	{
		CheckGameThread();
		Private::ShutdownTextureCompilingManager();
		std::unique_ptr<FTextureBuildServiceState> State;
		{
			std::lock_guard Lock(GTextureBuildServiceMutex);
			State = std::move(GTextureBuildService);
		}
		if (!State) return;
		State->Coordinator->Shutdown();
	}
}
