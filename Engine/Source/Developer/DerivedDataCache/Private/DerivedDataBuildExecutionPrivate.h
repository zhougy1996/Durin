#pragma once

#include "CoreMinimal.h"
#include "DerivedDataBuildSession.h"

namespace Durin::DerivedData::Private
{
	struct FBuildExecutionAccess
	{
		static auto Resolve(const FBuildInputs& Inputs, const FBuildCancellation& Cancel)
			-> std::expected<std::vector<FBuildInput>, FBuildInputError>;
		static auto Configure(const IBuildFunction& Function) -> FBuildFunctionDescriptor;
		static auto MakeContext(const FBuildAction& Action, std::span<const FBuildInput> Inputs,
			FBuildOutputBuilder& Output, FBuildCancellation Cancel,
			uint64 MaximumWorkingSetBytes) -> FBuildContext;
		static auto MakeOutputBuilder(std::string Schema, uint32 SchemaVersion,
			FBuildOutputLimits Limits) -> FBuildOutputBuilder;
	};

	struct FBuildCompletionAccess
	{
		static auto Ok(FBuildOutput Output, FCacheKey Key,
			EBuildStatus Status) -> FBuildCompleteParams;
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
		auto Freeze() -> std::expected<FBuildRegistrySnapshot, std::string>;
	private:
		struct FState;
		std::unique_ptr<FState> State;
	};

	auto ExecuteBuild(const std::variant<FBuildDefinition, FBuildAction>& Request,
		const FBuildRegistrySnapshot& Registry,
		const std::shared_ptr<const IBuildInputResolver>& SessionResolver,
		const FBuildInputs& RequestInputs, const FBuildRequestOptions& Options,
		const FBuildServiceOptions& Service)
		-> FBuildCompleteParams;
}
