#pragma once

#include "DerivedDataBuildDefinition.h"
#include "DerivedDataBuildOutput.h"
#include <functional>
#include <optional>
#include <limits>

namespace Durin::DerivedData
{
	enum class EBuildSessionPhase : uint8
	{
		Admission, Describe, Action, Lookup, Decode, Resolve, Build, Validate, Record, Encode, Compress, Store, Dispatch, Count
	};
	enum class EBuildErrorCategory : uint8 { InvalidInput, InvalidOutput, ProducerFailure, Unavailable, Cancelled };

	// One boundary diagnostic; producer scalars remain opaque to DDC and are never persisted.
	struct FBuildError
	{
		EBuildSessionPhase Phase = EBuildSessionPhase::Admission;
		EBuildErrorCategory Category = EBuildErrorCategory::InvalidInput;
		std::string Description;
		std::optional<uint32> ProducerCode;
		std::optional<FXxHash128> DiagnosticIdentity;
		static constexpr size_t MaximumDescriptionBytes = 4096;
		auto BoundDescription() -> void
		{
			if (Description.size() > MaximumDescriptionBytes) Description.resize(MaximumDescriptionBytes);
		}
	};
	using FBuildResult = std::expected<FBuildOutput, FBuildError>;

	// The owning request supplies a thread-safe predicate. Functions only observe it.
	class FBuildCancellation
	{
	public:
		FBuildCancellation() = default;
		explicit FBuildCancellation(std::function<bool()> Predicate) : Predicate(std::move(Predicate)) {}
		auto IsCancelled() const -> bool { return Predicate && Predicate(); }
	private:
		std::function<bool()> Predicate;
	};

	// Representation metadata and named immutable blocks; no family product sidecar.
	struct FBuildInput
	{
		// Source closures may contain more files than an output/cache value table.
		// Request policy defaults to 4096; larger captures require explicit admission.
		static constexpr uint32 MaximumValues = 131072;
		FBuildInputReference Identity;
		FSharedByteBuffer Metadata;
		// The executor validates and canonicalizes IDs before invoking a function.
		std::vector<FBuildValue> Values;
	};

	class IBuildInputResolver
	{
	public:
		virtual ~IBuildInputResolver() = default;
		// Captured metadata only: no source-byte reads are allowed here.
		virtual auto Describe(std::span<const FBuildSourceReference> Sources, const FBuildCancellation& Cancel) const
			-> std::expected<std::vector<FBuildInputReference>, FBuildError> = 0;
		// Must verify materialized bytes against the promised semantic identity.
		virtual auto Resolve(std::span<const FBuildInputReference> Inputs, const FBuildCancellation& Cancel) const
			-> std::expected<std::vector<FBuildInput>, FBuildError> = 0;
	};

	using FBuildMetricObserver = std::function<void(std::string_view, uint64)>;

	// Borrowed action/inputs are owned by the admitted request for the entire call.
	class FBuildContext
	{
	public:
		FBuildContext(const FBuildAction& Action, std::span<const FBuildInput> Inputs, FBuildCancellation Cancel, FBuildMetricObserver Metrics = {},
			uint64 MaximumWorkingSetBytes = std::numeric_limits<uint64>::max())
			: Action(Action), Inputs(Inputs), Cancel(std::move(Cancel)), Metrics(std::move(Metrics)), MaximumWorkingSetBytes(MaximumWorkingSetBytes) {}
		auto GetAction() const -> const FBuildAction& { return Action; }
		// Execution admission only; this reservation is never part of action identity.
		auto GetMaximumWorkingSetBytes() const -> uint64 { return MaximumWorkingSetBytes; }
		auto GetInputs() const -> std::span<const FBuildInput> { return Inputs; }
		auto IsCancelled() const -> bool { return Cancel.IsCancelled(); }
		// Execution-local scalar observations never enter deterministic output/records.
		auto ReportMetric(std::string_view Name, uint64 Value) const noexcept -> void
		{
			try { if (Metrics) Metrics(Name, Value); } catch (...) {}
		}
	private:
		const FBuildAction& Action;
		std::span<const FBuildInput> Inputs;
		FBuildCancellation Cancel;
		FBuildMetricObserver Metrics;
		uint64 MaximumWorkingSetBytes;
	};

	class IBuildFunction
	{
	public:
		virtual ~IBuildFunction() = default;
		// Queried once at explicit registration. Implementations retain their execution services.
		virtual auto GetDescriptor() const -> FBuildFunctionDescriptor = 0;
		virtual auto Build(FBuildContext& Context) const -> FBuildResult = 0;
		// Descriptor-only semantic validation; never resolve source or construct a product.
		virtual auto Validate(const FBuildAction& Action, const FBuildOutput& Output,
			const FBuildCancellation& Cancel) const -> std::expected<void, FBuildError> = 0;
	};

	struct FRegisteredBuildFunction
	{
		FBuildFunctionDescriptor Descriptor;
		std::shared_ptr<const IBuildFunction> Function;
	};

	class FBuildRegistrySnapshot
	{
	public:
		DERIVEDDATACACHE_API auto Find(std::string_view Name) const -> std::shared_ptr<const FRegisteredBuildFunction>;
	private:
		friend class FBuildRegistry;
		std::vector<std::shared_ptr<const FRegisteredBuildFunction>> Entries;
	};

	// Bootstrap explicitly, then freeze before admitting any work. Snapshot entries
	// retain functions/services after the registry owner is destroyed; no hot replacement.
	class FBuildRegistry
	{
	public:
		DERIVEDDATACACHE_API FBuildRegistry();
		DERIVEDDATACACHE_API ~FBuildRegistry();
		FBuildRegistry(const FBuildRegistry&) = delete;
		auto operator=(const FBuildRegistry&) -> FBuildRegistry& = delete;
		DERIVEDDATACACHE_API auto Register(std::shared_ptr<const IBuildFunction> Function) -> std::expected<void, FBuildError>;
		DERIVEDDATACACHE_API auto Freeze() -> std::expected<FBuildRegistrySnapshot, FBuildError>;
	private:
		struct FState;
		std::unique_ptr<FState> State;
	};
}
