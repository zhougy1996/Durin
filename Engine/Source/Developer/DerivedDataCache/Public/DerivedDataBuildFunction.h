#pragma once

#include "DerivedDataBuildDefinition.h"
#include "DerivedDataBuildOutput.h"
#include <functional>
#include <limits>

namespace Durin::DerivedData
{
	namespace Private { struct FBuildExecutionAccess; }
	class FBuildCancellation
	{
	public:
		FBuildCancellation() = default;
		explicit FBuildCancellation(std::function<bool()> Predicate) : Predicate(std::move(Predicate)) {}
		auto IsCancelled() const -> bool { return Predicate && Predicate(); }
	private:
		std::function<bool()> Predicate;
	};

	struct FBuildInputValue { std::string Name; FSharedByteBuffer Data; };
	struct FBuildInput
	{
		static constexpr uint32 MaximumValues = 131072;
		FBuildInputReference Identity;
		FSharedByteBuffer Metadata;
		std::vector<FBuildInputValue> Values;
	};
	struct FBuildInputError { std::string Description; };

	class IBuildInputResolver
	{
	public:
		virtual ~IBuildInputResolver() = default;
		virtual auto Describe(std::span<const FBuildSourceReference> Sources, const FBuildCancellation& Cancel) const
			-> std::expected<std::vector<FBuildInputReference>, FBuildInputError> = 0;
		virtual auto Resolve(std::span<const FBuildInputReference> Inputs, const FBuildCancellation& Cancel) const
			-> std::expected<std::vector<FBuildInput>, FBuildInputError> = 0;
	};

	class FBuildInputs
	{
	public:
		FBuildInputs() = default;
		auto IsValid() const -> bool { return State != nullptr; }
		DERIVEDDATACACHE_API auto GetIdentities() const -> std::span<const FBuildInputReference>;
	private:
		friend class FBuildInputsBuilder;
		friend struct Private::FBuildExecutionAccess;
		DERIVEDDATACACHE_API static auto TryCreate(std::span<const FBuildSourceReference> Sources,
			std::shared_ptr<const IBuildInputResolver> Resolver, const FBuildCancellation& Cancel = {})
			-> std::expected<FBuildInputs, FBuildInputError>;
		struct FState;
		std::shared_ptr<const FState> State;
	};

	class FBuildInputsBuilder
	{
	public:
		FBuildInputsBuilder(std::span<const FBuildSourceReference> Sources,
			std::shared_ptr<const IBuildInputResolver> Resolver)
			: Sources(Sources.begin(), Sources.end()), Resolver(std::move(Resolver)) {}
		FBuildInputsBuilder(FBuildInputsBuilder&&) noexcept = default;
		auto operator=(FBuildInputsBuilder&&) noexcept -> FBuildInputsBuilder& = default;
		FBuildInputsBuilder(const FBuildInputsBuilder&) = delete;
		auto SetCancellation(FBuildCancellation Value) -> FBuildInputsBuilder& { Cancel = std::move(Value); return *this; }
		DERIVEDDATACACHE_API auto Build() && -> std::expected<FBuildInputs, FBuildInputError>;
	private:
		std::vector<FBuildSourceReference> Sources;
		std::shared_ptr<const IBuildInputResolver> Resolver;
		FBuildCancellation Cancel;
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
		DERIVEDDATACACHE_API auto AddMessage(std::string Text) -> bool;
		DERIVEDDATACACHE_API auto AddWarning(std::string Text) -> bool;
		DERIVEDDATACACHE_API auto AddError(std::string Text) -> bool;
		DERIVEDDATACACHE_API auto AddLog(std::string Category, EBuildLogSeverity Severity,
			std::string Text) -> bool;
		auto GetMaximumWorkingSetBytes() const -> uint64 { return MaximumWorkingSetBytes; }
		auto IsCancelled() const -> bool { return Cancel.IsCancelled(); }
	private:
		friend struct Private::FBuildExecutionAccess;
		FBuildContext(const FBuildAction& Action, std::span<const FBuildInput> Inputs, FBuildOutputBuilder& Output,
			FBuildCancellation Cancel, uint64 MaximumWorkingSetBytes)
			: Action(Action), Inputs(Inputs), Output(Output), Cancel(std::move(Cancel)), MaximumWorkingSetBytes(MaximumWorkingSetBytes) {}
		const FBuildAction& Action;
		std::span<const FBuildInput> Inputs;
		FBuildOutputBuilder& Output;
		FBuildCancellation Cancel;
		uint64 MaximumWorkingSetBytes;
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
