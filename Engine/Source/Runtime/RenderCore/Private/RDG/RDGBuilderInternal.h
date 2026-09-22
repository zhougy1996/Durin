#pragma once

#include "RDGInternal.h"

// The builder owns declarations, immutable compiled records, and lazy diagnostics.
namespace Durin
{
	struct FRDGBuilder::FState
	{
		bool bAsyncComputeEnabled = false;
		uint64 CompileMicroseconds = 0;
		uint64 ExecuteMicroseconds = 0;
		FRDGPhaseTimings Phases;
		uint64 Owner = 0;
		std::vector<RDGPrivate::FGraphResource> Resources;
		std::unordered_map<const void*, uint32> ExternalResources;
		std::vector<RDGPrivate::FGraphPass> Passes;
		std::vector<FRDGCompileError> DeclarationErrors;
		bool bEnableCulling = false;
		FRDGBudget Budget;
		uint64 QueuedUploadBytes = 0;
		ERDGBuilderState Lifecycle = ERDGBuilderState::Building;
		std::optional<FRDGExecutionResult> ExecutionResult;
		std::vector<FRHIGPUSyncPointRef> SubmissionSyncPoints;
		std::shared_ptr<FRDGAllocationRetirement> AllocationRetirement;
		bool bCompiled = false;
		uint32 PendingConstructions = 0;
		RDGPrivate::FGraphParameterStorage ParameterStorage;
		RDGPrivate::FGraphParameterStorage ValueStorage;
	};

	struct FRDGBuilder::FDiagnostics final
	{
		std::vector<FRDGResourceCapture> Resources;
		std::vector<FRDGParameterCapture> Parameters;
		std::vector<FRDGUseCapture> Uses;
		std::vector<FRDGTransitionCapture> Transitions;
		std::vector<FRDGCullingDecision> CullingDecisions;
	};

	struct FRDGBuilder::FCompiledState
	{
		// Keeps all execution-only state for one scheduled pass in one record.
		struct FCompiledPassRuntime final
		{
			const FRDGParameterizedPassExecute* ParameterizedExecute = nullptr;
			const FRDGRecordingPassExecute* RecordingExecute = nullptr;
			ERDGRecordingPolicy RecordingPolicy = ERDGRecordingPolicy::Serial;
			const FRDGParameterLayout* ParameterLayout = nullptr;
			const void* Parameters = nullptr;
			// Borrows immutable declaration storage for the builder execution lifetime.
			std::span<const RDGPrivate::FOptionalAlias> OptionalAliases;
			std::vector<uint32> ResourceIndices;
			std::vector<std::pair<uint32, ERDGUse>> ValueUses;
		};

		uint64 Owner = 0;
		// Borrows declarations owned by this single-use builder.
		std::span<const RDGPrivate::FGraphResource> Resources;
		std::vector<RDGPrivate::FGraphResourceBacking> Backings;
		std::vector<FRDGCompiledPass> Passes;
		std::vector<FCompiledPassRuntime> RuntimePasses;
		std::vector<FRDGDependency> Dependencies;
		std::vector<FRDGResourceLifetime> ResourceLifetimes;
		std::vector<bool> Retained;
		RDGPrivate::FGraphPass ExportPass;
		FRDGBarrierBatch FinalBarriers;
		FRDGExecutionPlan ExecutionPlan;
		std::vector<FRDGAllocationRequest> AllocationRequests;
		FRDGBudget Budget;
		FRDGAllocationStatistics AllocationStatistics;
	};

}
