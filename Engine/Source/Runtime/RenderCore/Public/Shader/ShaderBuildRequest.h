#pragma once
#include "CoreMinimal.h"
#include "Shader/ShaderCompilerCore.h"

namespace Durin
{
	enum class EShaderBuildPriority : uint8 { High, Normal, Low };
	enum class EShaderBuildWaitResult : uint8 { Completed, WouldBlock, InvalidRequest };
	enum class EShaderBuildAdmissionReason : uint8 { Closed, Capacity, DispatchRejected, InvalidRequest };
	struct FShaderBuildAdmissionError
	{
		EShaderBuildAdmissionReason Reason = EShaderBuildAdmissionReason::InvalidRequest;
	};
	struct FShaderBuildRequestOptions
	{
		EShaderBuildPriority Priority = EShaderBuildPriority::Normal;
		std::function<bool()> IsCancelled;
		// Explicit caller-boundary execution, usable without the Core scheduler.
		bool bInlineExecution = false;
	};
	using FShaderBuildCompletion = std::function<void(FShaderCompilerOutput)>;
	class IShaderBuildRequest
	{
	public:
		virtual ~IShaderBuildRequest() = default;
		virtual auto RequestCancel() -> bool = 0;
		virtual auto SetPriority(EShaderBuildPriority Priority) -> void = 0;
		virtual auto Wait() -> EShaderBuildWaitResult = 0;
		virtual auto IsComplete() const -> bool = 0;
	};
	// One subscriber; moving transfers cancellation authority. Wait joins request end.
	class FShaderBuildHandle
	{
	public:
		FShaderBuildHandle() = default;
		explicit FShaderBuildHandle(std::shared_ptr<IShaderBuildRequest> InRequest) : Request(std::move(InRequest)) {}
		~FShaderBuildHandle() { RequestCancel(); }
		FShaderBuildHandle(FShaderBuildHandle&&) noexcept = default;
		auto operator=(FShaderBuildHandle&& Other) noexcept -> FShaderBuildHandle&
		{ if (this != &Other) { RequestCancel(); Request = std::move(Other.Request); } return *this; }
		FShaderBuildHandle(const FShaderBuildHandle&) = delete;
		auto operator=(const FShaderBuildHandle&) -> FShaderBuildHandle& = delete;
		auto RequestCancel() const -> bool { return Request && Request->RequestCancel(); }
		auto SetPriority(EShaderBuildPriority Priority) const -> void { if (Request) Request->SetPriority(Priority); }
		auto Wait() const -> EShaderBuildWaitResult { return Request ? Request->Wait() : EShaderBuildWaitResult::InvalidRequest; }
		auto IsComplete() const -> bool { return !Request || Request->IsComplete(); }
	private:
		std::shared_ptr<IShaderBuildRequest> Request;
	};
}
