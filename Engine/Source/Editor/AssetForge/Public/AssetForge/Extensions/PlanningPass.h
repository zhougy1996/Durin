#pragma once

#include "AssetForge/Extensions/SourceTranslator.h"
#include "AssetForge/Graph/BuildGraph.h"

namespace Durin::AssetForge
{
struct FPlanningPassDescriptor
	{
		FComponentIdentity Identity;
		int32 Priority = 0;
		EThreadCapability ExecutionThread = EThreadCapability::WorkerSafe;
	};

class IPlanningPass
	{
	public:
		virtual ~IPlanningPass() = default;
		virtual auto Execute(
			const FSourceGraph& SourceGraph,
			const FBuildGraph* PreviousGraph,
			const FSchemaPayload& Settings,
			FBuildGraphBuilder& Builder,
			std::vector<FImportDiagnostic>& OutDiagnostics) const -> bool = 0;
	};

	struct FPlanningPassStackEntry
	{
		std::string PlanningPassId;
		uint32 ContractVersion = 0;
		FSchemaPayload Settings;

		auto operator==(const FPlanningPassStackEntry&) const -> bool = default;
	};

struct FPlanningPassExecutionResult
	{
		bool bSucceeded = false;
		FBuildGraph Graph;
		std::vector<FImportDiagnostic> Diagnostics;
		uint64 RegistryRevision = 0;

		explicit operator bool() const { return bSucceeded; }
	};

	ASSETFORGE_API auto ExecutePlanningPassStack(
		const FSourceGraph& SourceGraph,
		std::span<const FPlanningPassStackEntry> PlanningPassStack,
		const FGraphLimits& Limits = {})
		-> FPlanningPassExecutionResult;
}
