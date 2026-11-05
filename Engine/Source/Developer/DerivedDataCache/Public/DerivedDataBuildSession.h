#pragma once

#include "CoreMinimal.h"
#include "DerivedDataBuildScheduler.h"
#include "DerivedDataBuildExecution.h"
#include "DerivedDataRequestOwner.h"

namespace Durin::DerivedData
{
	namespace Private { struct FBuildRequestState; struct FBuildSessionState; struct FBuildServiceState; }

	using FBuildCompletionCallback = TMoveOnlyFunction<void(FBuildCompleteParams&&)>;

	class FBuildSession
	{
	public:
		DERIVEDDATACACHE_API ~FBuildSession();
		FBuildSession(const FBuildSession&) = delete;
		FBuildSession(FBuildSession&&) noexcept = default;
		auto operator=(const FBuildSession&) -> FBuildSession& = delete;
		auto operator=(FBuildSession&&) noexcept -> FBuildSession& = default;
		// Completes once after execution and any cache-store response, including admission failure.
		// Store failure preserves a valid local output; cancellation before completion discards it.
		// Callbacks may run inline. Owner waits also include callback completion and capture release.
		DERIVEDDATACACHE_API auto Build(const FBuildDefinition& Definition, const FBuildInputs& Inputs, const FBuildPolicy& Policy, IRequestOwner& Owner, FBuildCompletionCallback&& Completion) -> void;
		DERIVEDDATACACHE_API auto Build(const FBuildAction& Action, const FBuildInputs& Inputs, const FBuildPolicy& Policy, IRequestOwner& Owner, FBuildCompletionCallback&& Completion) -> void;
		DERIVEDDATACACHE_API auto GetName() const -> std::string_view;
	private:
		friend class FBuildService;
		explicit FBuildSession(std::shared_ptr<Private::FBuildSessionState> State);
		auto BuildImpl(std::variant<FBuildDefinition, FBuildAction> Request, FBuildCompletionCallback Completion, FBuildInputs Inputs, FBuildPolicy Policy, IRequestOwner& RequestOwner)
			-> void;
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
		auto CreateInputsBuilder() const -> FBuildInputsBuilder { return {}; }
		auto CreateOutputBuilder(std::string Schema, uint32 SchemaVersion, FBuildOutputLimits Limits = {}) const -> FBuildOutputBuilder
		{ return FBuildOutputBuilder(std::move(Schema), SchemaVersion, Limits); }
		virtual auto Register(std::shared_ptr<const IBuildFunction> Function)
			-> std::expected<void, std::string> = 0;
		// The session groups named builds with a resolver and producer scheduler.
		// Cache execution belongs to the cache implementation.
		virtual auto CreateSession(std::string Name, std::shared_ptr<const IBuildInputResolver> Resolver = {},
			std::shared_ptr<IBuildScheduler> Scheduler = {})
			-> FBuildSession = 0;
	};

	DERIVEDDATACACHE_API auto CreateBuild(FBuildServiceOptions Options = {}) -> std::shared_ptr<IBuild>;
	DERIVEDDATACACHE_API auto GetBuild() -> IBuild&;
}
