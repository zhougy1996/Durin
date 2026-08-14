#include "Resources/RendererResourceCoordinator.h"

#include "RHICommandList.h"
#include "RenderingThread.h"

namespace Durin
{
	namespace
	{
		FRendererResourceCoordinator* GActiveRendererResourceCoordinator =
			nullptr;
	}

	struct FRendererResourceCoordinator::FSharedState
	{
		std::mutex Mutex;
		bool bAcceptingRequests = true;
		FRequestSink RequestSink;
	};

	auto FRendererResourceCoordinator::QueueRequest(
		const std::shared_ptr<FSharedState>& State,
		ERendererResourceInvalidationCause Cause,
		std::string Message) -> FConsoleCommandResult
	{
		std::scoped_lock Lock(State->Mutex);
		if (!State->bAcceptingRequests || !State->RequestSink)
		{
			return FConsoleCommandResult::Failure(
				"Renderer resource invalidation is shutting down.");
		}
		State->RequestSink(Cause);
		return FConsoleCommandResult::Success(std::move(Message));
	}

	FRendererResourceCoordinator::~FRendererResourceCoordinator()
	{
		Stop();
	}

	auto FRendererResourceCoordinator::Start(
		FConsoleCommandRegistry& InRegistry,
		FRequestSink RequestSink,
		FModuleOwnedCallbackGate OwnerGate) -> bool
	{
		Stop();
		auto State = std::make_shared<FSharedState>();
		State->RequestSink = std::move(RequestSink);
		Registry = &InRegistry;
		SharedState = State;

		ReloadShadersHandle = Registry->RegisterCommand({
			.Name = "renderer.reload-shaders",
			.Description =
				"Invalidates renderer shader resources for lazy reconstruction.",
			.Usage = "renderer.reload-shaders <changed|all>",
			.Execute = [State](std::span<const std::string> Args) {
				if (Args.size() != 1)
				{
					return FConsoleCommandResult::Failure(
						"Usage: renderer.reload-shaders <changed|all>");
				}
				if (Args[0] == "changed")
				{
					return QueueRequest(
						State,
						ERendererResourceInvalidationCause::ShaderChanged,
						"Queued changed-shader invalidation; renderer resources "
						"will rebuild lazily on demand.");
				}
				if (Args[0] == "all")
				{
					return QueueRequest(
						State,
						ERendererResourceInvalidationCause::ShaderAll,
						"Queued forced shader invalidation; renderer resources "
						"will rebuild lazily on demand.");
				}
				return FConsoleCommandResult::Failure(
					"Usage: renderer.reload-shaders <changed|all>");
			},
		}, OwnerGate);
		RetryResourcesHandle = Registry->RegisterCommand({
			.Name = "renderer.retry-resources",
			.Description =
				"Retries renderer resources that accept explicit recovery.",
			.Usage = "renderer.retry-resources",
			.Execute = [State](std::span<const std::string> Args) {
				if (!Args.empty())
				{
					return FConsoleCommandResult::Failure(
						"Usage: renderer.retry-resources");
				}
				return QueueRequest(
					State,
					ERendererResourceInvalidationCause::ManualRetry,
					"Queued renderer resource retry; reconstruction remains "
					"demand-driven.");
			},
		}, std::move(OwnerGate));
		if (ReloadShadersHandle != 0 && RetryResourcesHandle != 0)
		{
			return true;
		}

		Stop();
		return false;
	}

	auto FRendererResourceCoordinator::Stop() -> void
	{
		if (SharedState)
		{
			std::scoped_lock Lock(SharedState->Mutex);
			SharedState->bAcceptingRequests = false;
			SharedState->RequestSink = {};
		}
		if (Registry != nullptr)
		{
			Registry->UnregisterCommand(ReloadShadersHandle);
			Registry->UnregisterCommand(RetryResourcesHandle);
		}
		ReloadShadersHandle = 0;
		RetryResourcesHandle = 0;
		Registry = nullptr;
		SharedState.reset();
	}

	auto FRendererResourceCoordinator::Request(
		ERendererResourceInvalidationCause Cause)
		-> FConsoleCommandResult
	{
		if (!SharedState)
		{
			return FConsoleCommandResult::Failure(
				"Renderer resource invalidation is not available.");
		}
		return QueueRequest(
			SharedState,
			Cause,
			"Queued internal renderer resource invalidation.");
	}

	auto FRendererResourceCoordinator::Apply_RenderThread(
		ERendererResourceInvalidationCause Cause,
		const FRendererResourceInvalidationTargets& Targets) -> void
	{
		check(IsInRenderingThread());
		switch (Cause)
		{
		case ERendererResourceInvalidationCause::ShaderChanged:
		case ERendererResourceInvalidationCause::ShaderAll:
			Generation.Advance(
				ERenderResourceGenerationDependency::Shader);
			ForceRecompileShaderGeneration =
				Cause == ERendererResourceInvalidationCause::ShaderAll
					? std::optional<uint64>(Generation.Shader)
					: std::nullopt;
			if (Targets.InvalidateShaderResources)
			{
				Targets.InvalidateShaderResources(
					Cause == ERendererResourceInvalidationCause::ShaderAll);
			}
			break;
		case ERendererResourceInvalidationCause::Device:
		{
			FRenderResourceGeneration NextGeneration = Generation;
			NextGeneration.Advance(
				ERenderResourceGenerationDependency::Device);
			if (Targets.ReleaseDeviceResources)
			{
				Targets.ReleaseDeviceResources();
			}
			Generation = NextGeneration;
			if (Targets.RecreateStartupResources)
			{
				Targets.RecreateStartupResources();
			}
			break;
		}
		case ERendererResourceInvalidationCause::ManualRetry:
			Generation.Advance(
				ERenderResourceGenerationDependency::Manual);
			if (Targets.RetryFailedResources)
			{
				Targets.RetryFailedResources();
			}
			break;
		}

		DURIN_INFO(
			"Renderer resource invalidation applied: cause={}, generation={}/{}/{}; reconstruction is demand-driven",
			static_cast<uint8>(Cause),
			Generation.Shader,
			Generation.Device,
			Generation.Manual);
	}

	auto FRendererResourceCoordinator::GetSnapshot_RenderThread() const
		-> FRendererResourceInvalidationSnapshot
	{
		check(IsInRenderingThread());
		return {
			.Generation = Generation,
			.bForceShaderRecompile =
				ForceRecompileShaderGeneration == Generation.Shader,
		};
	}

	auto FRendererResourceCoordinator::GetGeneration_RenderThread() const
		-> const FRenderResourceGeneration&
	{
		check(IsInRenderingThread());
		return Generation;
	}

	auto FRendererResourceCoordinator::
		ShouldForceShaderRecompile_RenderThread() const -> bool
	{
		check(IsInRenderingThread());
		return ForceRecompileShaderGeneration == Generation.Shader;
	}

	auto FRendererResourceCoordinator::ReleaseResources_RenderThread() -> void
	{
		check(IsInRenderingThread());
		Generation = {};
		ForceRecompileShaderGeneration.reset();
	}

	auto GetRendererResourceCoordinator() -> FRendererResourceCoordinator&
	{
		check(GActiveRendererResourceCoordinator != nullptr);
		return *GActiveRendererResourceCoordinator;
	}

	auto SetActiveRendererResourceCoordinator(
		FRendererResourceCoordinator* Coordinator) -> void
	{
		GActiveRendererResourceCoordinator = Coordinator;
	}

	auto RequestRendererDeviceInvalidation() -> FConsoleCommandResult
	{
		if (GActiveRendererResourceCoordinator == nullptr)
		{
			return FConsoleCommandResult::Failure(
				"Renderer resource invalidation is not available.");
		}
		return GActiveRendererResourceCoordinator->Request(
			ERendererResourceInvalidationCause::Device);
	}

	auto GetRendererResourceInvalidationSnapshot_RenderThread()
		-> FRendererResourceInvalidationSnapshot
	{
		check(IsInRenderingThread());
		return GetRendererResourceCoordinator().GetSnapshot_RenderThread();
	}
} // namespace Durin
