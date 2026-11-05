#pragma once

#include "CoreMinimal.h"

#include "DerivedDataBuildDefinition.h"
#include "DerivedDataRequestOwner.h"
#include "DerivedDataStatus.h"
#include "DerivedDataBuildOutput.h"

namespace Durin::DerivedData
{
	namespace Private { struct FBuildExecutionAccess; }
	struct FBuildInputValue { std::string Name; FSharedByteBuffer Data; };
	struct FBuildInput
	{
		static constexpr uint32 MaximumValues = 131072;
		FBuildInputReference Identity;
		FSharedByteBuffer Metadata;
		std::vector<FBuildInputValue> Values;
	};
	class IRequestOwner;
	struct FBuildInputMetaResolvedParams { std::vector<FBuildInputReference> Inputs; EStatus Status = EStatus::Error; };
	struct FBuildInputDataResolvedParams { std::vector<FBuildInput> Inputs; EStatus Status = EStatus::Error; };
	using FOnBuildInputMetaResolved = TMoveOnlyFunction<void(FBuildInputMetaResolvedParams&&)>;
	using FOnBuildInputDataResolved = TMoveOnlyFunction<void(FBuildInputDataResolvedParams&&)>;
	class IBuildInputResolver
	{
	public:
		virtual ~IBuildInputResolver() = default;
		virtual auto ResolveInputMeta(const FBuildDefinition& Definition, IRequestOwner& Owner, FOnBuildInputMetaResolved&& OnResolved) const -> void = 0;
		virtual auto ResolveInputData(const FBuildAction& Action, IRequestOwner& Owner, FOnBuildInputDataResolved&& OnResolved) const -> void = 0;
	};
	class FBuildInputs
	{
	public:
		FBuildInputs() = default;
		auto IsValid() const -> bool { return Values != nullptr; }
		auto GetInputs() const -> std::span<const FBuildInput> { return Values ? std::span<const FBuildInput>(*Values) : std::span<const FBuildInput>{}; }
	private:
		friend class FBuildInputsBuilder;
		std::shared_ptr<const std::vector<FBuildInput>> Values;
	};
	class FBuildInputsBuilder
	{
	public:
		auto AddInput(FBuildInput Input) -> void { Values.push_back(std::move(Input)); }
		auto Build() && -> FBuildInputs
		{
			std::ranges::sort(Values, {}, [](const FBuildInput& Input) -> const std::string& { return Input.Identity.Name; });
			FBuildInputs Result; Result.Values = std::make_shared<const std::vector<FBuildInput>>(std::move(Values)); return Result;
		}
	private:
		std::vector<FBuildInput> Values;
	};
	class FBuildConfigContext
	{
	public:
		auto SetConstantsSchema(uint32 Value) -> void { Descriptor.ConstantsSchema = Value; }
		auto SetOutput(std::string Type, uint32 Schema) -> void { Descriptor.OutputType = std::move(Type); Descriptor.OutputSchema = Schema; }
		auto SetCacheBucket(FCacheBucket Bucket) -> void { Descriptor.Bucket = std::move(Bucket); }
	private:
		friend struct Private::FBuildExecutionAccess;
		friend class IBuildFunction;
		FBuildFunctionDescriptor Descriptor;
	};

	class FBuildContext
	{
	public:
		DERIVEDDATACACHE_API auto FindConstant(std::string_view Name) const -> const FBuildConstantValue*;
		template<typename T> auto FindConstant(std::string_view Name) const -> const T*
		{
			const auto* Value = FindConstant(Name); return Value ? std::get_if<T>(Value) : nullptr;
		}
		DERIVEDDATACACHE_API auto FindInput(std::string_view Name) const -> const FBuildInput*;
		DERIVEDDATACACHE_API auto AddValue(FValueId Id, FSharedByteBuffer Data) -> bool;
		DERIVEDDATACACHE_API auto AddMeta(FValueId Id, FCbObject Object) -> bool;
		DERIVEDDATACACHE_API auto AddObservation(FValueId Id, FCbObject Object) -> bool;
		DERIVEDDATACACHE_API auto AddMessage(std::string Text) -> bool;
		DERIVEDDATACACHE_API auto AddWarning(std::string Text) -> bool;
		// Only deterministic failures based on action inputs may use AddError.
		DERIVEDDATACACHE_API auto AddError(std::string Text) -> bool;
		// Resource/environment failures have no cacheable output.
		DERIVEDDATACACHE_API auto FailExecution(std::string Text) -> void;
		DERIVEDDATACACHE_API auto AddLog(std::string Category, EBuildLogSeverity Severity,
			std::string Text) -> bool;
		auto IsCanceled() const -> bool { return Owner.IsCanceled(); }
	private:
		friend struct Private::FBuildExecutionAccess;
		FBuildContext(const FBuildAction& Action, std::span<const FBuildInput> Inputs, FBuildOutputBuilder& Output,
			IRequestOwner& Owner)
			: Action(Action), Inputs(Inputs), Output(Output), Owner(Owner) {}
		const FBuildAction& Action;
		std::span<const FBuildInput> Inputs;
		FBuildOutputBuilder& Output;
		IRequestOwner& Owner;
		std::optional<std::string> ExecutionFailure;
	};

	class IBuildFunction
	{
	public:
		virtual ~IBuildFunction() = default;
		virtual auto GetName() const -> std::string_view = 0;
		virtual auto GetVersion() const -> uint32 = 0;
		virtual auto Configure(FBuildConfigContext& Context) const -> void = 0;
		virtual auto Build(FBuildContext& Context) const -> void = 0;
	};
}
