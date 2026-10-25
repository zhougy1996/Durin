#pragma once

#include "CoreMinimal.h"
#include "RenderCoreAPI.h"

namespace Durin
{
	enum class ERenderResourcePreparationState : uint8 { Pending, Ready, Failed, Canceled, Stale };

	struct FRenderResourcePreparationProgress
	{
		ERenderResourcePreparationState State = ERenderResourcePreparationState::Pending;
		uint64 RetainedObjects = 0;
		uint64 AccountedBytes = 0;
		uint64 ShaderAdmissions = 0;
		uint64 PipelineAdmissions = 0;
		uint64 UncoveredShaderAdmissions = 0;
		uint64 UncoveredPipelineAdmissions = 0;
	};

	struct FRenderResourcePreparationState;
	// Copies alias one loading request. Separate requests share normal resource
	// caches; canceling one request does not cancel their pipeline work.
	class FRenderResourcePreparation
	{
	public:
		RENDERCORE_API static auto Create() -> FRenderResourcePreparation;
		RENDERCORE_API auto GetProgress() const -> FRenderResourcePreparationProgress;
		RENDERCORE_API auto RequestCancel() const -> void;
		auto IsSameRequest(const FRenderResourcePreparation& Other) const -> bool { return State == Other.State; }
		auto IsValid() const -> bool { return State != nullptr; }
		// The renderer owns a reference until it retires it on its own thread.
		auto HasOtherOwners() const -> bool { return State && State.use_count() > 1; }
		RENDERCORE_API auto Retire_RenderThread(ERenderResourcePreparationState Reason) const -> void;
	private:
		std::shared_ptr<FRenderResourcePreparationState> State;
		friend class FRenderResourcePreparationScope;
		friend class FRenderResourcePreparationUseScope;
	};

	// Observe new demand admissions during activated drawing separately from
	// preparation failure. Cache hits can still satisfy an uncovered PSO request.
	class FRenderResourcePreparationUseScope
	{
	public:
		RENDERCORE_API explicit FRenderResourcePreparationUseScope(const FRenderResourcePreparation& Preparation);
		RENDERCORE_API ~FRenderResourcePreparationUseScope();
		FRenderResourcePreparationUseScope(const FRenderResourcePreparationUseScope&) = delete;
	private:
		std::shared_ptr<FRenderResourcePreparationState> State;
		FRenderResourcePreparationUseScope* Previous = nullptr;
		friend class FRenderResourcePreparationScope;
	};

	// Control-flow exceptions never represent resource failure or failed attempts.
	struct FRenderResourcePreparationYield {};
	struct FRenderResourcePreparationCapacityExceeded {};

	class FRenderResourcePreparationScope
	{
	public:
		RENDERCORE_API explicit FRenderResourcePreparationScope(const FRenderResourcePreparation& Preparation);
		RENDERCORE_API ~FRenderResourcePreparationScope();
		FRenderResourcePreparationScope(const FRenderResourcePreparationScope&) = delete;
		RENDERCORE_API auto Complete(bool Ready, bool Failed = false) -> void;
		RENDERCORE_API static auto IsActive() -> bool;
		RENDERCORE_API static auto BeforeShader() -> void;
		RENDERCORE_API static auto BeforePipeline() -> void;
		// Accounted bytes cover explicitly supplied CPU metadata/code, not GPU RSS.
		RENDERCORE_API static auto Retain(std::shared_ptr<const void> Resource, uint64 AccountedBytes) -> void;
	private:
		auto Admit(bool Shader) -> void;
		std::shared_ptr<FRenderResourcePreparationState> State;
		FRenderResourcePreparationScope* Previous = nullptr;
		std::chrono::steady_clock::time_point Start;
		uint32 Shaders = 0, Pipelines = 0;
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
		static auto RecordPendingRequest() -> void;
		// Counts unavailable CPU outputs encountered in the current outer scope.
		// Resource attempts compare before/after counts to defer only their own work.
		static auto GetPendingRequestCount() -> uint64;
	private:
		bool bAllowsAsyncRequests = false;
	};

}
