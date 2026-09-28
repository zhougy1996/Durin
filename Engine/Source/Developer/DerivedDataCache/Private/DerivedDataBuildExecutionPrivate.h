#pragma once
#include "DerivedDataBuildSession.h"

namespace Durin::DerivedData::Private
{
	struct FBuildExecutionAccess
	{
		static auto Resolve(const FBuildInputs& Inputs, const FBuildCancellation& Cancel)
			-> std::expected<std::vector<FBuildInput>, FBuildFailure>;
	};

	struct FBuildCompletionAccess
	{
		static auto Ok(FBuildOutput Output, std::shared_ptr<const FBuildValidationReceipt> ValidationReceipt,
			FCacheKey Key,
			EBuildStatus Status, FBuildExecutionReport Report) -> FBuildCompleteParams;
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
		auto Register(std::shared_ptr<const IBuildFunction> Function) -> std::expected<void, FBuildFailure>;
		auto Freeze() -> std::expected<FBuildRegistrySnapshot, FBuildFailure>;
	private:
		struct FState;
		std::unique_ptr<FState> State;
	};

	auto ExecuteBuild(const std::variant<FBuildDefinition, FBuildAction>& Request,
		const FBuildRegistrySnapshot& Registry,
		const std::shared_ptr<const IBuildInputResolver>& SessionResolver,
		const FBuildInputs& RequestInputs, const FBuildPolicy& Policy,
		const FBuildCancellation& Cancel, const FBuildServiceOptions& Service)
		-> FBuildCompleteParams;
}
