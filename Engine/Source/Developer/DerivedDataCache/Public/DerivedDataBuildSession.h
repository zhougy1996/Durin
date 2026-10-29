#pragma once

#include "CoreMinimal.h"
#include "DerivedDataBuildScheduler.h"
#include "DerivedDataBuildExecution.h"
#include "DerivedDataRequestOwner.h"

namespace Durin::DerivedData
{
	namespace Private { struct FBuildRequestState; struct FBuildSessionState; struct FBuildServiceState; }

	using FBuildCompletionCallback = std::function<void(FBuildCompleteParams)>;
	enum class EBuildCacheExecution : uint8 { Inline, Task };

	struct FBuildRequestOptions
	{
		FBuildPolicy Policy;
		FBuildRequestLimits Limits;
		// Early notification leaves persistence tracked by owner/session waits.
		EBuildResultDelivery ResultDelivery = EBuildResultDelivery::AfterPersistence;
		// Resolver owns captured request state; description runs inside the admitted build.
		std::shared_ptr<const IBuildInputResolver> InputResolver;
	};

	enum class EBuildDrainResult : uint8 { Drained, WouldBlock };

	class FBuildSession
	{
	public:
		DERIVEDDATACACHE_API ~FBuildSession();
		FBuildSession(const FBuildSession&) = delete;
		auto operator=(const FBuildSession&) -> FBuildSession& = delete;
		// Every request completes exactly once, including admission failure. Callbacks may run inline.
		DERIVEDDATACACHE_API auto Build(FBuildDefinition Definition, IRequestOwner& Owner, FBuildCompletionCallback Completion, FBuildInputs Inputs = {}, FBuildRequestOptions Options = {}) -> void;
		DERIVEDDATACACHE_API auto Build(FBuildAction Action, IRequestOwner& Owner, FBuildCompletionCallback Completion, FBuildInputs Inputs = {}, FBuildRequestOptions Options = {}) -> void;
		DERIVEDDATACACHE_API auto Close() -> void;
		DERIVEDDATACACHE_API auto Drain() -> EBuildDrainResult;
	private:
		friend class FBuildService;
		explicit FBuildSession(std::shared_ptr<Private::FBuildSessionState> State);
		auto BuildImpl(std::variant<FBuildDefinition, FBuildAction> Request, FBuildCompletionCallback Completion, FBuildInputs Inputs, FBuildRequestOptions Options, IRequestOwner& RequestOwner)
			-> std::expected<void, FBuildAdmissionError>;
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
		// Producer and cache execution default to inline. Task sessions share the
		// service dispatcher; cache implementation and dispatch handles remain private.
		virtual auto CreateSession(std::shared_ptr<const IBuildInputResolver> Resolver = {},
			std::shared_ptr<IBuildScheduler> Scheduler = {},
			EBuildCacheExecution CacheExecution = EBuildCacheExecution::Inline)
			-> std::expected<std::shared_ptr<FBuildSession>, FBuildAdmissionError> = 0;
		virtual auto Close() -> void = 0;
		virtual auto Drain() -> EBuildDrainResult = 0;
	};

	DERIVEDDATACACHE_API auto CreateBuild(FBuildServiceOptions Options = {}) -> std::shared_ptr<IBuild>;
	DERIVEDDATACACHE_API auto GetBuild() -> IBuild&;
}
