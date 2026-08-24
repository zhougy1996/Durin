#pragma once

#include "AssetForge/Graph/SchemaPayload.h"

namespace Durin::AssetForge
{
// One source-semantic node emitted by a translator. Dependencies refer to
	// other translated-node identities and are independent of storage order.
	struct FSourceNode
	{
		std::string StableIdentity;
		std::string NodeKind;
		FSchemaPayload Payload;
		std::vector<std::string> SourceIdentities;
		std::vector<std::string> Dependencies;

		auto operator==(const FSourceNode&) const -> bool = default;
	};

	// Immutable, canonical translator output safe for worker use and persistence.
	class FSourceGraph
	{
	public:
		auto GetNodes() const -> std::span<const FSourceNode> { return Nodes; }
		auto GetFingerprint() const -> const FXxHash128& { return Fingerprint; }
		ASSETFORGE_API auto FindNode(std::string_view StableIdentity) const
			-> const FSourceNode*;

	private:
		std::vector<FSourceNode> Nodes;
		FXxHash128 Fingerprint{};

		friend class FSourceGraphBuilder;
	};

	// Validates and finalizes translator output into one immutable graph.
	class FSourceGraphBuilder
	{
	public:
		explicit FSourceGraphBuilder(FGraphLimits InLimits = {})
			: Limits(InLimits) {}

		ASSETFORGE_API auto AddNode(FSourceNode Node) -> bool;
		ASSETFORGE_API auto Finalize(
			FSourceGraph& OutGraph,
			std::vector<FImportDiagnostic>& OutDiagnostics) -> bool;

	private:
		FGraphLimits Limits;
		std::vector<FSourceNode> Nodes;
		bool bFinalized = false;
	};
}
