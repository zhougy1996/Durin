#pragma once

#include "AssetForge/Graph/SourceGraph.h"

namespace Durin::AssetForge
{
enum class EThreadCapability : uint8
	{
		WorkerSafe,
		EditorOnly
	};

	struct FComponentIdentity
	{
		std::string Id;
		uint32 ContractVersion = 0;
		FSchemaPayload Settings;

		auto operator==(const FComponentIdentity&) const -> bool = default;
	};

struct FTranslatorDescriptor
	{
		FComponentIdentity Identity;
		std::vector<std::string> Extensions;
		int32 Priority = 0;
		EThreadCapability TranslationThread = EThreadCapability::WorkerSafe;
	};

class ISourceTranslator
	{
	public:
		virtual ~ISourceTranslator() = default;
		virtual auto Recognize(const FImportSourceRecognition& Source) const -> bool = 0;
		virtual auto DiscoverDependencies(
			std::span<const FSourceSnapshotEntry> Sources,
			FDependencyRequestSink& Sink,
			std::vector<FImportDiagnostic>& OutDiagnostics) const -> bool = 0;
		virtual auto Translate(
			const FSourceSnapshot& Snapshot,
			const FSchemaPayload& Settings,
			FSourceGraphBuilder& Builder,
			std::vector<FImportDiagnostic>& OutDiagnostics) const -> bool = 0;
	};
}
