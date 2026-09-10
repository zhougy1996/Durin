#include "Threading/TaskComposition.h"
#include "Asset/OfflinePreparation.h"
#include "Asset/Asset.h"
#include "Asset/CookDependencies.h"
#include "DObject/Package.h"
#include "Materials/MaterialCompileLifecycle.h"

#include "Asset/AssetCompilingManager.h"
#include "CoreGlobals.h"
#include "DObject/DObjectArray.h"
#include "DObject/ObjectLifecycle.h"
#include "Logging/LogMacros.h"
#include "Materials/Material.h"
#include "Shader/ShaderCompilerCore.h"
#include "Threading/RunnableThread.h"

#include <atomic>
#include <deque>
#include <mutex>
#include <unordered_map>

namespace Durin
{
	namespace
	{
		constexpr uint64 MaterialCompileMaxRetainedProgramBytes =
			256ull * 1024ull * 1024ull;

		auto CheckMaterialCompileGameThread() -> void
		{
			if (GIsGameThreadIdInitialized) CheckGameThread();
		}

		auto AdvanceNonzero(uint64 Value) -> uint64
		{
			return Value == std::numeric_limits<uint64>::max() ? 1 : Value + 1;
		}

		auto SameOwner(FObjectHandle Left, FObjectHandle Right) -> bool
		{
			return Left.Index == Right.Index
				&& Left.Generation == Right.Generation;
		}

		auto EstimateRequestBytes(const FMaterialCompileRequest& Request) -> uint64
		{
			uint64 Bytes = sizeof(Request) + Request.AssetPath.size()
				+ Request.Target.size()
				+ Request.CompilerInput.Environment.CompilerIdentity.size();
			for (const FMaterialProgramNode& Node : Request.CompilerInput.Program.Nodes)
				Bytes += sizeof(Node) + Node.DisplayName.size()
					+ Node.Inputs.size() * sizeof(FMaterialProgramLink);
			Bytes += Request.CompilerInput.Parameters.size()
				* sizeof(FMaterialCompilerParameterDeclaration);
			for (const FMaterialCompilerDependency& Dependency
				: Request.CompilerInput.Environment.Dependencies)
				Bytes += sizeof(Dependency) + Dependency.VirtualPath.size();
			return Bytes;
		}

		auto EstimateResultBytes(const FMaterialCompilerResult& Result) -> uint64
		{
			uint64 Bytes = sizeof(Result) + Result.GeneratedSource.size()
				+ Result.CompilerIdentity.size() + Result.Target.size()
				+ Result.IR.Nodes.size() * sizeof(FMaterialIRNode)
				+ Result.ActiveParameters.size() * sizeof(FMaterialCompilerParameterDeclaration)
				+ Result.Layout.Fields.size() * sizeof(FMaterialRenderField)
				+ sizeof(Result.IR.SurfaceRoot);
			for (const FMaterialIRNode& Node : Result.IR.Nodes)
				Bytes += Node.Inputs.size() * sizeof(uint32);
			for (const FMaterialCompilerDependency& Dependency : Result.Dependencies)
				Bytes += sizeof(Dependency) + Dependency.VirtualPath.size();
			for (const FCompiledShader& Shader : Result.CompiledShaders)
			{
				Bytes += sizeof(Shader) + Shader.SourceEntryPoint.size()
					+ Shader.BinaryEntryPoint.size() + Shader.DebugName.size();
				if (Shader.Code) Bytes += Shader.Code->size();
				for (const FShaderResourceBinding& Binding
					: Shader.Reflection.ResourceBindings)
					Bytes += sizeof(Binding) + Binding.Name.size();
				Bytes += Shader.Reflection.PushConstantRanges.size()
					* sizeof(FPushConstantRange);
			}
			for (const FMaterialProgramDiagnostic& Diagnostic : Result.Diagnostics)
				Bytes += sizeof(Diagnostic) + Diagnostic.Message.size();
			return Bytes;
		}

		auto MapProgramCategory(EMaterialProgramDiagnosticCategory Category)
			-> EMaterialCompileResultCategory
		{
			switch (Category)
			{
			case EMaterialProgramDiagnosticCategory::Schema:
			case EMaterialProgramDiagnosticCategory::Bounds:
			case EMaterialProgramDiagnosticCategory::Graph:
			case EMaterialProgramDiagnosticCategory::Type:
				return EMaterialCompileResultCategory::Validation;
			case EMaterialProgramDiagnosticCategory::Normalization:
				return EMaterialCompileResultCategory::Normalization;
			case EMaterialProgramDiagnosticCategory::Generation:
				return EMaterialCompileResultCategory::Generation;
			case EMaterialProgramDiagnosticCategory::Dependency:
				return EMaterialCompileResultCategory::Dependency;
			case EMaterialProgramDiagnosticCategory::Compile:
				return EMaterialCompileResultCategory::Compile;
			case EMaterialProgramDiagnosticCategory::Reflection:
				return EMaterialCompileResultCategory::Reflection;
			case EMaterialProgramDiagnosticCategory::Binding:
				return EMaterialCompileResultCategory::Binding;
			}
			return EMaterialCompileResultCategory::Compile;
		}

		auto MakeDiagnostic(
			const FMaterialCompileRequest& Request,
			EMaterialCompileResultCategory Category,
			FMaterialProgramDiagnostic Source,
			bool bLastKnownGood) -> FMaterialCompileDiagnostic
		{
			if (Source.Message.size() > MaterialProgramMaxDiagnosticMessageBytes)
				Source.Message.resize(MaterialProgramMaxDiagnosticMessageBytes);
			std::string AssetPath = Request.AssetPath;
			if (AssetPath.size() > MaterialProgramMaxDiagnosticMessageBytes)
				AssetPath.resize(MaterialProgramMaxDiagnosticMessageBytes);
			return {
				.Category = Category,
				.Source = std::move(Source),
				.AssetPath = std::move(AssetPath),
				.ProgramIdentity = Request.ProgramIdentity,
				.Generation = Request.Generation,
				.bLastKnownGoodDisplayed = bLastKnownGood,
			};
		}

		struct FMaterialCompileFlightKey
		{
			FMaterialProgramIdentity Identity;
			bool bForceRecompile = false;

			auto operator==(const FMaterialCompileFlightKey&) const -> bool = default;
		};

		struct FMaterialCompileFlightKeyHash
		{
			auto operator()(const FMaterialCompileFlightKey& Key) const
				noexcept -> size_t
			{
				return std::hash<FMaterialProgramIdentity>{}(Key.Identity)
					^ (Key.bForceRecompile ? size_t{0x9e3779b9} : size_t{});
			}
		};

		struct FMaterialCompileFlight
		{
			FMaterialCompileFlightKey Key;
			FMaterialCompilerInput Input;
			std::vector<FMaterialCompileRequest> Consumers;
			FTaskCancellationSource Cancellation;
			FTaskHandle Task;
			std::atomic_uint64_t TaskId = 0;
		};

		class FMaterialCompilationState
		{
		public:
			auto Initialize() -> bool
			{
				CheckMaterialCompileGameThread();
				std::scoped_lock Lock(Mutex);
				if (bAcceptingRequests) return true;
				if (!IsTaskSchedulerRunning()) return false;
				Scope = CreateTaskScope();
				if (!Scope.IsValid()) return false;
				Attribution = RegisterTaskAttribution("Engine", "MaterialCompile");
				bAcceptingRequests = true;
				return true;
			}

			auto Shutdown() -> void
			{
				CheckMaterialCompileGameThread();
				{
					std::scoped_lock Lock(Mutex);
					if (!Scope.IsValid())
					{
						bAcceptingRequests = false;
						return;
					}
					bAcceptingRequests = false;
					for (auto& [Key, Flight] : Flights)
						Flight->Cancellation.RequestCancellation();
				}
				Scope.Close(ETaskScopeCloseMode::Cancel);
				const ETaskScopeWaitResult WaitResult = Scope.Wait();
				requiref(WaitResult == ETaskScopeWaitResult::Quiescent, "Material compile scope must drain before owner release.");
				std::scoped_lock Lock(Mutex);
				Diagnostics.CanceledRequests += OutstandingConsumers;
				Flights.clear();
				Mailbox.clear();
				OutstandingConsumers = 0;
				RetainedPrograms.clear();
				RetentionOrder.clear();
				RetainedProgramBytes = 0;
				Scope = {};
			}

			auto StopAdmission() -> void
			{
				CheckMaterialCompileGameThread();
				std::scoped_lock Lock(Mutex);
				bAcceptingRequests = false;
			}

			auto Submit(FMaterialCompileRequest Request) -> EMaterialCompileState
			{
				CheckMaterialCompileGameThread();
				if (EstimateRequestBytes(Request) > MaterialCompileMaxRequestBytes)
				{
					++Diagnostics.RejectedRequests;
					return EMaterialCompileState::Rejected;
				}

				std::shared_ptr<FMaterialCompileFlight> NewFlight;
				{
					std::scoped_lock Lock(Mutex);
					SupersedeOwnerLocked(Request.Owner);
					if (!bAcceptingRequests || !Scope.IsValid())
					{
						++Diagnostics.RejectedRequests;
						return EMaterialCompileState::Rejected;
					}

					if (OutstandingConsumers >= MaterialCompileMaxConsumers)
						return EMaterialCompileState::Deferred;

					if (!Request.bForceRecompile)
					{
						if (const auto RetainedIt = RetainedPrograms.find(
								Request.ProgramIdentity);
							RetainedIt != RetainedPrograms.end())
						{
							++Diagnostics.AcceptedRequests;
							++Diagnostics.RetainedHits;
							++Diagnostics.CompletedRequests;
							++OutstandingConsumers;
							Mailbox.push_back(MakeReadyResult(
								Request, RetainedIt->second,
								EMaterialCompileCacheOutcome::RetainedHit, 0));
							return EMaterialCompileState::Pending;
						}
					}

					const FMaterialCompileFlightKey Key{
						.Identity = Request.ProgramIdentity,
						.bForceRecompile = Request.bForceRecompile};
					if (const auto Existing = Flights.find(Key);
						Existing != Flights.end())
					{
						Request.bSingleFlightConsumer = true;
						Existing->second->Consumers.push_back(std::move(Request));
						++Diagnostics.AcceptedRequests;
						++Diagnostics.SingleFlightConsumers;
						++OutstandingConsumers;
						return EMaterialCompileState::Pending;
					}
					if (Flights.size() >= MaterialCompileMaxConcurrentRequests)
						return EMaterialCompileState::Deferred;

					NewFlight = std::make_shared<FMaterialCompileFlight>();
					NewFlight->Key = Key;
					NewFlight->Input = Request.CompilerInput;
					NewFlight->Consumers.push_back(std::move(Request));
					Flights.emplace(Key, NewFlight);
					++Diagnostics.AcceptedRequests;
					++OutstandingConsumers;
				}

				Tasks::FTaskExecutionOptions Options{
					.Cancellation = NewFlight->Cancellation.GetToken(),
					.Attribution = Attribution,
					.Scope = Scope.GetToken(),
				};
				NewFlight->Task = Tasks::LaunchTask(
					"MaterialCompile",
					[this, Flight = NewFlight](const FTaskCancellationToken& Token) {
						if (Token.IsCancellationRequested())
						{
							CompleteFlight(Flight, {}, EMaterialCompileState::Canceled);
							return;
						}
						FMaterialCompilerResult Compiled = CompileMaterialProgram(
							Flight->Input, Flight->Key.bForceRecompile);
						if (Token.IsCancellationRequested())
						{
							CompleteFlight(Flight, {}, EMaterialCompileState::Canceled);
							return;
						}
						CompleteFlight(Flight, std::move(Compiled),
							EMaterialCompileState::Ready);
					}, Options).GetCompletion().GetTaskHandle();
				NewFlight->TaskId.store(NewFlight->Task.GetTaskId(), std::memory_order_release);
				return EMaterialCompileState::Running;
			}

			auto CancelOwner(FObjectHandle Owner) -> bool
			{
				CheckMaterialCompileGameThread();
				std::scoped_lock Lock(Mutex);
				return CancelOwnerLocked(Owner, false);
			}

			auto Pump(uint32 MaximumCount = std::numeric_limits<uint32>::max())
				-> std::vector<FMaterialCompileResult>
			{
				CheckMaterialCompileGameThread();
				// A canceled-before-entry body or an exception may never publish its mailbox.
				std::vector<std::shared_ptr<FMaterialCompileFlight>> TerminalFlights;
				{
					std::scoped_lock Lock(Mutex);
					for (const auto& [Key, Flight] : Flights)
						if (Flight->Task.IsValid() && Flight->Task.IsComplete())
							TerminalFlights.push_back(Flight);
				}
				for (const auto& Flight : TerminalFlights)
					CompleteFlight(Flight, {}, Flight->Task.GetState() == ETaskState::Canceled
						? EMaterialCompileState::Canceled : EMaterialCompileState::Failed);
				std::vector<FMaterialCompileResult> Results;
				std::scoped_lock Lock(Mutex);
				Results.reserve(std::min<size_t>(Mailbox.size(), MaximumCount));
				while (!Mailbox.empty() && Results.size() < MaximumCount)
				{
					Results.push_back(std::move(Mailbox.front()));
					Mailbox.pop_front();
				}
				return Results;
			}

			auto HasOwner(FObjectHandle Owner) const -> bool
			{
				std::scoped_lock Lock(Mutex);
				for (const auto& [Key, Flight] : Flights)
					if (std::ranges::any_of(Flight->Consumers,
						[Owner](const FMaterialCompileRequest& Consumer) {
							return SameOwner(Consumer.Owner, Owner);
						})) return true;
				return std::ranges::any_of(Mailbox,
					[Owner](const FMaterialCompileResult& Result) {
						return SameOwner(Result.Owner, Owner);
					});
			}

			auto ConsumeOutstanding() -> void
			{
				std::scoped_lock Lock(Mutex);
				check(OutstandingConsumers > 0);
				--OutstandingConsumers;
			}

			auto GetDiagnostics() const -> FMaterialCompilationDiagnostics
			{
				std::scoped_lock Lock(Mutex);
				FMaterialCompilationDiagnostics Result = Diagnostics;
				Result.bAcceptingRequests = bAcceptingRequests;
				Result.InFlightCount = static_cast<uint32>(Flights.size());
				Result.OutstandingConsumerCount = OutstandingConsumers;
				Result.PendingPublicationCount = static_cast<uint32>(Mailbox.size());
				Result.RetainedProgramCount =
					static_cast<uint32>(RetainedPrograms.size());
				Result.RetainedProgramBytes = RetainedProgramBytes;
				return Result;
			}

			auto IsAccepting() const -> bool
			{
				std::scoped_lock Lock(Mutex);
				return bAcceptingRequests;
			}

		private:
			auto MakeReadyResult(
				const FMaterialCompileRequest& Request,
				std::shared_ptr<const FMaterialCompilerResult> Program,
				EMaterialCompileCacheOutcome CacheOutcome,
				uint64 TaskId) -> FMaterialCompileResult
			{
				return {
					.Owner = Request.Owner,
					.AuthoredRevision = Request.AuthoredRevision,
					.Generation = Request.Generation,
					.DependencyRevision = Request.DependencyRevision,
					.ParentChainRevision = Request.ParentChainRevision,
					.ProgramIdentity = Request.ProgramIdentity,
					.StaticProperties = Request.CompilerInput.StaticProperties,
					.Target = Request.Target,
					.State = EMaterialCompileState::Ready,
					.Category = EMaterialCompileResultCategory::None,
					.CacheOutcome = CacheOutcome,
					.TaskId = TaskId,
					.CompiledProgram = std::move(Program),
				};
			}

			auto MakeTerminalResult(
				const FMaterialCompileRequest& Request,
				EMaterialCompileState State,
				EMaterialCompileResultCategory Category,
				std::string Message) -> FMaterialCompileResult
			{
				FMaterialCompileResult Result{
					.Owner = Request.Owner,
					.AuthoredRevision = Request.AuthoredRevision,
					.Generation = Request.Generation,
					.DependencyRevision = Request.DependencyRevision,
					.ParentChainRevision = Request.ParentChainRevision,
					.ProgramIdentity = Request.ProgramIdentity,
					.StaticProperties = Request.CompilerInput.StaticProperties,
					.Target = Request.Target,
					.State = State,
					.Category = Category,
				};
				Result.Diagnostics.push_back(MakeDiagnostic(
					Request, Category,
					{.Category = EMaterialProgramDiagnosticCategory::Compile,
					 .Message = std::move(Message)}, false));
				return Result;
			}

			auto CompleteFlight(
				const std::shared_ptr<FMaterialCompileFlight>& Flight,
				FMaterialCompilerResult Compiled,
				EMaterialCompileState CompletionState) -> void
			{
				std::scoped_lock Lock(Mutex);
				const auto It = Flights.find(Flight->Key);
				if (It == Flights.end() || It->second != Flight) return;
				Flights.erase(It);
				const uint64 TaskId = Flight->TaskId.load(
					std::memory_order_acquire);
				std::shared_ptr<const FMaterialCompilerResult> Program;
				EMaterialCompileState State = CompletionState;
				EMaterialCompileResultCategory Category =
					EMaterialCompileResultCategory::None;
				if (CompletionState == EMaterialCompileState::Ready)
				{
					if (!Compiled)
					{
						State = EMaterialCompileState::Failed;
						Category = Compiled.Diagnostics.empty()
							? EMaterialCompileResultCategory::Compile
							: MapProgramCategory(Compiled.Diagnostics.front().Category);
					}
					else if (EstimateResultBytes(Compiled) > MaterialCompileMaxResultBytes)
					{
						State = EMaterialCompileState::Failed;
						Category = EMaterialCompileResultCategory::Admission;
						Compiled.Diagnostics = {{
							.Category = EMaterialProgramDiagnosticCategory::Bounds,
							.Message = "Compiled material result exceeds the retained-result byte limit."}};
					}
					else
					{
						Program = std::make_shared<const FMaterialCompilerResult>(
							std::move(Compiled));
						RetainProgram(Flight->Key.Identity, Program);
					}
				}
				else if (CompletionState == EMaterialCompileState::Canceled)
					Category = EMaterialCompileResultCategory::Cancellation;
				if (State == EMaterialCompileState::Ready)
					Diagnostics.CompletedRequests += Flight->Consumers.size();
				else if (State == EMaterialCompileState::Failed)
					Diagnostics.FailedRequests += Flight->Consumers.size();
				else if (State == EMaterialCompileState::Canceled)
					Diagnostics.CanceledRequests += Flight->Consumers.size();

				for (const FMaterialCompileRequest& Consumer : Flight->Consumers)
				{
					if (State == EMaterialCompileState::Ready)
					{
						Mailbox.push_back(MakeReadyResult(
							Consumer, Program,
							Consumer.bSingleFlightConsumer
								? EMaterialCompileCacheOutcome::SingleFlight
								: Flight->Key.bForceRecompile
								? EMaterialCompileCacheOutcome::Forced
								: EMaterialCompileCacheOutcome::Compiled,
							TaskId));
						continue;
					}

					FMaterialCompileResult Result{
						.Owner = Consumer.Owner,
						.AuthoredRevision = Consumer.AuthoredRevision,
						.Generation = Consumer.Generation,
						.DependencyRevision = Consumer.DependencyRevision,
					.ParentChainRevision = Consumer.ParentChainRevision,
						.ProgramIdentity = Consumer.ProgramIdentity,
						.StaticProperties = Consumer.CompilerInput.StaticProperties,
						.Target = Consumer.Target,
						.State = State,
						.Category = Category,
						.TaskId = TaskId,
					};
					if (State == EMaterialCompileState::Canceled)
					{
						Result.Diagnostics.push_back(MakeDiagnostic(
							Consumer, Category,
							{.Category = EMaterialProgramDiagnosticCategory::Compile,
							 .Message = "Material compilation was canceled."}, false));
					}
					else
					{
						for (const FMaterialProgramDiagnostic& Diagnostic
							: Compiled.Diagnostics)
						{
							if (Result.Diagnostics.size()
								>= MaterialProgramMaxDiagnosticCount) break;
							Result.Diagnostics.push_back(MakeDiagnostic(
								Consumer, MapProgramCategory(Diagnostic.Category),
								Diagnostic, false));
						}
					}
					Mailbox.push_back(std::move(Result));
				}
			}

			auto RetainProgram(
				FMaterialProgramIdentity Identity,
				const std::shared_ptr<const FMaterialCompilerResult>& Program) -> void
			{
				const uint64 Bytes = EstimateResultBytes(*Program);
				if (const auto Existing = RetainedPrograms.find(Identity);
					Existing != RetainedPrograms.end()) return;
				while (!RetentionOrder.empty()
					&& (RetainedPrograms.size() >= MaterialCompileMaxResidentPrograms
						|| RetainedProgramBytes + Bytes
							> MaterialCompileMaxRetainedProgramBytes))
				{
					const FMaterialProgramIdentity Oldest = RetentionOrder.front();
					RetentionOrder.pop_front();
					const auto Old = RetainedPrograms.find(Oldest);
					if (Old == RetainedPrograms.end()) continue;
					RetainedProgramBytes -= EstimateResultBytes(*Old->second);
					RetainedPrograms.erase(Old);
				}
				RetainedPrograms.emplace(Identity, Program);
				RetentionOrder.push_back(Identity);
				RetainedProgramBytes += Bytes;
			}

			auto SupersedeOwnerLocked(FObjectHandle Owner) -> void
			{
				(void)CancelOwnerLocked(Owner, true);
			}

			auto CancelOwnerLocked(FObjectHandle Owner, bool bSuperseded) -> bool
			{
				bool bFound = false;
				for (auto& [Key, Flight] : Flights)
				{
					const size_t PreviousCount = Flight->Consumers.size();
					std::erase_if(Flight->Consumers,
						[Owner](const FMaterialCompileRequest& Consumer) {
							return SameOwner(Consumer.Owner, Owner);
						});
					const size_t Removed = PreviousCount - Flight->Consumers.size();
					if (Removed == 0) continue;
					bFound = true;
					OutstandingConsumers -= static_cast<uint32>(Removed);
					if (bSuperseded) Diagnostics.SupersededRequests += Removed;
					else Diagnostics.CanceledRequests += Removed;
					if (Flight->Consumers.empty()) Flight->Cancellation.RequestCancellation();
				}
				for (auto It = Mailbox.begin(); It != Mailbox.end();)
				{
					if (!SameOwner(It->Owner, Owner))
					{
						++It;
						continue;
					}
					bFound = true;
					It = Mailbox.erase(It);
					--OutstandingConsumers;
					if (bSuperseded) ++Diagnostics.SupersededRequests;
					else ++Diagnostics.CanceledRequests;
				}
				return bFound;
			}

			mutable std::mutex Mutex;
			FTaskScope Scope;
			FTaskAttribution Attribution;
			bool bAcceptingRequests = false;
			uint32 OutstandingConsumers = 0;
			std::unordered_map<FMaterialCompileFlightKey,
				std::shared_ptr<FMaterialCompileFlight>,
				FMaterialCompileFlightKeyHash> Flights;
			std::deque<FMaterialCompileResult> Mailbox;
			std::unordered_map<FMaterialProgramIdentity,
				std::shared_ptr<const FMaterialCompilerResult>> RetainedPrograms;
			std::deque<FMaterialProgramIdentity> RetentionOrder;
			uint64 RetainedProgramBytes = 0;
			FMaterialCompilationDiagnostics Diagnostics;
		};


		std::atomic_uint8_t GPendingShaderReloadMode = 0;
		auto CancelMaterialCompileDomain(DMaterialInterface& Material) -> bool;

		auto GetDeferredMaterialOwners() -> std::vector<DMaterialInterface*>
		{
			std::vector<DMaterialInterface*> Result;
			for (DObject* Object : GDObjectArray.Snapshot(EObjectQueryScope::LiveOnly))
				if (auto* Material = Cast<DMaterialInterface>(Object); IsValid(Material)
					&& Material->GetMaterialCompileStatus().State == EMaterialCompileState::Deferred)
					Result.push_back(Material);
			return Result;
		}

		auto PumpMaterialCompileResultsDetailed(
			FMaterialCompilationState& State, uint32 MaximumCount)
			-> FAssetCompileProcessResult
		{
			CheckMaterialCompileGameThread();
			const uint8 ReloadMode = GPendingShaderReloadMode.exchange(
				0, std::memory_order_acq_rel);
			if (ReloadMode != 0)
			{
				const std::vector<DObject*> Objects =
					GDObjectArray.Snapshot(EObjectQueryScope::LiveOnly);
				for (DObject* Object : Objects)
					if (auto* Material = Cast<DMaterialInterface>(Object); IsValid(Material))
					{
						const FMaterialRenderProxyRef Proxy =
							Material->GetMaterialRenderProxy();
						if (Proxy && Proxy->GetRefCount() > 2)
							RequestMaterialRecompile(*Material, ReloadMode > 1);
					}
			}

			FAssetCompileProcessResult Processed;
			std::vector<FMaterialCompileResult> Results =
				State.Pump(MaximumCount);
			Processed.ProcessedCompletionCount = static_cast<uint32>(Results.size());
			for (FMaterialCompileResult& Result : Results)
			{
				DObject* Object = ResolveObjectHandle(Result.Owner);
				if (auto* Material = Cast<DMaterialInterface>(Object); IsValid(Material)
					&& Private::FMaterialCompilationLifecycle::Admit(
						*Material, std::move(Result)))
					Processed.SuccessfullyCompiledAssets.emplace_back(Material);
				State.ConsumeOutstanding();
			}
			static uint32 RetryCursor = 0;
			auto Deferred = GetDeferredMaterialOwners();
			std::ranges::sort(Deferred, {}, [](DMaterialInterface* Material) {
				return MakeObjectHandle(Material).Index;
			});
			const auto Start = std::ranges::upper_bound(Deferred, RetryCursor, {},
				[](DMaterialInterface* Material) { return MakeObjectHandle(Material).Index; });
			std::rotate(Deferred.begin(), Start, Deferred.end());
			uint32 Retried = 0;
			for (auto* Material : Deferred)
			{
				if (!State.IsAccepting())
					Private::FMaterialCompilationLifecycle::MarkCanceled(*Material);
				else
					Private::FMaterialCompilationLifecycle::RetryDeferred(*Material);
				RetryCursor = MakeObjectHandle(Material).Index;
				if (++Retried == MaterialCompileMaxConsumers) break;
			}
			return Processed;
		}

		class FMaterialCompilingManager final : public IAssetCompilingManager
		{
		public:
			auto GetState() -> FMaterialCompilationState& { return State; }
			auto GetState() const -> const FMaterialCompilationState& { return State; }
			auto Start(std::string* OutError) -> bool override
			{
				if (State.Initialize())
				{
					if (OutError) OutError->clear();
					return true;
				}
				if (OutError) *OutError = "Material compile task scope could not start.";
				return false;
			}
			auto StopAdmission() -> void override
			{
				State.StopAdmission();
				for (auto* Material : GetDeferredMaterialOwners())
					Private::FMaterialCompilationLifecycle::MarkCanceled(*Material);
			}
			auto GetNumRemainingAssets() const -> uint64 override
			{
				return State.GetDiagnostics().OutstandingConsumerCount
					+ GetDeferredMaterialOwners().size();
			}
			auto ProcessAsyncTasks(const FAssetCompileProcessParams& Params)
				-> FAssetCompileProcessResult override
			{
				return PumpMaterialCompileResultsDetailed(
					State, Params.MaximumCompletions);
			}
			auto FinishCompilationForObjects(std::span<DObject* const> Objects)
				-> FAssetCompileProcessResult override
			{
				FAssetCompileProcessResult Aggregate;
				std::vector<FObjectHandle> Owners;
				for (DObject* Object : Objects)
					if (auto* Material = Cast<DMaterialInterface>(Object); IsValid(Material))
						Owners.push_back(MakeObjectHandle(Material));
				while (std::ranges::any_of(Owners, [this](FObjectHandle Owner) {
					auto* Material = Cast<DMaterialInterface>(ResolveObjectHandle(Owner));
					return State.HasOwner(Owner) || (IsValid(Material)
						&& Material->GetMaterialCompileStatus().State == EMaterialCompileState::Deferred);
				}))
				{
					auto Item = PumpMaterialCompileResultsDetailed(
						State, std::numeric_limits<uint32>::max());
					Aggregate.ProcessedCompletionCount += Item.ProcessedCompletionCount;
					Aggregate.SuccessfullyCompiledAssets.insert(
						Aggregate.SuccessfullyCompiledAssets.end(),
						Item.SuccessfullyCompiledAssets.begin(),
						Item.SuccessfullyCompiledAssets.end());
					if (Item.ProcessedCompletionCount == 0) std::this_thread::yield();
				}
				return Aggregate;
			}
			auto MarkCompilationAsCanceled(std::span<DObject* const> Objects)
				-> void override
			{
				for (DObject* Object : Objects)
					if (auto* Material = Cast<DMaterialInterface>(Object); IsValid(Material))
						CancelMaterialCompileDomain(*Material);
			}
			auto FinishAllCompilation() -> FAssetCompileProcessResult override
			{
				FAssetCompileProcessResult Aggregate;
				while (GetNumRemainingAssets() != 0)
				{
					auto Item = PumpMaterialCompileResultsDetailed(
						State, std::numeric_limits<uint32>::max());
					Aggregate.ProcessedCompletionCount += Item.ProcessedCompletionCount;
					Aggregate.SuccessfullyCompiledAssets.insert(
						Aggregate.SuccessfullyCompiledAssets.end(),
						Item.SuccessfullyCompiledAssets.begin(),
						Item.SuccessfullyCompiledAssets.end());
					if (Item.ProcessedCompletionCount == 0) std::this_thread::yield();
				}
				return Aggregate;
			}
			auto Shutdown() -> void override
			{
				State.Shutdown();
			}

		private:
			FMaterialCompilationState State;
		};

		std::mutex GMaterialCompilingManagerMutex;
		std::weak_ptr<FMaterialCompilingManager> GMaterialCompilingManager;

		auto GetMaterialCompilingManager() -> std::shared_ptr<FMaterialCompilingManager>
		{
			std::lock_guard Lock(GMaterialCompilingManagerMutex);
			return GMaterialCompilingManager.lock();
		}
	}

	namespace Private
	{
		auto FMaterialCompilationLifecycle::Submit(
			DMaterialInterface& Material,
			FMaterialCompilerInput Input,
			bool bForceRecompile) -> bool
		{
			CheckMaterialCompileGameThread();
			const auto Manager = GetMaterialCompilingManager();
			FMaterialCompilationState* Compilation =
				Manager ? &Manager->GetState() : nullptr;
			// Construction precedes DObject handle registration. In a running engine,
			// defer that bootstrap request to PostLoad, an authored edit, or an
			// explicit request instead of performing expensive work on GameThread.
			if (IsObjectHandleNull(MakeObjectHandle(&Material))
				&& Compilation && Compilation->IsAccepting())
			{
				Material.CompilationOwner.MaterialCompileStatus.State =
					EMaterialCompileState::NeverRequested;
				return true;
			}
			const FMaterialNormalizationResult Normalized =
					NormalizeMaterialProgram(Input);
				Material.CompilationOwner.MaterialCompileStatus.RequestGeneration = AdvanceNonzero(
					Material.CompilationOwner.MaterialCompileStatus.RequestGeneration);
				Material.CompilationOwner.MaterialCompileStatus.DependencyRevision =
					GetShaderReloadGeneration();
				Material.CompilationOwner.MaterialCompileStatus.Target = Input.Environment.Target;
				Material.CompilationOwner.MaterialCompileStatus.RequestedIdentity =
					Normalized.Identity;
				Material.CompilationOwner.MaterialCompileStatus.TaskId = 0;
				Material.CompilationOwner.MaterialCompileStatus.CacheOutcome =
					EMaterialCompileCacheOutcome::None;
				Material.CompilationOwner.MaterialCompileStatus.bHasLastKnownGood =
					Material.CompilationOwner.AcceptedGeneration.Program != nullptr;
				Material.CompilationOwner.MaterialCompileStatus.bLastKnownGoodDisplayed =
					Material.CompilationOwner.AcceptedGeneration.Program != nullptr;
				Material.CompilationOwner.MaterialCompileDiagnostics.clear();

				FMaterialCompileRequest Request{
					.Owner = MakeObjectHandle(&Material),
					.AuthoredRevision = Material.CompilationOwner.MaterialCompileStatus.AuthoredRevision,
					.Generation = Material.CompilationOwner.MaterialCompileStatus.RequestGeneration,
					.DependencyRevision = Material.CompilationOwner.MaterialCompileStatus.DependencyRevision,
					.ParentChainRevision = Material.CompilationOwner.MaterialCompileStatus.ParentChainRevision,
					.ProgramIdentity = Normalized.Identity,
					.CompilerInput = std::move(Input),
					.AssetPath = Material.GetObjectPath(),
					.Target = Material.CompilationOwner.MaterialCompileStatus.Target,
					.bForceRecompile = bForceRecompile,
				};

				if (!Normalized)
				{
					Material.CompilationOwner.MaterialCompileStatus.State = EMaterialCompileState::Failed;
					Material.CompilationOwner.MaterialCompileStatus.ResultCategory =
						Normalized.Diagnostics.empty()
							? EMaterialCompileResultCategory::Normalization
							: MapProgramCategory(
								Normalized.Diagnostics.front().Category);
					for (const FMaterialProgramDiagnostic& Diagnostic
						: Normalized.Diagnostics)
					{
						if (Material.CompilationOwner.MaterialCompileDiagnostics.size()
							>= MaterialProgramMaxDiagnosticCount) break;
						Material.CompilationOwner.MaterialCompileDiagnostics.push_back(MakeDiagnostic(
							Request, MapProgramCategory(Diagnostic.Category), Diagnostic,
							Material.CompilationOwner.AcceptedGeneration.Program != nullptr));
					}
					return false;
				}

				// Tooling can adopt the root's exact immutable result through normal
				// owner admission even when no asynchronous retained store is running.
				if (!bForceRecompile && (!Compilation || !Compilation->IsAccepting()
					|| FScopedOfflinePreparation::IsActive()))
				{
					for (DObject* Object : GDObjectArray.Snapshot(EObjectQueryScope::LiveOnly))
					{
						auto* Owner = Cast<DMaterialInterface>(Object);
						if (!IsValid(Owner)) continue;
						const auto Program = Owner->CompilationOwner.AcceptedGeneration.Program;
						if (Program && Program->Identity == Request.ProgramIdentity)
						{
							return Admit(Material, {
								.Owner = Request.Owner,
								.AuthoredRevision = Request.AuthoredRevision,
								.Generation = Request.Generation,
								.DependencyRevision = Request.DependencyRevision,
								.ParentChainRevision = Request.ParentChainRevision,
								.ProgramIdentity = Request.ProgramIdentity,
								.StaticProperties = Request.CompilerInput.StaticProperties,
								.Target = Request.Target,
								.State = EMaterialCompileState::Ready,
								.CacheOutcome = EMaterialCompileCacheOutcome::RetainedHit,
								.CompiledProgram = Program});
						}
					}
				}

				if (FScopedOfflinePreparation::IsActive()
					|| IsObjectHandleNull(Request.Owner)
					|| !Compilation || !Compilation->IsAccepting()
					|| !IsTaskSchedulerRunning())
				{
					FMaterialCompilerResult Compiled = CompileMaterialProgram(
						Request.CompilerInput, bForceRecompile);
					FMaterialCompileResult Result{
						.Owner = Request.Owner,
						.AuthoredRevision = Request.AuthoredRevision,
						.Generation = Request.Generation,
						.DependencyRevision = Request.DependencyRevision,
					.ParentChainRevision = Request.ParentChainRevision,
						.ProgramIdentity = Request.ProgramIdentity,
						.StaticProperties = Request.CompilerInput.StaticProperties,
						.Target = Request.Target,
						.State = Compiled ? EMaterialCompileState::Ready
							: EMaterialCompileState::Failed,
						.Category = Compiled || Compiled.Diagnostics.empty()
							? EMaterialCompileResultCategory::None
							: MapProgramCategory(Compiled.Diagnostics.front().Category),
						.CacheOutcome = bForceRecompile
							? EMaterialCompileCacheOutcome::Forced
							: EMaterialCompileCacheOutcome::Compiled,
					};
					if (Compiled)
						Result.CompiledProgram =
							std::make_shared<const FMaterialCompilerResult>(std::move(Compiled));
					else
						for (const FMaterialProgramDiagnostic& Diagnostic
							: Compiled.Diagnostics)
							Result.Diagnostics.push_back(MakeDiagnostic(
								Request, MapProgramCategory(Diagnostic.Category), Diagnostic,
								Material.CompilationOwner.AcceptedGeneration.Program != nullptr));
					Admit(Material, std::move(Result));
					return Material.CompilationOwner.MaterialCompileStatus.State
						== EMaterialCompileState::Ready;
				}

				Material.CompilationOwner.MaterialCompileStatus.State = EMaterialCompileState::Pending;
				Material.CompilationOwner.MaterialCompileStatus.ResultCategory =
					EMaterialCompileResultCategory::None;
				const EMaterialCompileState Submitted =
					Compilation->Submit(std::move(Request));
				if (Submitted == EMaterialCompileState::Rejected)
				{
					Material.CompilationOwner.MaterialCompileStatus.State = EMaterialCompileState::Rejected;
					Material.CompilationOwner.MaterialCompileStatus.ResultCategory =
						EMaterialCompileResultCategory::Admission;
					FMaterialCompileRequest DiagnosticRequest;
					DiagnosticRequest.Owner = MakeObjectHandle(&Material);
					DiagnosticRequest.Generation =
						Material.CompilationOwner.MaterialCompileStatus.RequestGeneration;
					DiagnosticRequest.ProgramIdentity =
						Material.CompilationOwner.MaterialCompileStatus.RequestedIdentity;
					DiagnosticRequest.AssetPath = Material.GetObjectPath();
					Material.CompilationOwner.MaterialCompileDiagnostics.push_back(MakeDiagnostic(
						DiagnosticRequest, EMaterialCompileResultCategory::Admission,
						{.Category = EMaterialProgramDiagnosticCategory::Compile,
						 .Message = "Material compile admission was rejected."},
						Material.CompilationOwner.AcceptedGeneration.Program != nullptr));
					return false;
				}
				Material.CompilationOwner.bDeferredForceRecompile = bForceRecompile;
				Material.CompilationOwner.MaterialCompileStatus.State = Submitted;
				return true;
		}

		auto FMaterialCompilationLifecycle::Admit(
			DMaterialInterface& Material, FMaterialCompileResult Result) -> bool
		{
				CheckMaterialCompileGameThread();
				FMaterialCompileStatus& Status = Material.CompilationOwner.MaterialCompileStatus;
				if (Result.Generation != Status.RequestGeneration
					|| Result.AuthoredRevision != Status.AuthoredRevision
					|| Result.DependencyRevision != Status.DependencyRevision
					|| Result.ParentChainRevision != Status.ParentChainRevision
					|| Result.Target != Status.Target)
				{
					return false;
				}

				const auto OwnerHandle = MakeObjectHandle(&Material);
				if (!IsObjectHandleNull(OwnerHandle))
				{
					if (!SameOwner(Result.Owner, OwnerHandle)) return false;
					FResolvedMaterialProperties Resolved;
					std::string Error;
					if (!ResolveMaterialProperties(Material, Resolved, Error))
					{
						RequestCurrent(Material, false);
						return false;
					}
				}

				Status.State = Result.State;
				Status.ResultCategory = Result.Category;
				Status.CacheOutcome = Result.CacheOutcome;
				Status.TaskId = Result.TaskId;
				Material.CompilationOwner.MaterialCompileDiagnostics = std::move(Result.Diagnostics);
				Status.bHasLastKnownGood = Material.CompilationOwner.AcceptedGeneration.Program != nullptr;
				if (Result.State == EMaterialCompileState::Ready
					&& Result.CompiledProgram
					&& Result.ProgramIdentity == Status.RequestedIdentity
					&& Result.CompiledProgram->Identity == Result.ProgramIdentity
					&& ValidateMaterialCompilerResult(*Result.CompiledProgram))
				{
					FMaterialAcceptedGeneration Candidate;
					Candidate.Program = Result.CompiledProgram;
					Candidate.ShaderProperties = CanonicalizeMaterialShaderProperties(Result.StaticProperties);
					Candidate.Properties = Material.GetStaticProperties();
					Candidate.Properties.OpacityMaskThreshold = Candidate.ShaderProperties.OpacityMaskThreshold;
					if (CanonicalizeMaterialShaderProperties(Candidate.Properties) != Candidate.ShaderProperties)
					{
						Status.State = EMaterialCompileState::Superseded;
						Status.bLastKnownGoodDisplayed = Status.bHasLastKnownGood;
						return false;
					}
					for (const auto& Parameter : Candidate.Program->ActiveParameters)
					{
						FResolvedMaterialParameter Resolved;
						if (!Material.ResolveParameterValue(Parameter.Id, Resolved)
							|| !Resolved.Definition || Resolved.Definition->Type != Parameter.Type)
						{
							Status.State = EMaterialCompileState::Rejected;
							Status.ResultCategory = EMaterialCompileResultCategory::Admission;
							Status.bLastKnownGoodDisplayed = Status.bHasLastKnownGood;
							return false;
						}
						Candidate.Parameters.push_back(BuildMaterialLocalRenderParameter(
							Parameter.Id, Parameter.Type, Resolved.Value));
					}
					Material.CompilationOwner.AcceptedGeneration = std::move(Candidate);
					Status.CompiledIdentity = Result.ProgramIdentity;
					Status.CompiledAuthoredRevision = Result.AuthoredRevision;
					Status.DurationMicroseconds =
						Material.CompilationOwner.AcceptedGeneration.Program->Timings.NormalizationMicroseconds
						+ Material.CompilationOwner.AcceptedGeneration.Program->Timings.GenerationMicroseconds
						+ Material.CompilationOwner.AcceptedGeneration.Program->Timings.CompilationMicroseconds;
					Status.bHasLastKnownGood = true;
					Status.bLastKnownGoodDisplayed = false;
					Material.MarkRenderDataDirty(
						EMaterialRenderDirtyFlags::ShaderMap
							| EMaterialRenderDirtyFlags::PipelineState);
					return true;
				}

				if (Result.State == EMaterialCompileState::Ready)
				{
					Status.State = EMaterialCompileState::Rejected;
					Status.ResultCategory = EMaterialCompileResultCategory::Admission;
				}
				Status.bLastKnownGoodDisplayed = Material.CompilationOwner.AcceptedGeneration.Program != nullptr;
				for (FMaterialCompileDiagnostic& Diagnostic
					: Material.CompilationOwner.MaterialCompileDiagnostics)
					Diagnostic.bLastKnownGoodDisplayed = Status.bLastKnownGoodDisplayed;
				return false;
		}

		auto FMaterialCompilationLifecycle::MarkCanceled(
			DMaterialInterface& Material) -> void
		{
			Material.CompilationOwner.MaterialCompileStatus.State = EMaterialCompileState::Canceled;
			Material.CompilationOwner.MaterialCompileStatus.ResultCategory =
				EMaterialCompileResultCategory::Cancellation;
			Material.CompilationOwner.MaterialCompileStatus.bLastKnownGoodDisplayed =
				Material.CompilationOwner.AcceptedGeneration.Program != nullptr;
		}

		auto FMaterialCompilationLifecycle::RetryDeferred(DMaterialInterface& Material) -> void
		{
			RequestCurrent(Material, Material.CompilationOwner.bDeferredForceRecompile);
		}

		auto FMaterialCompilationLifecycle::RequestCurrent(
			DMaterialInterface& Material, bool bForceRecompile) -> bool
		{
			if (GetAssetRuntimeConfiguration().RequiresCookedPayload()) return false;
			FResolvedMaterialProperties Resolved;
			std::string Error;
			if (!ResolveMaterialProperties(Material, Resolved, Error) || !Material.GetMaterialProgram())
			{
				Material.CompilationOwner.MaterialCompileStatus.RequestGeneration = AdvanceNonzero(
					Material.CompilationOwner.MaterialCompileStatus.RequestGeneration);
				Material.CompilationOwner.MaterialCompileStatus.State = EMaterialCompileState::Failed;
				Material.CompilationOwner.MaterialCompileStatus.ResultCategory = EMaterialCompileResultCategory::Dependency;
				Material.CompilationOwner.AcceptedGeneration.Program.reset();
				Material.CompilationOwner.AcceptedGeneration.Parameters.clear();
				Material.CompilationOwner.MaterialCompileStatus.bHasLastKnownGood = false;
				Material.CompilationOwner.MaterialCompileStatus.bLastKnownGoodDisplayed = false;
				Material.CompilationOwner.MaterialCompileDiagnostics = {{
					.Category = EMaterialCompileResultCategory::Dependency,
					.Source = {.Category = EMaterialProgramDiagnosticCategory::Dependency,
						.Message = Error.empty() ? "Material has no authored program." : Error},
					.AssetPath = Material.GetObjectPath(),
					.Generation = Material.CompilationOwner.MaterialCompileStatus.RequestGeneration}};
				return false;
			}
			return Material.RequestProgramCompile(
				*Material.GetMaterialProgram(), Material.GetStaticProperties(), bForceRecompile);
		}
	}

	auto IsMaterialCompilationAcceptingRequests() -> bool
	{
		const auto Manager = GetMaterialCompilingManager();
		return Manager && Manager->GetState().IsAccepting();
	}

	auto GetMaterialCompilationDiagnostics()
		-> FMaterialCompilationDiagnostics
	{
		const auto Manager = GetMaterialCompilingManager();
		return Manager ? Manager->GetState().GetDiagnostics()
			: FMaterialCompilationDiagnostics{};
	}

	auto NotifyMaterialShaderReload(bool bForceRecompile) -> void
	{
		uint8 Desired = bForceRecompile ? 2 : 1;
		uint8 Current = GPendingShaderReloadMode.load(std::memory_order_acquire);
		while (Current < Desired
			&& !GPendingShaderReloadMode.compare_exchange_weak(
				Current, Desired, std::memory_order_acq_rel)) {}
	}

	auto RequestMaterialRecompile(
		DMaterialInterface& Material, bool bForceRecompile) -> bool
	{
		return Private::FMaterialCompilationLifecycle::RequestCurrent(
			Material, bForceRecompile);
	}

	namespace
	{
		auto CancelMaterialCompileDomain(DMaterialInterface& Material) -> bool
		{
			CheckMaterialCompileGameThread();
			const auto Manager = GetMaterialCompilingManager();
			const bool bCanceled = Manager
				&& Manager->GetState().CancelOwner(MakeObjectHandle(&Material));
			if (bCanceled || Material.GetMaterialCompileStatus().State == EMaterialCompileState::Deferred)
				Private::FMaterialCompilationLifecycle::MarkCanceled(Material);
			return bCanceled;
		}
	}

	auto CreateMaterialCompilingManager() -> std::shared_ptr<IAssetCompilingManager>
	{
		std::lock_guard Lock(GMaterialCompilingManagerMutex);
		if (const auto Existing = GMaterialCompilingManager.lock()) return Existing;
		auto Manager = std::make_shared<FMaterialCompilingManager>();
		GMaterialCompilingManager = Manager;
		return Manager;
	}
}
