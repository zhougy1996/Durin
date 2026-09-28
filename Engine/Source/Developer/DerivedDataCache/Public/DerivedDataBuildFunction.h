#pragma once

#include "DerivedDataBuildDefinition.h"
#include "DerivedDataBuildOutput.h"
#include <functional>
#include <limits>
#include <optional>

namespace Durin::DerivedData
{
	namespace Private { struct FBuildExecutionAccess; }

	enum class EBuildOperation : uint8
	{
		Admission, Describe, Action, CacheQuery, Decode, Resolve, Build, Validate,
		Record, Encode, Compress, CacheStore, Dispatch
	};
	enum class EBuildFailureReason : uint8
	{
		InvalidInput, InputUnavailable, ProducerFailure, InvalidOutput,
		ResourceExhaustion, InternalFailure
	};
	struct FBuildFailure
	{
		EBuildFailureReason Reason = EBuildFailureReason::InvalidInput;
		EBuildOperation Operation = EBuildOperation::Admission;
		std::string Description;
		std::optional<uint32> ProducerCode;
		std::optional<FXxHash128> DiagnosticIdentity;
		static constexpr size_t MaximumDescriptionBytes = 4096;
		auto BoundDescription() -> void
		{
			if (Description.size() > MaximumDescriptionBytes) Description.resize(MaximumDescriptionBytes);
		}
	};
	using FBuildFunctionResult = std::expected<FBuildOutput, FBuildFailure>;
	class DERIVEDDATACACHE_API FBuildValidationReceipt
	{
	public:
		virtual ~FBuildValidationReceipt();
	};
	using FBuildValidationResult = std::expected<std::shared_ptr<const FBuildValidationReceipt>, FBuildFailure>;

	class FBuildCancellation
	{
	public:
		FBuildCancellation() = default;
		explicit FBuildCancellation(std::function<bool()> Predicate) : Predicate(std::move(Predicate)) {}
		auto IsCancelled() const -> bool { return Predicate && Predicate(); }
	private:
		std::function<bool()> Predicate;
	};

	struct FBuildInput
	{
		static constexpr uint32 MaximumValues = 131072;
		FBuildInputReference Identity;
		FSharedByteBuffer Metadata;
		std::vector<FBuildValue> Values;
	};
	class IBuildInputResolver
	{
	public:
		virtual ~IBuildInputResolver() = default;
		virtual auto Describe(std::span<const FBuildSourceReference> Sources,
			const FBuildCancellation& Cancel) const
			-> std::expected<std::vector<FBuildInputReference>, FBuildFailure> = 0;
		virtual auto Resolve(std::span<const FBuildInputReference> Inputs,
			const FBuildCancellation& Cancel) const
			-> std::expected<std::vector<FBuildInput>, FBuildFailure> = 0;
	};

	// Request-owned capture: identities are frozen now, while source bytes remain
	// lazy so a valid cache hit does not materialize them.
	class FBuildInputs
	{
	public:
		FBuildInputs() = default;
		DERIVEDDATACACHE_API static auto TryCreate(
			std::span<const FBuildSourceReference> Sources,
			std::shared_ptr<const IBuildInputResolver> Resolver,
			const FBuildCancellation& Cancel = {})
			-> std::expected<FBuildInputs, FBuildFailure>;
		auto IsValid() const -> bool { return State != nullptr; }
		DERIVEDDATACACHE_API auto GetIdentities() const
			-> std::span<const FBuildInputReference>;
	private:
		friend struct Private::FBuildExecutionAccess;
		struct FState;
		std::shared_ptr<const FState> State;
	};

	using FBuildMetricSink = std::function<void(std::string_view, uint64)>;
	class FBuildContext
	{
	public:
		FBuildContext(const FBuildAction& Action, std::span<const FBuildInput> Inputs,
			FBuildCancellation Cancel, FBuildMetricSink Metrics = {},
			uint64 MaximumWorkingSetBytes = std::numeric_limits<uint64>::max())
			: Action(Action), Inputs(Inputs), Cancel(std::move(Cancel)), Metrics(std::move(Metrics)),
			  MaximumWorkingSetBytes(MaximumWorkingSetBytes) {}
		auto GetAction() const -> const FBuildAction& { return Action; }
		auto GetMaximumWorkingSetBytes() const -> uint64 { return MaximumWorkingSetBytes; }
		auto GetInputs() const -> std::span<const FBuildInput> { return Inputs; }
		auto IsCancelled() const -> bool { return Cancel.IsCancelled(); }
		auto ReportMetric(std::string_view Name, uint64 Value) const noexcept -> void
		{
			try { if (Metrics) Metrics(Name, Value); } catch (...) {}
		}
	private:
		const FBuildAction& Action;
		std::span<const FBuildInput> Inputs;
		FBuildCancellation Cancel;
		FBuildMetricSink Metrics;
		uint64 MaximumWorkingSetBytes;
	};

	class IBuildFunction
	{
	public:
		virtual ~IBuildFunction() = default;
		virtual auto GetDescriptor() const -> FBuildFunctionDescriptor = 0;
		virtual auto Build(FBuildContext& Context) const -> FBuildFunctionResult = 0;
		virtual auto Validate(const FBuildAction& Action, const FBuildOutput& Output,
			const FBuildCancellation& Cancel) const -> FBuildValidationResult = 0;
	};
}
