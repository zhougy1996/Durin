#include "AssetForge/Operations/ImportExecution.h"

#include "AssetTools.h"
#include "AssetForge/ImportService.h"
#include "DObject/Package.h"

namespace Durin::AssetForge
{
	struct FImportResultState
	{
		mutable std::mutex Mutex;
		std::optional<FImportResult> Result;
		FImportCompletion Completion;
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

		auto TopologicalBuildLevels(const FBuildGraph& Graph)
			-> std::vector<std::vector<size_t>>
		{
			const auto Nodes = Graph.GetNodes();
			std::unordered_map<std::string_view, size_t> Indices;
			for (size_t Index = 0; Index < Nodes.size(); ++Index)
				Indices.emplace(Nodes[Index].StableIdentity, Index);
			std::vector<uint32> InDegree(Nodes.size());
			std::vector<std::vector<size_t>> Dependents(Nodes.size());
			for (size_t Index = 0; Index < Nodes.size(); ++Index)
				for (const std::string& Dependency : Nodes[Index].BuildDependencies)
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

		struct FImportGraphJobValue final : IImportJobValue
		{
			FImportGraphJobValue() = default;
			FImportGraphJobValue(const FImportGraphJobValue&) = delete;
			auto operator=(const FImportGraphJobValue&)
				-> FImportGraphJobValue& = delete;
			FImportGraphJobValue(FImportGraphJobValue&&) noexcept = default;
			auto operator=(FImportGraphJobValue&&) noexcept
				-> FImportGraphJobValue& = default;
			std::shared_ptr<const FSourceSnapshot> Snapshot;
			FSourceGraph SourceGraph;
			FBuildGraph AssetBuilderGraph;
			FImportProvenance Provenance;
			FImportInspection Inspection;
			uint64 RegistryRevision = 0;
			std::vector<std::unique_ptr<IReconciliationContext>>
				ReconciliationContexts;
		};

		struct FBuildProductEntry
		{
			FComponentLease AssetBuilder;
			std::unique_ptr<IBuildProduct> Product;
			size_t NodeIndex = 0;
		};

		struct FBuildProductJobValue final : IImportJobValue
		{
			FImportGraphJobValue Graphs;
			std::vector<FBuildProductEntry> Products;
		};


		struct FPreparedImportOutput
		{
			FComponentLease AssetBuilder;
			std::unique_ptr<ISingleAssetCandidate> Candidate;
			DObject* ExistingTarget = nullptr;
			size_t NodeIndex = 0;
		};

		class FGraphImportJob final : public IImportJob
		{
		public:
			FGraphImportJob(FImportRequest InRequest,
				std::shared_ptr<FImportResultState> InResultState)
				: Request(std::move(InRequest)), ResultState(std::move(InResultState))
			{
				if (Request.Owner.OwnerId.empty())
					Request.Owner.OwnerId = "AssetForge.Import";
				if (Request.Owner.ConflictIdentities.empty()
					&& Request.Destination.IsValid())
					Request.Owner.ConflictIdentities.push_back(Request.Destination.ToString());
			}

			auto GetProviderId() const -> std::string_view override
			{
				return Request.TranslatorId.empty()
					? std::string_view("AssetForge")
					: std::string_view(Request.TranslatorId);
			}
			auto GetOwner() const -> const FImportOperationOwner& override { return Request.Owner; }
			auto GetLifetime() const -> EImportOperationLifetime override { return Request.Lifetime; }

			auto AdvanceOnEditor(FImportJobEditorContext& Context,
				std::unique_ptr<IImportJobValue> PreviousWorkerResult)
				-> FImportJobEditorAdvance override
			{
				if (Context.IsCancellationRequested() && State != EState::Materialize)
					return Complete(FailureOutcome({}, "AssetForge import was canceled.", true));
				switch (State)
				{
				case EState::Start:
					State = EState::AwaitGraphs;
					WorkerRound = 0;
					return FImportJobEditorAdvance::ContinueWith({
						.Name = "AssetForge.TranslateAndPlan",
						.Attribution = GetAttribution(),
						.EstimatedResultBytes = 16ull * 1'024ull * 1'024ull});
				case EState::AwaitGraphs:
				{
					auto* Graphs = dynamic_cast<FImportGraphJobValue*>(PreviousWorkerResult.get());
					if (!Graphs) return Complete(FailureOutcome({},
						"AssetForge graph preparation returned an invalid value."));
					std::vector<FImportDiagnostic> Diagnostics;
					const auto Nodes = Graphs->AssetBuilderGraph.GetNodes();
					Graphs->ReconciliationContexts.resize(Nodes.size());
					for (size_t NodeIndex = 0; NodeIndex < Nodes.size(); ++NodeIndex)
					{
						const FBuildNode& Node = Nodes[NodeIndex];
						if (Node.Policy == EImportOutputPolicy::Create) continue;
						auto Selected = GetImportService().SelectAssetBuilder(
							Node.OutputClassName, Node.BuilderId, Node.BuilderContractVersion);
						if (!Selected)
							return Complete(FailureOutcome(std::move(Selected.Diagnostics),
								"AssetBuilder selection failed during replacement snapshot capture."));
						auto Invocation = Selected.Lease.TryEnter();
						DObject* ExistingTarget = nullptr;
						const Asset::FAssetResult Loaded = Invocation && Selected.Lease.GetAssetBuilder()
							? Selected.Lease.GetAssetBuilder()->LoadExistingTarget(Node, ExistingTarget)
							: Asset::FAssetResult{
								.Error = Asset::EAssetError::ShuttingDown,
								.Message = "AssetForge factory retired before snapshot capture."};
						if (!Loaded || !ExistingTarget)
							return Complete(FailureOutcome(std::move(Diagnostics),
								std::format("Replacement target '{}' is unavailable: {}",
									Node.Destination.ToString(), Loaded.Message)));
						Graphs->ReconciliationContexts[NodeIndex] =
							Selected.Lease.GetAssetBuilder()->CaptureReconciliationContext(
								Node, *ExistingTarget, Diagnostics);
						if (HasError(Diagnostics))
							return Complete(FailureOutcome(std::move(Diagnostics),
								std::format("Replacement target '{}' snapshot capture failed.",
									Node.Destination.ToString())));
					}
					State = EState::AwaitProducts;
					WorkerRound = 1;
					return FImportJobEditorAdvance::ContinueWith({
						.Name = "AssetForge.BuildProducts",
						.Attribution = GetAttribution(),
						.EstimatedResultBytes = MaximumImportJobDetachedValueBytes,
						.Input = std::move(PreviousWorkerResult)});
				}
				case EState::AwaitProducts:
				{
					auto* Products = dynamic_cast<FBuildProductJobValue*>(PreviousWorkerResult.get());
					if (!Products) return Complete(FailureOutcome({},
						"AssetForge product construction returned an invalid value."));
					State = EState::Materialize;
					return MaterializeAndPublish(Context, *Products);
				}
				case EState::Materialize:
				case EState::Terminal:
					return Complete(FailureOutcome({}, "AssetForge job was advanced after terminal."));
				}
				return Complete(FailureOutcome({}, "AssetForge job state is invalid."));
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
					RegisterTaskAttribution("AssetForge", "Durin.AssetForge.Import");
				return Attribution;
			}

			auto PrepareGraphs(FImportJobWorkerContext& Context)
				-> FImportJobWorkerResult
			{
				std::vector<FImportDiagnostic> Diagnostics;
				FSourceSnapshotBuilder SnapshotBuilder(Request.SourceLimits);
				if (!SnapshotBuilder.CaptureRoot(Request.RootSource, Diagnostics))
					return WorkerFailure(std::move(Diagnostics), "Source capture failed.");
				for (const FDeclaredSource& Source : Request.DeclaredSources)
					if (!SnapshotBuilder.CaptureDeclaredSource(
						Source.StableIdentity, Source.Role, Source.SourcePath, Diagnostics))
						return WorkerFailure(std::move(Diagnostics),
							"Declared source capture failed.");
				const FSourceSnapshotEntry& Root = SnapshotBuilder.GetCapturedSources().front();
				const std::filesystem::path RootPath(Request.RootSource.Path);
				const size_t PrefixSize = static_cast<size_t>(std::min<uint64>(
					Root.ByteCount, Request.SourceLimits.RecognitionPrefixBytes));
				const FImportSourceRecognition Recognition{
					.RootSource = Request.RootSource,
					.Extension = RootPath.extension().string(),
					.ByteCount = Root.ByteCount,
					.Prefix = Root.GetBytes().first(PrefixSize)};
				auto Selection = GetImportService().SelectSourceTranslator(
					Recognition, Request.TranslatorId,
					Request.ExistingProvenance
						? Request.ExistingProvenance->Translator.ContractVersion : 0);
				if (!Selection)
					return WorkerFailure(std::move(Selection.Diagnostics), "Translator selection failed.");
				if (Context.Cancellation.IsCancellationRequested())
					return {.bSucceeded = false, .bCanceled = true,
						.Diagnostic = "AssetForge translation was canceled."};
				if (!SnapshotBuilder.DiscoverSourceDependencies(
					Selection.Lease, Diagnostics))
					return WorkerFailure(std::move(Diagnostics), "Dependency discovery failed.");
				auto Snapshot = SnapshotBuilder.Freeze(Diagnostics);
				if (!Snapshot) return WorkerFailure(std::move(Diagnostics), "Snapshot finalization failed.");
				FSourceGraphBuilder GraphBuilder(Request.GraphLimits);
				auto Invocation = Selection.Lease.TryEnter();
				if (!Invocation || !Selection.Lease.GetSourceTranslator()
					|| !Selection.Lease.GetSourceTranslator()->Translate(*Snapshot,
						Request.TranslatorSettings, GraphBuilder, Diagnostics)
					|| HasError(Diagnostics))
					return WorkerFailure(std::move(Diagnostics), "Source translation failed.");
				FSourceGraph SourceGraph;
				if (!GraphBuilder.Finalize(SourceGraph, Diagnostics))
					return WorkerFailure(std::move(Diagnostics), "Translated graph validation failed.");
				auto PlanningPassResult = ExecutePlanningPassStack(
					SourceGraph, Request.PlanningPassStack, Request.GraphLimits);
				if (!PlanningPassResult)
					return WorkerFailure(std::move(PlanningPassResult.Diagnostics), "PlanningPass execution failed.");

				auto Value = std::make_unique<FImportGraphJobValue>();
				Value->Snapshot = std::move(Snapshot);
				Value->Provenance.Translator = {
					.Id = std::string(Selection.Lease.GetId()),
					.ContractVersion = Selection.Lease.GetContractVersion(),
					.Settings = Request.TranslatorSettings};
				Value->Provenance.PlanningPassStack = Request.PlanningPassStack;
				for (const FSourceSnapshotEntry& Source : Value->Snapshot->GetSources())
				{
					Value->Provenance.Sources.push_back({
						.StableIdentity = Source.StableIdentity,
						.Role = Source.Role,
						.SourcePath = Source.SourcePath,
						.ContentHash = Source.ContentHash,
						.ByteCount = Source.ByteCount,
						.LastWriteTime = Source.LastWriteTime});
					Value->Inspection.Sources.push_back({
						.StableIdentity = Source.StableIdentity,
						.Role = Source.Role,
						.SourcePath = Source.SourcePath,
						.ByteCount = Source.ByteCount,
						.bEmbedded = Source.bEmbedded});
				}
				Value->Provenance.SourceGraphFingerprint = SourceGraph.GetFingerprint();
				Value->Provenance.BuildGraphFingerprint = PlanningPassResult.Graph.GetFingerprint();
				Value->Inspection.bCompatible = true;
				Value->Inspection.SourceGraphFingerprint = SourceGraph.GetFingerprint();
				Value->Inspection.BuildGraphFingerprint = PlanningPassResult.Graph.GetFingerprint();
				for (const FBuildNode& Node : PlanningPassResult.Graph.GetNodes())
				{
					Value->Inspection.Outputs.push_back({
						.StableIdentity = Node.StableIdentity,
						.Role = Node.OutputClassName,
						.AssetPath = Node.Destination,
						.AssetClassName = Node.OutputClassName,
						.Policy = Node.Policy});
					Value->Provenance.OutputMappings.push_back({
						.SourceNodeIdentity = Node.SourceNodeReferences.empty()
							? std::string{} : Node.SourceNodeReferences.front(),
						.OutputIdentity = Node.StableIdentity,
						.AssetPath = Node.Destination});
				}
				Value->SourceGraph = std::move(SourceGraph);
				Value->AssetBuilderGraph = std::move(PlanningPassResult.Graph);
				Value->RegistryRevision = PlanningPassResult.RegistryRevision;
				return {.Value = std::move(Value)};
			}

			auto BuildProducts(FImportJobWorkerContext& Context,
				std::unique_ptr<IImportJobValue> Input) -> FImportJobWorkerResult
			{
				auto* Graphs = dynamic_cast<FImportGraphJobValue*>(Input.get());
				if (!Graphs) return {.bSucceeded = false,
					.Diagnostic = "AssetForge product input is invalid."};
				auto Value = std::make_unique<FBuildProductJobValue>();
				Value->Graphs = std::move(*Graphs);
				const auto Nodes = Value->Graphs.AssetBuilderGraph.GetNodes();
				for (const std::vector<size_t>& Level :
					TopologicalBuildLevels(Value->Graphs.AssetBuilderGraph))
				{
					if (Context.Cancellation.IsCancellationRequested())
						return {.bSucceeded = false, .bCanceled = true,
							.Diagnostic = "AssetForge product construction was canceled."};
					std::vector<FBuildProductEntry> Entries(Level.size());
					std::vector<std::vector<FImportDiagnostic>> Diagnostics(Level.size());
					std::vector<uint8> Succeeded(Level.size());
					bool bParallelSafe = Level.size() > 1;
					for (size_t LevelIndex = 0; LevelIndex < Level.size(); ++LevelIndex)
					{
						const size_t NodeIndex = Level[LevelIndex];
						const FBuildNode& Node = Nodes[NodeIndex];
						auto Selected = GetImportService().SelectAssetBuilder(
							Node.OutputClassName, Node.BuilderId, Node.BuilderContractVersion);
						if (!Selected)
							return WorkerFailure(std::move(Selected.Diagnostics),
								"AssetBuilder selection failed.");
						bParallelSafe = bParallelSafe && Selected.Lease.GetThreadCapability()
							== EThreadCapability::WorkerSafe;
						Entries[LevelIndex].AssetBuilder = std::move(Selected.Lease);
						Entries[LevelIndex].NodeIndex = NodeIndex;
					}

					auto BuildOne = [&](size_t LevelIndex,
						const FParallelForCancellationToken* GroupCancellation) {
						FBuildProductEntry& Entry = Entries[LevelIndex];
						const FBuildNode& Node = Nodes[Entry.NodeIndex];
						auto Invocation = Entry.AssetBuilder.TryEnter();
						if (!Invocation || !Entry.AssetBuilder.GetAssetBuilder()) return;
						Entry.Product = Entry.AssetBuilder.GetAssetBuilder()->BuildDetachedProduct(
							Node, Value->Graphs.SourceGraph, &Context.Progress,
							[&] {
								return Context.Cancellation.IsCancellationRequested()
									|| (GroupCancellation
										&& GroupCancellation->IsCancellationRequested());
							}, Diagnostics[LevelIndex]);
						if (Entry.Product
							&& !Entry.AssetBuilder.GetAssetBuilder()->ReconcileDetachedProduct(
								Node,
								Value->Graphs.ReconciliationContexts.empty() ? nullptr
									: Value->Graphs.ReconciliationContexts[Entry.NodeIndex].get(),
								*Entry.Product, Diagnostics[LevelIndex]))
							Entry.Product.reset();
						Succeeded[LevelIndex] = Entry.Product
							&& !HasError(Diagnostics[LevelIndex]);
					};

					if (bParallelSafe)
					{
						const FParallelForResult ParallelResult = ParallelForCancelable(
							"AssetForge.BuildIndependentProducts", Level.size(),
							[&](uint64 Index, const FParallelForCancellationToken& Cancellation) {
								BuildOne(static_cast<size_t>(Index), &Cancellation);
							}, {.MinBatchSize = 1, .CancellationToken = Context.Cancellation});
						if (ParallelResult.State != ETaskState::Succeeded)
							return {.bSucceeded = false,
								.bCanceled = Context.Cancellation.IsCancellationRequested(),
								.Diagnostic = ParallelResult.Diagnostic.empty()
									? "Parallel AssetForge product construction failed."
									: ParallelResult.Diagnostic};
					}
					else
						for (size_t LevelIndex = 0; LevelIndex < Level.size(); ++LevelIndex)
							BuildOne(LevelIndex, nullptr);

					for (size_t LevelIndex = 0; LevelIndex < Level.size(); ++LevelIndex)
					{
						if (!Succeeded[LevelIndex])
						{
							const FBuildNode& Node = Nodes[Level[LevelIndex]];
							return WorkerFailure(std::move(Diagnostics[LevelIndex]),
								std::format("AssetBuilder '{}' failed to build output '{}'.",
									Node.BuilderId, Node.StableIdentity));
						}
						Value->Products.push_back(std::move(Entries[LevelIndex]));
					}
				}
				return {.Value = std::move(Value)};
			}

			auto MaterializeAndPublish(FImportJobEditorContext& Context,
				FBuildProductJobValue& Products) -> FImportJobEditorAdvance
			{
				std::vector<FImportDiagnostic> Diagnostics;
				std::vector<FPreparedImportOutput> Prepared;
				const auto Nodes = Products.Graphs.AssetBuilderGraph.GetNodes();
				const bool bPersistAuthoredPackages =
					Request.Mode != EImportMode::Recover;
				for (FBuildProductEntry& Product : Products.Products)
				{
					if (Context.IsCancellationRequested())
						return Complete(FailureOutcome(std::move(Diagnostics),
							"AssetForge materialization was canceled.", true));
					const FBuildNode& Node = Nodes[Product.NodeIndex];
					if (!bPersistAuthoredPackages
						&& Node.Policy == EImportOutputPolicy::Create)
						return Complete(FailureOutcome(std::move(Diagnostics),
							"AssetForge recovery cannot create an authored asset."));
					auto Invocation = Product.AssetBuilder.TryEnter();
					DObject* ExistingTarget = nullptr;
					if (Node.Policy != EImportOutputPolicy::Create)
					{
						const Asset::FAssetResult Loaded =
							Invocation && Product.AssetBuilder.GetAssetBuilder()
							? Product.AssetBuilder.GetAssetBuilder()->LoadExistingTarget(
								Node, ExistingTarget)
							: Asset::FAssetResult{
								.Error = Asset::EAssetError::ShuttingDown,
								.Message = "AssetForge factory retired before target loading."};
						if (!Loaded || !ExistingTarget)
							return Complete(FailureOutcome(std::move(Diagnostics),
								std::format("Replacement target '{}' is unavailable: {}",
									Node.Destination.ToString(), Loaded.Message)));
					}
					auto Candidate = Invocation && Product.AssetBuilder.GetAssetBuilder()
						? Product.AssetBuilder.GetAssetBuilder()->MaterializeCandidate(
							Node, std::move(Product.Product), Diagnostics)
						: nullptr;
					if (!Candidate || !Candidate->GetAsset() || !Candidate->GetPackage())
					{
						Abandon(Prepared);
						return Complete(FailureOutcome(std::move(Diagnostics),
							std::format("Candidate '{}' failed validation.", Node.StableIdentity)));
					}
					FPreparedImportOutput Output{
						.AssetBuilder = std::move(Product.AssetBuilder),
						.Candidate = std::move(Candidate),
						.ExistingTarget = ExistingTarget,
						.NodeIndex = Product.NodeIndex};
					Prepared.push_back(std::move(Output));
				}

				auto FindPreparedObject = [&](std::string_view Identity, bool bProspective)
					-> DObject* {
					const auto It = std::ranges::find_if(Prepared,
						[&](const FPreparedImportOutput& Output) {
							return Nodes[Output.NodeIndex].StableIdentity == Identity;
						});
					if (It == Prepared.end()) return nullptr;
					return bProspective ? It->Candidate->GetAsset() :
						(It->ExistingTarget ? It->ExistingTarget : It->Candidate->GetAsset());
				};
				const FMaterializationContext MaterializationContext{
					.ExistingTarget = [&](std::string_view Identity) {
						return FindPreparedObject(Identity, false); },
					.ProspectiveObject = [&](std::string_view Identity) {
						return FindPreparedObject(Identity, true); }};
				for (FPreparedImportOutput& Output : Prepared)
				{
					const FBuildNode& Node = Nodes[Output.NodeIndex];
					auto Invocation = Output.AssetBuilder.TryEnter();
					if (!Invocation || !Output.AssetBuilder.GetAssetBuilder()
						|| !Output.AssetBuilder.GetAssetBuilder()->ResolveCandidateDependencies(
							Node, *Output.Candidate, MaterializationContext, Diagnostics)
						|| !Output.Candidate->Validate(Diagnostics))
					{
						std::string Message = std::format(
							"Candidate '{}' dependency binding or validation failed.",
							Node.StableIdentity);
						if (!Diagnostics.empty() && !Diagnostics.back().Message.empty())
							Message = Diagnostics.back().Message;
						Abandon(Prepared);
						return Complete(FailureOutcome(std::move(Diagnostics),
							std::move(Message)));
					}
				}

				if (!Context.EnterFinalization())
				{
					Abandon(Prepared);
					return Complete(FailureOutcome(std::move(Diagnostics),
						"AssetForge import was canceled before finalization.", true));
				}
				std::lock_guard PublicationLock(GetImportPublicationMutex());
				if (GetImportService().GetComponentRevision() != Products.Graphs.RegistryRevision)
				{
					Abandon(Prepared);
					return Complete(FailureOutcome(std::move(Diagnostics),
						"AssetForge registry changed before publication."));
				}
				std::vector<DPackage*> Packages;
				std::vector<std::pair<DPackage*, bool>> PackageDirtyStates;
				auto RememberPackageDirtyState = [&](DPackage* Package) {
					if (std::ranges::none_of(PackageDirtyStates,
						[&](const auto& Entry) { return Entry.first == Package; }))
						PackageDirtyStates.emplace_back(Package, Package->IsDirty());
				};
				for (FPreparedImportOutput& Output : Prepared)
				{
					const FBuildNode& Node = Nodes[Output.NodeIndex];
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
						auto Invocation = Output.AssetBuilder.TryEnter();
						if (!Invocation || !Output.AssetBuilder.GetAssetBuilder()
							|| !Output.AssetBuilder.GetAssetBuilder()->PublishImportedState(
								*Output.ExistingTarget, *Output.Candidate, Diagnostics))
						{
							Abandon(Prepared, true);
							return Complete(FailureOutcome(std::move(Diagnostics),
								"AssetBuilder failed to publish completed imported state."));
						}
						Packages.push_back(Output.ExistingTarget->GetPackage());
					}
				}
				FImportPersistenceResult Persistence;
				if (bPersistAuthoredPackages)
				{
					Asset::FAssetBundleSaveOptions SaveOptions = Request.SaveOptions;
					if (!Packages.empty()) SaveOptions.RootPackage = Packages.back();
					FXxHash128Builder AuthoredFingerprintBuilder;
					for (const FPreparedImportOutput& Output : Prepared)
					{
						const std::string Fingerprint =
							Output.Candidate->GetAuthoredFingerprint();
						const uint64 Size = Fingerprint.size();
						AuthoredFingerprintBuilder.UpdateValue(Size);
						AuthoredFingerprintBuilder.Update(Fingerprint);
					}
					Products.Graphs.Provenance.AuthoredOutputFingerprint =
						AuthoredFingerprintBuilder.Finalize().ToString();
					for (FPreparedImportOutput& Output : Prepared)
					{
						const FBuildNode& Node = Nodes[Output.NodeIndex];
						DObject* PublishedAsset = Node.Policy == EImportOutputPolicy::Create
							? Output.Candidate->GetAsset() : Output.ExistingTarget;
						auto Invocation = Output.AssetBuilder.TryEnter();
						if (!PublishedAsset || !Invocation
							|| !Output.AssetBuilder.GetAssetBuilder()
							|| !Output.AssetBuilder.GetAssetBuilder()->ApplyProvenance(
								*PublishedAsset, Products.Graphs.Provenance, Diagnostics))
						{
							Abandon(Prepared, true);
							return Complete(FailureOutcome(std::move(Diagnostics),
								std::format("AssetBuilder '{}' failed to persist AssetForge provenance.",
									Node.BuilderId)));
						}
					}
					const Asset::FAssetResult Saved =
						Asset::SavePackagesAtomically(Packages, SaveOptions);
					if (!Saved)
					{
						Diagnostics.push_back({
							.Severity = EImportDiagnosticSeverity::Warning,
							.Category = EImportDiagnosticCategory::PersistenceFailure,
							.Identity = "Durin.AssetForge.Diagnostic.PersistenceFailed",
							.Phase = "Persistence",
							.Message = Saved.Message});
						Persistence = {
							.State = EImportPersistenceState::Failed,
							.Diagnostic = Saved.Message};
					}
					else Persistence.State = EImportPersistenceState::Succeeded;
				}
				else
				{
					if (Request.ExistingProvenance)
					{
						FImportProvenance RecoveryProvenance = Products.Graphs.Provenance;
						RecoveryProvenance.AuthoredOutputFingerprint =
							Request.ExistingProvenance->AuthoredOutputFingerprint;
						for (FPreparedImportOutput& Output : Prepared)
						{
							auto Invocation = Output.AssetBuilder.TryEnter();
							if (!Output.ExistingTarget || !Invocation
								|| !Output.AssetBuilder.GetAssetBuilder()
								|| !Output.AssetBuilder.GetAssetBuilder()->ApplyProvenance(
									*Output.ExistingTarget, RecoveryProvenance,
									Diagnostics))
							{
								Abandon(Prepared, true);
								return Complete(FailureOutcome(std::move(Diagnostics),
									"AssetForge recovery failed to restore existing provenance."));
							}
						}
					}
					for (FPreparedImportOutput& Output : Prepared)
					{
						DPackage* Package = Output.ExistingTarget->GetPackage();
						const auto DirtyState = std::ranges::find(
							PackageDirtyStates, Package, &std::pair<DPackage*, bool>::first);
						if (DirtyState != PackageDirtyStates.end() && DirtyState->second)
							continue;
						auto Invocation = Output.AssetBuilder.TryEnter();
						if (Invocation && Output.AssetBuilder.GetAssetBuilder()
							&& !Output.AssetBuilder.GetAssetBuilder()->HasAuthoredRecoveryChanges(
								*Output.ExistingTarget, *Output.Candidate))
							Package->ClearDirty();
					}
				}

				FImportResult Result;
				Result.Outcome.State = EImportOperationState::Succeeded;
				Result.Outcome.Diagnostics = std::move(Diagnostics);
				Result.Provenance = Products.Graphs.Provenance;
				Result.Inspection = Products.Graphs.Inspection;
				Result.Persistence = std::move(Persistence);
				for (FPreparedImportOutput& Output : Prepared)
				{
					const FBuildNode& Node = Nodes[Output.NodeIndex];
					Result.Outcome.PublishedAssetIdentities.push_back(Node.Destination.ToString());
				}
				Abandon(Prepared, true);
				for (FPreparedImportOutput& Output : Prepared)
					Output.Candidate.reset();
				StoreResult(Result);
				State = EState::Terminal;
				return FImportJobEditorAdvance::Complete(Result.Outcome);
			}

			struct FImportDiagnosticJobValue final : IImportJobValue
			{
				explicit FImportDiagnosticJobValue(
					std::vector<FImportDiagnostic> InDiagnostics)
					: Diagnostics(std::move(InDiagnostics)) {}
				std::vector<FImportDiagnostic> Diagnostics;
			};

			auto WorkerFailure(std::vector<FImportDiagnostic> Diagnostics,
				std::string Message) const -> FImportJobWorkerResult
			{
				FinalizeImportDiagnostics(Diagnostics, "interchange-worker");
				for (auto It = Diagnostics.rbegin(); It != Diagnostics.rend(); ++It)
				{
					if (It->Severity == EImportDiagnosticSeverity::Error && !It->Message.empty())
					{
						Message = It->Message;
						break;
					}
				}
				return {.bSucceeded = false, .Diagnostic = std::move(Message),
					.Value = std::make_unique<FImportDiagnosticJobValue>(
						std::move(Diagnostics))};
			}

			auto CompensateWorkerFailureOnEditor(FImportJobEditorContext&,
				FImportJobWorkerResult Result) -> FImportOutcome override
			{
				std::vector<FImportDiagnostic> Diagnostics;
				if (auto* Value = dynamic_cast<FImportDiagnosticJobValue*>(Result.Value.get()))
					Diagnostics = std::move(Value->Diagnostics);
				FImportOutcome Outcome = FailureOutcome(std::move(Diagnostics),
					std::move(Result.Diagnostic), Result.bCanceled);
				FImportResult ImportResult;
				ImportResult.Outcome = Outcome;
				StoreResult(ImportResult);
				State = EState::Terminal;
				return Outcome;
			}

			auto Complete(FImportOutcome Outcome) -> FImportJobEditorAdvance
			{
				FImportResult Result;
				Result.Outcome = Outcome;
				StoreResult(Result);
				State = EState::Terminal;
				return FImportJobEditorAdvance::Complete(std::move(Outcome));
			}

			auto StoreResult(const FImportResult& Result) -> void
			{
				FImportCompletion Completion;
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

			static auto Abandon(std::vector<FPreparedImportOutput>& Outputs,
				bool bOnlyReplacementCandidates = false) -> void
			{
				std::vector<FAssetPath> Packages;
				for (FPreparedImportOutput& Output : Outputs)
				{
					if (!Output.Candidate
						|| (bOnlyReplacementCandidates && Output.Candidate->IsNewAsset()))
						continue;
					DPackage* Package = Output.Candidate->DetachPackageForAbandon();
					FAssetPath Path;
					if (Package && FAssetPath::TryCreate(Package->GetPackagePath(), Path))
						Packages.push_back(std::move(Path));
					Output.Candidate.reset();
				}
				std::ranges::sort(Packages, {}, &FAssetPath::ToString);
				Packages.erase(std::unique(Packages.begin(), Packages.end()), Packages.end());
				for (const FAssetPath& Path : Packages)
					(void)Asset::UnloadPackage(
						Path, Asset::EAssetPackageUnloadPolicy::DiscardUnsaved);
			}

			FImportRequest Request;
			std::shared_ptr<FImportResultState> ResultState;
			EState State = EState::Start;
			uint8 WorkerRound = 0;
		};
	}

	auto FImportHandle::TryGetResult(
		FImportResult& OutResult) const -> bool
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

	auto FImportService::SubmitImport(
		FImportRequest Request,
		std::string_view Title,
		FImportCompletion Completion) -> FImportHandle
	{
		auto State = std::make_shared<FImportResultState>();
		State->Completion = std::move(Completion);
		auto Job = std::make_unique<FGraphImportJob>(std::move(Request), State);
		FImportOperationHandle Operation = SubmitImportJob(std::move(Job),
			Title.empty() ? "AssetForge import" : Title);
		return FImportHandle(std::move(Operation), std::move(State));
	}

	auto FImportService::RunImportInline(
		FImportRequest Request,
		std::string_view Title) -> FImportResult
	{
		auto State = std::make_shared<FImportResultState>();
		auto Job = std::make_unique<FGraphImportJob>(std::move(Request), State);
		const FImportOutcome Outcome = RunImportJobInline(std::move(Job), Title);
		std::lock_guard Lock(State->Mutex);
		if (State->Result) return *State->Result;
		return {.Outcome = Outcome};
	}
}
