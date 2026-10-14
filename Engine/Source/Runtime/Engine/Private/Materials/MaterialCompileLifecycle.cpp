#include "Materials/ObjectCacheContext.h"
#include "MaterialPreparedProgram.h"
#include "MaterialBuildOperation.h"
#include "MaterialProgramCache.h"
#include "MaterialCompileTestSupport.h"
#include "MaterialCollectionBinding.h"
#include "MaterialTranslationCodec.h"
#include "MaterialCompileRetryQueue.h"
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
#include "Serialization/BinaryFormat.h"

namespace Durin::MaterialBuildPrivate
{
	static std::atomic<std::shared_ptr<const std::function<void()>>> ResultFanoutHook;
	auto SetResultFanoutHookForTests(std::function<void()> Hook) -> void
	{ ResultFanoutHook.store(Hook ? std::make_shared<const std::function<void()>>(std::move(Hook)) : nullptr); }
}

namespace Durin
{
	namespace
	{
		auto CheckMaterialCompileGameThread() -> void
		{
			if (GIsGameThreadIdInitialized) CheckGameThread();
		}

		auto AdvanceNonzero(uint64 Value) -> uint64
		{
			return Value == std::numeric_limits<uint64>::max() ? 1 : Value + 1;
		}

		auto SameOwner(FWeakObjectPtr Left, FWeakObjectPtr Right) -> bool
		{
			return Left.GetKey() == Right.GetKey();
		}


		auto EstimateRequestBytes(const FMaterialCompileRequest& Request) -> uint64
		{
			if (!Request.CompilerInput) return 0;
			uint64 Bytes = sizeof(Request) + Request.AssetPath.capacity() + Request.Target.capacity()
				+ MaterialBuildPrivate::EstimateTranslationCaptureBytes(*Request.CompilerInput)
				+ Request.ExpressionSources.capacity() * sizeof(MIR::FSource);
			for (const auto& Source : Request.ExpressionSources) Bytes += Source.FunctionAssetPath.capacity() + Source.CallPath.capacity() * sizeof(FGuid);
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
			FMaterialProgramDiagnostic Source) -> FMaterialCompileDiagnostic
		{
			if (Source.Error.ExternalDiagnostic.size() > MaterialProgramMaxDiagnosticMessageBytes)
				Source.Error.ExternalDiagnostic.resize(MaterialProgramMaxDiagnosticMessageBytes);
			std::string AssetPath = Request.AssetPath;
			if (AssetPath.size() > MaterialProgramMaxDiagnosticMessageBytes)
				AssetPath.resize(MaterialProgramMaxDiagnosticMessageBytes);
			return {
				.Category = Category,
				.Source = std::move(Source),
				.AssetPath = std::move(AssetPath),
				.ProgramIdentity = Request.ProgramIdentity,
				.Generation = Request.Generation,
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
			std::vector<FMaterialCompileRequest> Consumers;
			std::vector<FWeakObjectPtr> CompletingOwners;
			FTaskCancellationSource Cancellation;
			std::shared_ptr<MaterialBuildPrivate::FMaterialBuildOperation> Operation;
			FMaterialProgramIdentity ProgramIdentity;
			std::vector<std::pair<uint32, uint32>> SourceMap;
			std::shared_ptr<const MIR::FCompilerInput> DiagnosticInput;
			std::vector<FWeakObjectPtr> Origins;
			FWeakObjectPtr InitialOwner;
			uint64 CapturedBytes = 0;
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
				Attribution = RegisterTaskAttribution("Engine", "MaterialCompile");
				bAcceptingRequests = true;
				return true;
			}

			auto Shutdown() -> void
			{
				CheckMaterialCompileGameThread();
				std::vector<std::shared_ptr<FMaterialCompileFlight>> Pending;
				{
					std::scoped_lock Lock(Mutex);
					bAcceptingRequests = false;
					Pending = Operations;
					for (auto& Flight : Pending)
					{
						Flight->Cancellation.RequestCancellation();
					}
				}
				for (const auto& Flight : Pending) Flight->Operation->Cancel();
				for (const auto& Flight : Pending) Flight->Operation->Wait();
				std::scoped_lock Lock(Mutex);
				Diagnostics.CanceledRequests += OutstandingConsumers;
				Flights.clear();
				ProgramFlights.clear(); CompletingFlights.clear(); Operations.clear(); CapturedBytes = 0;
				Mailbox.clear();
				OutstandingConsumers = 0;
				MaterialBuildPrivate::ClearPrograms();
			}

			auto StopAdmission() -> void
			{
				CheckMaterialCompileGameThread();
				std::scoped_lock Lock(Mutex);
				bAcceptingRequests = false;
			}

			auto RetireOperations() -> void
			{
				std::vector<std::shared_ptr<FMaterialCompileFlight>> Retired;
				{ std::scoped_lock Lock(Mutex); Retired = Operations; }
				std::erase_if(Retired, [](const auto& Flight) { return !Flight->Operation->IsFinished(); });
				std::scoped_lock Lock(Mutex);
				for (const auto& Flight : Retired)
				{
					const auto It = std::ranges::find(Operations, Flight);
					if (It != Operations.end()) { CapturedBytes -= Flight->CapturedBytes; Operations.erase(It); }
				}
			}

			auto CancelLatchedOperations() -> void
			{
				std::vector<std::shared_ptr<FMaterialCompileFlight>> Pending;
				{ std::scoped_lock Lock(Mutex); Pending = Operations; }
				for (const auto& Flight : Pending) if (Flight->Cancellation.GetToken().IsCancellationRequested()) Flight->Operation->Cancel();
			}

			auto RaisePendingPriority(std::span<const FWeakObjectPtr> Owners = {}) -> void
			{
				std::vector<std::shared_ptr<FMaterialCompileFlight>> Pending;
				{ std::scoped_lock Lock(Mutex); Pending = Operations; }
				for (const auto& Flight : Pending)
					if (Owners.empty() || std::ranges::any_of(Flight->Origins, [&](auto Origin) { return std::ranges::any_of(Owners, [&](auto Owner) { return SameOwner(Origin, Owner); }); })) Flight->Operation->RaisePriority();
			}

			auto DrainOperations() -> void
			{
				std::vector<std::shared_ptr<FMaterialCompileFlight>> Pending;
				{ std::scoped_lock Lock(Mutex); Pending = Operations; }
				for (const auto& Flight : Pending) Flight->Operation->Wait();
				RetireOperations();
			}
			auto DrainSelected(std::span<const FWeakObjectPtr> Owners) -> void
			{
				std::vector<std::shared_ptr<FMaterialCompileFlight>> Pending;
				{ std::scoped_lock Lock(Mutex); Pending = Operations; }
				for (const auto& Flight : Pending)
					if (std::ranges::any_of(Flight->Origins, [&](const auto& Origin) { return std::ranges::any_of(Owners, [&](const auto& Owner) { return SameOwner(Origin, Owner); }); })) Flight->Operation->Wait();
				RetireOperations();
			}

			auto Submit(FMaterialCompileRequest Request) -> EMaterialCompileState
			{
				CheckMaterialCompileGameThread();
				RetireOperations();
				const auto Bytes = EstimateRequestBytes(Request);
				if (!Request.CompilerInput || Bytes > MaterialCompileMaxRequestBytes) return EMaterialCompileState::Rejected;
				struct FCancelOnExit { FMaterialCompilationState& State; ~FCancelOnExit() { State.CancelLatchedOperations(); } } CancelOnExit{*this};
				// Admission precedes encoding and normalization. Captured encoding is
				// bounded by the input contract and belongs to GameThread snapshot capture.
				{
					std::scoped_lock Lock(Mutex);
					SupersedeOwnerLocked(Request.Owner);
					if (!bAcceptingRequests) return EMaterialCompileState::Rejected;
					if (OutstandingConsumers >= MaterialCompileMaxConsumers || Bytes > 128ull * 1024 * 1024 - CapturedBytes) return EMaterialCompileState::Deferred;
				}
				auto Encoded = MaterialBuildPrivate::EncodeTranslationInput(*Request.CompilerInput);
				if (!Encoded) return EMaterialCompileState::Rejected;
				const auto FlightBytes = Bytes + Encoded->GetRetainedCapacityBytes();
				auto TranslationInput = std::make_shared<const FMaterialTranslationInput>(FMaterialTranslationInput{*Encoded, FXxHash128::HashBuffer(Encoded->GetBytes())});
				FBinaryWriter Identity;
				Identity.WriteHash128(TranslationInput->Identity);
				const auto& Environment = Request.CompilerInput->Environment;
				Identity.WriteString(Environment.CompilerIdentity); Identity.WriteString(Environment.Target); Identity.WriteU32(Environment.PassContractVersion);
				auto Dependencies = Environment.Dependencies;
				std::ranges::sort(Dependencies, {}, &FMaterialCompilerDependency::VirtualPath);
				for (const auto& Dependency : Dependencies) { Identity.WriteString(Dependency.VirtualPath); Identity.WriteHash128(Dependency.ContentHash); }
				const FMaterialCompileFlightKey Key{.Identity = {.Digest = FXxHash128::HashBuffer(Identity.GetBytes())}, .bForceRecompile = Request.bForceRecompile};
				std::shared_ptr<FMaterialCompileFlight> NewFlight;
				{
					std::scoped_lock Lock(Mutex);
					if (const auto Existing = Flights.find(Key); Existing != Flights.end())
					{
						if (Existing->second->Cancellation.GetToken().IsCancellationRequested() || Bytes > 128ull * 1024 * 1024 - CapturedBytes) return EMaterialCompileState::Deferred;
						Request.bSingleFlightConsumer = true;
						if (!std::ranges::any_of(Existing->second->Origins, [&](auto Origin) { return SameOwner(Origin, Request.Owner); })) Existing->second->Origins.push_back(Request.Owner);
						Request.ProgramIdentity = Existing->second->ProgramIdentity;
						for (const auto& [Ordinal, NormalizedIndex] : Existing->second->SourceMap)
						{ auto Source = Request.CompilerInput->Sources[Ordinal]; Source.ExpressionIndex = NormalizedIndex; Request.ExpressionSources.push_back(std::move(Source)); }
						Existing->second->Consumers.push_back(std::move(Request)); Existing->second->CapturedBytes += Bytes; CapturedBytes += Bytes;
						++Diagnostics.AcceptedRequests; ++Diagnostics.SingleFlightConsumers; ++OutstandingConsumers;
						return EMaterialCompileState::Pending;
					}
					if (FlightBytes > 128ull * 1024 * 1024 - CapturedBytes || Operations.size() >= MaterialCompileMaxConcurrentRequests) return EMaterialCompileState::Deferred;
					NewFlight = std::make_shared<FMaterialCompileFlight>(); NewFlight->Key = Key;
					NewFlight->Operation = std::make_shared<MaterialBuildPrivate::FMaterialBuildOperation>();
					if (!NewFlight->Operation->TryReserve()) return EMaterialCompileState::Deferred;
					NewFlight->CapturedBytes = FlightBytes; CapturedBytes += FlightBytes;
					NewFlight->Origins.push_back(Request.Owner);
					NewFlight->InitialOwner = Request.Owner;
					NewFlight->Consumers.push_back(std::move(Request));
					Flights.emplace(Key, NewFlight); Operations.push_back(NewFlight);
					++Diagnostics.AcceptedRequests; ++OutstandingConsumers;
				}
				const auto Input = NewFlight->Consumers.front().CompilerInput;
				NewFlight->DiagnosticInput = Input;
				NewFlight->Operation->Start(Input, Key.bForceRecompile, NewFlight->Cancellation.GetToken(),
					[this, Weak = std::weak_ptr(NewFlight)](FMaterialTranslatedProgram& Translation) {
						const auto Flight = Weak.lock(); if (!Flight) return MaterialBuildPrivate::EMaterialTranslationAction::Join;
						if (!Translation) return MaterialBuildPrivate::EMaterialTranslationAction::Compile;
						std::shared_ptr<const FMaterialCompilerResult> Retained;
						{
							std::scoped_lock Lock(Mutex);
							Flight->ProgramIdentity = Translation.Identity;
							for (size_t Index = 0; Index < Translation.Sources.size(); ++Index) Flight->SourceMap.emplace_back(Translation.Sources[Index].Ordinal, Translation.Sources[Index].Source.ExpressionIndex);
							for (auto& Consumer : Flight->Consumers)
							{
								Consumer.ProgramIdentity = Flight->ProgramIdentity;
								Consumer.ExpressionSources.clear();
								for (size_t Index = 0; Index < Translation.Sources.size(); ++Index)
								{
									const auto Ordinal = Translation.Sources[Index].Ordinal;
									auto Source = Consumer.CompilerInput->Sources[Ordinal]; Source.ExpressionIndex = Translation.Sources[Index].Source.ExpressionIndex;
									Consumer.ExpressionSources.push_back(std::move(Source));
								}
							}
							if (Flight->Cancellation.GetToken().IsCancellationRequested()) return MaterialBuildPrivate::EMaterialTranslationAction::Compile;
							if (!Flight->Key.bForceRecompile)
							{
								Retained = MaterialBuildPrivate::FindProgram(Flight->ProgramIdentity);
							}
							const FMaterialCompileFlightKey ProgramKey{Flight->ProgramIdentity, Flight->Key.bForceRecompile};
							if (!Retained)
							{
								const auto Existing = ProgramFlights.find(ProgramKey);
								if (Existing != ProgramFlights.end() && !Existing->second->Cancellation.GetToken().IsCancellationRequested())
								{
									MergeConsumersLocked(*Existing->second, *Flight); return MaterialBuildPrivate::EMaterialTranslationAction::Join;
								}
								ProgramFlights.insert_or_assign(ProgramKey, Flight);
							}
						}
						if (Retained)
						{
							CompleteFlight(Flight, {}, EMaterialCompileState::Ready, EMaterialCompileCacheOutcome::RetainedHit, Retained); return MaterialBuildPrivate::EMaterialTranslationAction::Reuse;
						}
						return MaterialBuildPrivate::EMaterialTranslationAction::Compile;
					},
					[this, Weak = std::weak_ptr(NewFlight)](FMaterialCompilerResult Compiled) {
						const auto Flight = Weak.lock(); if (!Flight) return;
						CompleteFlight(Flight, std::move(Compiled), Flight->Cancellation.GetToken().IsCancellationRequested() ? EMaterialCompileState::Canceled : EMaterialCompileState::Ready);
					}, std::move(TranslationInput));
				NewFlight->TaskId.store(NewFlight->Operation->GetTaskId());
				return EMaterialCompileState::Running;
			}

			auto MergeConsumersLocked(FMaterialCompileFlight& Target, FMaterialCompileFlight& Source) -> void
			{
				for (auto& Consumer : Source.Consumers)
				{
					Consumer.bSingleFlightConsumer = true; Target.Consumers.push_back(std::move(Consumer));
					++Diagnostics.SingleFlightConsumers;
				}
				Target.CapturedBytes += Source.CapturedBytes; Source.CapturedBytes = 0;
				for (const auto Origin : Source.Origins)
					if (!std::ranges::any_of(Target.Origins, [&](auto Other) { return SameOwner(Origin, Other); })) Target.Origins.push_back(Origin);
				Source.Consumers.clear(); Flights.erase(Source.Key);
			}

			auto CancelOwner(FWeakObjectPtr Owner) -> bool
			{
				CheckMaterialCompileGameThread();
				bool Canceled;
				{ std::scoped_lock Lock(Mutex); Canceled = CancelOwnerLocked(Owner, false); }
				CancelLatchedOperations(); return Canceled;
			}

			auto Pump(uint32 MaximumCount = std::numeric_limits<uint32>::max())
				-> std::vector<FMaterialCompileResult>
			{
				CheckMaterialCompileGameThread();
				RetireOperations();
				// A canceled-before-entry body or an exception may never publish its mailbox.
				std::vector<std::shared_ptr<FMaterialCompileFlight>> TerminalFlights;
				{
					std::scoped_lock Lock(Mutex);
					for (const auto& [Key, Flight] : Flights)
						if (Flight->Operation->IsFinished())
							TerminalFlights.push_back(Flight);
				}
				for (const auto& Flight : TerminalFlights)
					CompleteFlight(Flight, {}, Flight->Cancellation.GetToken().IsCancellationRequested()
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

			auto HasOwner(FWeakObjectPtr Owner) const -> bool
			{
				std::scoped_lock Lock(Mutex);
				for (const auto& [Key, Flight] : Flights)
					if (std::ranges::any_of(Flight->Consumers,
						[Owner](const FMaterialCompileRequest& Consumer) {
							return SameOwner(Consumer.Owner, Owner);
						})) return true;
				if (std::ranges::any_of(CompletingFlights, [&](const auto& Flight) { return std::ranges::any_of(Flight->CompletingOwners, [&](auto Pending) { return SameOwner(Pending, Owner); }); })) return true;
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
				Result.InFlightCount = static_cast<uint32>(Operations.size());
				Result.OutstandingConsumerCount = OutstandingConsumers;
				Result.PendingPublicationCount = static_cast<uint32>(Mailbox.size());
				const auto Cache = MaterialBuildPrivate::ProgramCacheStats();
				Result.RetainedProgramCount = Cache.first; Result.RetainedProgramBytes = Cache.second;
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
				Program = MaterialBuildPrivate::ConfigureProgram(std::move(Program), *Request.CompilerInput);
				return {
					.Owner = Request.Owner,
					.AuthoredRevision = Request.AuthoredRevision,
					.Generation = Request.Generation,
					.DependencyRevision = Request.DependencyRevision,
					.ParentChainRevision = Request.ParentChainRevision,
					.ProgramIdentity = Request.ProgramIdentity,
					.StaticProperties = Request.CompilerInput->StaticProperties,
					.Target = Request.Target,
					.State = EMaterialCompileState::Ready,
					.Category = EMaterialCompileResultCategory::None,
					.CacheOutcome = CacheOutcome,
					.TaskId = TaskId,
					.CompiledProgram = std::move(Program),
					.ExpressionSources = Request.ExpressionSources,
				};
			}

			static auto MakeTerminalResult(const FMaterialCompileRequest& Consumer, EMaterialCompileState State,
				EMaterialCompileResultCategory Category, uint64 TaskId) -> FMaterialCompileResult
			{
				return {.Owner = Consumer.Owner, .AuthoredRevision = Consumer.AuthoredRevision, .Generation = Consumer.Generation,
					.DependencyRevision = Consumer.DependencyRevision, .ParentChainRevision = Consumer.ParentChainRevision,
					.ProgramIdentity = Consumer.ProgramIdentity, .StaticProperties = Consumer.CompilerInput->StaticProperties,
					.Target = Consumer.Target, .State = State, .Category = Category, .TaskId = TaskId, .ExpressionSources = Consumer.ExpressionSources};
			}

			auto CompleteFlight(
				const std::shared_ptr<FMaterialCompileFlight>& Flight,
				FMaterialCompilerResult Compiled,
				EMaterialCompileState CompletionState, EMaterialCompileCacheOutcome Override = EMaterialCompileCacheOutcome::None,
				std::shared_ptr<const FMaterialCompilerResult> Retained = {}) -> void
			{
				std::vector<FMaterialCompileRequest> Consumers;
				{
					std::scoped_lock Lock(Mutex);
					const auto It = Flights.find(Flight->Key);
					if (It == Flights.end() || It->second != Flight) return;
					Flights.erase(It);
					const FMaterialCompileFlightKey ProgramKey{Flight->ProgramIdentity, Flight->Key.bForceRecompile};
					if (const auto ProgramIt = ProgramFlights.find(ProgramKey); ProgramIt != ProgramFlights.end() && ProgramIt->second == Flight) ProgramFlights.erase(ProgramIt);
					Consumers = std::move(Flight->Consumers);
					for (const auto& Consumer : Consumers) Flight->CompletingOwners.push_back(Consumer.Owner);
					CompletingFlights.push_back(Flight);
				}
				std::vector<FMaterialCompileResult> Results;
				const uint64 TaskId = Flight->TaskId.load(
					std::memory_order_acquire);
				std::shared_ptr<const FMaterialCompilerResult> Program;
				EMaterialCompileState State = CompletionState;
				EMaterialCompileResultCategory Category =
					EMaterialCompileResultCategory::None;
				try
				{
					if (const auto Hook = MaterialBuildPrivate::ResultFanoutHook.load()) (*Hook)();
					if (CompletionState == EMaterialCompileState::Ready)
					{
						if (Retained) Program = std::move(Retained);
						else if (!Compiled)
						{
							State = EMaterialCompileState::Failed;
							Category = Compiled.Diagnostics.empty()
								? EMaterialCompileResultCategory::Compile
								: MapProgramCategory(Compiled.Diagnostics.front().Category);
						}
						else
						{
							Program = std::make_shared<const FMaterialCompilerResult>(
								std::move(Compiled));
							Program = MaterialBuildPrivate::RetainProgram(Program, Flight->Key.bForceRecompile);
						}
					}
					else if (CompletionState == EMaterialCompileState::Canceled)
						Category = EMaterialCompileResultCategory::Cancellation;
					for (const FMaterialCompileRequest& Consumer : Consumers)
					{
						if (State == EMaterialCompileState::Ready)
						{
							Results.push_back(MakeReadyResult(
								Consumer, Program,
								Override != EMaterialCompileCacheOutcome::None ? Override : Consumer.bSingleFlightConsumer
									? EMaterialCompileCacheOutcome::SingleFlight
									: Flight->Key.bForceRecompile
									? EMaterialCompileCacheOutcome::Forced
									: EMaterialCompileCacheOutcome::Compiled,
								TaskId));
							continue;
						}

						auto Result = MakeTerminalResult(Consumer, State, Category, TaskId);
						if (State == EMaterialCompileState::Canceled)
						{
							Result.Diagnostics.push_back(MakeDiagnostic(
								Consumer, Category,
								{.Category = EMaterialProgramDiagnosticCategory::Compile,
								 .Error = EMaterialCompileError::CompilationCanceled}));
						}
						else
						{
							for (const FMaterialProgramDiagnostic& Diagnostic
								: Compiled.Diagnostics)
							{
								if (Result.Diagnostics.size()
									>= MaterialProgramMaxDiagnosticCount) break;
								auto Qualified = Diagnostic;
								if (Diagnostic.NodeId.IsValid() && Flight->DiagnosticInput && Consumer.CompilerInput)
								{
									const auto Origin = std::ranges::find(Flight->DiagnosticInput->Sources, Diagnostic.NodeId, &MIR::FSource::NodeId);
									if (Origin != Flight->DiagnosticInput->Sources.end())
									{
										const auto Index = std::distance(Flight->DiagnosticInput->Sources.begin(), Origin);
										const auto& Source = Consumer.CompilerInput->Sources[Index];
										Qualified.NodeId = Source.NodeId; Qualified.PortId = Source.PortId; Qualified.FunctionAssetPath = Source.FunctionAssetPath; Qualified.CallPath = Source.CallPath;
									}
								}
								Result.Diagnostics.push_back(MakeDiagnostic(
									Consumer, MapProgramCategory(Diagnostic.Category),
									std::move(Qualified)));
							}
						}
						Results.push_back(std::move(Result));
					}
				}
				catch (...)
				{
					State = EMaterialCompileState::Failed; Category = EMaterialCompileResultCategory::Compile; Results.clear();
					for (const auto& Consumer : Consumers)
					{
						auto Result = MakeTerminalResult(Consumer, State, Category, TaskId);
						Result.Diagnostics.push_back(MakeDiagnostic(Consumer, Category,
							{.Category = EMaterialProgramDiagnosticCategory::Compile, .Error = EMaterialCompileError::AdmissionRejected}));
						Results.push_back(std::move(Result));
					}
				}

				{
					std::scoped_lock Lock(Mutex);
					// Detachment wins completion; subsequent cancellation withdraws publication separately.
					if (State == EMaterialCompileState::Ready)
					{
						Diagnostics.CompletedRequests += Consumers.size();
						if (Override == EMaterialCompileCacheOutcome::RetainedHit) Diagnostics.RetainedHits += Consumers.size();
					}
					else if (State == EMaterialCompileState::Failed)
						Diagnostics.FailedRequests += Consumers.size();
					else if (State == EMaterialCompileState::Canceled)
						Diagnostics.CanceledRequests += Consumers.size();

					for (auto& Result : Results)
						if (std::ranges::any_of(Flight->CompletingOwners, [&](auto Owner) { return SameOwner(Owner, Result.Owner); })) Mailbox.push_back(std::move(Result));
					Flight->CompletingOwners.clear(); std::erase(CompletingFlights, Flight);
				}
			}

			auto SupersedeOwnerLocked(FWeakObjectPtr Owner) -> void
			{
				(void)CancelOwnerLocked(Owner, true);
			}

			auto CancelOwnerLocked(FWeakObjectPtr Owner, bool bSuperseded) -> bool
			{
				bool bFound = false;
				const auto AccountRemoved = [&](const auto& Flight, size_t Removed, bool Empty) {
					if (Removed == 0) return;
					if (!SameOwner(Owner, Flight->InitialOwner)) std::erase_if(Flight->Origins, [&](auto Origin) { return SameOwner(Origin, Owner); });
					bFound = true; OutstandingConsumers -= static_cast<uint32>(Removed);
					if (bSuperseded) Diagnostics.SupersededRequests += Removed; else Diagnostics.CanceledRequests += Removed;
					if (Empty) Flight->Cancellation.RequestCancellation();
				};
				for (auto& [Key, Flight] : Flights)
				{
					const auto Removed = std::erase_if(Flight->Consumers, [Owner](const auto& Consumer) { return SameOwner(Consumer.Owner, Owner); });
					AccountRemoved(Flight, Removed, Flight->Consumers.empty());
				}
				for (auto& Flight : CompletingFlights)
				{
					const auto Removed = std::erase_if(Flight->CompletingOwners, [Owner](auto Pending) { return SameOwner(Pending, Owner); });
					AccountRemoved(Flight, Removed, Flight->CompletingOwners.empty());
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
			FTaskAttribution Attribution;
			bool bAcceptingRequests = false;
			uint32 OutstandingConsumers = 0;
			std::unordered_map<FMaterialCompileFlightKey,
				std::shared_ptr<FMaterialCompileFlight>,
				FMaterialCompileFlightKeyHash> Flights;
			std::deque<FMaterialCompileResult> Mailbox;
			std::vector<std::shared_ptr<FMaterialCompileFlight>> Operations;
			std::vector<std::shared_ptr<FMaterialCompileFlight>> CompletingFlights;
			std::unordered_map<FMaterialCompileFlightKey, std::shared_ptr<FMaterialCompileFlight>, FMaterialCompileFlightKeyHash> ProgramFlights;
			uint64 CapturedBytes = 0;
			FMaterialCompilationDiagnostics Diagnostics;
		};


		std::atomic_uint8_t GPendingShaderReloadMode = 0;
		auto CancelMaterialCompileDomain(DMaterialInterface& Material) -> bool;

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
			FObjectCacheContext CompletionContext;
			for (FMaterialCompileResult& Result : Results)
			{
				DObject* Object = Result.Owner.Get();
				if (auto* Material = Cast<DMaterialInterface>(Object); IsValid(Material)
					&& Private::FMaterialCompilationLifecycle::Admit(
						*Material, std::move(Result), &CompletionContext))
					Processed.SuccessfullyCompiledAssets.emplace_back(Material);
				State.ConsumeOutstanding();
			}
			Private::GetMaterialCompileRetryQueue().Process(
				Private::MaterialCompileMaxRetryChecks, [&State](FWeakObjectPtr Owner) {
					auto* Material = Cast<DMaterialInterface>(Owner.Get());
					if (!IsValid(Material)) return false;
					const auto IsDeferred = [&] {
						const auto Status = Material->GetMaterialCompileStatus().State;
						return Status == EMaterialCompileState::Deferred
							|| Status == EMaterialCompileState::Scheduled;
					};
					if (!IsDeferred()) return false;
					if (!State.IsAccepting())
						Private::FMaterialCompilationLifecycle::MarkCanceled(*Material);
					else
						Private::FMaterialCompilationLifecycle::RetryDeferred(*Material);
					return IsDeferred();
				});
			return Processed;
		}

		class FMaterialCompilingManager final : public IAssetCompilingManager
		{
		public:
			auto GetState() -> FMaterialCompilationState& { return State; }
			auto GetState() const -> const FMaterialCompilationState& { return State; }
			auto Start() -> FAssetCompilerStartResult override
			{
				return {State.Initialize() ? EAssetCompilerStartError::None : EAssetCompilerStartError::StateUnavailable};
			}
			auto StopAdmission() -> void override
			{
				State.StopAdmission();
				auto& RetryQueue = Private::GetMaterialCompileRetryQueue();
				RetryQueue.Process(RetryQueue.Num(), [](FWeakObjectPtr Owner) {
					if (auto* Material = Cast<DMaterialInterface>(Owner.Get()); IsValid(Material))
						Private::FMaterialCompilationLifecycle::MarkCanceled(*Material);
					return false;
				});
			}
			auto GetNumRemainingAssets() const -> uint64 override
			{
				return State.GetDiagnostics().OutstandingConsumerCount
					+ Private::GetMaterialCompileRetryQueue().Num();
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
				std::vector<FWeakObjectPtr> Owners;
				for (DObject* Object : Objects)
					if (auto* Material = Cast<DMaterialInterface>(Object); IsValid(Material))
					{
						if (Material->GetMaterialCompileStatus().State == EMaterialCompileState::Scheduled)
							Private::FMaterialCompilationLifecycle::RequestCurrent(*Material, false);
						Owners.push_back(FWeakObjectPtr(Material));
					}
				while (std::ranges::any_of(Owners, [this](FWeakObjectPtr Owner) {
					auto* Material = Cast<DMaterialInterface>(Owner.Get());
					return State.HasOwner(Owner) || (IsValid(Material)
						&& Material->GetMaterialCompileStatus().State == EMaterialCompileState::Deferred);
				}))
				{
					State.RaisePendingPriority(Owners);
					auto Item = PumpMaterialCompileResultsDetailed(
						State, std::numeric_limits<uint32>::max());
					Aggregate.ProcessedCompletionCount += Item.ProcessedCompletionCount;
					Aggregate.SuccessfullyCompiledAssets.insert(
						Aggregate.SuccessfullyCompiledAssets.end(),
						Item.SuccessfullyCompiledAssets.begin(),
						Item.SuccessfullyCompiledAssets.end());
					if (Item.ProcessedCompletionCount == 0) std::this_thread::yield();
				}
				State.DrainSelected(Owners);
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
					State.RaisePendingPriority();
					auto Item = PumpMaterialCompileResultsDetailed(
						State, std::numeric_limits<uint32>::max());
					Aggregate.ProcessedCompletionCount += Item.ProcessedCompletionCount;
					Aggregate.SuccessfullyCompiledAssets.insert(
						Aggregate.SuccessfullyCompiledAssets.end(),
						Item.SuccessfullyCompiledAssets.begin(),
						Item.SuccessfullyCompiledAssets.end());
					if (Item.ProcessedCompletionCount == 0) std::this_thread::yield();
				}
				State.DrainOperations();
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
		auto GetMaterialCompileRetryQueue() -> FMaterialCompileRetryQueue&
		{
			CheckMaterialCompileGameThread();
			static FMaterialCompileRetryQueue Queue;
			return Queue;
		}

		auto FMaterialCompilationLifecycle::ScheduleEdit(DMaterialInterface& Material, FObjectCacheContext* Context) -> void
		{
			CheckMaterialCompileGameThread();
#if DURIN_WITH_EDITORONLY_DATA
			if (Material.IsDynamicInstance()) return;
			if (GetAssetRuntimeConfiguration().RequiresCookedPayload()) return;
			DMaterialInterface* Root = &Material;
			for (uint32 Depth = 0; Root && Root->GetParent() && Depth < MaterialMaximumParentDepth; ++Depth)
				Root = Root->GetParent();
			const auto* Base = Cast<DMaterial>(Root);
			const auto Mode = Base ? Base->GetEditCompileMode() : EMaterialEditCompileMode::Immediate;
			if (Mode == EMaterialEditCompileMode::Immediate)
			{
				RequestCurrent(Material, false, Context);
				return;
			}
			CancelMaterialCompileDomain(Material);
			Material.CompilationOwner.LastObservedShaderProperties =
				CanonicalizeMaterialShaderProperties(Material.GetStaticProperties());
			auto& Declarations = Material.CompilationOwner.LastObservedParameters;
			Declarations.clear();
			for (const auto& Definition : Material.GetParameterDefinitions())
				Declarations.push_back({Definition.Id, Definition.Type});
			std::ranges::sort(Declarations, {}, &FMaterialCompilerParameterDeclaration::Id);
			auto& Status = Material.CompilationOwner.MaterialCompileStatus;
			Status.State = Mode == EMaterialEditCompileMode::Automatic
				? EMaterialCompileState::Scheduled : EMaterialCompileState::NeedsCompile;
			Status.ResultCategory = EMaterialCompileResultCategory::None;
			Status.CacheOutcome = EMaterialCompileCacheOutcome::None;
			Status.TaskId = 0;
			Material.CompilationOwner.MaterialCompileDiagnostics.clear();
			Material.CompilationOwner.bDeferredForceRecompile = false;
			Material.CompilationOwner.EditCompileDeadline =
				std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
			if (Status.State == EMaterialCompileState::Scheduled)
				GetMaterialCompileRetryQueue().Add(FWeakObjectPtr(&Material));
#endif
		}

		auto FMaterialCompilationLifecycle::Submit(
			DMaterialInterface& Material,
			MIR::FCompilerInput Input,
			bool bForceRecompile, std::vector<FMaterialFunctionOwnerStamp> FunctionOwners, FObjectCacheContext* Context) -> bool
		{
			CheckMaterialCompileGameThread();
			if (Material.IsDynamicInstance()) return false;
			Private::GetMaterialCompileRetryQueue().Remove(FWeakObjectPtr(&Material));
			const auto Manager = GetMaterialCompilingManager();
			FMaterialCompilationState* Compilation =
				Manager ? &Manager->GetState() : nullptr;
			// Construction precedes DObject handle registration. In a running engine,
			// defer that bootstrap request to PostLoad, an authored edit, or an
			// explicit request instead of performing expensive work on GameThread.
			if (FWeakObjectPtr(&Material).GetKey().IsNull()
				&& Compilation && Compilation->IsAccepting())
			{
				Material.CompilationOwner.MaterialCompileStatus.State =
					EMaterialCompileState::NeverRequested;
				return true;
			}
			Material.CompilationOwner.MaterialCompileStatus.State = EMaterialCompileState::Pending;
			Material.CompilationOwner.RequestedFunctionOwners = std::move(FunctionOwners);
#if DURIN_WITH_EDITOR
			if (Compilation && Compilation->IsAccepting() && IsTaskSchedulerRunning() && !FScopedOfflinePreparation::IsActive())
			{
				auto& Status = Material.CompilationOwner.MaterialCompileStatus;
				Status.RequestGeneration = AdvanceNonzero(Status.RequestGeneration);
				Status.DependencyRevision = GetShaderReloadGeneration(); Status.Target = Input.Environment.Target;
				Status.RequestedIdentity = {}; Status.TaskId = 0; Status.CacheOutcome = EMaterialCompileCacheOutcome::None;
				Material.CompilationOwner.RequestedExpressionSources = Input.Sources;
				Material.CompilationOwner.MaterialCompileDiagnostics.clear();
				FMaterialCompileRequest Request{
					.Owner = FWeakObjectPtr(&Material), .AuthoredRevision = Status.AuthoredRevision,
					.Generation = Status.RequestGeneration, .DependencyRevision = Status.DependencyRevision,
					.ParentChainRevision = Status.ParentChainRevision,
					.CompilerInput = std::make_shared<const MIR::FCompilerInput>(std::move(Input)),
					.AssetPath = Material.GetObjectPath(), .Target = Status.Target, .bForceRecompile = bForceRecompile};
				const auto Submitted = Compilation->Submit(std::move(Request));
				Status.State = Submitted;
				if (Submitted == EMaterialCompileState::Deferred)
				{
					Material.CompilationOwner.bDeferredForceRecompile = bForceRecompile;
					GetMaterialCompileRetryQueue().Add(FWeakObjectPtr(&Material));
					return true;
				}
				if (Submitted == EMaterialCompileState::Rejected)
				{
					Status.ResultCategory = EMaterialCompileResultCategory::Admission;
					Material.RetireFailedMaterialGeneration(Context); return false;
				}
				return true;
			}
#endif
			auto& Status = Material.CompilationOwner.MaterialCompileStatus;
			Status.RequestGeneration = AdvanceNonzero(Status.RequestGeneration);
			Status.DependencyRevision = GetShaderReloadGeneration(); Status.Target = Input.Environment.Target;
			Status.TaskId = 0; Status.CacheOutcome = EMaterialCompileCacheOutcome::None;
			Material.CompilationOwner.MaterialCompileDiagnostics.clear();
			auto Build = MaterialBuildPrivate::CompileMaterialInput(Input, bForceRecompile);
			auto& Compiled = Build.Program;
			Status.RequestedIdentity = Compiled.Identity;
			Material.CompilationOwner.RequestedExpressionSources = Build.Sources.empty() ? Input.Sources : Build.Sources;
			FMaterialCompileResult Result{
				.Owner = FWeakObjectPtr(&Material), .AuthoredRevision = Status.AuthoredRevision,
				.Generation = Status.RequestGeneration, .DependencyRevision = Status.DependencyRevision,
				.ParentChainRevision = Status.ParentChainRevision, .ProgramIdentity = Compiled.Identity,
				.StaticProperties = Input.StaticProperties, .Target = Status.Target,
				.State = Compiled ? EMaterialCompileState::Ready : EMaterialCompileState::Failed,
				.Category = Compiled || Compiled.Diagnostics.empty() ? EMaterialCompileResultCategory::None : MapProgramCategory(Compiled.Diagnostics.front().Category),
				.CacheOutcome = Build.RetainedHit ? EMaterialCompileCacheOutcome::RetainedHit : bForceRecompile ? EMaterialCompileCacheOutcome::Forced : EMaterialCompileCacheOutcome::Compiled,
				.CompiledProgram = Build.Shared};
			if (!Compiled) for (auto& Diagnostic : Compiled.Diagnostics) Result.Diagnostics.push_back({.Category = MapProgramCategory(Diagnostic.Category), .Source = std::move(Diagnostic), .AssetPath = Material.GetObjectPath(), .Generation = Status.RequestGeneration});
			Admit(Material, std::move(Result), Context);
			return Status.State == EMaterialCompileState::Ready;
		}

		auto FMaterialCompilationLifecycle::Admit(
			DMaterialInterface& Material, FMaterialCompileResult Result, FObjectCacheContext* Context) -> bool
		{
				CheckMaterialCompileGameThread();
				FMaterialCompileStatus& Status = Material.CompilationOwner.MaterialCompileStatus;
				if (Status.HasUnsubmittedEdits()) return false;
				if (Result.Generation != Status.RequestGeneration
					|| Result.AuthoredRevision != Status.AuthoredRevision
					|| Result.DependencyRevision != Status.DependencyRevision
					|| Result.ParentChainRevision != Status.ParentChainRevision
					|| Result.Target != Status.Target)
				{
					return false;
				}

				if (Result.DependencyRevision != GetShaderReloadGeneration())
				{ RequestCurrent(Material, false, Context); return false; }

				const auto OwnerHandle = FWeakObjectPtr(&Material);
				if (!OwnerHandle.GetKey().IsNull())
				{
					if (!SameOwner(Result.Owner, OwnerHandle)) return false;
					FResolvedMaterialProperties Resolved;
					const auto Error = ResolveMaterialProperties(Material, Resolved);
					if (!Error)
					{
						RequestCurrent(Material, false, Context);
						return false;
					}
				}

				if (!AreMaterialFunctionOwnersCurrent(Material.CompilationOwner.RequestedFunctionOwners))
				{
					Status.AuthoredRevision = AdvanceNonzero(Status.AuthoredRevision);
					ScheduleEdit(Material, Context);
					return false;
				}
				// Detached translation supplies the identity for this qualified generation.
				if (!Status.RequestedIdentity.IsValid()) Status.RequestedIdentity = Result.ProgramIdentity;
				if (!Result.ExpressionSources.empty()) Material.CompilationOwner.RequestedExpressionSources = std::move(Result.ExpressionSources);
				const bool SourceChanged = std::ranges::any_of(Result.Diagnostics, [](const auto& Diagnostic) {
					const auto* Error = std::get_if<EMaterialCompileError>(&Diagnostic.Source.Error.Code);
					return Error && *Error == EMaterialCompileError::SourceDependencyChanged;
				});
				if (SourceChanged && Status.SourceRetryCount == 0)
				{
					++Status.SourceRetryCount; AdvanceShaderReloadGeneration();
					RequestCurrent(Material, false, Context); return false;
				}

				Status.State = Result.State;
				Status.ResultCategory = Result.Category;
				Status.CacheOutcome = Result.CacheOutcome;
				Status.TaskId = Result.TaskId;
				Material.CompilationOwner.MaterialCompileDiagnostics = std::move(Result.Diagnostics);
				if (Result.State == EMaterialCompileState::Ready
					&& Result.CompiledProgram
					&& Result.ProgramIdentity == Status.RequestedIdentity
					&& Result.CompiledProgram->Identity == Result.ProgramIdentity
					&& ValidateMaterialCompilerResult(*Result.CompiledProgram))
				{
					FMaterialLocalRenderLayer Candidate;
					Candidate.CompiledProgram = Result.CompiledProgram;
					Status.SourceRetryCount = 0;
					if (!Candidate.CompiledProgram->ActiveCollections.empty())
					{
						auto Current = MaterialBuildPrivate::ResolvePublicationCollections(Candidate.CompiledProgram->ActiveCollections);
						if (!Current) { ScheduleEdit(Material, Context); return false; }
						if (auto Refreshed = MaterialBuildPrivate::BindProgramCollections(Candidate.CompiledProgram, *Current)) Candidate.CompiledProgram = std::move(Refreshed);
					}
					const auto ShaderProperties = CanonicalizeMaterialShaderProperties(Result.StaticProperties);
					Candidate.StaticProperties = Material.GetStaticProperties();
					Candidate.StaticProperties->OpacityMaskThreshold = ShaderProperties.OpacityMaskThreshold;
					if (CanonicalizeMaterialShaderProperties(*Candidate.StaticProperties) != ShaderProperties)
					{
						Status.State = EMaterialCompileState::Superseded;
						return false;
					}
					for (const auto& Parameter : Candidate.CompiledProgram->ActiveParameters)
					{
						FResolvedMaterialParameter Resolved;
						if (!Material.ResolveParameterValue(Parameter.Id, Resolved)
							|| !Resolved.Definition || Resolved.Definition->Type != Parameter.Type)
						{
							Status.State = EMaterialCompileState::Rejected;
							Status.ResultCategory = EMaterialCompileResultCategory::Admission;
							Material.RetireFailedMaterialGeneration(Context);
							return false;
						}
						Candidate.Parameters.push_back(BuildMaterialLocalRenderParameter(
							Parameter.Id, Resolved.Value));
					}
					Material.CompilationOwner.RenderLayer = std::move(Candidate);
					Material.CompilationOwner.AcceptedExpressionSources = Material.CompilationOwner.RequestedExpressionSources;
					Status.CompiledIdentity = Result.ProgramIdentity;
					Status.DurationMicroseconds =
						Material.CompilationOwner.RenderLayer.CompiledProgram->Timings.NormalizationMicroseconds
						+ Material.CompilationOwner.RenderLayer.CompiledProgram->Timings.GenerationMicroseconds
						+ Material.CompilationOwner.RenderLayer.CompiledProgram->Timings.CompilationMicroseconds;
					Material.MarkRenderDataDirty(
						EMaterialRenderDirtyFlags::ShaderMap
							| EMaterialRenderDirtyFlags::PipelineState, false, Context);
					return true;
				}

				if (Result.State == EMaterialCompileState::Ready)
				{
					Status.State = EMaterialCompileState::Rejected;
					Status.ResultCategory = EMaterialCompileResultCategory::Admission;
				}
				if (Status.State == EMaterialCompileState::Failed
					|| Status.State == EMaterialCompileState::Rejected)
					Material.RetireFailedMaterialGeneration(Context);
				return false;
		}

		auto FMaterialCompilationLifecycle::MarkCanceled(
			DMaterialInterface& Material) -> void
		{
			GetMaterialCompileRetryQueue().Remove(FWeakObjectPtr(&Material));
			Material.CompilationOwner.MaterialCompileStatus.State = EMaterialCompileState::Canceled;
			Material.CompilationOwner.MaterialCompileStatus.ResultCategory =
				EMaterialCompileResultCategory::Cancellation;
		}

		auto FMaterialCompilationLifecycle::RetryDeferred(DMaterialInterface& Material) -> void
		{
			if (Material.GetMaterialCompileStatus().State == EMaterialCompileState::Scheduled
				&& std::chrono::steady_clock::now() < Material.CompilationOwner.EditCompileDeadline) return;
			RequestCurrent(Material, Material.CompilationOwner.bDeferredForceRecompile);
		}

		auto FMaterialCompilationLifecycle::RequestCurrent(
			DMaterialInterface& Material, bool bForceRecompile, FObjectCacheContext* Context) -> bool
		{
			if (Material.IsDynamicInstance()) return false;
			GetMaterialCompileRetryQueue().Remove(FWeakObjectPtr(&Material));
			if (GetAssetRuntimeConfiguration().RequiresCookedPayload()) return false;
			FResolvedMaterialProperties Resolved;
			const auto Error = ResolveMaterialProperties(Material, Resolved);
			if (!Error)
			{
				Material.CompilationOwner.MaterialCompileStatus.RequestGeneration = AdvanceNonzero(
					Material.CompilationOwner.MaterialCompileStatus.RequestGeneration);
				Material.CompilationOwner.MaterialCompileStatus.State = EMaterialCompileState::Failed;
				Material.CompilationOwner.MaterialCompileStatus.ResultCategory = EMaterialCompileResultCategory::Dependency;
				Material.CompilationOwner.MaterialCompileDiagnostics = {{
					.Category = EMaterialCompileResultCategory::Dependency,
					.Source = {.Category = EMaterialProgramDiagnosticCategory::Dependency,
						.Error = Error.Error.HasError() ? Error.Error : FMaterialError(EMaterialCompileError::NoAuthoredProgram)},
					.AssetPath = Material.GetObjectPath(),
					.Generation = Material.CompilationOwner.MaterialCompileStatus.RequestGeneration}};
				Material.RetireFailedMaterialGeneration(Context);
				return false;
			}
			return Material.RequestProgramCompile(
				Material.GetStaticProperties(), bForceRecompile, Context);
		}
	}

	auto IsMaterialCompilationAcceptingRequests() -> bool
	{
		const auto Manager = GetMaterialCompilingManager();
		return Manager && Manager->GetState().IsAccepting();
	}

	auto HasPendingMaterialCompilation(const DMaterialInterface& Material) -> bool
	{
		CheckMaterialCompileGameThread();
		const auto Manager = GetMaterialCompilingManager();
		const auto State = Material.GetMaterialCompileStatus().State;
		return (Manager && Manager->GetState().HasOwner(FWeakObjectPtr(const_cast<DMaterialInterface*>(&Material))))
			|| State == EMaterialCompileState::Deferred || State == EMaterialCompileState::Scheduled;
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
			Private::GetMaterialCompileRetryQueue().Remove(FWeakObjectPtr(&Material));
			const auto Manager = GetMaterialCompilingManager();
			const bool bCanceled = Manager
				&& Manager->GetState().CancelOwner(FWeakObjectPtr(&Material));
			if (bCanceled || Material.GetMaterialCompileStatus().State == EMaterialCompileState::Deferred
				|| Material.GetMaterialCompileStatus().State == EMaterialCompileState::Scheduled)
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
