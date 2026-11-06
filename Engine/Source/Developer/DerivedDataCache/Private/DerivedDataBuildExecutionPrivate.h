#pragma once

#include "CoreMinimal.h"
#include "DerivedDataBuildSession.h"

namespace Durin::DerivedData::Private
{
	struct FBuildExecutionAccess
	{
		static auto Configure(const IBuildFunction& Function) -> FBuildFunctionDescriptor;
		static auto MakeContext(const FBuildAction& Action, std::span<const FBuildInput> Inputs,
			FBuildOutputBuilder& Output, IRequestOwner& Owner) -> FBuildContext;
		static auto GetExecutionFailure(const FBuildContext& Context) -> const std::optional<std::string>& { return Context.ExecutionFailure; }
		static auto MakeOutputBuilder(std::string Schema, uint32 SchemaVersion,
			FBuildOutputLimits Limits) -> FBuildOutputBuilder;
	};

	struct FBuildCompletionAccess
	{
		static auto Ok(FBuildOutput Output, FCacheKey Key,
			EBuildExecutionFlags ExecutionFlags) -> FBuildCompleteParams;
		static auto Canceled(FBuildCompleteParams Completion) -> FBuildCompleteParams;
	};

	struct FRegisteredBuildFunction
	{
		FBuildFunctionDescriptor Descriptor;
		std::shared_ptr<const IBuildFunction> Function;
	};
	class FBuildRegistrySnapshot
	{
	public:
		auto Find(std::string_view Name) const -> std::shared_ptr<const FRegisteredBuildFunction>;
	private:
		friend class FBuildRegistry;
		std::vector<std::shared_ptr<const FRegisteredBuildFunction>> Entries;
	};
	class FBuildRegistry
	{
	public:
		FBuildRegistry();
		~FBuildRegistry();
		FBuildRegistry(const FBuildRegistry&) = delete;
		auto operator=(const FBuildRegistry&) -> FBuildRegistry& = delete;
		auto Register(std::shared_ptr<const IBuildFunction> Function) -> std::expected<void, std::string>;
		auto Freeze() -> FBuildRegistrySnapshot;
	private:
		struct FState;
		std::unique_ptr<FState> State;
	};

	auto PrepareBuildAction(const FBuildDefinition& Definition,
		const FBuildRegistrySnapshot& Registry, std::span<const FBuildInputReference> Identities) -> std::expected<FBuildAction, std::string>;
	auto ExecuteBuildLocal(const FBuildAction& Action, const FBuildRegistrySnapshot& Registry,
		std::vector<FBuildInput> Inputs, const FBuildPolicy& Policy, const FBuildRequestLimits& Limits, IRequestOwner& Owner) -> FBuildCompleteParams;
}
