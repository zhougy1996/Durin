#pragma once

#include "CoreMinimal.h"
#include "RenderCoreAPI.h"

namespace Durin
{
	class FPipelineState;
	enum class ERenderResourcePreparationState : uint8 { Pending, Ready, Failed, Canceled, Stale };
	struct FRenderResourcePreparationProgress
	{
		ERenderResourcePreparationState State = ERenderResourcePreparationState::Pending;
		uint64 PipelineRequests = 0;
	};
	struct FRenderResourcePreparationState;
	// A caller-owned observation of shared PSO requests. It owns no renderer
	// payloads and never intercepts creation. Copies alias the same request.
	class FRenderResourcePreparation
	{
	public:
		RENDERCORE_API static auto Create() -> FRenderResourcePreparation;
		RENDERCORE_API auto GetProgress() const -> FRenderResourcePreparationProgress;
		RENDERCORE_API auto RequestCancel() const -> void;
		RENDERCORE_API auto NeedsPreparation() const -> bool;
		auto IsValid() const -> bool { return State != nullptr; }
		RENDERCORE_API auto Complete_RenderThread(ERenderResourcePreparationState Result,
			std::vector<std::shared_ptr<FPipelineState>> Requests = {},
			const std::shared_ptr<const void>& Owner = {}) const -> void;
		RENDERCORE_API auto Retire_RenderThread(ERenderResourcePreparationState Reason) const -> void;
	private:
		std::shared_ptr<FRenderResourcePreparationState> State;
	};
	// A thumbnail capture may consume completed outputs, never compile on demand.
	// Resource preparation may opt into asynchronous demand; nested captures still
	// forbid new requests. Neither mode permits waiting for compilation.
	class RENDERCORE_API FPreparedShaderMapScope
	{
	public:
		FPreparedShaderMapScope();
		explicit FPreparedShaderMapScope(bool bAllowAsyncRequests);
		~FPreparedShaderMapScope();
		FPreparedShaderMapScope(const FPreparedShaderMapScope&) = delete;
		static auto IsActive() -> bool;
		static auto AllowsAsyncRequests() -> bool;
	private:
		bool bAllowsAsyncRequests = false;
	};

}
