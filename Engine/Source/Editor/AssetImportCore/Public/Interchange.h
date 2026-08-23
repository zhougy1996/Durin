#pragma once

#include "AssetImportCore.h"
#include "AsyncImport.h"

namespace Durin::Asset
{
	class FImportService;
	struct FInterchangeProvenance;
	inline constexpr uint32 InterchangeContractVersion = 1;
	inline constexpr uint32 MaximumInterchangeGraphNodes = 1'000'000;
	inline constexpr uint32 MaximumInterchangeGraphDependencies = 4'000'000;
	inline constexpr uint32 MaximumInterchangeDiagnostics = 4'096;
	inline constexpr uint64 MaximumInterchangePayloadBytes = 16ull * 1'024ull * 1'024ull * 1'024ull;

	// Bounds graph construction before provider-controlled values can allocate
	// unbounded framework storage.
	struct FInterchangeGraphLimits
	{
		uint32 MaximumNodes = MaximumInterchangeGraphNodes;
		uint32 MaximumDependencies = MaximumInterchangeGraphDependencies;
		uint32 MaximumDiagnostics = MaximumInterchangeDiagnostics;
		uint64 MaximumPayloadBytes = MaximumInterchangePayloadBytes;
	};

	// Owns one versioned payload. Persistent and cross-stage values use this
	// schema boundary instead of type-erased provider pointers.
	struct FInterchangePayload
	{
		std::string SchemaId;
		uint32 SchemaVersion = 0;
		std::vector<std::byte> Bytes;
		FXxHash128 ContentHash{};

		ASSETIMPORTCORE_API auto Finalize(std::string& OutError) -> bool;
		auto operator==(const FInterchangePayload&) const -> bool = default;
	};

	template<typename TValue>
	concept CInterchangePayloadValue = requires(
		std::span<const std::byte> Bytes, TValue& Value, std::string& Error)
	{
		{ TValue::InterchangeSchemaId } -> std::convertible_to<std::string_view>;
		{ TValue::InterchangeSchemaVersion } -> std::convertible_to<uint32>;
		{ TValue::DecodeInterchangePayload(Bytes, Value, Error) } -> std::same_as<bool>;
	};

	// Decodes only after exact schema identity and version validation.
	template<CInterchangePayloadValue TValue>
	auto DecodeInterchangePayload(
		const FInterchangePayload& Payload, TValue& OutValue, std::string& OutError) -> bool
	{
		if (Payload.SchemaId != std::string_view(TValue::InterchangeSchemaId))
		{
			OutError = std::format("InterchangeSchemaMismatch: expected schema '{}' but received '{}'.",
				std::string_view(TValue::InterchangeSchemaId), Payload.SchemaId);
			return false;
		}
		if (Payload.SchemaVersion != static_cast<uint32>(TValue::InterchangeSchemaVersion))
		{
			OutError = std::format("InterchangeSchemaVersionMismatch: schema '{}' expects version {} but received {}.",
				Payload.SchemaId, static_cast<uint32>(TValue::InterchangeSchemaVersion), Payload.SchemaVersion);
			return false;
		}
		if (Payload.ContentHash != FXxHash128::HashBuffer(std::span<const std::byte>(Payload.Bytes)))
		{
			OutError = std::format("InterchangePayloadHashMismatch: schema '{}' payload bytes do not match the recorded hash.",
				Payload.SchemaId);
			return false;
		}
		return TValue::DecodeInterchangePayload(Payload.Bytes, OutValue, OutError);
	}

	// One source-semantic node emitted by a translator. Dependencies refer to
	// other translated-node identities and are independent of storage order.
	struct FTranslatedAssetNode
	{
		std::string StableIdentity;
		std::string NodeKind;
		FInterchangePayload Payload;
		std::vector<std::string> SourceIdentities;
		std::vector<std::string> Dependencies;

		auto operator==(const FTranslatedAssetNode&) const -> bool = default;
	};

	// Immutable, canonical translator output safe for worker use and persistence.
	class FTranslatedAssetGraph
	{
	public:
		auto GetNodes() const -> std::span<const FTranslatedAssetNode> { return Nodes; }
		auto GetFingerprint() const -> const FXxHash128& { return Fingerprint; }
		ASSETIMPORTCORE_API auto FindNode(std::string_view StableIdentity) const
			-> const FTranslatedAssetNode*;

	private:
		std::vector<FTranslatedAssetNode> Nodes;
		FXxHash128 Fingerprint{};

		friend class FTranslatedAssetGraphBuilder;
	};

	// Validates and finalizes translator output into one immutable graph.
	class FTranslatedAssetGraphBuilder
	{
	public:
		explicit FTranslatedAssetGraphBuilder(FInterchangeGraphLimits InLimits = {})
			: Limits(InLimits) {}

		ASSETIMPORTCORE_API auto AddNode(FTranslatedAssetNode Node) -> bool;
		ASSETIMPORTCORE_API auto Finalize(
			FTranslatedAssetGraph& OutGraph,
			std::vector<FImportDiagnostic>& OutDiagnostics) -> bool;

	private:
		FInterchangeGraphLimits Limits;
		std::vector<FTranslatedAssetNode> Nodes;
		bool bFinalized = false;
	};

	// One planned authored output. Factory dependencies define construction and
	// publication order; translated references select immutable source values.
	struct FImportFactoryNode
	{
		std::string StableIdentity;
		std::string FactoryId;
		uint32 FactoryContractVersion = 0;
		std::string OutputClassName;
		FAssetPath Destination;
		EImportOutputPolicy Policy = EImportOutputPolicy::Create;
		FInterchangePayload Settings;
		std::vector<std::string> TranslatedNodeReferences;
		std::vector<std::string> FactoryDependencies;

		auto operator==(const FImportFactoryNode&) const -> bool = default;
	};

	// Immutable, canonical pipeline output consumed by typed factories.
	class FImportFactoryGraph
	{
	public:
		auto GetNodes() const -> std::span<const FImportFactoryNode> { return Nodes; }
		auto GetFingerprint() const -> const FXxHash128& { return Fingerprint; }
		ASSETIMPORTCORE_API auto FindNode(std::string_view StableIdentity) const
			-> const FImportFactoryNode*;

	private:
		std::vector<FImportFactoryNode> Nodes;
		FXxHash128 Fingerprint{};

		friend class FImportFactoryGraphBuilder;
	};

	// Validates output destinations, translated references and factory DAG edges
	// before detached product construction can start.
	class FImportFactoryGraphBuilder
	{
	public:
		explicit FImportFactoryGraphBuilder(
			const FTranslatedAssetGraph& InTranslatedGraph,
			FInterchangeGraphLimits InLimits = {})
			: TranslatedGraph(InTranslatedGraph), Limits(InLimits) {}

		ASSETIMPORTCORE_API auto AddNode(FImportFactoryNode Node) -> bool;
		ASSETIMPORTCORE_API auto Finalize(
			FImportFactoryGraph& OutGraph,
			std::vector<FImportDiagnostic>& OutDiagnostics) -> bool;

	private:
		const FTranslatedAssetGraph& TranslatedGraph;
		FInterchangeGraphLimits Limits;
		std::vector<FImportFactoryNode> Nodes;
		bool bFinalized = false;
	};

	enum class EInterchangeThreadCapability : uint8
	{
		WorkerSafe,
		EditorOnly
	};

	struct FInterchangeComponentIdentity
	{
		std::string Id;
		uint32 ContractVersion = 0;
		FInterchangePayload Settings;

		auto operator==(const FInterchangeComponentIdentity&) const -> bool = default;
	};

	struct FTranslatorDescriptor
	{
		FInterchangeComponentIdentity Identity;
		std::vector<std::string> Extensions;
		int32 Priority = 0;
		EInterchangeThreadCapability TranslationThread = EInterchangeThreadCapability::WorkerSafe;
	};

	struct FPipelineDescriptor
	{
		FInterchangeComponentIdentity Identity;
		int32 Priority = 0;
		EInterchangeThreadCapability ExecutionThread = EInterchangeThreadCapability::WorkerSafe;
	};

	struct FFactoryDescriptor
	{
		FInterchangeComponentIdentity Identity;
		std::string OutputClassName;
		int32 Priority = 0;
		EInterchangeThreadCapability ProductBuildThread = EInterchangeThreadCapability::WorkerSafe;
	};

	class IInterchangeTranslator
	{
	public:
		virtual ~IInterchangeTranslator() = default;
		virtual auto Recognize(const FImportSourceRecognition& Source) const -> bool = 0;
		virtual auto DiscoverDependencies(
			std::span<const FSourceSnapshotEntry> Sources,
			FDependencyRequestSink& Sink,
			std::vector<FImportDiagnostic>& OutDiagnostics) const -> bool = 0;
		virtual auto Translate(
			const FSourceSnapshot& Snapshot,
			const FInterchangePayload& Settings,
			FTranslatedAssetGraphBuilder& Builder,
			std::vector<FImportDiagnostic>& OutDiagnostics) const -> bool = 0;
	};

	class IInterchangePipeline
	{
	public:
		virtual ~IInterchangePipeline() = default;
		virtual auto Execute(
			const FTranslatedAssetGraph& TranslatedGraph,
			const FImportFactoryGraph* PreviousGraph,
			const FInterchangePayload& Settings,
			FImportFactoryGraphBuilder& Builder,
			std::vector<FImportDiagnostic>& OutDiagnostics) const -> bool = 0;
	};

	class IInterchangeFactoryProduct
	{
	public:
		virtual ~IInterchangeFactoryProduct() = default;
	};

	// Read-only view of the complete prospective authored graph. Factories use
	// it to bind relationships only after every candidate has been materialized.
	// ExistingTarget returns the currently published object for replacements;
	// ProspectiveObject returns the candidate that will be published.
	struct FInterchangeMaterializationContext
	{
		std::function<DObject*(std::string_view)> ExistingTarget;
		std::function<DObject*(std::string_view)> ProspectiveObject;
	};

	class IInterchangeFactory
	{
	public:
		virtual ~IInterchangeFactory() = default;
		virtual auto BuildDetachedProduct(
			const FImportFactoryNode& FactoryNode,
			const FTranslatedAssetGraph& TranslatedGraph,
			IImportProgressReporter* Progress,
			const std::function<bool()>& IsCancellationRequested,
			std::vector<FImportDiagnostic>& OutDiagnostics) const
			-> std::unique_ptr<IInterchangeFactoryProduct> = 0;
		virtual auto MaterializeCandidate(
			const FImportFactoryNode& FactoryNode,
			std::unique_ptr<IInterchangeFactoryProduct> Product,
			std::vector<FImportDiagnostic>& OutDiagnostics) const
			-> std::unique_ptr<ISingleAssetCandidate> = 0;
		virtual auto ResolveCandidateDependencies(
			const FImportFactoryNode&,
			ISingleAssetCandidate&,
			const FInterchangeMaterializationContext&,
			std::vector<FImportDiagnostic>&) const -> bool { return true; }
		virtual auto PrepareImportedStateExchange(
			DObject&,
			ISingleAssetCandidate&,
			std::vector<FImportDiagnostic>&) const
			-> std::unique_ptr<IPreparedImportedStateExchange> { return {}; }
		// Persists framework reproduction metadata after the reversible state
		// exchange and authored fingerprint are complete, but before atomic save.
		virtual auto ApplyProvenance(
			DObject&,
			const FInterchangeProvenance&,
			std::vector<FImportDiagnostic>&) const -> bool { return true; }
	};

	enum class EInterchangeComponentRole : uint8
	{
		Translator,
		Pipeline,
		Factory
	};

	struct FTranslatorRegistrationDescriptor
	{
		FTranslatorDescriptor Descriptor;
		std::shared_ptr<IInterchangeTranslator> Implementation;
	};

	struct FPipelineRegistrationDescriptor
	{
		FPipelineDescriptor Descriptor;
		std::shared_ptr<IInterchangePipeline> Implementation;
	};

	struct FFactoryRegistrationDescriptor
	{
		FFactoryDescriptor Descriptor;
		std::shared_ptr<IInterchangeFactory> Implementation;
	};

	struct FInterchangeComponentLeaseState;

	// Retains the implementation and its module resource until every escaped
	// factory product or invocation-owned value is destroyed.
	class FInterchangeComponentLease
	{
	public:
		ASSETIMPORTCORE_API FInterchangeComponentLease();
		ASSETIMPORTCORE_API ~FInterchangeComponentLease();
		ASSETIMPORTCORE_API FInterchangeComponentLease(const FInterchangeComponentLease&);
		ASSETIMPORTCORE_API FInterchangeComponentLease(FInterchangeComponentLease&&) noexcept;
		ASSETIMPORTCORE_API auto operator=(const FInterchangeComponentLease&)
			-> FInterchangeComponentLease&;
		ASSETIMPORTCORE_API auto operator=(FInterchangeComponentLease&&) noexcept
			-> FInterchangeComponentLease&;

		auto IsValid() const -> bool { return State != nullptr; }
		explicit operator bool() const { return IsValid(); }
		ASSETIMPORTCORE_API auto GetRole() const -> EInterchangeComponentRole;
		ASSETIMPORTCORE_API auto GetId() const -> std::string_view;
		ASSETIMPORTCORE_API auto GetContractVersion() const -> uint32;
		ASSETIMPORTCORE_API auto GetSettingsSchemaId() const -> std::string_view;
		ASSETIMPORTCORE_API auto GetSettingsSchemaVersion() const -> uint32;
		ASSETIMPORTCORE_API auto GetOutputClassName() const -> std::string_view;
		ASSETIMPORTCORE_API auto GetThreadCapability() const
			-> EInterchangeThreadCapability;
		ASSETIMPORTCORE_API auto GetTranslator() const -> const IInterchangeTranslator*;
		ASSETIMPORTCORE_API auto GetPipeline() const -> const IInterchangePipeline*;
		ASSETIMPORTCORE_API auto GetFactory() const -> const IInterchangeFactory*;
		ASSETIMPORTCORE_API auto TryEnter() const -> FModuleOwnedCallbackInvocation;

		// Registry infrastructure constructs leases only from an opaque state.
		explicit FInterchangeComponentLease(
			std::shared_ptr<const FInterchangeComponentLeaseState> InState,
			FModuleOwnedResourceLease InResource);

	private:
		std::shared_ptr<const FInterchangeComponentLeaseState> State;
		std::shared_ptr<FModuleOwnedResourceLease> ResourceLease;

		friend class FInterchangeRegistryStore;
	};

	// Owns one exact translator, pipeline, or factory registration identity.
	class FInterchangeRegistration final
	{
	public:
		FInterchangeRegistration() = default;
		ASSETIMPORTCORE_API ~FInterchangeRegistration();
		FInterchangeRegistration(const FInterchangeRegistration&) = delete;
		auto operator=(const FInterchangeRegistration&)
			-> FInterchangeRegistration& = delete;
		ASSETIMPORTCORE_API FInterchangeRegistration(
			FInterchangeRegistration&& Other) noexcept;
		ASSETIMPORTCORE_API auto operator=(FInterchangeRegistration&& Other) noexcept
			-> FInterchangeRegistration&;

		explicit operator bool() const { return Owner != nullptr; }
		ASSETIMPORTCORE_API auto Reset() -> bool;

	private:
		FInterchangeRegistration(
			FImportService& InOwner,
			std::weak_ptr<void> InOwnerLifetime,
			EInterchangeComponentRole InRole,
			std::string InId,
			uint64 InIdentity);

		FImportService* Owner = nullptr;
		std::weak_ptr<void> OwnerLifetime;
		EInterchangeComponentRole Role = EInterchangeComponentRole::Translator;
		std::string Id;
		uint64 Identity = 0;

		friend class FImportService;
	};

	struct FInterchangePipelineStackEntry;
	struct FInterchangeProvenance;

	struct FInterchangeSelectionResult
	{
		FInterchangeComponentLease Lease;
		std::vector<FImportDiagnostic> Diagnostics;

		explicit operator bool() const { return static_cast<bool>(Lease); }
	};

	struct FInterchangePipelineExecutionResult
	{
		bool bSucceeded = false;
		FImportFactoryGraph Graph;
		std::vector<FImportDiagnostic> Diagnostics;
		uint64 RegistryRevision = 0;

		explicit operator bool() const { return bSucceeded; }
	};

	ASSETIMPORTCORE_API auto ExecuteInterchangePipelineStack(
		const FTranslatedAssetGraph& TranslatedGraph,
		std::span<const FInterchangePipelineStackEntry> PipelineStack,
		const FInterchangeGraphLimits& Limits = {})
		-> FInterchangePipelineExecutionResult;
	ASSETIMPORTCORE_API auto SerializeInterchangeProvenance(
		const FInterchangeProvenance& Provenance,
		std::vector<std::byte>& OutBytes,
		std::string& OutError) -> bool;
	ASSETIMPORTCORE_API auto DeserializeInterchangeProvenance(
		std::span<const std::byte> Bytes,
		FInterchangeProvenance& OutProvenance,
		std::string& OutError) -> bool;

	enum class EInterchangeImportMode : uint8
	{
		Import,
		Preview,
		Reimport,
		ReplaceSource,
		Repair,
		Recover
	};

	struct FInterchangePipelineStackEntry
	{
		std::string PipelineId;
		uint32 ContractVersion = 0;
		FInterchangePayload Settings;

		auto operator==(const FInterchangePipelineStackEntry&) const -> bool = default;
	};

	struct FInterchangeOutputMapping
	{
		std::string TranslatedNodeIdentity;
		std::string OutputIdentity;
		FAssetPath AssetPath;

		auto operator==(const FInterchangeOutputMapping&) const -> bool = default;
	};

	// Framework-owned reproduction identity persisted by both single and
	// multi-output imports.
	struct FInterchangeProvenance
	{
		uint32 SchemaVersion = InterchangeContractVersion;
		FInterchangeComponentIdentity Translator;
		std::vector<FInterchangePipelineStackEntry> PipelineStack;
		std::vector<FSingleAssetSourceProvenance> Sources;
		std::vector<FInterchangeOutputMapping> OutputMappings;
		FXxHash128 TranslatedGraphFingerprint{};
		FXxHash128 FactoryGraphFingerprint{};
		std::string AuthoredOutputFingerprint;

		ASSETIMPORTCORE_API auto Validate(std::string& OutError) const -> bool;
		auto operator==(const FInterchangeProvenance&) const -> bool = default;
	};

	struct FInterchangeImportRequest
	{
		EInterchangeImportMode Mode = EInterchangeImportMode::Import;
		FSourcePath RootSource;
		std::string TranslatorId;
		FInterchangePayload TranslatorSettings;
		std::vector<FInterchangePipelineStackEntry> PipelineStack;
		FAssetPath Destination;
		FImportOperationOwner Owner;
		EImportOperationLifetime Lifetime = EImportOperationLifetime::EditorOperation;
		FSourceCaptureLimits SourceLimits;
		FInterchangeGraphLimits GraphLimits;
		Asset::FAssetBundleSaveOptions SaveOptions;
		std::optional<FInterchangeProvenance> ExistingProvenance;
	};

	struct FInterchangeInspection
	{
		bool bCompatible = false;
		std::vector<FImportDiagnostic> Diagnostics;
		std::vector<FImportSourcePreview> Sources;
		std::vector<FImportOutputPreview> Outputs;
		FXxHash128 TranslatedGraphFingerprint{};
		FXxHash128 FactoryGraphFingerprint{};
	};

	struct FInterchangeImportResult
	{
		FImportOutcome Outcome;
		FInterchangeProvenance Provenance;
		FInterchangeInspection Inspection;
	};
}
