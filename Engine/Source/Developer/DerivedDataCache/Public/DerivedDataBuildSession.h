#pragma once

#include "CoreMinimal.h"
#include "DerivedDataBuildScheduler.h"

namespace Durin::DerivedData
{
	namespace Private { struct FBuildRequestState; struct FBuildSessionState; struct FBuildServiceState; struct FBuildRequestOwnerState; }

	using FBuildCompletionCallback = std::function<void(FBuildCompleteParams)>;
	class FBuildRequestOwner;

	struct FBuildRequestOptions
	{
		FBuildPolicy Policy;
		FBuildCancellation Cancellation;
		// Resolver owns captured request state; description runs inside the admitted build.
		std::shared_ptr<const IBuildInputResolver> InputResolver;
	};

	class FBuildRequest
	{
	public:
		FBuildRequest() = default;
		DERIVEDDATACACHE_API auto Cancel() const -> bool;
		DERIVEDDATACACHE_API auto IsComplete() const -> bool;
		DERIVEDDATACACHE_API auto Wait() const -> EBuildWaitResult;
	private:
		friend class FBuildSession;
		std::shared_ptr<Private::FBuildRequestState> State;
	};
	// Groups requests across sessions. Destruction cancels and waits unless reentrant.
	class FBuildRequestOwner
	{
	public:
		DERIVEDDATACACHE_API explicit FBuildRequestOwner(EBuildPriority Priority = EBuildPriority::Normal);
		DERIVEDDATACACHE_API ~FBuildRequestOwner();
		FBuildRequestOwner(const FBuildRequestOwner&) = delete;
		auto operator=(const FBuildRequestOwner&) -> FBuildRequestOwner& = delete;
		DERIVEDDATACACHE_API auto Cancel() -> void;
		DERIVEDDATACACHE_API auto IsCanceled() const -> bool;
		DERIVEDDATACACHE_API auto GetPriority() const -> EBuildPriority;
		DERIVEDDATACACHE_API auto Poll() const -> bool;
		DERIVEDDATACACHE_API auto Wait() const -> EBuildWaitResult;
	private:
		friend class FBuildSession;
		friend class FBuildRequestBarrier;
		std::shared_ptr<Private::FBuildRequestOwnerState> State;
	};

	// Keeps an owner non-idle while another thread may still submit requests.
	class FBuildRequestBarrier
	{
	public:
		DERIVEDDATACACHE_API explicit FBuildRequestBarrier(FBuildRequestOwner& Owner);
		DERIVEDDATACACHE_API ~FBuildRequestBarrier();
		FBuildRequestBarrier(const FBuildRequestBarrier&) = delete;
		auto operator=(const FBuildRequestBarrier&) -> FBuildRequestBarrier& = delete;
	private:
		std::shared_ptr<Private::FBuildRequestOwnerState> State;
		std::thread::id Thread;
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
		DERIVEDDATACACHE_API auto Build(FBuildDefinition Definition, FBuildRequestOwner& Owner,
			FBuildCompletionCallback Completion, FBuildInputs Inputs = {},
			FBuildRequestOptions Options = {}) -> std::expected<FBuildRequest, FBuildAdmissionError>;
		DERIVEDDATACACHE_API auto Build(FBuildAction Action, FBuildRequestOwner& Owner,
			FBuildCompletionCallback Completion, FBuildInputs Inputs = {},
			FBuildRequestOptions Options = {}) -> std::expected<FBuildRequest, FBuildAdmissionError>;
		DERIVEDDATACACHE_API auto Close() -> void;
		DERIVEDDATACACHE_API auto Drain() -> EBuildDrainResult;
	private:
		friend class FBuildService;
		explicit FBuildSession(std::shared_ptr<Private::FBuildSessionState> State);
		auto BuildImpl(std::variant<FBuildDefinition, FBuildAction> Request,
			FBuildCompletionCallback Completion, FBuildInputs Inputs,
			FBuildRequestOptions Options, std::shared_ptr<Private::FBuildRequestOwnerState> RequestOwner = {})
			-> std::expected<FBuildRequest, FBuildAdmissionError>;
		std::shared_ptr<Private::FBuildSessionState> State;
	};

	class IBuild
	{
	public:
		virtual ~IBuild() = default;
		auto CreateDefinitionBuilder(std::string FunctionName) const -> FBuildDefinitionBuilder
		{ return FBuildDefinitionBuilder(std::move(FunctionName)); }
		auto CreateActionBuilder(FBuildDefinition Definition, FBuildFunctionDescriptor Function) const -> FBuildActionBuilder
		{ return FBuildActionBuilder(std::move(Definition), std::move(Function)); }
		auto CreateInputsBuilder(std::span<const FBuildSourceReference> Sources,
			std::shared_ptr<const IBuildInputResolver> Resolver) const -> FBuildInputsBuilder
		{ return FBuildInputsBuilder(Sources, std::move(Resolver)); }
		auto CreateOutputBuilder(std::string Schema, uint32 SchemaVersion, FBuildOutputLimits Limits = {}) const -> FBuildOutputBuilder
		{ return FBuildOutputBuilder(std::move(Schema), SchemaVersion, Limits); }
		virtual auto Register(std::shared_ptr<const IBuildFunction> Function)
			-> std::expected<void, FBuildAdmissionError> = 0;
		virtual auto CreateSession(std::shared_ptr<const IBuildInputResolver> Resolver = {},
			std::shared_ptr<IBuildScheduler> Scheduler = {})
			-> std::expected<std::shared_ptr<FBuildSession>, FBuildAdmissionError> = 0;
		virtual auto Close() -> void = 0;
		virtual auto Drain() -> EBuildDrainResult = 0;
	};

	DERIVEDDATACACHE_API auto CreateBuild(FBuildServiceOptions Options = {}) -> std::shared_ptr<IBuild>;
	DERIVEDDATACACHE_API auto GetBuild() -> IBuild&;
}
