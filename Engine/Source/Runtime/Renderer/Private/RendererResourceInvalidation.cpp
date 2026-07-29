#include "RendererResourceInvalidation.h"

namespace Durin
{
	struct FRendererResourceInvalidationController::FSharedState
	{
		std::mutex Mutex;
		bool bAcceptingRequests = true;
		FRequestSink RequestSink;
	};

	auto FRendererResourceInvalidationController::QueueRequest(
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

	FRendererResourceInvalidationController::
		~FRendererResourceInvalidationController()
	{
		Stop();
	}

	auto FRendererResourceInvalidationController::Start(
		FConsoleCommandRegistry& InRegistry,
		FRequestSink RequestSink) -> bool
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
		});
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
		});
		if (ReloadShadersHandle != 0 && RetryResourcesHandle != 0)
		{
			return true;
		}

		Stop();
		return false;
	}

	auto FRendererResourceInvalidationController::Stop() -> void
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

	auto FRendererResourceInvalidationController::Request(
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
} // namespace Durin
