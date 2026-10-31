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
	class FRenderResourcePreparationScope;
	class FRenderResourcePreparationUseScope
	{
	public:
		RENDERCORE_API explicit FRenderResourcePreparationUseScope(const FRenderResourcePreparation& Preparation);
		// Transition a successfully prepared invocation to recording without charging
		// its shader/pipeline admissions or GPU readback time to the preparation budget.
		RENDERCORE_API explicit FRenderResourcePreparationUseScope(FRenderResourcePreparationScope& Preparation);
		RENDERCORE_API ~FRenderResourcePreparationUseScope();
		FRenderResourcePreparationUseScope(const FRenderResourcePreparationUseScope&) = delete;
	private:
		std::shared_ptr<FRenderResourcePreparationState> State;
		FRenderResourcePreparationUseScope* Previous = nullptr;
		FRenderResourcePreparationScope* SuspendedPreparation = nullptr;
		friend class FRenderResourcePreparationScope;
	};

	// Shared normal-render admission allowance across views in one render frame.
	// Exhaustion defers local resource attempts; it never throws a view-wide yield.
	class FRenderResourceAdmissionBudget
	{
	public:
		FRenderResourceAdmissionBudget() = default;
		explicit FRenderResourceAdmissionBudget(uint32 InShaders, uint32 InPipelines = 16,
			std::chrono::microseconds InTime = std::chrono::milliseconds(2))
			: MaximumShaders(InShaders), MaximumPipelines(InPipelines), TimeBudget(InTime), bUseBatchMode(false) {}
		RENDERCORE_API auto BeginFrame(uint64 Frame) -> void;
		auto GetShaderAdmissions() const -> uint32 { return Shaders; }
		auto GetPipelineAdmissions() const -> uint32 { return Pipelines; }
		auto GetDeferredAdmissions() const -> uint32 { return Deferred; }
	private:
		auto Admit(bool bShader) -> bool;
		uint64 Frame = UINT64_MAX;
		uint32 MaximumShaders = 8, MaximumPipelines = 16, Shaders = 0, Pipelines = 0, Deferred = 0;
		std::chrono::microseconds TimeBudget{std::chrono::milliseconds(2)};
		bool bUseBatchMode = true;
		std::chrono::steady_clock::time_point Start;
		friend class FRenderResourceAdmissionScope;
		friend class FRenderResourcePreparationScope;
	};
	class FRenderResourceAdmissionScope
	{
	public:
		RENDERCORE_API FRenderResourceAdmissionScope(FRenderResourceAdmissionBudget& Budget, uint64 Frame);
		RENDERCORE_API ~FRenderResourceAdmissionScope();
		FRenderResourceAdmissionScope(const FRenderResourceAdmissionScope&) = delete;
		// Compare around one creation attempt; earlier frame deferrals are unrelated.
		RENDERCORE_API static auto GetDeferredAdmissions() -> uint32;
	private:
		FRenderResourceAdmissionBudget& Budget;
		FRenderResourceAdmissionScope* Previous;
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
		RENDERCORE_API auto Complete(ERenderResourcePreparationState Result) -> void;
		RENDERCORE_API static auto IsActive() -> bool;
		RENDERCORE_API static auto BeforeShader() -> void;
		RENDERCORE_API static auto TryBeforeShader() -> bool;
		RENDERCORE_API static auto BeforePipeline() -> void;
		RENDERCORE_API static auto TryBeforePipeline() -> bool;
		// Accounted bytes cover explicitly supplied CPU metadata/code, not GPU RSS.
		RENDERCORE_API static auto Retain(std::shared_ptr<const void> Resource, uint64 AccountedBytes) -> void;
	private:
		auto Admit(bool Shader) -> void;
		std::shared_ptr<FRenderResourcePreparationState> State;
		FRenderResourcePreparationScope* Previous = nullptr;
		FRenderResourceAdmissionBudget LocalBudget;
		FRenderResourceAdmissionBudget* Budget = nullptr;
		friend class FRenderResourcePreparationUseScope;
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
