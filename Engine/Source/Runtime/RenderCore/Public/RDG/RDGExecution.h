#pragma once

#include "RDG/RDGAllocator.h"
#include "RDG/RDGParameters.h"
#include <concepts>
#include <expected>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace Durin
{
	enum class ERDGUseError : uint8
	{
		ResourceHandleInvalid,
		FinalAccessInvalid,
		RequiredAccessInvalid,
		PassAccessIncompatible,
		UseAccessMismatch,
		ReadDiscardInvalid,
		ManagedResultAccessInvalid,
		BufferRangeInvalid,
		TextureRangeInvalid,
		UsesOverlap,
		BufferProducerMissing,
		ResourceProducerMissing,
	};
	RENDERCORE_API auto ToString(ERDGUseError Error) -> std::string_view;

	enum class ERDGIdentityError : uint8
	{
		FinalAccessInvalid,
		ResourceNameEmpty,
		PhysicalResourceMissing,
		ExternalFinalAccessMissing,
		ExternalBufferContentModeInvalid,
		ResourceNameDuplicate,
		PassNameEmpty,
		PassNameDuplicate,
		ValueWriterCount,
		ValueStorageInvalid,
		ValueTypeNameChanged,
		ValueTypeNameReused,
		TextureExtractionHandleInvalid,
		TextureExtractionInvalid,
		TextureExtractionDuplicate,
		BufferExtractionHandleInvalid,
		BufferExtractionInvalid,
		BufferExtractionDuplicate,
		ParameterAllocationInvalid,
		ParameterAllocationSubmitted,
		PassHandleInvalid,
		ManualUseOnParameterizedPass,
		RootHandleInvalid,
		AsyncPassInvalid,
		ValueDirectionInvalid,
		ValueHandleInvalid,
	};
	RENDERCORE_API auto ToString(ERDGIdentityError Error) -> std::string_view;

	enum class ERDGDependencyError : uint8
	{
		DependencyNotForward,
		ProducerHandleInvalid,
		ConsumerHandleInvalid,
	};
	RENDERCORE_API auto ToString(ERDGDependencyError Error) -> std::string_view;

	enum class ERDGStateError : uint8
	{
		BuilderConsumed,
		CompilationIncomplete,
		PreparationIncomplete,
		StorageIncomplete,
		RecordingIncomplete,
	};
	RENDERCORE_API auto ToString(ERDGStateError Error) -> std::string_view;

	enum class ERDGPreparationError : uint8
	{
		AllocatorMissing,
		QueueTransferFailed,
	};
	RENDERCORE_API auto ToString(ERDGPreparationError Error) -> std::string_view;

	struct FRDGUseErrorContext
	{
		std::string PassName, ResourceName, ParameterPath, OtherParameterPath;
		ERDGResourceKind Kind{};
		ERDGPassType PassType{};
		ERDGUse Use{};
		uint32 PassIndex = UINT32_MAX, ResourceIndex = UINT32_MAX;
		uint32 UseIndex = UINT32_MAX, OtherUseIndex = UINT32_MAX;
		ERHIAccess Access = ERHIAccess::None, ResultAccess = ERHIAccess::None;
		uint64 BufferOffset = 0, BufferSize = 0, BufferCapacity = 0;
		FRHITextureSubresourceRange TextureRange;
		uint32 TextureMips = 0, TextureLayers = 0;
	};
	struct FRDGIdentityErrorContext
	{
		std::string Name, OtherName, TypeName;
		uint64 Index = 0, OtherIndex = 0, Expected = 0, Actual = 0;
	};
	enum class ERDGLimit : uint8
	{
		Passes, Resources, Uses, Dependencies, RangeCells, RangeCellCandidates, CellVisits, TextureTransitions, BufferTransitions, AllocationBytes, UploadPayloadBytes
	};
	struct FRDGResourceContractContext
	{
		std::string Name;
		ERDGResourceKind Kind{};
		FRHITextureDesc Texture;
		FRHIBufferDesc Buffer;
		ERHIAccess InitialAccess = ERHIAccess::None, FinalAccess = ERHIAccess::None;
	};

	struct FRDGUseError
	{
		ERDGUseError Reason;
		FRDGUseErrorContext Context{};
	};
	RENDERCORE_API auto ToString(const FRDGUseError& Error) -> std::string;
	struct FRDGIdentityError
	{
		ERDGIdentityError Reason;
		FRDGIdentityErrorContext Context{};
	};
	RENDERCORE_API auto ToString(const FRDGIdentityError& Error) -> std::string;
	struct FRDGDependencyError
	{
		ERDGDependencyError Reason;
		uint32 Producer = UINT32_MAX, Consumer = UINT32_MAX;
	};
	RENDERCORE_API auto ToString(const FRDGDependencyError& Error) -> std::string;

	struct FRDGLimitError
	{
		ERDGLimit Dimension;
		uint64 Actual = 0, Limit = 0;
	};
	RENDERCORE_API auto ToString(const FRDGLimitError& Error) -> std::string;
	struct FRDGExternalConflictError
	{
		FRDGResourceContractContext Canonical, Requested;
	};
	RENDERCORE_API auto ToString(const FRDGExternalConflictError& Error) -> std::string;
	struct FRDGMissingAllocationError
	{
		uint32 ResourceId = UINT32_MAX;
	};
	RENDERCORE_API auto ToString(const FRDGMissingAllocationError& Error) -> std::string;
	struct FRDGTextureAllocationError
	{
		uint32 ResourceId = UINT32_MAX;
		FRHITextureDesc Expected, Actual;
	};
	RENDERCORE_API auto ToString(const FRDGTextureAllocationError& Error) -> std::string;
	struct FRDGBufferAllocationError
	{
		uint32 ResourceId = UINT32_MAX;
		FRHIBufferDesc Expected, Actual;
	};
	RENDERCORE_API auto ToString(const FRDGBufferAllocationError& Error) -> std::string;

	using FRDGLimitResult = std::expected<void, FRDGLimitError>;

	// Aggregate complete errors only at the operation that can produce them.
	struct FRDGCompileError
	{
		using FDetail = std::variant<ERDGStateError, FRDGMetadataError, FRDGUseError,
			FRDGIdentityError, FRDGDependencyError, FRDGLimitError, FRDGExternalConflictError>;
		FDetail Detail;
		template<typename T> requires std::constructible_from<FDetail, T>
		FRDGCompileError(T Error) : Detail(std::move(Error)) {}
		FRDGCompileError(std::variant<FRDGDependencyError, FRDGLimitError> Error)
			: Detail(std::visit([](auto&& Value) -> FDetail { return std::move(Value); }, std::move(Error))) {}
	};
	RENDERCORE_API auto ToString(const FRDGCompileError& Error) -> std::string;
	using FRDGCompileResult = std::expected<void, FRDGCompileError>;

	struct FRDGPreparationError
	{
		using FDetail = std::variant<ERDGStateError, ERDGPreparationError, FRDGAllocationError,
			FRDGMissingAllocationError, FRDGTextureAllocationError, FRDGBufferAllocationError>;
		FDetail Detail;
		template<typename T> requires std::constructible_from<FDetail, T>
		FRDGPreparationError(T Error) : Detail(std::move(Error)) {}
	};
	RENDERCORE_API auto ToString(const FRDGPreparationError& Error) -> std::string;
	using FRDGPreparationResult = std::expected<void, FRDGPreparationError>;


	// Records one immutable dependency edge in compiler diagnostics.
	struct FRDGDependency final
	{
		uint32 BeforePass = 0;
		uint32 AfterPass = 0;
		std::string Cause;
		ERDGDependencyKind Kind = ERDGDependencyKind::Execution;

		auto operator==(const FRDGDependency&) const -> bool = default;
	};

	// Separates catastrophic graph-shape safety limits from observational budgets.
	struct FRDGBudget final
	{
		uint32 MaxPasses = std::numeric_limits<uint32>::max();
		// Counts unique declared edges before culling; regression budgets count retained edges.
		uint32 MaxDependencies = std::numeric_limits<uint32>::max();
		uint32 MaxBufferTransitions = std::numeric_limits<uint32>::max();
		// Bounds generated subresource events before executable range compaction.
		uint32 MaxTextureTransitions = std::numeric_limits<uint32>::max();
		uint32 RegressionMaxPasses = std::numeric_limits<uint32>::max();
		uint32 RegressionMaxDependencies = std::numeric_limits<uint32>::max();
		uint32 RegressionMaxBufferTransitions =
			std::numeric_limits<uint32>::max();
		uint32 RegressionMaxTextureTransitions =
			std::numeric_limits<uint32>::max();
		// Bound scratch allocation and range work before culling, including exports.
		uint32 MaxResources = 65'536;
		uint32 MaxUses = 1'048'576;
		uint32 MaxRangeCells = 262'144;
		// Candidates count fixed layout cells; visits include layout construction,
		// dependency analysis and barrier traversal. Unused texture cells still count.
		uint32 MaxRangeCellCandidates = 1'048'576;
		uint32 MaxCellVisits = 16'777'216;
		uint64 MaxCompileMicroseconds = std::numeric_limits<uint64>::max();
		uint64 MaxExecuteMicroseconds = std::numeric_limits<uint64>::max();
	};

	// Graph-local submission identity, independent of native queues and GPU values.
	struct FRDGSubmissionId final
	{
		uint32 Index = 0;
		auto operator==(const FRDGSubmissionId&) const -> bool = default;
	};

	// Logical scheduling role; the backend may map both roles to one physical queue.

	// Contiguous scheduled pass interval, or a graph epilogue with no pass callback.
	struct FRDGSubmissionBatch final
	{
		FRDGSubmissionId Id;
		ERDGQueueAssignment Queue = ERDGQueueAssignment::Graphics;
		uint32 FirstPass = 0;
		uint32 NumPasses = 0;
		bool bEpilogue = false;
		auto operator==(const FRDGSubmissionBatch&) const -> bool = default;
	};

	// Dependency causes survive batching; queue order is an explicit execution edge.
	struct FRDGSubmissionDependency final
	{
		FRDGSubmissionId Before;
		FRDGSubmissionId After;
		ERDGDependencyKind Kind = ERDGDependencyKind::Execution;
		std::string Cause;
		auto operator==(const FRDGSubmissionDependency&) const -> bool = default;
	};

	// Locates an exact logical transition in its consumer's prologue or epilogue.
	// Dependencies own execution ordering; this record owns no physical backing.
	struct FRDGResourceHandoff final
	{
		uint32 ResourceId = 0;
		FRDGSubmissionId Consumer;
		uint32 TransitionIndex = 0;
		bool bTexture = false;
		// Latest prior use per logical queue for this exact tracked range.
		std::vector<FRDGSubmissionId> Producers;
		ERDGQueueAssignment SourceQueue = ERDGQueueAssignment::Graphics;
		// Compiled pass whose barrier owns TransitionIndex; unused for the epilogue.
		uint32 ConsumerPass = UINT32_MAX;
		auto operator==(const FRDGResourceHandoff&) const -> bool = default;
	};

	// Immutable logical execution data, prepared and replayed through separate state.
	struct FRDGExecutionPlan final
	{
		std::vector<FRDGSubmissionBatch> Batches;
		std::vector<FRDGSubmissionDependency> Dependencies;
		std::vector<FRDGResourceHandoff> Handoffs;
		auto operator==(const FRDGExecutionPlan&) const -> bool = default;
	};

	// CPU recording outcome; Recorded does not imply GPU completion.
	enum class ERDGExecutionStatus : uint8
	{
		CompileFailed, PreparationFailed, Recorded, InvalidState
	};

	// Thread-confined, single-use graph lifecycle; failure is terminal.
	enum class ERDGBuilderState : uint8
	{
		Building, Compiling, Preparing, Recording, Recorded, Failed
	};

	// Only Execute combines phase errors. The active alternative identifies the phase.
	struct FRDGExecutionError
	{
		using FDetail = std::variant<ERDGStateError, FRDGCompileError, FRDGPreparationError>;
		FDetail Detail;
		template<typename T> requires std::constructible_from<FDetail, T>
		FRDGExecutionError(T Error) : Detail(std::move(Error)) {}
		auto GetStatus() const -> ERDGExecutionStatus
		{
			if (std::holds_alternative<FRDGCompileError>(Detail)) return ERDGExecutionStatus::CompileFailed;
			if (std::holds_alternative<FRDGPreparationError>(Detail)) return ERDGExecutionStatus::PreparationFailed;
			return ERDGExecutionStatus::InvalidState;
		}
	};
	RENDERCORE_API auto ToString(const FRDGExecutionError& Error) -> std::string;
	using FRDGExecutionResult = std::expected<void, FRDGExecutionError>;
	inline auto GetRDGExecutionStatus(const FRDGExecutionResult& Result) -> ERDGExecutionStatus
	{ return Result ? ERDGExecutionStatus::Recorded : Result.error().GetStatus(); }

	// A graph-local buffer handoff; physical backing is resolved only for recording.
	struct FRDGBufferTransition final
	{
		uint32 ResourceId = std::numeric_limits<uint32>::max();
		uint64 Offset = 0;
		uint64 Size = 0;
		ERHIAccess ExpectedBefore = ERHIAccess::None;
		ERHIAccess RequiredAfter = ERHIAccess::None;
		bool bDiscardContents = false;

		auto operator==(const FRDGBufferTransition&) const -> bool = default;
	};

	// A graph-local texture handoff retaining its exact subresource range.
	struct FRDGTextureTransition final
	{
		uint32 ResourceId = std::numeric_limits<uint32>::max();
		FRHITextureSubresourceRange Range{};
		ERHIAccess ExpectedBefore = ERHIAccess::None;
		ERHIAccess RequiredAfter = ERHIAccess::None;
		bool bDiscardContents = false;

		auto operator==(const FRDGTextureTransition&) const -> bool = default;
	};

	// Owns logical barriers for a pass or the graph epilogue. Each record carries
	// its resource identity; compiled batches are observed without physical pointers.
	class FRDGBarrierBatch final
	{
	public:
		auto Reserve(size_t BufferCount, size_t TextureCount) -> void
		{
			BufferTransitions.reserve(BufferCount);
			TextureTransitions.reserve(TextureCount);
		}
		auto AddTransition(FRDGBufferTransition Transition) -> void
		{ BufferTransitions.push_back(Transition); }
		auto AddTransition(FRDGTextureTransition Transition) -> void
		{ TextureTransitions.push_back(Transition); }
		auto GetBufferTransitions() const -> std::span<const FRDGBufferTransition>
		{ return BufferTransitions; }
		auto GetTextureTransitions() const -> std::span<const FRDGTextureTransition>
		{ return TextureTransitions; }

	private:
		std::vector<FRDGBufferTransition> BufferTransitions;
		std::vector<FRDGTextureTransition> TextureTransitions;
	};

	// Owns the compiled pass order and transition batches for one graph.
	struct FRDGCompiledPass final
	{
		std::string Name;
		ERDGPassType Type = ERDGPassType::Graphics;
		uint32 DeclarationIndex = 0;
		std::string ParameterStructName;
		FRDGBarrierBatch Barriers;
	};
} // namespace Durin
