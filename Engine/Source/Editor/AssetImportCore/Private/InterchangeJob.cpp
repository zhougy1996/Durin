#include "InterchangeJob.h"

#include "AssetTools.h"
#include "ImportRecordIndex.h"
#include "ImportService.h"

namespace Durin::Asset
{
	struct FInterchangeImportResultState
	{
		mutable std::mutex Mutex;
		std::optional<FInterchangeImportResult> Result;
		FInterchangeImportCompletion Completion;
		bool bCompletionDelivered = false;
	};

	namespace
	{
		auto HasError(std::span<const FImportDiagnostic> Diagnostics) -> bool
		{
			return std::ranges::any_of(Diagnostics, [](const FImportDiagnostic& Diagnostic) {
				return Diagnostic.Severity == EImportDiagnosticSeverity::Error;
			});
		}

		auto FailureOutcome(std::vector<FImportDiagnostic> Diagnostics,
			std::string Message, bool bCanceled = false) -> FImportOutcome
		{
			FinalizeImportDiagnostics(Diagnostics, "interchange-job");
			return {
				.State = bCanceled ? EImportOperationState::Canceled
					: EImportOperationState::Failed,
				.Diagnostics = std::move(Diagnostics),
				.Diagnostic = std::move(Message)};
		}

		auto TopologicalFactoryLevels(const FImportFactoryGraph& Graph)
			-> std::vector<std::vector<size_t>>
		{
			const auto Nodes = Graph.GetNodes();
			std::unordered_map<std::string_view, size_t> Indices;
			for (size_t Index = 0; Index < Nodes.size(); ++Index)
				Indices.emplace(Nodes[Index].StableIdentity, Index);
			std::vector<uint32> InDegree(Nodes.size());
			std::vector<std::vector<size_t>> Dependents(Nodes.size());
			for (size_t Index = 0; Index < Nodes.size(); ++Index)
				for (const std::string& Dependency : Nodes[Index].FactoryDependencies)
				{
					++InDegree[Index];
					Dependents[Indices.at(Dependency)].push_back(Index);
				}
			std::vector<size_t> Ready;
			for (size_t Index = 0; Index < Nodes.size(); ++Index)
				if (InDegree[Index] == 0) Ready.push_back(Index);
			std::vector<std::vector<size_t>> Result;
			while (!Ready.empty())
			{
				Result.push_back(Ready);
				std::vector<size_t> Next;
				for (const size_t Index : Ready)
					for (const size_t Dependent : Dependents[Index])
						if (--InDegree[Dependent] == 0) Next.push_back(Dependent);
				std::ranges::sort(Next);
				Ready = std::move(Next);
			}
			return Result;
		}

		struct FInterchangeGraphJobValue final : IImportJobValue
		{
			std::shared_ptr<const FSourceSnapshot> Snapshot;
			FTranslatedAssetGraph TranslatedGraph;
			FImportFactoryGraph FactoryGraph;
			FInterchangeProvenance Provenance;
			FInterchangeInspection Inspection;
			uint64 RegistryRevision = 0;
		};

		auto CloneGraphs(const FInterchangeGraphJobValue& Source)
			-> std::unique_ptr<FInterchangeGraphJobValue>
		{
			auto Result = std::make_unique<FInterchangeGraphJobValue>();
			Result->Snapshot = Source.Snapshot;
			Result->TranslatedGraph = Source.TranslatedGraph;
			Result->FactoryGraph = Source.FactoryGraph;
			Result->Provenance = Source.Provenance;
			Result->Inspection = Source.Inspection;
			Result->RegistryRevision = Source.RegistryRevision;
			return Result;
		}

		auto BuildPreviewReuseFingerprint(
			const FInterchangeImportRequest& Request,
			const FSourceSnapshot& Snapshot,
			const FInterchangeComponentLease& Translator,
			uint64 RegistryRevision) -> FXxHash128
		{
			FXxHash128Builder Builder;
			auto AddString = [&](std::string_view Value) {
				const uint64 Size = Value.size();
				Builder.UpdateValue(Size);
				Builder.Update(Value);
			};
			AddString("Durin.Interchange.PreviewReuse");
			Builder.UpdateValue(InterchangeContractVersion);
			Builder.UpdateValue(RegistryRevision);
			Builder.UpdateValue(Asset::GetAssetCatalogRevision());
			AddString(Translator.GetId());
			Builder.UpdateValue(Translator.GetContractVersion());
			AddString(Request.TranslatorSettings.SchemaId);
			Builder.UpdateValue(Request.TranslatorSettings.SchemaVersion);
			const FXxHash128 TranslatorSettingsHash = FXxHash128::HashBuffer(
				std::span<const std::byte>(Request.TranslatorSettings.Bytes));
			Builder.UpdateValue(TranslatorSettingsHash.HashLow);
			Builder.UpdateValue(TranslatorSettingsHash.HashHigh);
			for (const FInterchangePipelineStackEntry& Pipeline : Request.PipelineStack)
			{
				AddString(Pipeline.PipelineId);
				Builder.UpdateValue(Pipeline.ContractVersion);
				AddString(Pipeline.Settings.SchemaId);
				Builder.UpdateValue(Pipeline.Settings.SchemaVersion);
				const FXxHash128 SettingsHash = FXxHash128::HashBuffer(
					std::span<const std::byte>(Pipeline.Settings.Bytes));
				Builder.UpdateValue(SettingsHash.HashLow);
				Builder.UpdateValue(SettingsHash.HashHigh);
			}
			for (const FSourceSnapshotEntry& Source : Snapshot.GetSources())
			{
				AddString(Source.StableIdentity);
				AddString(Source.SourcePath.Path);
				Builder.UpdateValue(Source.ContentHash.HashLow);
				Builder.UpdateValue(Source.ContentHash.HashHigh);
			}
			AddString(Request.Destination.GetView());
			if (Request.ExistingProvenance)
				AddString(Request.ExistingProvenance->AuthoredOutputFingerprint);
			return Builder.Finalize();
		}

		class FInterchangePreviewCache
		{
		public:
			auto Find(const FXxHash128& Fingerprint)
				-> std::unique_ptr<FInterchangeGraphJobValue>
			{
				std::lock_guard Lock(Mutex);
				const auto It = Entries.find(Fingerprint);
				return It == Entries.end() ? nullptr : CloneGraphs(*It->second.Value);
			}

			auto Store(const FXxHash128& Fingerprint,
				const FInterchangeGraphJobValue& Value) -> void
			{
				const uint64 Bytes = Value.Snapshot->GetAggregateByteCount()
					+ EstimateGraphBytes(Value.TranslatedGraph, Value.FactoryGraph);
				if (Bytes > MaximumBytes) return;
				std::lock_guard Lock(Mutex);
				if (const auto Existing = Entries.find(Fingerprint); Existing != Entries.end())
				{
					RetainedBytes -= Existing->second.Bytes;
					Entries.erase(Existing);
				}
				while (!Order.empty()
					&& (Entries.size() >= MaximumEntries || Bytes > MaximumBytes - RetainedBytes))
				{
					const FXxHash128 Oldest = Order.front();
					Order.pop_front();
					if (const auto It = Entries.find(Oldest); It != Entries.end())
					{
						RetainedBytes -= It->second.Bytes;
						Entries.erase(It);
					}
				}
				Entries.emplace(Fingerprint, FEntry{
					.Value = std::shared_ptr<FInterchangeGraphJobValue>(CloneGraphs(Value).release()),
					.Bytes = Bytes});
				Order.push_back(Fingerprint);
				RetainedBytes += Bytes;
			}

		private:
			struct FEntry
			{
				std::shared_ptr<FInterchangeGraphJobValue> Value;
				uint64 Bytes = 0;
			};

			static auto EstimateGraphBytes(
				const FTranslatedAssetGraph& Translated,
				const FImportFactoryGraph& Factory) -> uint64
			{
				uint64 Bytes = 0;
				for (const FTranslatedAssetNode& Node : Translated.GetNodes())
					Bytes += sizeof(Node) + Node.Payload.Bytes.size();
				for (const FImportFactoryNode& Node : Factory.GetNodes())
					Bytes += sizeof(Node) + Node.Settings.Bytes.size();
				return Bytes;
			}

			static constexpr size_t MaximumEntries = 8;
			static constexpr uint64 MaximumBytes = 256ull * 1'024ull * 1'024ull;
			std::mutex Mutex;
			std::unordered_map<FXxHash128, FEntry> Entries;
			std::deque<FXxHash128> Order;
			uint64 RetainedBytes = 0;
		};

		auto GetInterchangePreviewCache() -> FInterchangePreviewCache&
		{
			static FInterchangePreviewCache Cache;
			return Cache;
		}

		struct FInterchangeProductEntry
		{
			FInterchangeComponentLease Factory;
			std::unique_ptr<IInterchangeFactoryProduct> Product;
			size_t NodeIndex = 0;
		};

		struct FInterchangeProductJobValue final : IImportJobValue
		{
			FInterchangeGraphJobValue Graphs;
			std::vector<FInterchangeProductEntry> Products;
		};

		struct FPreparedInterchangeOutput
		{
			FInterchangeComponentLease Factory;
			std::unique_ptr<ISingleAssetCandidate> Candidate;
			std::unique_ptr<IPreparedImportedStateExchange> Exchange;
			DObject* ExistingTarget = nullptr;
			size_t NodeIndex = 0;
		};

		class FInterchangeImportJob final : public IImportJob
		{
		public:
			FInterchangeImportJob(FInterchangeImportRequest InRequest,
				std::shared_ptr<FInterchangeImportResultState> InResultState)
				: Request(std::move(InRequest)), ResultState(std::move(InResultState))
			{
				if (Request.Owner.OwnerId.empty())
					Request.Owner.OwnerId = "Interchange.Import";
				if (Request.Owner.ConflictIdentities.empty()
					&& Request.Destination.IsValid())
					Request.Owner.ConflictIdentities.push_back(Request.Destination.ToString());
			}

			auto GetProviderId() const -> std::string_view override
			{
				return Request.TranslatorId.empty()
					? std::string_view("Interchange")
					: std::string_view(Request.TranslatorId);
			}
			auto RequiresLegacyProviderLease() const -> bool override { return false; }
			auto GetOwner() const -> const FImportOperationOwner& override { return Request.Owner; }
			auto GetLifetime() const -> EImportOperationLifetime override { return Request.Lifetime; }

			auto AdvanceOnEditor(FImportJobEditorContext& Context,
				std::unique_ptr<IImportJobValue> PreviousWorkerResult)
				-> FImportJobEditorAdvance override
			{
				if (Context.IsCancellationRequested() && State != EState::Materialize)
					return Complete(FailureOutcome({}, "Interchange import was canceled.", true));
				switch (State)
				{
				case EState::Start:
					State = EState::AwaitGraphs;
					WorkerRound = 0;
					return FImportJobEditorAdvance::ContinueWith({
						.Name = "Interchange.TranslateAndPlan",
						.Attribution = GetAttribution(),
						.EstimatedResultBytes = 16ull * 1'024ull * 1'024ull});
				case EState::AwaitGraphs:
				{
					auto* Graphs = dynamic_cast<FInterchangeGraphJobValue*>(PreviousWorkerResult.get());
					if (!Graphs) return Complete(FailureOutcome({},
						"Interchange graph preparation returned an invalid value."));
					if (Request.Mode == EInterchangeImportMode::Preview)
					{
						FInterchangeImportResult Result;
						Result.Outcome.State = EImportOperationState::Succeeded;
						Result.Provenance = Graphs->Provenance;
						Result.Inspection = Graphs->Inspection;
						StoreResult(Result);
						return FImportJobEditorAdvance::Complete(Result.Outcome);
					}
					State = EState::AwaitProducts;
					WorkerRound = 1;
					return FImportJobEditorAdvance::ContinueWith({
						.Name = "Interchange.BuildProducts",
						.Attribution = GetAttribution(),
						.EstimatedResultBytes = MaximumImportJobDetachedValueBytes,
						.Input = std::move(PreviousWorkerResult)});
				}
				case EState::AwaitProducts:
				{
					auto* Products = dynamic_cast<FInterchangeProductJobValue*>(PreviousWorkerResult.get());
					if (!Products) return Complete(FailureOutcome({},
						"Interchange product construction returned an invalid value."));
					State = EState::Materialize;
					return MaterializeAndPublish(Context, *Products);
				}
				case EState::Materialize:
				case EState::Terminal:
					return Complete(FailureOutcome({}, "Interchange job was advanced after terminal."));
				}
				return Complete(FailureOutcome({}, "Interchange job state is invalid."));
			}

			auto ExecuteWorkerStep(FImportJobWorkerContext& Context,
				std::unique_ptr<IImportJobValue> Input)
				-> FImportJobWorkerResult override
			{
				return WorkerRound == 0
					? PrepareGraphs(Context)
					: BuildProducts(Context, std::move(Input));
			}

		private:
			enum class EState : uint8 { Start, AwaitGraphs, AwaitProducts, Materialize, Terminal };

			static auto GetAttribution() -> FTaskAttribution
			{
				static const FTaskAttribution Attribution =
					RegisterTaskAttribution("AssetImportCore", "InterchangeImport");
				return Attribution;
			}

			auto PrepareGraphs(FImportJobWorkerContext& Context)
				-> FImportJobWorkerResult
			{
				std::vector<FImportDiagnostic> Diagnostics;
				FSourceSnapshotBuilder SnapshotBuilder(Request.SourceLimits);
				if (!SnapshotBuilder.CaptureRoot(Request.RootSource, Diagnostics))
					return WorkerFailure(std::move(Diagnostics), "Source capture failed.");
				const FSourceSnapshotEntry& Root = SnapshotBuilder.GetCapturedSources().front();
				const std::filesystem::path RootPath(Request.RootSource.Path);
				const size_t PrefixSize = static_cast<size_t>(std::min<uint64>(
					Root.ByteCount, Request.SourceLimits.RecognitionPrefixBytes));
				const FImportSourceRecognition Recognition{
					.RootSource = Request.RootSource,
					.Extension = RootPath.extension().string(),
					.ByteCount = Root.ByteCount,
					.Prefix = Root.GetBytes().first(PrefixSize)};
				auto Selection = GetImportService().SelectTranslator(
					Recognition, Request.TranslatorId,
					Request.ExistingProvenance
						? Request.ExistingProvenance->Translator.ContractVersion : 0);
				if (!Selection)
					return WorkerFailure(std::move(Selection.Diagnostics), "Translator selection failed.");
				if (Context.Cancellation.IsCancellationRequested())
					return {.bSucceeded = false, .bCanceled = true,
						.Diagnostic = "Interchange translation was canceled."};
				if (!SnapshotBuilder.DiscoverInterchangeDependencies(
					Selection.Lease, Diagnostics))
					return WorkerFailure(std::move(Diagnostics), "Dependency discovery failed.");
				auto Snapshot = SnapshotBuilder.Freeze(Diagnostics);
				if (!Snapshot) return WorkerFailure(std::move(Diagnostics), "Snapshot finalization failed.");
				const uint64 RegistryRevision = GetImportService().GetInterchangeRevision();
				const FXxHash128 PreviewReuseFingerprint = BuildPreviewReuseFingerprint(
					Request, *Snapshot, Selection.Lease, RegistryRevision);
				if (Request.Mode != EInterchangeImportMode::Preview)
					if (auto Cached = GetInterchangePreviewCache().Find(PreviewReuseFingerprint))
						return {.Value = std::move(Cached)};

				FTranslatedAssetGraphBuilder GraphBuilder(Request.GraphLimits);
				auto Invocation = Selection.Lease.TryEnter();
				if (!Invocation || !Selection.Lease.GetTranslator()
					|| !Selection.Lease.GetTranslator()->Translate(*Snapshot,
						Request.TranslatorSettings, GraphBuilder, Diagnostics)
					|| HasError(Diagnostics))
					return WorkerFailure(std::move(Diagnostics), "Source translation failed.");
				FTranslatedAssetGraph TranslatedGraph;
				if (!GraphBuilder.Finalize(TranslatedGraph, Diagnostics))
					return WorkerFailure(std::move(Diagnostics), "Translated graph validation failed.");
				auto PipelineResult = ExecuteInterchangePipelineStack(
					TranslatedGraph, Request.PipelineStack, Request.GraphLimits);
				if (!PipelineResult)
					return WorkerFailure(std::move(PipelineResult.Diagnostics), "Pipeline execution failed.");

				auto Value = std::make_unique<FInterchangeGraphJobValue>();
				Value->Snapshot = std::move(Snapshot);
				Value->Provenance.Translator = {
					.Id = std::string(Selection.Lease.GetId()),
					.ContractVersion = Selection.Lease.GetContractVersion(),
					.Settings = Request.TranslatorSettings};
				Value->Provenance.PipelineStack = Request.PipelineStack;
				for (const FSourceSnapshotEntry& Source : Value->Snapshot->GetSources())
				{
					Value->Provenance.Sources.push_back({
						.StableIdentity = Source.StableIdentity,
						.Role = Source.Role,
						.SourcePath = Source.SourcePath,
						.ContentHash = Source.ContentHash,
						.ByteCount = Source.ByteCount});
					Value->Inspection.Sources.push_back({
						.StableIdentity = Source.StableIdentity,
						.Role = Source.Role,
						.SourcePath = Source.SourcePath,
						.ByteCount = Source.ByteCount,
						.bEmbedded = Source.bEmbedded});
				}
				Value->Provenance.TranslatedGraphFingerprint = TranslatedGraph.GetFingerprint();
				Value->Provenance.FactoryGraphFingerprint = PipelineResult.Graph.GetFingerprint();
				Value->Inspection.bCompatible = true;
				Value->Inspection.TranslatedGraphFingerprint = TranslatedGraph.GetFingerprint();
				Value->Inspection.FactoryGraphFingerprint = PipelineResult.Graph.GetFingerprint();
				for (const FImportFactoryNode& Node : PipelineResult.Graph.GetNodes())
				{
					Value->Inspection.Outputs.push_back({
						.StableIdentity = Node.StableIdentity,
						.Role = Node.OutputClassName,
						.AssetPath = Node.Destination,
						.AssetClassName = Node.OutputClassName,
						.Policy = Node.Policy});
					Value->Provenance.OutputMappings.push_back({
						.TranslatedNodeIdentity = Node.TranslatedNodeReferences.empty()
							? std::string{} : Node.TranslatedNodeReferences.front(),
						.OutputIdentity = Node.StableIdentity,
						.AssetPath = Node.Destination});
				}
				Value->TranslatedGraph = std::move(TranslatedGraph);
				Value->FactoryGraph = std::move(PipelineResult.Graph);
				Value->RegistryRevision = PipelineResult.RegistryRevision;
				if (Request.Mode == EInterchangeImportMode::Preview)
					GetInterchangePreviewCache().Store(PreviewReuseFingerprint, *Value);
				return {.Value = std::move(Value)};
			}

			auto BuildProducts(FImportJobWorkerContext& Context,
				std::unique_ptr<IImportJobValue> Input) -> FImportJobWorkerResult
			{
				auto* Graphs = dynamic_cast<FInterchangeGraphJobValue*>(Input.get());
				if (!Graphs) return {.bSucceeded = false,
					.Diagnostic = "Interchange product input is invalid."};
				auto Value = std::make_unique<FInterchangeProductJobValue>();
				Value->Graphs = std::move(*Graphs);
				const auto Nodes = Value->Graphs.FactoryGraph.GetNodes();
				for (const std::vector<size_t>& Level :
					TopologicalFactoryLevels(Value->Graphs.FactoryGraph))
				{
					if (Context.Cancellation.IsCancellationRequested())
						return {.bSucceeded = false, .bCanceled = true,
							.Diagnostic = "Interchange product construction was canceled."};
					std::vector<FInterchangeProductEntry> Entries(Level.size());
					std::vector<std::vector<FImportDiagnostic>> Diagnostics(Level.size());
					std::vector<uint8> Succeeded(Level.size());
					bool bParallelSafe = Level.size() > 1;
					for (size_t LevelIndex = 0; LevelIndex < Level.size(); ++LevelIndex)
					{
						const size_t NodeIndex = Level[LevelIndex];
						const FImportFactoryNode& Node = Nodes[NodeIndex];
						auto Selected = GetImportService().SelectFactory(
							Node.OutputClassName, Node.FactoryId, Node.FactoryContractVersion);
						if (!Selected)
							return WorkerFailure(std::move(Selected.Diagnostics),
								"Factory selection failed.");
						bParallelSafe = bParallelSafe && Selected.Lease.GetThreadCapability()
							== EInterchangeThreadCapability::WorkerSafe;
						Entries[LevelIndex].Factory = std::move(Selected.Lease);
						Entries[LevelIndex].NodeIndex = NodeIndex;
					}

					auto BuildOne = [&](size_t LevelIndex,
						const FParallelForCancellationToken* GroupCancellation) {
						FInterchangeProductEntry& Entry = Entries[LevelIndex];
						const FImportFactoryNode& Node = Nodes[Entry.NodeIndex];
						auto Invocation = Entry.Factory.TryEnter();
						if (!Invocation || !Entry.Factory.GetFactory()) return;
						Entry.Product = Entry.Factory.GetFactory()->BuildDetachedProduct(
							Node, Value->Graphs.TranslatedGraph, &Context.Progress,
							[&] {
								return Context.Cancellation.IsCancellationRequested()
									|| (GroupCancellation
										&& GroupCancellation->IsCancellationRequested());
							}, Diagnostics[LevelIndex]);
						Succeeded[LevelIndex] = Entry.Product
							&& !HasError(Diagnostics[LevelIndex]);
					};

					if (bParallelSafe)
					{
						const FParallelForResult ParallelResult = ParallelForCancelable(
							"Interchange.BuildIndependentProducts", Level.size(),
							[&](uint64 Index, const FParallelForCancellationToken& Cancellation) {
								BuildOne(static_cast<size_t>(Index), &Cancellation);
							}, {.MinBatchSize = 1, .CancellationToken = Context.Cancellation});
						if (ParallelResult.State != ETaskState::Succeeded)
							return {.bSucceeded = false,
								.bCanceled = Context.Cancellation.IsCancellationRequested(),
								.Diagnostic = ParallelResult.Diagnostic.empty()
									? "Parallel Interchange product construction failed."
									: ParallelResult.Diagnostic};
					}
					else
						for (size_t LevelIndex = 0; LevelIndex < Level.size(); ++LevelIndex)
							BuildOne(LevelIndex, nullptr);

					for (size_t LevelIndex = 0; LevelIndex < Level.size(); ++LevelIndex)
					{
						if (!Succeeded[LevelIndex])
						{
							const FImportFactoryNode& Node = Nodes[Level[LevelIndex]];
							return WorkerFailure(std::move(Diagnostics[LevelIndex]),
								std::format("Factory '{}' failed to build output '{}'.",
									Node.FactoryId, Node.StableIdentity));
						}
						Value->Products.push_back(std::move(Entries[LevelIndex]));
					}
				}
				return {.Value = std::move(Value)};
			}

			auto MaterializeAndPublish(FImportJobEditorContext& Context,
				FInterchangeProductJobValue& Products) -> FImportJobEditorAdvance
			{
				std::vector<FImportDiagnostic> Diagnostics;
				std::vector<FPreparedInterchangeOutput> Prepared;
				const auto Nodes = Products.Graphs.FactoryGraph.GetNodes();
				for (FInterchangeProductEntry& Product : Products.Products)
				{
					if (Context.IsCancellationRequested())
						return Complete(FailureOutcome(std::move(Diagnostics),
							"Interchange materialization was canceled.", true));
					const FImportFactoryNode& Node = Nodes[Product.NodeIndex];
					auto Invocation = Product.Factory.TryEnter();
					auto Candidate = Invocation && Product.Factory.GetFactory()
						? Product.Factory.GetFactory()->MaterializeCandidate(
							Node, std::move(Product.Product), Diagnostics)
						: nullptr;
					if (!Candidate || !Candidate->GetAsset() || !Candidate->GetPackage())
					{
						Abandon(Prepared);
						return Complete(FailureOutcome(std::move(Diagnostics),
							std::format("Candidate '{}' failed validation.", Node.StableIdentity)));
					}
					FPreparedInterchangeOutput Output{
						.Factory = std::move(Product.Factory),
						.Candidate = std::move(Candidate),
						.NodeIndex = Product.NodeIndex};
					if (Node.Policy != EImportOutputPolicy::Create)
					{
						const Asset::FAssetResult Loaded = Asset::LoadAsset(
							Node.Destination, Output.ExistingTarget);
						if (!Loaded || !Output.ExistingTarget)
						{
							Abandon(Prepared);
							Output.Candidate->Abandon();
							return Complete(FailureOutcome(std::move(Diagnostics),
								std::format("Replacement target '{}' is unavailable.",
									Node.Destination.ToString())));
						}
					}
					Prepared.push_back(std::move(Output));
				}

				auto FindPreparedObject = [&](std::string_view Identity, bool bProspective)
					-> DObject* {
					const auto It = std::ranges::find_if(Prepared,
						[&](const FPreparedInterchangeOutput& Output) {
							return Nodes[Output.NodeIndex].StableIdentity == Identity;
						});
					if (It == Prepared.end()) return nullptr;
					return bProspective ? It->Candidate->GetAsset() :
						(It->ExistingTarget ? It->ExistingTarget : It->Candidate->GetAsset());
				};
				const FInterchangeMaterializationContext MaterializationContext{
					.ExistingTarget = [&](std::string_view Identity) {
						return FindPreparedObject(Identity, false); },
					.ProspectiveObject = [&](std::string_view Identity) {
						return FindPreparedObject(Identity, true); }};
				for (FPreparedInterchangeOutput& Output : Prepared)
				{
					const FImportFactoryNode& Node = Nodes[Output.NodeIndex];
					auto Invocation = Output.Factory.TryEnter();
					if (!Invocation || !Output.Factory.GetFactory()
						|| !Output.Factory.GetFactory()->ResolveCandidateDependencies(
							Node, *Output.Candidate, MaterializationContext, Diagnostics)
						|| !Output.Candidate->Validate(Diagnostics))
					{
						Abandon(Prepared);
						return Complete(FailureOutcome(std::move(Diagnostics),
							std::format("Candidate '{}' dependency binding or validation failed.",
								Node.StableIdentity)));
					}
					if (Node.Policy != EImportOutputPolicy::Create)
					{
						Output.Exchange = Output.Factory.GetFactory()->PrepareImportedStateExchange(
							*Output.ExistingTarget, *Output.Candidate, Diagnostics);
						if (!Output.Exchange)
						{
							Abandon(Prepared);
							return Complete(FailureOutcome(std::move(Diagnostics),
								"Factory did not prepare a reversible replacement exchange."));
						}
					}
				}

				if (!Context.EnterFinalization())
				{
					Abandon(Prepared);
					return Complete(FailureOutcome(std::move(Diagnostics),
						"Interchange import was canceled before finalization.", true));
				}
				std::lock_guard PublicationLock(GetImportPublicationMutex());
				if (GetImportService().GetInterchangeRevision() != Products.Graphs.RegistryRevision)
				{
					Abandon(Prepared);
					return Complete(FailureOutcome(std::move(Diagnostics),
						"Interchange registry changed before publication."));
				}
				std::vector<DPackage*> Packages;
				std::vector<std::pair<DPackage*, bool>> PackageDirtyStates;
				std::vector<FPreparedInterchangeOutput*> Committed;
				auto RememberPackageDirtyState = [&](DPackage* Package) {
					if (std::ranges::none_of(PackageDirtyStates,
						[&](const auto& Entry) { return Entry.first == Package; }))
						PackageDirtyStates.emplace_back(Package, Package->IsDirty());
				};
				auto RestorePackageDirtyStates = [&] {
					for (const auto& [Package, bWasDirty] : PackageDirtyStates)
						if (!bWasDirty) Package->ClearDirty();
				};
				for (FPreparedInterchangeOutput& Output : Prepared)
				{
					const FImportFactoryNode& Node = Nodes[Output.NodeIndex];
					if (Node.Policy == EImportOutputPolicy::Create)
					{
						FAssetPath CandidatePath;
						if (!Output.Candidate->IsNewAsset()
							|| !FAssetPath::TryCreate(Output.Candidate->GetPackage()->GetPackagePath(), CandidatePath)
							|| CandidatePath != Node.Destination
							|| Asset::FindAssetExact(Node.Destination)
							|| Asset::FindResidentPackage(Node.Destination)
								!= Output.Candidate->GetPackage())
						{
							Abandon(Prepared);
							return Complete(FailureOutcome(std::move(Diagnostics),
								"Created candidate no longer owns its requested destination."));
						}
						Packages.push_back(Output.Candidate->GetPackage());
					}
					else
					{
						RememberPackageDirtyState(Output.ExistingTarget->GetPackage());
						Output.Exchange->Commit();
						Committed.push_back(&Output);
						Packages.push_back(Output.ExistingTarget->GetPackage());
					}
				}
				Asset::FAssetBundleSaveOptions SaveOptions = Request.SaveOptions;
				if (!Packages.empty()) SaveOptions.RootPackage = Packages.back();
				FXxHash128Builder AuthoredFingerprintBuilder;
				bool bFingerprintSucceeded = true;
				std::string FingerprintError;
				for (DPackage* Package : Packages)
				{
					std::string Fingerprint;
					if (!ComputeImportPackageFingerprint(
						Package, Fingerprint, FingerprintError))
					{
						bFingerprintSucceeded = false;
						break;
					}
					const uint64 Size = Fingerprint.size();
					AuthoredFingerprintBuilder.UpdateValue(Size);
					AuthoredFingerprintBuilder.Update(Fingerprint);
				}
				if (!bFingerprintSucceeded)
				{
					for (auto It = Committed.rbegin(); It != Committed.rend(); ++It)
						(*It)->Exchange->Reverse();
					RestorePackageDirtyStates();
					Abandon(Prepared);
					return Complete(FailureOutcome(std::move(Diagnostics),
						std::move(FingerprintError)));
				}
				Products.Graphs.Provenance.AuthoredOutputFingerprint =
					AuthoredFingerprintBuilder.Finalize().ToString();
				for (FPreparedInterchangeOutput& Output : Prepared)
				{
					const FImportFactoryNode& Node = Nodes[Output.NodeIndex];
					DObject* PublishedAsset = Node.Policy == EImportOutputPolicy::Create
						? Output.Candidate->GetAsset() : Output.ExistingTarget;
					auto Invocation = Output.Factory.TryEnter();
					if (!PublishedAsset || !Invocation || !Output.Factory.GetFactory()
						|| !Output.Factory.GetFactory()->ApplyProvenance(
							*PublishedAsset, Products.Graphs.Provenance, Diagnostics))
					{
						for (auto It = Committed.rbegin(); It != Committed.rend(); ++It)
							(*It)->Exchange->Reverse();
						RestorePackageDirtyStates();
						Abandon(Prepared);
						return Complete(FailureOutcome(std::move(Diagnostics),
							std::format("Factory '{}' failed to persist Interchange provenance.",
								Node.FactoryId)));
					}
				}
				const Asset::FAssetResult Saved = Asset::SavePackagesAtomically(Packages, SaveOptions);
				if (!Saved)
				{
					for (auto It = Committed.rbegin(); It != Committed.rend(); ++It)
						(*It)->Exchange->Reverse();
					RestorePackageDirtyStates();
					Abandon(Prepared);
					Diagnostics.push_back({
						.Category = EImportDiagnosticCategory::PublicationFailure,
						.Identity = "InterchangePublicationFailed",
						.Phase = "Publication",
						.Message = Saved.Message});
					return Complete(FailureOutcome(std::move(Diagnostics), Saved.Message));
				}

				FInterchangeImportResult Result;
				Result.Outcome.State = EImportOperationState::Succeeded;
				Result.Provenance = Products.Graphs.Provenance;
				Result.Inspection = Products.Graphs.Inspection;
				for (FPreparedInterchangeOutput& Output : Prepared)
				{
					const FImportFactoryNode& Node = Nodes[Output.NodeIndex];
					Result.Outcome.PublishedAssetIdentities.push_back(Node.Destination.ToString());
					if (Output.Exchange)
					{
						Output.Exchange->Finalize();
						Output.Exchange.reset();
						Output.Candidate->Abandon();
					}
					Output.Candidate.reset();
				}
				StoreResult(Result);
				State = EState::Terminal;
				return FImportJobEditorAdvance::Complete(Result.Outcome);
			}

			struct FInterchangeDiagnosticJobValue final : IImportJobValue
			{
				explicit FInterchangeDiagnosticJobValue(
					std::vector<FImportDiagnostic> InDiagnostics)
					: Diagnostics(std::move(InDiagnostics)) {}
				std::vector<FImportDiagnostic> Diagnostics;
			};

			auto WorkerFailure(std::vector<FImportDiagnostic> Diagnostics,
				std::string Message) const -> FImportJobWorkerResult
			{
				FinalizeImportDiagnostics(Diagnostics, "interchange-worker");
				return {.bSucceeded = false, .Diagnostic = std::move(Message),
					.Value = std::make_unique<FInterchangeDiagnosticJobValue>(
						std::move(Diagnostics))};
			}

			auto CompensateWorkerFailureOnEditor(FImportJobEditorContext&,
				FImportJobWorkerResult Result) -> FImportOutcome override
			{
				std::vector<FImportDiagnostic> Diagnostics;
				if (auto* Value = dynamic_cast<FInterchangeDiagnosticJobValue*>(Result.Value.get()))
					Diagnostics = std::move(Value->Diagnostics);
				FImportOutcome Outcome = FailureOutcome(std::move(Diagnostics),
					std::move(Result.Diagnostic), Result.bCanceled);
				FInterchangeImportResult ImportResult;
				ImportResult.Outcome = Outcome;
				StoreResult(ImportResult);
				State = EState::Terminal;
				return Outcome;
			}

			auto Complete(FImportOutcome Outcome) -> FImportJobEditorAdvance
			{
				FInterchangeImportResult Result;
				Result.Outcome = Outcome;
				StoreResult(Result);
				State = EState::Terminal;
				return FImportJobEditorAdvance::Complete(std::move(Outcome));
			}

			auto StoreResult(const FInterchangeImportResult& Result) -> void
			{
				FInterchangeImportCompletion Completion;
				{
					std::lock_guard Lock(ResultState->Mutex);
					ResultState->Result = Result;
					if (!ResultState->bCompletionDelivered)
					{
						ResultState->bCompletionDelivered = true;
						Completion = ResultState->Completion;
					}
				}
				if (Completion) Completion(Result);
			}

			static auto Abandon(std::vector<FPreparedInterchangeOutput>& Outputs) -> void
			{
				for (FPreparedInterchangeOutput& Output : Outputs)
					if (Output.Candidate) Output.Candidate->Abandon();
			}

			FInterchangeImportRequest Request;
			std::shared_ptr<FInterchangeImportResultState> ResultState;
			EState State = EState::Start;
			uint8 WorkerRound = 0;
		};
	}

	auto FInterchangeImportHandle::TryGetResult(
		FInterchangeImportResult& OutResult) const -> bool
	{
		if (!State) return false;
		{
			std::lock_guard Lock(State->Mutex);
			if (State->Result)
			{
				OutResult = *State->Result;
				return true;
			}
		}
		FImportOutcome Outcome;
		if (!Operation.TryGetOutcome(Outcome)) return false;
		// Worker failure becomes visible on the operation before the editor-side
		// compensation round stores the framework result. Do not expose that
		// transient ordering gap as a second terminal result. Admission failures
		// and cancellation drained before an editor compensation round are the
		// terminal paths that may not write the job-owned result.
		if (Outcome.State != EImportOperationState::Canceled
			&& Outcome.State != EImportOperationState::Rejected
			&& Outcome.State != EImportOperationState::Superseded)
			return false;
		OutResult = {.Outcome = std::move(Outcome)};
		return true;
	}

	auto FImportService::SubmitInterchangeImport(
		FInterchangeImportRequest Request,
		std::string_view Title,
		FInterchangeImportCompletion Completion) -> FInterchangeImportHandle
	{
		auto State = std::make_shared<FInterchangeImportResultState>();
		State->Completion = std::move(Completion);
		auto Job = std::make_unique<FInterchangeImportJob>(std::move(Request), State);
		FImportOperationHandle Operation = SubmitImportJob(std::move(Job),
			Title.empty() ? "Interchange import" : Title);
		return FInterchangeImportHandle(std::move(Operation), std::move(State));
	}

	auto FImportService::RunInterchangeImportInline(
		FInterchangeImportRequest Request,
		std::string_view Title) -> FInterchangeImportResult
	{
		auto State = std::make_shared<FInterchangeImportResultState>();
		auto Job = std::make_unique<FInterchangeImportJob>(std::move(Request), State);
		const FImportOutcome Outcome = RunImportJobInline(std::move(Job), Title);
		std::lock_guard Lock(State->Mutex);
		if (State->Result) return *State->Result;
		return {.Outcome = Outcome};
	}
}
