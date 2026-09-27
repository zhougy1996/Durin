#pragma once
#include "DerivedDataBuildExecution.h"

namespace Durin::DerivedData
{
	namespace Private { struct FBuildRequestState; struct FBuildSessionState; }
	using FBuildCompletionCallback = std::function<void(FBuildCompletion)>;
	// Reject without invoking/retaining Work, or accept and run it at most once.
	// Accepted work may run inline. Dropped accepted work completes as cancelled.
	using FBuildDispatcher = std::function<std::expected<void, FBuildError>(std::function<void()> Work)>;
	struct FBuildRequestOptions
	{
		FBuildRequestPolicy Policy;
		FBuildCancellation Cancellation;
		FBuildCacheOperations Cache;
		FBuildRunObserver Observer;
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
	// Blocking drain is invalid inside this session's execution/completion stack.
	enum class EBuildDrainResult : uint8 { Drained, WouldBlock };

	class FBuildSession
	{
	public:
		// Empty dispatcher means inline. Resolvers/registry/functions remain owned
		// until terminal callbacks finish; drained handles retain no callable owners.
		DERIVEDDATACACHE_API FBuildSession(FBuildRegistrySnapshot Registry,
			std::shared_ptr<const IBuildInputResolver> Resolver, FBuildDispatcher Dispatcher = {});
		DERIVEDDATACACHE_API ~FBuildSession();
		FBuildSession(const FBuildSession&) = delete;
		auto operator=(const FBuildSession&) -> FBuildSession& = delete;
		DERIVEDDATACACHE_API auto Submit(FBuildDefinition Definition, FBuildCompletionCallback Completion,
			FBuildRequestOptions Options = {}) -> std::expected<FBuildRequest, FBuildError>;
		// Already-admitted workers call this without queueing another task.
		DERIVEDDATACACHE_API auto ExecuteInline(FBuildDefinition Definition, FBuildRequestOptions Options = {})
			-> std::expected<FBuildCompletion, FBuildError>;
		DERIVEDDATACACHE_API auto Close() -> void;
		DERIVEDDATACACHE_API auto Drain() -> EBuildDrainResult;
	private:
		auto SubmitImpl(FBuildDefinition, FBuildCompletionCallback, FBuildRequestOptions, bool Inline)
			-> std::expected<FBuildRequest, FBuildError>;
		std::shared_ptr<Private::FBuildSessionState> State;
	};
}
