#pragma once

#include "CoreMinimal.h"
#include "Modules/ModularFeature.h"

namespace Durin
{
	// Detached values: no document views, engine objects, or provider-owned deleters.
	struct FToolValue
	{
		using FArray = std::vector<FToolValue>;
		using FObject = std::map<std::string, FToolValue, std::less<>>;
		using FStorage = std::variant<std::monostate, bool, int64, uint64, double, std::string, FArray, FObject>;
		FStorage Data;

		FToolValue() = default;
		FToolValue(std::nullptr_t) : Data(std::monostate{}) {}
		FToolValue(bool Value) : Data(Value) {}
		template<std::signed_integral T> requires (!std::same_as<T, bool>)
		FToolValue(T Value) : Data(static_cast<int64>(Value)) {}
		template<std::unsigned_integral T> requires (!std::same_as<T, bool>)
		FToolValue(T Value) : Data(static_cast<uint64>(Value)) {}
		FToolValue(double Value) : Data(Value) {}
		FToolValue(std::string Value) : Data(std::move(Value)) {}
		FToolValue(std::string_view Value) : Data(std::string(Value)) {}
		FToolValue(const char* Value) : Data(std::string(Value)) {}
		FToolValue(FArray Value) : Data(std::move(Value)) {}
		FToolValue(FObject Value) : Data(std::move(Value)) {}
		auto operator==(const FToolValue&) const -> bool = default;
	};

	enum class EToolValueType : uint8 { Null, Boolean, Integer, UnsignedInteger, Number, String, Array, Object };
	enum class EToolError : uint8
	{
		InvalidSchema, InvalidDescriptor, InvalidArguments, InvalidOutput, LimitExceeded,
		VersionMismatch, ToolUnavailable, TargetNotFound, RevisionConflict, Busy,
		Cancelled, InvocationExpired, WrongThread, InternalError, DomainFailure
	};

	struct FToolDiagnostic
	{
		EToolError Code = EToolError::DomainFailure;
		std::string Message;
		// RFC 6901 JSON pointer; empty means the root. Schema paths use the same notation.
		std::string Path;
	};

	// Limits apply independently to schema metadata and normalized input/output trees.
	struct FToolLimits
	{
		size_t MaxDepth = 32;
		size_t MaxNodes = 4096;
		size_t MaxStringBytes = 64 * 1024;
		size_t MaxTotalStringBytes = 1024 * 1024;
		size_t MaxContainerEntries = 1024;
	};

	// A closed schema vocabulary. Inapplicable fields and invalid enum tags are errors.
	struct FToolSchema
	{
		EToolValueType Type = EToolValueType::Null;
		std::string Description;
		std::map<std::string, FToolSchema, std::less<>> Properties;
		std::vector<std::string> Required;
		std::unique_ptr<FToolSchema> Items;
		std::vector<FToolValue> Enum;
		std::optional<FToolValue> Default;
		// Bounds have the exact schema numeric type, preserving 64-bit comparisons.
		std::optional<FToolValue> Minimum;
		std::optional<FToolValue> Maximum;
		std::optional<size_t> MinSize;
		std::optional<size_t> MaxSize;

		FToolSchema() = default;
		explicit FToolSchema(EToolValueType InType) : Type(InType) {}
		CORE_API FToolSchema(const FToolSchema& Other);
		CORE_API auto operator=(const FToolSchema& Other) -> FToolSchema&;
		FToolSchema(FToolSchema&&) noexcept = default;
		auto operator=(FToolSchema&&) noexcept -> FToolSchema& = default;
	};

	enum class EToolEffect : uint8 { ReadOnly = 0, MutatesMemory = 1, WritesStorage = 2, External = 4 };
	constexpr auto operator|(EToolEffect Left, EToolEffect Right) -> EToolEffect
	{
		return static_cast<EToolEffect>(static_cast<uint8>(Left) | static_cast<uint8>(Right));
	}
	enum class EToolRetryPolicy : uint8 { Unsafe, Idempotent };

	struct FToolDescriptor
	{
		std::string Name;
		uint32 ContractVersion = 1;
		std::string Description;
		FToolSchema Input{EToolValueType::Object};
		FToolSchema Output{EToolValueType::Object};
		EToolEffect Effects = EToolEffect::ReadOnly;
		EToolRetryPolicy RetryPolicy = EToolRetryPolicy::Unsafe;
		bool bExposeExternally = false;
	};

	// Explicit two-part identity; tool names are local to their owning toolset.
	struct FToolAddress
	{
		std::string Toolset;
		std::string Tool;
		auto operator<=>(const FToolAddress&) const = default;
	};

	// Detached group metadata; local tool names are scoped by Name.
	struct FToolsetDescriptor
	{
		std::string Name;
		std::string Description;
		std::vector<FToolDescriptor> Tools;
	};

	struct FToolRequest
	{
		FToolAddress Address;
		uint32 ContractVersion = 1;
		FToolValue Arguments{FToolValue::FObject{}};
	};

	// A token is read-only to a handler; the admitting host owns the source.
	class FToolCancellationToken
	{
	public:
		[[nodiscard]] auto IsCancellationRequested() const -> bool
		{
			return State && State->load(std::memory_order_acquire);
		}
	private:
		std::shared_ptr<std::atomic<bool>> State;
		friend class FToolCancellationSource;
	};

	class FToolCancellationSource
	{
	public:
		FToolCancellationSource() : State(std::make_shared<std::atomic<bool>>(false)) {}
		[[nodiscard]] auto GetToken() const -> FToolCancellationToken
		{
			FToolCancellationToken Token;
			Token.State = State;
			return Token;
		}
		auto Cancel() const -> void { State->store(true, std::memory_order_release); }
	private:
		std::shared_ptr<std::atomic<bool>> State;
	};

	enum class EToolSource : uint8 { Native, Console, External };
	struct FToolContext
	{
		std::string InvocationId;
		std::string SessionId;
		EToolSource Source = EToolSource::Native;
		FToolCancellationToken Cancellation;
	};

	enum class EToolStatus : uint8 { Succeeded, Failed };
	enum class EToolInvocationState : uint8 { Queued, Running, Completed, Cancelled, Expired };
	struct FToolResult
	{
		EToolStatus Status = EToolStatus::Failed;
		FToolValue Data;
		std::vector<FToolDiagnostic> Diagnostics;
		// Reserved for domain-owned asynchronous work; no scheduler is implied.
		std::optional<std::string> OperationId;
	};
	struct FToolAvailability
	{
		bool bAvailable = true;
		std::vector<FToolDiagnostic> Diagnostics;
	};

	// One registered capability group. Descriptions are immutable until re-registration.
	class IToolset : public IModularFeature
	{
	public:
		static constexpr std::string_view FeatureName = "Durin.Toolset";
		static constexpr uint32 FeatureVersion = 1;
		virtual auto DescribeToolset() const -> FToolsetDescriptor = 0;
		virtual auto CheckAvailability(const FToolRequest&, const FToolContext&) -> FToolAvailability = 0;
		// The caller validates/normalizes Arguments once before availability and execution.
		virtual auto Call(const FToolRequest&, const FToolContext&) -> FToolResult = 0;
	};

	using FToolValidationResult = std::expected<void, FToolDiagnostic>;
	[[nodiscard]] CORE_API auto ValidateToolName(std::string_view Name) -> bool;
	// Validates group metadata; individual tool schemas are validated separately.
	[[nodiscard]] CORE_API auto ValidateToolsetDescriptor(const FToolsetDescriptor& Descriptor,
		const FToolLimits& Limits = {}) -> FToolValidationResult;
	// Schema-independent preflight before copying a detached submission across threads.
	[[nodiscard]] CORE_API auto ValidateToolValue(const FToolValue& Value,
		const FToolLimits& Limits = {}) -> FToolValidationResult;
	// Validate provider metadata before copying it into a published catalog.
	[[nodiscard]] CORE_API auto ValidateToolDescriptor(const FToolDescriptor& Descriptor,
		const FToolLimits& Limits = {}) -> FToolValidationResult;
	// Produces an owned normalized copy, applying field defaults only to absent input fields.
	[[nodiscard]] CORE_API auto PrepareToolArguments(const FToolDescriptor& Descriptor,
		const FToolRequest& Request, const FToolLimits& Limits = {})
		-> std::expected<FToolRequest, FToolDiagnostic>;
	// Outputs must already be complete; defaults are never applied to provider results.
	[[nodiscard]] CORE_API auto ValidateToolOutput(const FToolDescriptor& Descriptor,
		const FToolValue& Output, const FToolLimits& Limits = {}) -> FToolValidationResult;
}
