#include "Interchange.h"
#include "ImportService.h"

#include <unordered_set>

namespace Durin::Asset
{
	namespace
	{
		auto IsStableIdentity(std::string_view Value) -> bool
		{
			if (Value.empty() || Value.size() > 1'024) return false;
			return std::ranges::all_of(Value, [](char Character) {
				const unsigned char Byte = static_cast<unsigned char>(Character);
				return Byte >= 0x21 && Byte <= 0x7e && Character != '\\';
			});
		}

		auto AddDiagnostic(
			std::vector<FImportDiagnostic>& Diagnostics,
			const FInterchangeGraphLimits& Limits,
			EImportDiagnosticCategory Category,
			std::string Identity,
			std::string Message) -> void
		{
			if (Diagnostics.size() >= Limits.MaximumDiagnostics) return;
			Diagnostics.push_back({
				.Severity = EImportDiagnosticSeverity::Error,
				.Category = Category,
				.Identity = std::move(Identity),
				.Phase = "InterchangeGraph",
				.Message = std::move(Message)});
		}

		template<typename TNode, typename TDependencies>
		auto ValidateDag(
			const std::vector<TNode>& Nodes,
			TDependencies GetDependencies,
			const FInterchangeGraphLimits& Limits,
			std::vector<FImportDiagnostic>& Diagnostics) -> bool
		{
			std::unordered_map<std::string_view, size_t> Indices;
			Indices.reserve(Nodes.size());
			for (size_t Index = 0; Index < Nodes.size(); ++Index)
			{
				if (!Indices.emplace(Nodes[Index].StableIdentity, Index).second)
				{
					AddDiagnostic(Diagnostics, Limits, EImportDiagnosticCategory::InvalidPlan,
						"InterchangeDuplicateIdentity",
						std::format("Duplicate interchange node identity '{}'.", Nodes[Index].StableIdentity));
					return false;
				}
			}

			std::vector<uint32> InDegree(Nodes.size());
			std::vector<std::vector<size_t>> Dependents(Nodes.size());
			uint64 DependencyCount = 0;
			for (size_t Index = 0; Index < Nodes.size(); ++Index)
			{
				std::unordered_set<std::string_view> Unique;
				for (const std::string& Dependency : GetDependencies(Nodes[Index]))
				{
					if (++DependencyCount > Limits.MaximumDependencies)
					{
						AddDiagnostic(Diagnostics, Limits, EImportDiagnosticCategory::ResourceLimitExceeded,
							"InterchangeDependencyLimit",
							"Interchange graph dependency count exceeds its configured limit.");
						return false;
					}
					const auto Found = Indices.find(Dependency);
					if (Found == Indices.end())
					{
						AddDiagnostic(Diagnostics, Limits, EImportDiagnosticCategory::InvalidPlan,
							"InterchangeMissingDependency",
							std::format("Node '{}' references missing dependency '{}'.",
								Nodes[Index].StableIdentity, Dependency));
						return false;
					}
					if (!Unique.emplace(Dependency).second)
					{
						AddDiagnostic(Diagnostics, Limits, EImportDiagnosticCategory::InvalidPlan,
							"InterchangeDuplicateDependency",
							std::format("Node '{}' repeats dependency '{}'.",
								Nodes[Index].StableIdentity, Dependency));
						return false;
					}
					++InDegree[Index];
					Dependents[Found->second].push_back(Index);
				}
			}

			std::vector<size_t> Ready;
			for (size_t Index = 0; Index < InDegree.size(); ++Index)
				if (InDegree[Index] == 0) Ready.push_back(Index);
			size_t Visited = 0;
			while (!Ready.empty())
			{
				const size_t Index = Ready.back();
				Ready.pop_back();
				++Visited;
				for (const size_t Dependent : Dependents[Index])
					if (--InDegree[Dependent] == 0) Ready.push_back(Dependent);
			}
			if (Visited != Nodes.size())
			{
				AddDiagnostic(Diagnostics, Limits, EImportDiagnosticCategory::DependencyCycle,
					"InterchangeDependencyCycle",
					"Interchange graph dependencies contain a cycle.");
				return false;
			}
			return true;
		}

		auto UpdateString(FXxHash128Builder& Builder, std::string_view Value) -> void
		{
			const uint64 Size = Value.size();
			Builder.UpdateValue(Size);
			Builder.Update(Value);
		}

		auto UpdatePayload(FXxHash128Builder& Builder, const FInterchangePayload& Payload) -> void
		{
			UpdateString(Builder, Payload.SchemaId);
			Builder.UpdateValue(Payload.SchemaVersion);
			Builder.UpdateValue(Payload.ContentHash.HashLow);
			Builder.UpdateValue(Payload.ContentHash.HashHigh);
		}

		auto CanonicalizeStrings(std::vector<std::string>& Values) -> void
		{
			std::ranges::sort(Values);
		}
	}

	auto FInterchangePayload::Finalize(std::string& OutError) -> bool
	{
		if (!IsStableIdentity(SchemaId) || SchemaVersion == 0)
		{
			OutError = "InterchangePayloadSchemaInvalid: schema identity and non-zero version are required.";
			return false;
		}
		ContentHash = FXxHash128::HashBuffer(std::span<const std::byte>(Bytes));
		OutError.clear();
		return true;
	}

	auto FTranslatedAssetGraph::FindNode(std::string_view StableIdentity) const
		-> const FTranslatedAssetNode*
	{
		const auto It = std::ranges::lower_bound(Nodes, StableIdentity, {}, &FTranslatedAssetNode::StableIdentity);
		return It != Nodes.end() && It->StableIdentity == StableIdentity ? &*It : nullptr;
	}

	auto FTranslatedAssetGraphBuilder::AddNode(FTranslatedAssetNode Node) -> bool
	{
		if (bFinalized || Nodes.size() >= Limits.MaximumNodes) return false;
		Nodes.push_back(std::move(Node));
		return true;
	}

	auto FTranslatedAssetGraphBuilder::Finalize(
		FTranslatedAssetGraph& OutGraph,
		std::vector<FImportDiagnostic>& OutDiagnostics) -> bool
	{
		if (bFinalized)
		{
			AddDiagnostic(OutDiagnostics, Limits, EImportDiagnosticCategory::InvalidPlan,
				"InterchangeBuilderAlreadyFinalized", "Translated graph builder can be finalized only once.");
			return false;
		}
		bFinalized = true;
		if (Nodes.empty())
		{
			AddDiagnostic(OutDiagnostics, Limits, EImportDiagnosticCategory::InvalidPlan,
				"InterchangeGraphEmpty", "Translated graph must contain at least one node.");
			return false;
		}

		uint64 PayloadBytes = 0;
		for (FTranslatedAssetNode& Node : Nodes)
		{
			if (!IsStableIdentity(Node.StableIdentity) || !IsStableIdentity(Node.NodeKind))
			{
				AddDiagnostic(OutDiagnostics, Limits, EImportDiagnosticCategory::InvalidPlan,
					"InterchangeNodeIdentityInvalid", "Translated nodes require valid stable identity and kind values.");
				return false;
			}
			std::string Error;
			if (!Node.Payload.Finalize(Error))
			{
				AddDiagnostic(OutDiagnostics, Limits, EImportDiagnosticCategory::InvalidPlan,
					"InterchangeNodePayloadInvalid", std::move(Error));
				return false;
			}
			if (Node.Payload.Bytes.size() > Limits.MaximumPayloadBytes - PayloadBytes)
			{
				AddDiagnostic(OutDiagnostics, Limits, EImportDiagnosticCategory::ResourceLimitExceeded,
					"InterchangePayloadLimit", "Translated graph payload bytes exceed the configured limit.");
				return false;
			}
			PayloadBytes += Node.Payload.Bytes.size();
			CanonicalizeStrings(Node.SourceIdentities);
			CanonicalizeStrings(Node.Dependencies);
		}
		std::ranges::sort(Nodes, {}, &FTranslatedAssetNode::StableIdentity);
		if (!ValidateDag(Nodes, [](const FTranslatedAssetNode& Node) -> const auto& {
			return Node.Dependencies; }, Limits, OutDiagnostics)) return false;

		FXxHash128Builder Fingerprint;
		UpdateString(Fingerprint, "Durin.Interchange.TranslatedGraph");
		Fingerprint.UpdateValue(InterchangeContractVersion);
		for (const FTranslatedAssetNode& Node : Nodes)
		{
			UpdateString(Fingerprint, Node.StableIdentity);
			UpdateString(Fingerprint, Node.NodeKind);
			UpdatePayload(Fingerprint, Node.Payload);
			for (const std::string& Source : Node.SourceIdentities) UpdateString(Fingerprint, Source);
			for (const std::string& Dependency : Node.Dependencies) UpdateString(Fingerprint, Dependency);
		}
		OutGraph.Nodes = std::move(Nodes);
		OutGraph.Fingerprint = Fingerprint.Finalize();
		return true;
	}

	auto FImportFactoryGraph::FindNode(std::string_view StableIdentity) const
		-> const FImportFactoryNode*
	{
		const auto It = std::ranges::lower_bound(Nodes, StableIdentity, {}, &FImportFactoryNode::StableIdentity);
		return It != Nodes.end() && It->StableIdentity == StableIdentity ? &*It : nullptr;
	}

	auto FImportFactoryGraphBuilder::AddNode(FImportFactoryNode Node) -> bool
	{
		if (bFinalized || Nodes.size() >= Limits.MaximumNodes) return false;
		Nodes.push_back(std::move(Node));
		return true;
	}

	auto FImportFactoryGraphBuilder::Finalize(
		FImportFactoryGraph& OutGraph,
		std::vector<FImportDiagnostic>& OutDiagnostics) -> bool
	{
		if (bFinalized)
		{
			AddDiagnostic(OutDiagnostics, Limits, EImportDiagnosticCategory::InvalidPlan,
				"InterchangeBuilderAlreadyFinalized", "Factory graph builder can be finalized only once.");
			return false;
		}
		bFinalized = true;
		if (Nodes.empty())
		{
			AddDiagnostic(OutDiagnostics, Limits, EImportDiagnosticCategory::InvalidPlan,
				"InterchangeGraphEmpty", "Factory graph must contain at least one node.");
			return false;
		}

		uint64 PayloadBytes = 0;
		for (FImportFactoryNode& Node : Nodes)
		{
			if (!IsStableIdentity(Node.StableIdentity) || !IsStableIdentity(Node.FactoryId)
				|| Node.FactoryContractVersion == 0 || Node.OutputClassName.empty()
				|| !Node.Destination.IsValid())
			{
				AddDiagnostic(OutDiagnostics, Limits, EImportDiagnosticCategory::InvalidPlan,
					"InterchangeFactoryNodeInvalid",
					"Factory nodes require stable identities, a versioned factory, output class, and destination.");
				return false;
			}
			std::string Error;
			if (!Node.Settings.Finalize(Error))
			{
				AddDiagnostic(OutDiagnostics, Limits, EImportDiagnosticCategory::InvalidPlan,
					"InterchangeFactorySettingsInvalid", std::move(Error));
				return false;
			}
			if (Node.Settings.Bytes.size() > Limits.MaximumPayloadBytes - PayloadBytes)
			{
				AddDiagnostic(OutDiagnostics, Limits, EImportDiagnosticCategory::ResourceLimitExceeded,
					"InterchangePayloadLimit", "Factory graph payload bytes exceed the configured limit.");
				return false;
			}
			PayloadBytes += Node.Settings.Bytes.size();
			CanonicalizeStrings(Node.TranslatedNodeReferences);
			CanonicalizeStrings(Node.FactoryDependencies);
			for (const std::string& Reference : Node.TranslatedNodeReferences)
			{
				if (!TranslatedGraph.FindNode(Reference))
				{
					AddDiagnostic(OutDiagnostics, Limits, EImportDiagnosticCategory::InvalidPlan,
						"InterchangeMissingTranslatedReference",
						std::format("Factory node '{}' references missing translated node '{}'.",
							Node.StableIdentity, Reference));
					return false;
				}
			}
		}
		std::ranges::sort(Nodes, {}, &FImportFactoryNode::StableIdentity);
		if (!ValidateDag(Nodes, [](const FImportFactoryNode& Node) -> const auto& {
			return Node.FactoryDependencies; }, Limits, OutDiagnostics)) return false;

		FXxHash128Builder Fingerprint;
		UpdateString(Fingerprint, "Durin.Interchange.FactoryGraph");
		Fingerprint.UpdateValue(InterchangeContractVersion);
		Fingerprint.UpdateValue(TranslatedGraph.GetFingerprint().HashLow);
		Fingerprint.UpdateValue(TranslatedGraph.GetFingerprint().HashHigh);
		for (const FImportFactoryNode& Node : Nodes)
		{
			UpdateString(Fingerprint, Node.StableIdentity);
			UpdateString(Fingerprint, Node.FactoryId);
			Fingerprint.UpdateValue(Node.FactoryContractVersion);
			UpdateString(Fingerprint, Node.OutputClassName);
			UpdateString(Fingerprint, Node.Destination.GetView());
			Fingerprint.UpdateValue(Node.Policy);
			UpdatePayload(Fingerprint, Node.Settings);
			for (const std::string& Reference : Node.TranslatedNodeReferences) UpdateString(Fingerprint, Reference);
			for (const std::string& Dependency : Node.FactoryDependencies) UpdateString(Fingerprint, Dependency);
		}
		OutGraph.Nodes = std::move(Nodes);
		OutGraph.Fingerprint = Fingerprint.Finalize();
		return true;
	}

	auto FInterchangeProvenance::Validate(std::string& OutError) const -> bool
	{
		if (SchemaVersion != InterchangeContractVersion)
		{
			OutError = std::format("InterchangeProvenanceVersionMismatch: expected version {} but received {}.",
				InterchangeContractVersion, SchemaVersion);
			return false;
		}
		if (!IsStableIdentity(Translator.Id) || Translator.ContractVersion == 0
			|| Sources.empty() || TranslatedGraphFingerprint.IsZero()
			|| FactoryGraphFingerprint.IsZero())
		{
			OutError = "InterchangeProvenanceIncomplete: translator, sources, and graph fingerprints are required.";
			return false;
		}
		for (const FInterchangePipelineStackEntry& Entry : PipelineStack)
		{
			if (!IsStableIdentity(Entry.PipelineId) || Entry.ContractVersion == 0)
			{
				OutError = "InterchangePipelineStackInvalid: entries require stable IDs and non-zero versions.";
				return false;
			}
		}
		OutError.clear();
		return true;
	}

	auto ExecuteInterchangePipelineStack(
		const FTranslatedAssetGraph& TranslatedGraph,
		std::span<const FInterchangePipelineStackEntry> PipelineStack,
		const FInterchangeGraphLimits& Limits)
		-> FInterchangePipelineExecutionResult
	{
		FInterchangePipelineExecutionResult Result;
		if (PipelineStack.empty())
		{
			Result.Diagnostics.push_back({
				.Category = EImportDiagnosticCategory::InvalidPlan,
				.Identity = "InterchangePipelineStackEmpty",
				.Phase = "Pipeline",
				.Message = "Interchange pipeline stack must contain at least one entry."});
			return Result;
		}
		auto& Service = GetImportService();
		Result.RegistryRevision = Service.GetInterchangeRevision();
		std::optional<FImportFactoryGraph> Previous;
		for (const FInterchangePipelineStackEntry& Entry : PipelineStack)
		{
			FInterchangeComponentLease Lease = Service.FindInterchangeComponent(
				EInterchangeComponentRole::Pipeline, Entry.PipelineId, Entry.ContractVersion);
			if (!Lease)
			{
				Result.Diagnostics.push_back({
					.Category = EImportDiagnosticCategory::ProviderUnavailable,
					.Identity = "InterchangePipelineUnavailable",
					.Phase = "Pipeline",
					.Message = std::format("Pipeline '{}' version {} is unavailable.",
						Entry.PipelineId, Entry.ContractVersion)});
				return Result;
			}
			auto Invocation = Lease.TryEnter();
			if (!Invocation || !Lease.GetPipeline())
			{
				Result.Diagnostics.push_back({
					.Category = EImportDiagnosticCategory::ProviderUnavailable,
					.Identity = "InterchangePipelineRetired",
					.Phase = "Pipeline",
					.Message = std::format("Pipeline '{}' retired before execution.", Entry.PipelineId)});
				return Result;
			}
			if (!Lease.GetSettingsSchemaId().empty()
				&& (Entry.Settings.SchemaId != Lease.GetSettingsSchemaId()
					|| Entry.Settings.SchemaVersion != Lease.GetSettingsSchemaVersion()))
			{
				Result.Diagnostics.push_back({
					.Category = EImportDiagnosticCategory::InvalidPlan,
					.Identity = "InterchangePipelineSettingsMigrationRequired",
					.Phase = "Pipeline",
					.Message = std::format(
						"Pipeline '{}' requires settings schema '{}' version {}; recorded settings are '{}' version {}.",
						Entry.PipelineId, Lease.GetSettingsSchemaId(),
						Lease.GetSettingsSchemaVersion(), Entry.Settings.SchemaId,
						Entry.Settings.SchemaVersion)});
				return Result;
			}
			FImportFactoryGraphBuilder Builder(TranslatedGraph, Limits);
			if (!Lease.GetPipeline()->Execute(TranslatedGraph,
				Previous ? &*Previous : nullptr, Entry.Settings, Builder, Result.Diagnostics))
			{
				if (Result.Diagnostics.empty())
					Result.Diagnostics.push_back({
						.Category = EImportDiagnosticCategory::ProviderFailure,
						.Identity = "InterchangePipelineRejected",
						.Phase = "Pipeline",
						.Message = std::format("Pipeline '{}' rejected the graph.", Entry.PipelineId)});
				return Result;
			}
			FImportFactoryGraph Next;
			if (!Builder.Finalize(Next, Result.Diagnostics)) return Result;
			Previous = std::move(Next);
		}
		if (Service.GetInterchangeRevision() != Result.RegistryRevision)
		{
			Result.Diagnostics.push_back({
				.Category = EImportDiagnosticCategory::StalePlan,
				.Identity = "InterchangeRegistryChanged",
				.Phase = "Pipeline",
				.Message = "Interchange registry changed while executing the pipeline stack."});
			return Result;
		}
		Result.Graph = std::move(*Previous);
		Result.bSucceeded = true;
		return Result;
	}

	namespace
	{
		class FInterchangeByteWriter
		{
		public:
			template<std::unsigned_integral TValue>
			auto Write(TValue Value) -> void
			{
				for (size_t Index = 0; Index < sizeof(TValue); ++Index)
					Bytes.push_back(static_cast<std::byte>((Value >> (Index * 8)) & 0xff));
			}

			auto WriteString(std::string_view Value) -> bool
			{
				if (Value.size() > std::numeric_limits<uint32>::max()) return false;
				Write(static_cast<uint32>(Value.size()));
				const auto Raw = std::as_bytes(std::span(Value));
				Bytes.insert(Bytes.end(), Raw.begin(), Raw.end());
				return true;
			}

			auto WritePayload(const FInterchangePayload& Payload) -> bool
			{
				if (!WriteString(Payload.SchemaId)) return false;
				Write(Payload.SchemaVersion);
				Write(static_cast<uint64>(Payload.Bytes.size()));
				Bytes.insert(Bytes.end(), Payload.Bytes.begin(), Payload.Bytes.end());
				Write(Payload.ContentHash.HashLow);
				Write(Payload.ContentHash.HashHigh);
				return true;
			}

			std::vector<std::byte> Bytes;
		};

		class FInterchangeByteReader
		{
		public:
			explicit FInterchangeByteReader(std::span<const std::byte> InBytes)
				: Bytes(InBytes) {}

			template<std::unsigned_integral TValue>
			auto Read(TValue& OutValue) -> bool
			{
				if (Offset > Bytes.size() || sizeof(TValue) > Bytes.size() - Offset)
					return false;
				OutValue = 0;
				for (size_t Index = 0; Index < sizeof(TValue); ++Index)
					OutValue |= static_cast<TValue>(std::to_integer<uint8>(Bytes[Offset++]))
						<< (Index * 8);
				return true;
			}

			auto ReadString(std::string& OutValue) -> bool
			{
				uint32 Size = 0;
				if (!Read(Size) || Size > 16ull * 1'024ull * 1'024ull
					|| Offset > Bytes.size() || Size > Bytes.size() - Offset) return false;
				OutValue.resize(Size);
				if (Size != 0)
					std::memcpy(OutValue.data(), Bytes.data() + Offset, Size);
				Offset += Size;
				return true;
			}

			auto ReadPayload(FInterchangePayload& OutPayload) -> bool
			{
				uint64 Size = 0;
				if (!ReadString(OutPayload.SchemaId) || !Read(OutPayload.SchemaVersion)
					|| !Read(Size) || Size > MaximumInterchangePayloadBytes
					|| Offset > Bytes.size() || Size > Bytes.size() - Offset) return false;
				OutPayload.Bytes.assign(Bytes.begin() + static_cast<ptrdiff_t>(Offset),
					Bytes.begin() + static_cast<ptrdiff_t>(Offset + Size));
				Offset += static_cast<size_t>(Size);
				if (!Read(OutPayload.ContentHash.HashLow)
					|| !Read(OutPayload.ContentHash.HashHigh)) return false;
				if (OutPayload.SchemaId.empty())
					return OutPayload.SchemaVersion == 0 && OutPayload.Bytes.empty()
						&& OutPayload.ContentHash.IsZero();
				return OutPayload.SchemaVersion != 0
					&& OutPayload.ContentHash == FXxHash128::HashBuffer(
						std::span<const std::byte>(OutPayload.Bytes));
			}

			auto IsAtEnd() const -> bool { return Offset == Bytes.size(); }

		private:
			std::span<const std::byte> Bytes;
			size_t Offset = 0;
		};

		template<typename TValue, typename TWriter>
		auto WriteVector(FInterchangeByteWriter& Writer,
			const std::vector<TValue>& Values, TWriter&& WriteValue) -> bool
		{
			if (Values.size() > std::numeric_limits<uint32>::max()) return false;
			Writer.Write(static_cast<uint32>(Values.size()));
			for (const TValue& Value : Values)
				if (!WriteValue(Value)) return false;
			return true;
		}

		template<typename TValue, typename TReader>
		auto ReadVector(FInterchangeByteReader& Reader,
			std::vector<TValue>& Values, uint32 Maximum, TReader&& ReadValue) -> bool
		{
			uint32 Count = 0;
			if (!Reader.Read(Count) || Count > Maximum) return false;
			Values.resize(Count);
			for (TValue& Value : Values)
				if (!ReadValue(Value)) return false;
			return true;
		}
	}

	auto SerializeInterchangeProvenance(
		const FInterchangeProvenance& Provenance,
		std::vector<std::byte>& OutBytes,
		std::string& OutError) -> bool
	{
		if (!Provenance.Validate(OutError)) return false;
		FInterchangeByteWriter Writer;
		Writer.Write(uint32{0x50544e49});
		Writer.Write(Provenance.SchemaVersion);
		if (!Writer.WriteString(Provenance.Translator.Id)) return false;
		Writer.Write(Provenance.Translator.ContractVersion);
		if (!Writer.WritePayload(Provenance.Translator.Settings)
			|| !WriteVector(Writer, Provenance.PipelineStack,
				[&](const FInterchangePipelineStackEntry& Entry) {
					if (!Writer.WriteString(Entry.PipelineId)) return false;
					Writer.Write(Entry.ContractVersion);
					return Writer.WritePayload(Entry.Settings);
				})
			|| !WriteVector(Writer, Provenance.Sources,
				[&](const FInterchangeProvenance::FSourceProvenance& Source) {
					if (!Writer.WriteString(Source.StableIdentity)
						|| !Writer.WriteString(Source.Role)
						|| !Writer.WriteString(Source.SourcePath.Path)) return false;
					Writer.Write(Source.ContentHash.HashLow);
					Writer.Write(Source.ContentHash.HashHigh);
					Writer.Write(Source.ByteCount);
					return true;
				})
			|| !WriteVector(Writer, Provenance.OutputMappings,
				[&](const FInterchangeOutputMapping& Mapping) {
					return Writer.WriteString(Mapping.TranslatedNodeIdentity)
						&& Writer.WriteString(Mapping.OutputIdentity)
						&& Writer.WriteString(Mapping.AssetPath.GetView());
				}))
		{
			OutError = "InterchangeProvenanceSerializeLimit: a provenance collection or string exceeds its limit.";
			return false;
		}
		Writer.Write(Provenance.TranslatedGraphFingerprint.HashLow);
		Writer.Write(Provenance.TranslatedGraphFingerprint.HashHigh);
		Writer.Write(Provenance.FactoryGraphFingerprint.HashLow);
		Writer.Write(Provenance.FactoryGraphFingerprint.HashHigh);
		if (!Writer.WriteString(Provenance.AuthoredOutputFingerprint))
		{
			OutError = "InterchangeProvenanceSerializeLimit: authored fingerprint exceeds its limit.";
			return false;
		}
		OutBytes = std::move(Writer.Bytes);
		OutError.clear();
		return true;
	}

	auto DeserializeInterchangeProvenance(
		std::span<const std::byte> Bytes,
		FInterchangeProvenance& OutProvenance,
		std::string& OutError) -> bool
	{
		FInterchangeByteReader Reader(Bytes);
		FInterchangeProvenance Value;
		uint32 Magic = 0;
		if (!Reader.Read(Magic) || Magic != 0x50544e49
			|| !Reader.Read(Value.SchemaVersion)
			|| !Reader.ReadString(Value.Translator.Id)
			|| !Reader.Read(Value.Translator.ContractVersion)
			|| !Reader.ReadPayload(Value.Translator.Settings)
			|| !ReadVector(Reader, Value.PipelineStack, 4'096,
				[&](FInterchangePipelineStackEntry& Entry) {
					return Reader.ReadString(Entry.PipelineId)
						&& Reader.Read(Entry.ContractVersion)
						&& Reader.ReadPayload(Entry.Settings);
				})
			|| !ReadVector(Reader, Value.Sources, 8'192,
				[&](FInterchangeProvenance::FSourceProvenance& Source) {
					return Reader.ReadString(Source.StableIdentity)
						&& Reader.ReadString(Source.Role)
						&& Reader.ReadString(Source.SourcePath.Path)
						&& Reader.Read(Source.ContentHash.HashLow)
						&& Reader.Read(Source.ContentHash.HashHigh)
						&& Reader.Read(Source.ByteCount);
				})
			|| !ReadVector(Reader, Value.OutputMappings, MaximumInterchangeGraphNodes,
				[&](FInterchangeOutputMapping& Mapping) {
					std::string Path;
					return Reader.ReadString(Mapping.TranslatedNodeIdentity)
						&& Reader.ReadString(Mapping.OutputIdentity)
						&& Reader.ReadString(Path)
						&& FAssetPath::TryCreate(Path, Mapping.AssetPath);
				})
			|| !Reader.Read(Value.TranslatedGraphFingerprint.HashLow)
			|| !Reader.Read(Value.TranslatedGraphFingerprint.HashHigh)
			|| !Reader.Read(Value.FactoryGraphFingerprint.HashLow)
			|| !Reader.Read(Value.FactoryGraphFingerprint.HashHigh)
			|| !Reader.ReadString(Value.AuthoredOutputFingerprint)
			|| !Reader.IsAtEnd())
		{
			OutError = "InterchangeProvenanceMalformed: bytes are truncated, excessive, non-canonical, or invalid.";
			return false;
		}
		if (!Value.Validate(OutError)) return false;
		OutProvenance = std::move(Value);
		OutError.clear();
		return true;
	}
}
