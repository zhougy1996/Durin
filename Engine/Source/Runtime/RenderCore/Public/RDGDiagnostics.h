#pragma once

#include "RDGExecution.h"
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace Durin
{
	// Records one pointer-free declared resource and its preparation outcome.
	struct FRDGResourceCapture final
	{
		uint32 ResourceId = 0;
		std::string Name;
		ERDGResourceKind Kind = ERDGResourceKind::Texture;
		bool bExternal = false;
		std::string Preparation;
		std::string AllocationDisposition;
		uint64 PhysicalAllocationId = 0;
		std::string ValueType;
		EPixelFormat TextureFormat = EPixelFormat::Unknown;
		FIntPoint TextureExtent{0, 0};
		uint16 TextureArraySize = 0;
		uint8 TextureMips = 0;
		uint64 BufferSize = 0;
		uint32 BufferStride = 0;
	};

	// Records a pointer-free use: texture subresource or declared buffer byte range.
	// Buffer versions are resource-wide and advance once per writing pass.
	struct FRDGUseCapture final
	{
		uint32 PassDeclarationIndex = 0;
		uint32 ResourceId = 0;
		ERDGUse Use = ERDGUse::Read;
		ERHIAccess Access = ERHIAccess::None;
		FRHITextureSubresourceRange TextureRange{};
		uint64 BufferOffset = 0;
		uint64 BufferSize = 0;
		uint32 Version = 0;
		bool bDiscard = false;
		bool bStore = true;
		std::string ParameterPath;
		std::string ShaderBindingName;
		ERHIBindingType ShaderBindingType = ERHIBindingType::Texture;
	};

	// Records one submitted leaf parameter capability, including optional absence.
	struct FRDGParameterCapture final
	{
		uint32 PassDeclarationIndex = 0;
		std::string FieldPath;
		ERDGParameterMemberKind Kind =
			ERDGParameterMemberKind::Texture;
		ERDGResourceKind ResourceKind =
			ERDGResourceKind::Texture;
		bool bPresent = false;
		// Absent optional fields use max uint32 and never name a synthetic resource.
		uint32 ResourceId = std::numeric_limits<uint32>::max();
		ERDGUse Use = ERDGUse::Read;
		ERHIAccess Access = ERHIAccess::None;
		FRHITextureSubresourceRange TextureRange{};
		uint64 BufferOffset = 0;
		uint64 BufferSize = 0;
		bool bDiscard = false;
		bool bStore = true;
		bool bPassManagedTransition = false;
		ERHIAccess ResultAccess = ERHIAccess::None;
		std::string ShaderBindingName;
		ERHIBindingType ShaderBindingType = ERHIBindingType::Texture;
	};

	// Separates command-list barriers from transitions owned by a pass body.
	enum class ERDGTransitionKind : uint8
	{
		RHIBarrier,
		PassManaged
	};

	// Records one exact pointer-free transition at a pass or graph boundary.
	struct FRDGTransitionCapture final
	{
		uint32 ResourceId = 0;
		uint32 PassIndex = std::numeric_limits<uint32>::max();
		ERHIAccess Before = ERHIAccess::None;
		ERHIAccess After = ERHIAccess::None;
		FRHITextureSubresourceRange TextureRange{};
		uint64 BufferOffset = 0;
		uint64 BufferSize = 0;
		bool bFinal = false;
		bool bDiscardContents = false;
		ERDGTransitionKind Kind = ERDGTransitionKind::RHIBarrier;
		ERDGQueueAssignment SourceQueue = ERDGQueueAssignment::Graphics;
		ERDGQueueAssignment DestinationQueue = ERDGQueueAssignment::Graphics;
	};

	// Reports the retained scheduled interval of one declared resource.
	struct FRDGResourceLifetime final
	{
		std::string Name;
		uint32 FirstPass = 0;
		uint32 LastPass = 0;
		bool bExternal = false;
		bool bCulled = false;
	};

	// Explains whether one declared pass survived explicit-root reachability.
	struct FRDGCullingDecision final
	{
		std::string Name;
		bool bCulled = false;
		std::string Reason;
	};

	// CPU phase durations in microseconds. Unentered phases remain zero; entered
	// phases retain elapsed time on failure/unwinding. Sub-microsecond work rounds down.
	struct FRDGPhaseTimings final
	{
		uint64 ValidationMicroseconds = 0;
		uint64 RangeMicroseconds = 0;
		uint64 DependencyMicroseconds = 0;
		uint64 CullingMicroseconds = 0;
		uint64 PlanMicroseconds = 0;
		uint64 PreparationMicroseconds = 0;
		uint64 RecordingMicroseconds = 0;
	};

	// Reports graph shape and CPU cost without affecting execution correctness.
	struct FRDGStatistics final
	{
		uint32 DeclaredPasses = 0;
		uint32 ScheduledPasses = 0;
		uint32 CulledPasses = 0;
		uint32 Dependencies = 0;
		uint32 BufferTransitions = 0;
		// Executable entries after range compaction; regression budgets use this count.
		uint32 TextureTransitions = 0;
		// Subresource barrier events before compaction, including repeated transitions.
		uint32 TextureTransitionSubresources = 0;
		uint64 CompileMicroseconds = 0;
		// Includes preparation and recording; excludes compilation and authoring.
		uint64 ExecuteMicroseconds = 0;
		FRDGPhaseTimings Phases;
		bool bPassRegressionBudgetExceeded = false;
		bool bDependencyRegressionBudgetExceeded = false;
		bool bBufferTransitionRegressionBudgetExceeded = false;
		bool bTextureTransitionRegressionBudgetExceeded = false;
		bool bCompileBudgetExceeded = false;
		bool bExecuteBudgetExceeded = false;

		auto IsStructuralRegressionBudgetExceeded() const -> bool
		{
			return bPassRegressionBudgetExceeded
				|| bDependencyRegressionBudgetExceeded
				|| bBufferTransitionRegressionBudgetExceeded
				|| bTextureTransitionRegressionBudgetExceeded;
		}
	};

	// Pointer-free pass record suitable for persistence and tooling.
	struct FRDGPassCapture final
	{
		std::string Name;
		ERDGPassType Type = ERDGPassType::Graphics;
		uint32 DeclarationIndex = 0;
		std::string ParameterStructName;
		uint32 BufferTransitions = 0;
		uint32 TextureTransitions = 0;
	};

	// Owns an immutable diagnostic snapshot independent of graph/RHI lifetimes.
	struct FRDGCapture final
	{
		// Original execution report; rejected repeated attempts do not replace it.
		std::optional<FRDGExecutionResult> ExecutionResult;
		// False before execution or after compilation failure; compiled arrays are empty.
		bool bCompiled = false;
		FRDGBudget Budget;
		FRDGStatistics Statistics;
		FRDGAllocationStatistics AllocationStatistics;
		std::vector<FRDGPassCapture> Passes;
		std::vector<FRDGResourceCapture> Resources;
		std::vector<FRDGParameterCapture> Parameters;
		std::vector<FRDGUseCapture> Uses;
		std::vector<FRDGTransitionCapture> Transitions;
		std::vector<FRDGDependency> Dependencies;
		std::vector<FRDGResourceLifetime> ResourceLifetimes;
		std::vector<FRDGCullingDecision> CullingDecisions;
		FRDGExecutionPlan ExecutionPlan;
		std::string Dump;
	};
} // namespace Durin
