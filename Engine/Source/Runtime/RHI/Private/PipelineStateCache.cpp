#include "PipelineStateCache.h"
#include "Backend/RHICreation.h"
#include "Backend/RHIPipelineStateCacheBackend.h"
#include "PipelineCompileQueue.h"
#include "DynamicRHI.h"
#include "RHICommandList.h"

namespace Durin
{
	FPipelineState::FPipelineState(FPipelineCompilation&& InCompilation, std::shared_ptr<void> InMetadata)
		: Compilation(std::make_unique<FPipelineCompilation>(std::move(InCompilation))), Metadata(std::move(InMetadata)) {}
	FPipelineState::~FPipelineState() = default;
	auto FPipelineState::GetState() const -> ERHIPipelineState { return Compilation->GetState(); }
	auto FPipelineState::GetStateObservation() const -> FRHIPipelineStateObservation { return Compilation->GetStateObservation(); }
	auto FPipelineState::GetPipelineLayout() const -> std::shared_ptr<const FPipelineLayoutDesc> { return Compilation->GetPipelineLayout(); }
	auto FPipelineState::Wait() const -> ERHIPipelineState { return Compilation->Wait(); }
	auto FPipelineState::BoostPriority(EPSOPrecachePriority Priority) const -> void { Compilation->BoostPriority(Priority); }
	auto FPipelineState::GetPriority() const -> EPSOPrecachePriority { return Compilation->GetPriority(); }

	auto FGraphicsPipelineState::GetRHIPipeline() const -> FGraphicsPipelineStateRHIRef
	{ return FRHIPipelineStateCacheBackend::GetGraphicsPipeline(*this); }
	auto FComputePipelineState::GetRHIPipeline() const -> FComputePipelineStateRHIRef
	{ return FRHIPipelineStateCacheBackend::GetComputePipeline(*this); }

	struct FRHIPipelineStateCache::FState
	{
		using FKey = std::variant<FGraphicsPipelineStateKey, FComputePipelineStateKey>;
		struct FKeyHash
		{
			auto operator()(const FKey& Key) const -> size_t
			{
				return std::visit([](const auto& Value) -> size_t {
					if constexpr (std::same_as<std::decay_t<decltype(Value)>, FGraphicsPipelineStateKey>)
						return FGraphicsPipelineStateKeyHasher{}(Value);
					else return FComputePipelineStateKeyHasher{}(Value);
				}, Key) ^ Key.index();
			}
		};
		struct FEntry
		{
			std::weak_ptr<FPipelineState> Pipeline;
			std::shared_ptr<void> Metadata;
		};
		FState(const FRHICapabilities& InCapabilities, FRHIPipelineCompileBackend Backend)
			: Capabilities(InCapabilities), Queue(InCapabilities, std::move(Backend)) {}

		template<typename T, typename TKeyFactory>
		auto Get(const T& Initializer, std::string_view Name, TKeyFactory&& MakeKey)
		{
			constexpr bool Graphics = std::same_as<T, FGraphicsPipelineStateInitializer>;
			using TPipeline = std::conditional_t<Graphics, FGraphicsPipelineState, FComputePipelineState>;
			using TResult = std::shared_ptr<TPipeline>;
			std::lock_guard Lock(Mutex);
			const auto Now = std::chrono::steady_clock::now();
			if (Now - LastPrune >= std::chrono::seconds(60))
			{
				std::erase_if(Entries, [](const auto& Entry) { return Entry.second.Pipeline.expired(); });
				LastPrune = Now;
			}
			if (Closed || Queue.IsClosed()) return TResult{};
			try
			{
				auto Valid = MakeKey();
				if (!Valid)
				{
					return TResult{};
				}
				FKey Key(std::move(*Valid));
				if (auto Existing = Entries.find(Key); Existing != Entries.end())
				{
					if (auto Pipeline = Existing->second.Pipeline.lock())
					{
						const auto Status = Pipeline->GetState();
						if (Status == ERHIPipelineState::Pending || Status == ERHIPipelineState::Ready
							|| Status == ERHIPipelineState::Failed)
							return TResult(std::static_pointer_cast<TPipeline>(std::move(Pipeline)));
					}
					Entries.erase(Existing);
				}
				if (Private::FTaskRuntimeAccess::IsExecutingIndependentCPU()) return TResult{};
				const uint64 Bytes = sizeof(FEntry) + sizeof(TPipeline) + 256
					+ std::visit([](const auto& Value) { return GetPipelineKeyOwnedBytes(Value); }, Key);
				auto Metadata = Queue.ReserveCacheMetadata(Bytes);
				auto Observation = [&] {
					if constexpr (Graphics) return Queue.RequestValidated(Initializer, Name, std::get<FGraphicsPipelineStateKey>(Key));
					else return Queue.RequestValidated(Initializer, Name, std::get<FComputePipelineStateKey>(Key));
				}();
				if (!Observation.IsValid()) return TResult{};
				auto Pipeline = std::shared_ptr<TPipeline>(new TPipeline(std::move(Observation), Metadata));
				Entries.emplace(std::move(Key), FEntry{Pipeline, std::move(Metadata)});
				return TResult(std::move(Pipeline));
			}
			catch (const std::bad_alloc&)
			{
				RHITerminateOnOutOfMemory("RHI pipeline metadata");
			}
		}

		FRHICapabilities Capabilities;
		FPipelineCompileQueue Queue;
		std::mutex Mutex;
		std::unordered_map<FKey, FEntry, FKeyHash> Entries;
		std::chrono::steady_clock::time_point LastPrune = std::chrono::steady_clock::now();
		bool Closed = false;
	};

	FRHIPipelineStateCache::FRHIPipelineStateCache(const FRHICapabilities& Capabilities, FRHIPipelineCompileBackend Backend)
		: State(std::make_unique<FState>(Capabilities, std::move(Backend))) {}
	FRHIPipelineStateCache::~FRHIPipelineStateCache() { ReleaseResources(); }
	auto FRHIPipelineStateCache::GetGraphics(const FGraphicsPipelineStateInitializer& Initializer, std::string_view Name)
		-> FGraphicsPipelineStateRef
	{
		return State->Get(Initializer, Name, [&] {
			return BuildGraphicsPipelineStateKey(Initializer, &State->Capabilities);
		});
	}
	auto FRHIPipelineStateCache::GetCompute(const FComputePipelineStateInitializer& Initializer, std::string_view Name)
		-> FComputePipelineStateRef
	{
		return State->Get(Initializer, Name, [&] {
			return BuildComputePipelineStateKey(Initializer, &State->Capabilities);
		});
	}
	auto FRHIPipelineStateCacheBackend::GetGraphicsValidated(FRHIPipelineStateCache& Cache,
		const FGraphicsPipelineStateInitializer& Initializer, std::string_view Name, FGraphicsPipelineStateKey Key)
		-> FGraphicsPipelineStateRef
	{
		return Cache.State->Get(Initializer, Name, [&] { return std::optional{std::move(Key)}; });
	}
	auto FRHIPipelineStateCacheBackend::GetComputeValidated(FRHIPipelineStateCache& Cache,
		const FComputePipelineStateInitializer& Initializer, std::string_view Name, FComputePipelineStateKey Key)
		-> FComputePipelineStateRef
	{
		return Cache.State->Get(Initializer, Name, [&] { return std::optional{std::move(Key)}; });
	}

	auto FRHIPipelineStateCache::StopAndWait() -> void
	{
		{
			std::lock_guard Lock(State->Mutex);
			State->Closed = true;
			State->Entries.clear();
		}
		State->Queue.StopAndWait();
	}
	auto FRHIPipelineStateCache::ReleaseResources() -> void
	{
		StopAndWait();
		State->Queue.ReleaseResources();
	}
	auto FRHIPipelineStateCache::IsClosed() const -> bool
	{
		std::lock_guard Lock(State->Mutex);
		return State->Closed || State->Queue.IsClosed();
	}
	auto FRHIPipelineStateCache::GetStatistics() const -> FRHIPipelineCreationStatistics
	{ return State->Queue.GetStatistics(); }

	namespace PipelineStateCache
	{
		template<typename T> static auto Precache(const T& Initializer, FName Name)
		{
			constexpr bool Graphics = std::same_as<T, FGraphicsPipelineStateInitializer>;
			using TResult = std::conditional_t<Graphics, FGraphicsPipelineStateRef, FComputePipelineStateRef>;
			if (!GDynamicRHI) return TResult{};
			auto* Cache = GDynamicRHI->RHIGetPipelineStateCache();
			if (!Cache) return TResult{};
			char NameBuffer[FName::StringBufferSize];
			std::string_view NameView = Name.GetPlainNameView();
			if (Name.HasNumber())
			{
				size_t Length = 0;
				const bool Written = Name.TryWriteString(NameBuffer, Length);
				require(Written);
				NameView = std::string_view(NameBuffer, Length);
			}
			if constexpr (Graphics) return Cache->GetGraphics(Initializer, NameView);
			else return Cache->GetCompute(Initializer, NameView);
		}
		auto PrecacheGraphicsPipelineState(const FGraphicsPipelineStateInitializer& Initializer, FName Name) -> FGraphicsPipelineStateRef
		{ return Precache(Initializer, Name); }
		auto PrecacheComputePipelineState(const FComputePipelineStateInitializer& Initializer, FName Name) -> FComputePipelineStateRef
		{ return Precache(Initializer, Name); }
		template<typename T> static auto AcquireRequired(const T& Initializer, FName Name)
		{
			const char* Kind = std::same_as<T, FGraphicsPipelineStateInitializer> ? "Graphics PSO" : "Compute PSO";
			auto Pipeline = Precache(Initializer, Name);
			if (!Pipeline) RHITerminateOnFailure(Kind, "Required pipeline could not be acquired");
			if (Pipeline->GetState() == ERHIPipelineState::Failed)
				RHITerminateOnFailure(Kind, "Required pipeline compilation failed");
			if (Pipeline->GetState() == ERHIPipelineState::Pending) Pipeline->BoostPriority(EPSOPrecachePriority::Highest);
			return Pipeline;
		}
		auto GetAndOrCreateGraphicsPipelineState(FRHICommandListBase& CommandList, const FGraphicsPipelineStateInitializer& Initializer, FName Name) -> FGraphicsPipelineStateRef
		{
			auto Pipeline = AcquireRequired(Initializer, Name);
			CommandList.AddPipelineDependency(Pipeline);
			return Pipeline;
		}
		auto GetAndOrCreateComputePipelineState(FRHICommandListBase& CommandList, const FComputePipelineStateInitializer& Initializer, FName Name) -> FComputePipelineStateRef
		{
			auto Pipeline = AcquireRequired(Initializer, Name);
			CommandList.AddPipelineDependency(Pipeline);
			return Pipeline;
		}
	}

	auto SetGraphicsPipelineState(FRHICommandListBase& CommandList, const FGraphicsPipelineStateInitializer& Initializer, FName Name) -> void
	{
		const auto* Active = CommandList.GetActiveRenderTargetLayout();
		const auto& Layout = Initializer.RenderTargetLayout;
		requiref(Active && Layout.NumColorRenderTargets <= MaxSimultaneousRenderTargets && Active->NumColorRenderTargets == Layout.NumColorRenderTargets
			&& Active->bHasDepthStencil == Layout.bHasDepthStencil, "Graphics PSO requires matching active render-pass attachments.");
		const auto Matches = [](const FRHIAttachmentLayout& Left, const FRHIAttachmentLayout& Right) {
			return Left.Format == Right.Format && Left.NumSamples == Right.NumSamples;
		};
		for (uint32 Index = 0; Index < Layout.NumColorRenderTargets; ++Index)
		{
			const auto& Expected = Layout.ColorAttachments[Index];
			const auto& Actual = Active->ColorAttachments[Index];
			requiref(Matches(Expected.RenderTarget, Actual.RenderTarget) && Expected.bHasResolveTarget == Actual.bHasResolveTarget
				&& (!Expected.bHasResolveTarget || Matches(Expected.ResolveTarget, Actual.ResolveTarget)), "Graphics PSO color attachment mismatch.");
		}
		requiref(!Layout.bHasDepthStencil || Matches(Layout.DepthStencilAttachment, Active->DepthStencilAttachment), "Graphics PSO depth/stencil attachment mismatch.");
		CommandList.SetGraphicsPipelineState(PipelineStateCache::GetAndOrCreateGraphicsPipelineState(CommandList, Initializer, Name));
	}
	auto SetComputePipelineState(FRHICommandListBase& CommandList, const FComputePipelineStateInitializer& Initializer, FName Name) -> void
	{
		CommandList.SetComputePipelineState(PipelineStateCache::GetAndOrCreateComputePipelineState(CommandList, Initializer, Name));
	}
}
