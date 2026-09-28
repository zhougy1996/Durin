#pragma once
#include "DerivedDataBuildExecution.h"

namespace Durin::DerivedData
{
	namespace Private { struct FBuildRequestState; struct FBuildSessionState; struct FBuildServiceState; }

	enum class EBuildAdmissionReason : uint8
	{
		Closed, Capacity, MissingFunction, InvalidRequest, DispatchRejected, InternalFailure
	};
	struct FBuildAdmissionError
	{
		static constexpr size_t MaximumDescriptionBytes = 4096;
		EBuildAdmissionReason Reason = EBuildAdmissionReason::InvalidRequest;
		std::string Description;
		auto BoundDescription() -> void
		{ if (Description.size() > MaximumDescriptionBytes) Description.resize(MaximumDescriptionBytes); }
	};

	using FBuildCompletionCallback = std::function<void(FBuildCompleteParams)>;
	using FBuildDispatcher = std::function<std::expected<void, FBuildAdmissionError>(std::function<void()> Work)>;
	struct FBuildRequestOptions
	{
		FBuildPolicy Policy;
		FBuildCancellation Cancellation;
	};

	class FBuildRequest
	{
	public:
		FBuildRequest() = default;
		DERIVEDDATACACHE_API auto Cancel() const -> bool;
		DERIVEDDATACACHE_API auto IsComplete() const -> bool;
	private:
		friend class FBuildSession;
		std::shared_ptr<Private::FBuildRequestState> State;
	};
	enum class EBuildDrainResult : uint8 { Drained, WouldBlock };

	class FBuildSession
	{
	public:
		DERIVEDDATACACHE_API ~FBuildSession();
		FBuildSession(const FBuildSession&) = delete;
		auto operator=(const FBuildSession&) -> FBuildSession& = delete;
		DERIVEDDATACACHE_API auto Build(FBuildDefinition Definition,
			FBuildCompletionCallback Completion, FBuildInputs Inputs = {},
			FBuildRequestOptions Options = {}) -> std::expected<FBuildRequest, FBuildAdmissionError>;
		DERIVEDDATACACHE_API auto Build(FBuildAction Action,
			FBuildCompletionCallback Completion, FBuildInputs Inputs = {},
			FBuildRequestOptions Options = {}) -> std::expected<FBuildRequest, FBuildAdmissionError>;
		DERIVEDDATACACHE_API auto Close() -> void;
		DERIVEDDATACACHE_API auto Drain() -> EBuildDrainResult;
	private:
		friend class FBuildService;
		explicit FBuildSession(std::shared_ptr<Private::FBuildSessionState> State);
		auto BuildImpl(std::variant<FBuildDefinition, FBuildAction> Request,
			FBuildCompletionCallback Completion, FBuildInputs Inputs,
			FBuildRequestOptions Options) -> std::expected<FBuildRequest, FBuildAdmissionError>;
		std::shared_ptr<Private::FBuildSessionState> State;
	};

	class IBuild
	{
	public:
		virtual ~IBuild() = default;
		virtual auto Register(std::shared_ptr<const IBuildFunction> Function)
			-> std::expected<void, FBuildAdmissionError> = 0;
		virtual auto CreateSession(std::shared_ptr<const IBuildInputResolver> Resolver = {},
			FBuildDispatcher Dispatcher = {})
			-> std::expected<std::shared_ptr<FBuildSession>, FBuildAdmissionError> = 0;
		virtual auto Close() -> void = 0;
		virtual auto Drain() -> EBuildDrainResult = 0;
	};

	DERIVEDDATACACHE_API auto CreateBuild(FBuildServiceOptions Options = {}) -> std::shared_ptr<IBuild>;
}
