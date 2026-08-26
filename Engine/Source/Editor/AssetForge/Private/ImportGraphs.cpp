#include "AssetForge/Extensions/PlanningPass.h"
#include "AssetForge/Persistence/ImportProvenance.h"
#include "AssetForge/ImportRequest.h"
#include "AssetForge/ImportService.h"
#include "CoreGlobals.h"
#include "Threading/RunnableThread.h"

#include <unordered_set>

namespace Durin::AssetForge
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
			const FGraphLimits& Limits,
			EImportDiagnosticCategory Category,
			std::string Identity,
			std::string Message) -> void
		{
			if (Diagnostics.size() >= Limits.MaximumDiagnostics) return;
			Diagnostics.push_back({
				.Severity = EImportDiagnosticSeverity::Error,
				.Category = Category,
				.Identity = std::move(Identity),
				.Phase = "AssetForgeGraph",
				.Message = std::move(Message)});
		}

		template<typename TNode, typename TDependencies>
		auto ValidateDag(
			const std::vector<TNode>& Nodes,
			TDependencies GetDependencies,
			const FGraphLimits& Limits,
			std::vector<FImportDiagnostic>& Diagnostics) -> bool
		{
			std::unordered_map<std::string_view, size_t> Indices;
			Indices.reserve(Nodes.size());
			for (size_t Index = 0; Index < Nodes.size(); ++Index)
			{
				if (!Indices.emplace(Nodes[Index].StableIdentity, Index).second)
				{
					AddDiagnostic(Diagnostics, Limits, EImportDiagnosticCategory::InvalidPlan,
						"Durin.AssetForge.Diagnostic.DuplicateIdentity",
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
							"Durin.AssetForge.Diagnostic.DependencyLimit",
							"AssetForge graph dependency count exceeds its configured limit.");
						return false;
					}
					const auto Found = Indices.find(Dependency);
					if (Found == Indices.end())
					{
						AddDiagnostic(Diagnostics, Limits, EImportDiagnosticCategory::InvalidPlan,
							"Durin.AssetForge.Diagnostic.MissingDependency",
							std::format("Node '{}' references missing dependency '{}'.",
								Nodes[Index].StableIdentity, Dependency));
						return false;
					}
					if (!Unique.emplace(Dependency).second)
					{
						AddDiagnostic(Diagnostics, Limits, EImportDiagnosticCategory::InvalidPlan,
							"Durin.AssetForge.Diagnostic.DuplicateDependency",
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
					"Durin.AssetForge.Diagnostic.DependencyCycle",
					"AssetForge graph dependencies contain a cycle.");
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

		auto UpdatePayload(FXxHash128Builder& Builder, const FSchemaPayload& Payload) -> void
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

	auto FSchemaPayload::Finalize(std::string& OutError) -> bool
	{
		if (!IsStableIdentity(SchemaId) || SchemaVersion == 0)
		{
			OutError = "Durin.AssetForge.Diagnostic.PayloadSchemaInvalid: schema identity and non-zero version are required.";
			return false;
		}
		ContentHash = FXxHash128::HashBuffer(std::span<const std::byte>(Bytes));
		OutError.clear();
		return true;
	}

	auto FSourceGraph::FindNode(std::string_view StableIdentity) const
		-> const FSourceNode*
	{
		const auto It = std::ranges::lower_bound(Nodes, StableIdentity, {}, &FSourceNode::StableIdentity);
		return It != Nodes.end() && It->StableIdentity == StableIdentity ? &*It : nullptr;
	}

	auto FSourceGraphBuilder::AddNode(FSourceNode Node) -> bool
	{
		if (bFinalized || Nodes.size() >= Limits.MaximumNodes) return false;
		Nodes.push_back(std::move(Node));
		return true;
	}

	auto FSourceGraphBuilder::Finalize(
		FSourceGraph& OutGraph,
		std::vector<FImportDiagnostic>& OutDiagnostics) -> bool
	{
		if (bFinalized)
		{
			AddDiagnostic(OutDiagnostics, Limits, EImportDiagnosticCategory::InvalidPlan,
				"Durin.AssetForge.Diagnostic.BuilderAlreadyFinalized", "Source graph builder can be finalized only once.");
			return false;
		}
		bFinalized = true;
		if (Nodes.empty())
		{
			AddDiagnostic(OutDiagnostics, Limits, EImportDiagnosticCategory::InvalidPlan,
				"AssetForgeGraphEmpty", "Translated graph must contain at least one node.");
			return false;
		}

		uint64 PayloadBytes = 0;
		for (FSourceNode& Node : Nodes)
		{
			if (!IsStableIdentity(Node.StableIdentity) || !IsStableIdentity(Node.NodeKind))
			{
				AddDiagnostic(OutDiagnostics, Limits, EImportDiagnosticCategory::InvalidPlan,
					"Durin.AssetForge.Diagnostic.NodeIdentityInvalid", "Source nodes require valid stable identity and kind values.");
				return false;
			}
			std::string Error;
			if (!Node.Payload.Finalize(Error))
			{
				AddDiagnostic(OutDiagnostics, Limits, EImportDiagnosticCategory::InvalidPlan,
					"Durin.AssetForge.Diagnostic.NodePayloadInvalid", std::move(Error));
				return false;
			}
			if (Node.Payload.Bytes.size() > Limits.MaximumPayloadBytes - PayloadBytes)
			{
				AddDiagnostic(OutDiagnostics, Limits, EImportDiagnosticCategory::ResourceLimitExceeded,
					"Durin.AssetForge.Diagnostic.PayloadLimit", "Source graph payload bytes exceed the configured limit.");
				return false;
			}
			PayloadBytes += Node.Payload.Bytes.size();
			CanonicalizeStrings(Node.SourceIdentities);
			CanonicalizeStrings(Node.Dependencies);
		}
		std::ranges::sort(Nodes, {}, &FSourceNode::StableIdentity);
		if (!ValidateDag(Nodes, [](const FSourceNode& Node) -> const auto& {
			return Node.Dependencies; }, Limits, OutDiagnostics)) return false;

		FXxHash128Builder Fingerprint;
		UpdateString(Fingerprint, "Durin.AssetForge.SourceGraph");
		Fingerprint.UpdateValue(AssetForgeContractVersion);
		for (const FSourceNode& Node : Nodes)
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

	auto FBuildGraph::FindNode(std::string_view StableIdentity) const
		-> const FBuildNode*
	{
		const auto It = std::ranges::lower_bound(Nodes, StableIdentity, {}, &FBuildNode::StableIdentity);
		return It != Nodes.end() && It->StableIdentity == StableIdentity ? &*It : nullptr;
	}

	auto FBuildGraphBuilder::AddNode(FBuildNode Node) -> bool
	{
		if (bFinalized || Nodes.size() >= Limits.MaximumNodes) return false;
		Nodes.push_back(std::move(Node));
		return true;
	}

	auto FBuildGraphBuilder::Finalize(
		FBuildGraph& OutGraph,
		std::vector<FImportDiagnostic>& OutDiagnostics) -> bool
	{
		if (bFinalized)
		{
			AddDiagnostic(OutDiagnostics, Limits, EImportDiagnosticCategory::InvalidPlan,
				"Durin.AssetForge.Diagnostic.BuilderAlreadyFinalized", "Build graph builder can be finalized only once.");
			return false;
		}
		bFinalized = true;
		if (Nodes.empty())
		{
			AddDiagnostic(OutDiagnostics, Limits, EImportDiagnosticCategory::InvalidPlan,
				"AssetForgeGraphEmpty", "AssetBuilder graph must contain at least one node.");
			return false;
		}

		uint64 PayloadBytes = 0;
		for (FBuildNode& Node : Nodes)
		{
			if (!IsStableIdentity(Node.StableIdentity) || !IsStableIdentity(Node.BuilderId)
				|| Node.BuilderContractVersion == 0 || Node.OutputClassName.empty()
				|| !Node.Destination.IsValid())
			{
				AddDiagnostic(OutDiagnostics, Limits, EImportDiagnosticCategory::InvalidPlan,
					"Durin.AssetForge.Diagnostic.BuilderNodeInvalid",
					"AssetBuilder nodes require stable identities, a versioned factory, output class, and destination.");
				return false;
			}
			std::string Error;
			if (!Node.Settings.Finalize(Error))
			{
				AddDiagnostic(OutDiagnostics, Limits, EImportDiagnosticCategory::InvalidPlan,
					"Durin.AssetForge.Diagnostic.BuilderSettingsInvalid", std::move(Error));
				return false;
			}
			if (Node.Settings.Bytes.size() > Limits.MaximumPayloadBytes - PayloadBytes)
			{
				AddDiagnostic(OutDiagnostics, Limits, EImportDiagnosticCategory::ResourceLimitExceeded,
					"Durin.AssetForge.Diagnostic.PayloadLimit", "Build graph payload bytes exceed the configured limit.");
				return false;
			}
			PayloadBytes += Node.Settings.Bytes.size();
			CanonicalizeStrings(Node.SourceNodeReferences);
			CanonicalizeStrings(Node.BuildDependencies);
			for (const std::string& Reference : Node.SourceNodeReferences)
			{
				if (!SourceGraph.FindNode(Reference))
				{
					AddDiagnostic(OutDiagnostics, Limits, EImportDiagnosticCategory::InvalidPlan,
						"Durin.AssetForge.Diagnostic.MissingSourceReference",
						std::format("AssetBuilder node '{}' references missing translated node '{}'.",
							Node.StableIdentity, Reference));
					return false;
				}
			}
		}
		std::ranges::sort(Nodes, {}, &FBuildNode::StableIdentity);
		if (!ValidateDag(Nodes, [](const FBuildNode& Node) -> const auto& {
			return Node.BuildDependencies; }, Limits, OutDiagnostics)) return false;

		FXxHash128Builder Fingerprint;
		UpdateString(Fingerprint, "Durin.AssetForge.BuildGraph");
		Fingerprint.UpdateValue(AssetForgeContractVersion);
		Fingerprint.UpdateValue(SourceGraph.GetFingerprint().HashLow);
		Fingerprint.UpdateValue(SourceGraph.GetFingerprint().HashHigh);
		for (const FBuildNode& Node : Nodes)
		{
			UpdateString(Fingerprint, Node.StableIdentity);
			UpdateString(Fingerprint, Node.BuilderId);
			Fingerprint.UpdateValue(Node.BuilderContractVersion);
			UpdateString(Fingerprint, Node.OutputClassName);
			UpdateString(Fingerprint, Node.Destination.GetView());
			Fingerprint.UpdateValue(Node.Policy);
			UpdatePayload(Fingerprint, Node.Settings);
			for (const std::string& Reference : Node.SourceNodeReferences) UpdateString(Fingerprint, Reference);
			for (const std::string& Dependency : Node.BuildDependencies) UpdateString(Fingerprint, Dependency);
		}
		OutGraph.Nodes = std::move(Nodes);
		OutGraph.Fingerprint = Fingerprint.Finalize();
		return true;
	}

	auto FImportProvenance::Validate(std::string& OutError) const -> bool
	{
		if (SchemaVersion != AssetForgeContractVersion)
		{
			OutError = std::format("ImportProvenanceVersionMismatch: expected version {} but received {}.",
				AssetForgeContractVersion, SchemaVersion);
			return false;
		}
		if (!IsStableIdentity(Translator.Id) || Translator.ContractVersion == 0
			|| Sources.empty() || SourceGraphFingerprint.IsZero()
			|| BuildGraphFingerprint.IsZero())
		{
			OutError = "ImportProvenanceIncomplete: translator, sources, and graph fingerprints are required.";
			return false;
		}
		for (const FPlanningPassStackEntry& Entry : PlanningPassStack)
		{
			if (!IsStableIdentity(Entry.PlanningPassId) || Entry.ContractVersion == 0)
			{
				OutError = "Durin.AssetForge.Diagnostic.PlanningPassStackInvalid: entries require stable IDs and non-zero versions.";
				return false;
			}
		}
		OutError.clear();
		return true;
	}

	auto ExecutePlanningPassStack(
		const FSourceGraph& SourceGraph,
		std::span<const FPlanningPassStackEntry> PlanningPassStack,
		const FGraphLimits& Limits)
		-> FPlanningPassExecutionResult
	{
		FPlanningPassExecutionResult Result;
		if (PlanningPassStack.empty())
		{
			Result.Diagnostics.push_back({
				.Category = EImportDiagnosticCategory::InvalidPlan,
				.Identity = "Durin.AssetForge.Diagnostic.PlanningPassStackEmpty",
				.Phase = "PlanningPass",
				.Message = "AssetForge pipeline stack must contain at least one entry."});
			return Result;
		}
		auto& Service = GetImportService();
		Result.RegistryRevision = Service.GetComponentRevision();
		std::optional<FBuildGraph> Previous;
		for (const FPlanningPassStackEntry& Entry : PlanningPassStack)
		{
			FComponentLease Lease = Service.FindComponent(
				EComponentRole::PlanningPass, Entry.PlanningPassId, Entry.ContractVersion);
			if (!Lease)
			{
				Result.Diagnostics.push_back({
					.Category = EImportDiagnosticCategory::ProviderUnavailable,
					.Identity = "Durin.AssetForge.Diagnostic.PlanningPassUnavailable",
					.Phase = "PlanningPass",
					.Message = std::format("PlanningPass '{}' version {} is unavailable.",
						Entry.PlanningPassId, Entry.ContractVersion)});
				return Result;
			}
			auto Invocation = Lease.TryEnter();
			if (!Invocation || !Lease.GetPlanningPass())
			{
				Result.Diagnostics.push_back({
					.Category = EImportDiagnosticCategory::ProviderUnavailable,
					.Identity = "Durin.AssetForge.Diagnostic.PlanningPassRetired",
					.Phase = "PlanningPass",
					.Message = std::format("PlanningPass '{}' retired before execution.", Entry.PlanningPassId)});
				return Result;
			}
			if (!Lease.GetSettingsSchemaId().empty()
				&& (Entry.Settings.SchemaId != Lease.GetSettingsSchemaId()
					|| Entry.Settings.SchemaVersion != Lease.GetSettingsSchemaVersion()))
			{
				Result.Diagnostics.push_back({
					.Category = EImportDiagnosticCategory::InvalidPlan,
					.Identity = "Durin.AssetForge.Diagnostic.PlanningPassSettingsMigrationRequired",
					.Phase = "PlanningPass",
					.Message = std::format(
						"PlanningPass '{}' requires settings schema '{}' version {}; recorded settings are '{}' version {}.",
						Entry.PlanningPassId, Lease.GetSettingsSchemaId(),
						Lease.GetSettingsSchemaVersion(), Entry.Settings.SchemaId,
						Entry.Settings.SchemaVersion)});
				return Result;
			}
			FBuildGraphBuilder Builder(SourceGraph, Limits);
			if (!Lease.GetPlanningPass()->Execute(SourceGraph,
				Previous ? &*Previous : nullptr, Entry.Settings, Builder, Result.Diagnostics))
			{
				if (Result.Diagnostics.empty())
					Result.Diagnostics.push_back({
						.Category = EImportDiagnosticCategory::ProviderFailure,
						.Identity = "Durin.AssetForge.Diagnostic.PlanningPassRejected",
						.Phase = "PlanningPass",
						.Message = std::format("PlanningPass '{}' rejected the graph.", Entry.PlanningPassId)});
				return Result;
			}
			FBuildGraph Next;
			if (!Builder.Finalize(Next, Result.Diagnostics)) return Result;
			Previous = std::move(Next);
		}
		if (Service.GetComponentRevision() != Result.RegistryRevision)
		{
			Result.Diagnostics.push_back({
				.Category = EImportDiagnosticCategory::StalePlan,
				.Identity = "Durin.AssetForge.Diagnostic.RegistryChanged",
				.Phase = "PlanningPass",
				.Message = "AssetForge registry changed while executing the pipeline stack."});
			return Result;
		}
		Result.Graph = std::move(*Previous);
		Result.bSucceeded = true;
		return Result;
	}

	namespace
	{
		class FSchemaByteWriter
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

			auto WritePayload(const FSchemaPayload& Payload) -> bool
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

		class FSchemaByteReader
		{
		public:
			explicit FSchemaByteReader(std::span<const std::byte> InBytes)
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

			auto ReadPayload(FSchemaPayload& OutPayload) -> bool
			{
				uint64 Size = 0;
				if (!ReadString(OutPayload.SchemaId) || !Read(OutPayload.SchemaVersion)
					|| !Read(Size) || Size > MaximumSchemaPayloadBytes
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
		auto WriteVector(FSchemaByteWriter& Writer,
			const std::vector<TValue>& Values, TWriter&& WriteValue) -> bool
		{
			if (Values.size() > std::numeric_limits<uint32>::max()) return false;
			Writer.Write(static_cast<uint32>(Values.size()));
			for (const TValue& Value : Values)
				if (!WriteValue(Value)) return false;
			return true;
		}

		template<typename TValue, typename TReader>
		auto ReadVector(FSchemaByteReader& Reader,
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

	auto SerializeImportProvenance(
		const FImportProvenance& Provenance,
		std::vector<std::byte>& OutBytes,
		std::string& OutError) -> bool
	{
		if (!Provenance.Validate(OutError)) return false;
		FSchemaByteWriter Writer;
		Writer.Write(uint32{0x50544e49});
		Writer.Write(Provenance.SchemaVersion);
		if (!Writer.WriteString(Provenance.Translator.Id)) return false;
		Writer.Write(Provenance.Translator.ContractVersion);
		if (!Writer.WritePayload(Provenance.Translator.Settings)
			|| !WriteVector(Writer, Provenance.PlanningPassStack,
				[&](const FPlanningPassStackEntry& Entry) {
					if (!Writer.WriteString(Entry.PlanningPassId)) return false;
					Writer.Write(Entry.ContractVersion);
					return Writer.WritePayload(Entry.Settings);
				})
			|| !WriteVector(Writer, Provenance.Sources,
				[&](const FImportProvenance::FSourceProvenance& Source) {
					if (!Writer.WriteString(Source.StableIdentity)
						|| !Writer.WriteString(Source.Role)
						|| !Writer.WriteString(Source.SourcePath.Path)) return false;
					Writer.Write(Source.ContentHash.HashLow);
					Writer.Write(Source.ContentHash.HashHigh);
					Writer.Write(Source.ByteCount);
					return true;
				})
			|| !WriteVector(Writer, Provenance.OutputMappings,
				[&](const FOutputMapping& Mapping) {
					return Writer.WriteString(Mapping.SourceNodeIdentity)
						&& Writer.WriteString(Mapping.OutputIdentity)
						&& Writer.WriteString(Mapping.AssetPath.GetView());
				}))
		{
			OutError = "ImportProvenanceSerializeLimit: a provenance collection or string exceeds its limit.";
			return false;
		}
		Writer.Write(Provenance.SourceGraphFingerprint.HashLow);
		Writer.Write(Provenance.SourceGraphFingerprint.HashHigh);
		Writer.Write(Provenance.BuildGraphFingerprint.HashLow);
		Writer.Write(Provenance.BuildGraphFingerprint.HashHigh);
		if (!Writer.WriteString(Provenance.AuthoredOutputFingerprint))
		{
			OutError = "ImportProvenanceSerializeLimit: authored fingerprint exceeds its limit.";
			return false;
		}
		OutBytes = std::move(Writer.Bytes);
		OutError.clear();
		return true;
	}

	auto DeserializeImportProvenance(
		std::span<const std::byte> Bytes,
		FImportProvenance& OutProvenance,
		std::string& OutError) -> bool
	{
		FSchemaByteReader Reader(Bytes);
		FImportProvenance Value;
		uint32 Magic = 0;
		if (!Reader.Read(Magic) || Magic != 0x50544e49
			|| !Reader.Read(Value.SchemaVersion)
			|| !Reader.ReadString(Value.Translator.Id)
			|| !Reader.Read(Value.Translator.ContractVersion)
			|| !Reader.ReadPayload(Value.Translator.Settings)
			|| !ReadVector(Reader, Value.PlanningPassStack, 4'096,
				[&](FPlanningPassStackEntry& Entry) {
					return Reader.ReadString(Entry.PlanningPassId)
						&& Reader.Read(Entry.ContractVersion)
						&& Reader.ReadPayload(Entry.Settings);
				})
			|| !ReadVector(Reader, Value.Sources, 8'192,
				[&](FImportProvenance::FSourceProvenance& Source) {
					return Reader.ReadString(Source.StableIdentity)
						&& Reader.ReadString(Source.Role)
						&& Reader.ReadString(Source.SourcePath.Path)
						&& Reader.Read(Source.ContentHash.HashLow)
						&& Reader.Read(Source.ContentHash.HashHigh)
						&& Reader.Read(Source.ByteCount);
				})
			|| !ReadVector(Reader, Value.OutputMappings, MaximumGraphNodes,
				[&](FOutputMapping& Mapping) {
					std::string Path;
					return Reader.ReadString(Mapping.SourceNodeIdentity)
						&& Reader.ReadString(Mapping.OutputIdentity)
						&& Reader.ReadString(Path)
						&& FAssetPath::TryCreate(Path, Mapping.AssetPath);
				})
			|| !Reader.Read(Value.SourceGraphFingerprint.HashLow)
			|| !Reader.Read(Value.SourceGraphFingerprint.HashHigh)
			|| !Reader.Read(Value.BuildGraphFingerprint.HashLow)
			|| !Reader.Read(Value.BuildGraphFingerprint.HashHigh)
			|| !Reader.ReadString(Value.AuthoredOutputFingerprint)
			|| !Reader.IsAtEnd())
		{
			OutError = "ImportProvenanceMalformed: bytes are truncated, excessive, non-canonical, or invalid.";
			return false;
		}
		if (!Value.Validate(OutError)) return false;
		OutProvenance = std::move(Value);
		OutError.clear();
		return true;
	}

	auto MakeAssetImportDataState(
		const FImportProvenance& Provenance,
		FAssetForgeImportState& OutState,
		std::string& OutError) -> bool
	{
		if (!Provenance.Validate(OutError)) return false;
		FAssetForgeImportState State;
		State.Translator.ComponentId = Provenance.Translator.Id;
		State.Translator.ContractVersion = Provenance.Translator.ContractVersion;
		const auto ConvertPayload = [&](const FSchemaPayload& Source,
			FAssetImportPayload& Destination) -> bool {
			if (Source.SchemaId.empty())
			{
				Destination = {};
				return Source.SchemaVersion == 0 && Source.Bytes.empty()
					&& Source.ContentHash.IsZero();
			}
			if (Source.ContentHash != FXxHash128::HashBuffer(
					std::span<const std::byte>(Source.Bytes)))
			{
				OutError = "AssetImportDataPayloadHashMismatch: AssetForge settings hash does not match its bytes.";
				return false;
			}
			return MakeAssetImportPayload(
				Source.SchemaId, Source.SchemaVersion, Source.Bytes,
				MaximumAssetImportSettingsBytes, Destination, OutError);
		};
		if (!ConvertPayload(Provenance.Translator.Settings, State.Translator.Settings))
			return false;
		State.PlanningPassStack.reserve(Provenance.PlanningPassStack.size());
		for (const FPlanningPassStackEntry& Source : Provenance.PlanningPassStack)
		{
			FAssetImportPlanningPassDescriptor Destination;
			Destination.PlanningPassId = Source.PlanningPassId;
			Destination.ContractVersion = Source.ContractVersion;
			if (!ConvertPayload(Source.Settings, Destination.Settings)) return false;
			State.PlanningPassStack.push_back(std::move(Destination));
		}
		State.SourceData.Sources.reserve(Provenance.Sources.size());
		for (const FImportProvenance::FSourceProvenance& Source : Provenance.Sources)
		{
			State.SourceData.Sources.push_back({
				.StableIdentity = Source.StableIdentity,
				.Role = Source.Role,
				.DisplayLabel = Source.StableIdentity,
				.SourcePath = Source.SourcePath,
				.ContentHashLow = Source.ContentHash.HashLow,
				.ContentHashHigh = Source.ContentHash.HashHigh,
				.ByteCount = Source.ByteCount,
				.LastWriteTime = Source.LastWriteTime});
		}
		State.SourceData.Normalize();
		for (const AssetImport::FSourceFile& Source : State.SourceData.Sources)
			State.SourceReferences.push_back({Source.StableIdentity});
		State.OutputMappings.reserve(Provenance.OutputMappings.size());
		for (const FOutputMapping& Mapping : Provenance.OutputMappings)
			State.OutputMappings.push_back({
				.SourceNodeIdentity = Mapping.SourceNodeIdentity,
				.OutputIdentity = Mapping.OutputIdentity,
				.AssetPathText = Mapping.AssetPath.ToString()});
		State.SourceGraphFingerprintLow = Provenance.SourceGraphFingerprint.HashLow;
		State.SourceGraphFingerprintHigh = Provenance.SourceGraphFingerprint.HashHigh;
		State.BuildGraphFingerprintLow = Provenance.BuildGraphFingerprint.HashLow;
		State.BuildGraphFingerprintHigh = Provenance.BuildGraphFingerprint.HashHigh;
		State.AuthoredOutputFingerprint = Provenance.AuthoredOutputFingerprint;
		if (!ValidateAssetImportDataState(State, OutError)) return false;
		OutState = std::move(State);
		OutError.clear();
		return true;
	}

	auto MakeImportProvenance(
		const FAssetForgeImportState& State,
		FImportProvenance& OutProvenance,
		std::string& OutError) -> bool
	{
		if (!ValidateAssetImportDataState(State, OutError)) return false;
		FImportProvenance Provenance;
		Provenance.SchemaVersion = AssetForgeContractVersion;
		const auto ConvertPayload = [](const FAssetImportPayload& Source) {
			return FSchemaPayload{
				.SchemaId = Source.SchemaId,
				.SchemaVersion = Source.SchemaVersion,
				.Bytes = Source.Bytes,
				.ContentHash = {Source.ContentHashLow, Source.ContentHashHigh}};
		};
		Provenance.Translator = {
			.Id = State.Translator.ComponentId,
			.ContractVersion = State.Translator.ContractVersion,
			.Settings = ConvertPayload(State.Translator.Settings)};
		Provenance.PlanningPassStack.reserve(State.PlanningPassStack.size());
		for (const FAssetImportPlanningPassDescriptor& Source : State.PlanningPassStack)
			Provenance.PlanningPassStack.push_back({
				.PlanningPassId = Source.PlanningPassId,
				.ContractVersion = Source.ContractVersion,
				.Settings = ConvertPayload(Source.Settings)});
		Provenance.Sources.reserve(State.SourceData.Sources.size());
		for (const AssetImport::FSourceFile& Source : State.SourceData.Sources)
			Provenance.Sources.push_back({
				.StableIdentity = Source.StableIdentity,
				.Role = Source.Role,
				.SourcePath = Source.SourcePath,
				.ContentHash = {Source.ContentHashLow, Source.ContentHashHigh},
				.ByteCount = Source.ByteCount,
				.LastWriteTime = Source.LastWriteTime});
		Provenance.OutputMappings.reserve(State.OutputMappings.size());
		for (const FAssetImportOutputMapping& Source : State.OutputMappings)
		{
			FAssetPath Path;
			if (!FAssetPath::TryCreate(Source.AssetPathText, Path, &OutError)) return false;
			Provenance.OutputMappings.push_back({
				.SourceNodeIdentity = Source.SourceNodeIdentity,
				.OutputIdentity = Source.OutputIdentity,
				.AssetPath = std::move(Path)});
		}
		Provenance.SourceGraphFingerprint = {
			State.SourceGraphFingerprintLow, State.SourceGraphFingerprintHigh};
		Provenance.BuildGraphFingerprint = {
			State.BuildGraphFingerprintLow, State.BuildGraphFingerprintHigh};
		Provenance.AuthoredOutputFingerprint = State.AuthoredOutputFingerprint;
		if (!Provenance.Validate(OutError)) return false;
		OutProvenance = std::move(Provenance);
		OutError.clear();
		return true;
	}

	auto DecodeLegacyImportProvenanceState(
		std::span<const std::byte> Bytes,
		FAssetForgeImportState& OutState,
		std::string& OutError) -> bool
	{
		FImportProvenance Provenance;
		return DeserializeImportProvenance(Bytes, Provenance, OutError)
			&& MakeAssetImportDataState(Provenance, OutState, OutError);
	}

	auto CreateAssetImportData(
		const FImportProvenance& Provenance,
		DObject& Owner,
		FName Name,
		DAssetForgeImportData*& OutImportData,
		std::string& OutError) -> bool
	{
		OutImportData = nullptr;
		if (!GIsGameThreadIdInitialized || !IsInGameThread())
		{
			OutError = "Asset import data objects may only be created on the game thread.";
			return false;
		}
		FAssetForgeImportState State;
		if (!MakeAssetImportDataState(Provenance, State, OutError)) return false;
		auto* Value = NewObject<DAssetForgeImportData>(
			&Owner, Name);
		if (!Value || !Value->SetState(std::move(State), OutError)) return false;
		OutImportData = Value;
		OutError.clear();
		return true;
	}

	auto ApplyAssetImportDataStateToRequest(
		const FAssetForgeImportState& State,
		FImportRequest& OutRequest,
		std::string& OutError) -> bool
	{
		FImportProvenance Provenance;
		if (!MakeImportProvenance(State, Provenance, OutError)) return false;
		const AssetImport::FSourceFile* Root =
			State.SourceData.FindByStableIdentity("root");
		if (!Root)
		{
			OutError = "Asset import replay state has no stable 'root' source.";
			return false;
		}
		FAssetPath Destination;
		if (State.OutputMappings.size() != 1)
		{
			OutError = "Standalone asset replay requires exactly one output mapping.";
			return false;
		}
		if (!FAssetPath::TryCreate(
				State.OutputMappings.front().AssetPathText, Destination, &OutError)) return false;
		FImportRequest Request = OutRequest;
		Request.RootSource = Root->SourcePath;
		Request.DeclaredSources.clear();
		for (const AssetImport::FSourceFile& Source : State.SourceData.Sources)
			if (Source.StableIdentity != "root")
				Request.DeclaredSources.push_back({
					.StableIdentity = Source.StableIdentity,
					.Role = Source.Role,
					.SourcePath = Source.SourcePath});
		Request.TranslatorId = Provenance.Translator.Id;
		Request.TranslatorSettings = Provenance.Translator.Settings;
		Request.PlanningPassStack = Provenance.PlanningPassStack;
		Request.Destination = std::move(Destination);
		Request.ExistingProvenance = std::move(Provenance);
		OutRequest = std::move(Request);
		OutError.clear();
		return true;
	}
}
