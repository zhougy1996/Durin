#pragma once

#include "AssetForge/Graph/SourceGraph.h"

namespace Durin::AssetForge
{
// One planned authored output. AssetBuilder dependencies define construction and
	// publication order; translated references select immutable source values.
	struct FBuildNode
	{
		std::string StableIdentity;
		std::string BuilderId;
		uint32 BuilderContractVersion = 0;
		std::string OutputClassName;
		FAssetPath Destination;
		EImportOutputPolicy Policy = EImportOutputPolicy::Create;
		FSchemaPayload Settings;
		std::vector<std::string> SourceNodeReferences;
		std::vector<std::string> BuildDependencies;

		auto operator==(const FBuildNode&) const -> bool = default;
	};

	// Immutable, canonical pipeline output consumed by typed factories.
	class FBuildGraph
	{
	public:
		auto GetNodes() const -> std::span<const FBuildNode> { return Nodes; }
		auto GetFingerprint() const -> const FXxHash128& { return Fingerprint; }
		ASSETFORGE_API auto FindNode(std::string_view StableIdentity) const
			-> const FBuildNode*;

	private:
		std::vector<FBuildNode> Nodes;
		FXxHash128 Fingerprint{};

		friend class FBuildGraphBuilder;
	};

	// Validates output destinations, translated references and factory DAG edges
	// before detached product construction can start.
	class FBuildGraphBuilder
	{
	public:
		explicit FBuildGraphBuilder(
			const FSourceGraph& InSourceGraph,
			FGraphLimits InLimits = {})
			: SourceGraph(InSourceGraph), Limits(InLimits) {}

		ASSETFORGE_API auto AddNode(FBuildNode Node) -> bool;
		ASSETFORGE_API auto Finalize(
			FBuildGraph& OutGraph,
			std::vector<FImportDiagnostic>& OutDiagnostics) -> bool;

	private:
		const FSourceGraph& SourceGraph;
		FGraphLimits Limits;
		std::vector<FBuildNode> Nodes;
		bool bFinalized = false;
	};
}
