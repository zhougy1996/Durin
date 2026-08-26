#pragma once

#include "Console/ConsoleCommand.h"
#include "RenderResourceCreation.h"
#include "RendererAPI.h"

namespace Durin
{
	enum class ERendererResourceInvalidationCause : uint8
	{
		ShaderChanged,
		ShaderAll,
		Device,
		ManualRetry,
	};

	struct FRendererResourceInvalidationSnapshot
	{
		FRenderResourceGeneration Generation;
		bool bForceShaderRecompile = false;
	};

	// Supplies the concrete-owner fan-out while the coordinator preserves
	// generation and invalidation ordering.
	struct FRendererResourceInvalidationTargets
	{
		std::function<void(bool)> InvalidateShaderResources;
		std::function<void()> ReleaseDeviceResources;
		std::function<void()> RecreateStartupResources;
		std::function<void()> RetryFailedResources;
	};

	// Owns renderer generation state and development-command admission for one
	// module lifetime.
	class FRendererResourceCoordinator
	{
	public:
		using FRequestSink =
			std::function<void(ERendererResourceInvalidationCause)>;

		RENDERER_API ~FRendererResourceCoordinator();

		RENDERER_API auto Start(
			FConsoleCommandRegistry& Registry,
			FRequestSink RequestSink,
			FModuleOwnedCallbackGate OwnerGate = {}) -> bool;
		RENDERER_API auto Stop() -> void;
		RENDERER_API auto Request(
			ERendererResourceInvalidationCause Cause)
			-> FConsoleCommandResult;
		RENDERER_API auto Apply_RenderThread(
			ERendererResourceInvalidationCause Cause,
			const FRendererResourceInvalidationTargets& Targets) -> void;
		RENDERER_API auto GetSnapshot_RenderThread() const
			-> FRendererResourceInvalidationSnapshot;
		auto GetGeneration_RenderThread() const
			-> const FRenderResourceGeneration&;
		auto ShouldForceShaderRecompile_RenderThread() const -> bool;
		auto ReleaseResources_RenderThread() -> void;

	private:
		struct FSharedState;
		static auto QueueRequest(
			const std::shared_ptr<FSharedState>& State,
			ERendererResourceInvalidationCause Cause,
			std::string Message) -> FConsoleCommandResult;

		FRenderResourceGeneration Generation;
		std::optional<uint64> ForceRecompileShaderGeneration;
		std::shared_ptr<FSharedState> SharedState;
		FConsoleCommandRegistry* Registry = nullptr;
		FConsoleCommandHandle ReloadShadersHandle = 0;
		FConsoleCommandHandle RetryResourcesHandle = 0;
	};

} // namespace Durin
