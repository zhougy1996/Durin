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

	// Owns development command registration and closes request admission before
	// renderer resources begin their ordered shutdown.
	class FRendererResourceInvalidationController
	{
	public:
		using FRequestSink =
			std::function<void(ERendererResourceInvalidationCause)>;

		RENDERER_API ~FRendererResourceInvalidationController();

		RENDERER_API auto Start(
			FConsoleCommandRegistry& Registry,
			FRequestSink RequestSink) -> bool;
		RENDERER_API auto Stop() -> void;
		RENDERER_API auto Request(
			ERendererResourceInvalidationCause Cause)
			-> FConsoleCommandResult;

	private:
		struct FSharedState;
		static auto QueueRequest(
			const std::shared_ptr<FSharedState>& State,
			ERendererResourceInvalidationCause Cause,
			std::string Message) -> FConsoleCommandResult;

		std::shared_ptr<FSharedState> SharedState;
		FConsoleCommandRegistry* Registry = nullptr;
		FConsoleCommandHandle ReloadShadersHandle = 0;
		FConsoleCommandHandle RetryResourcesHandle = 0;
	};

	// Internal device-recovery seam. No backend publishes this request yet.
	RENDERER_API auto RequestRendererDeviceInvalidation()
		-> FConsoleCommandResult;

	struct FRendererResourceInvalidationSnapshot
	{
		FRenderResourceGeneration Generation;
		bool bForceShaderRecompile = false;
	};

	// Exposes the render-thread generation selected by the ordered command path
	// to focused owner integration tests.
	RENDERER_API auto GetRendererResourceInvalidationSnapshot_RenderThread()
		-> FRendererResourceInvalidationSnapshot;
} // namespace Durin
