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
		struct FGraphicsHash
		{
			auto operator()(const FGraphicsPipelineStateInitializer& Init) const -> size_t
			{
				// Shader and declaration identities select the bucket. Equality resolves
				// fixed state and attachment variants of the same shader combination.
				size_t Hash = std::hash<FRHIShader*>{}(Init.BoundShaders.VertexShader);
				const auto Combine = [&](size_t Value) {
					Hash ^= Value + 0x9e3779b97f4a7c15ull + (Hash << 6) + (Hash >> 2);
				};
				Combine(std::hash<FRHIShader*>{}(Init.BoundShaders.FragmentShader));
				Combine(std::hash<FRHIVertexDeclaration*>{}(Init.VertexDeclaration));
				Combine(std::hash<const void*>{}(Init.RasterizerState.get()));
				Combine(std::hash<const void*>{}(Init.DepthStencilState.get()));
				Combine(std::hash<const void*>{}(Init.BlendState.get()));
				Combine(static_cast<size_t>(Init.PrimitiveTopology));
				Combine(static_cast<size_t>(Init.RenderTargetLayout.ColorAttachments[0].RenderTarget.Format));
				return Hash;
			}
		};
		struct FEntry
		{
			std::weak_ptr<FPipelineState> Pipeline;
			std::shared_ptr<void> Metadata;
		};
		struct FGraphicsEntry : FEntry
		{
			// Keep pointer identities valid for the lifetime of the map key.
			FShaderRHIRef VertexShader, FragmentShader;
			FVertexDeclarationRHIRef Declaration;
		};
		FState(const FRHICapabilities& InCapabilities, FRHIPipelineCompileBackend Backend)
			: Capabilities(InCapabilities), Queue(InCapabilities, std::move(Backend)) {}

		template<typename TPipeline, typename TMap, typename TKey>
		static auto Find(TMap& Entries, const TKey& Key) -> std::shared_ptr<TPipeline>
		{
			auto It = Entries.find(Key);
			if (It == Entries.end()) return {};
			if (auto Pipeline = It->second.Pipeline.lock(); Pipeline && Pipeline->GetState() != ERHIPipelineState::Canceled)
				return std::static_pointer_cast<TPipeline>(std::move(Pipeline));
			Entries.erase(It);
			return {};
		}

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
				const auto Expired = [](const auto& Entry) { return Entry.second.Pipeline.expired(); };
				std::erase_if(GraphicsEntries, Expired);
				std::erase_if(ComputeEntries, Expired);
				LastPrune = Now;
			}
			if (Closed || Queue.IsClosed()) return TResult{};
			try
			{
				if constexpr (Graphics)
					if (auto Hit = Find<TPipeline>(GraphicsEntries, Initializer)) return Hit;
				auto Key = MakeKey();
				if (!Key) return TResult{};
				if constexpr (!Graphics)
					if (auto Hit = Find<TPipeline>(ComputeEntries, *Key)) return Hit;
				auto Observation = Queue.RequestValidated(Initializer, Name, *Key);
				if (!Observation.IsValid()) return TResult{};
				uint64 Bytes = sizeof(TPipeline) + 256;
				if constexpr (Graphics)
				{
					Bytes += sizeof(FGraphicsEntry) + sizeof(T)
						+ Initializer.PipelineLayout.BindingLayouts.size() * sizeof(FBindingLayout)
						+ Initializer.PipelineLayout.PushConstantRanges.size() * sizeof(FPushConstantRange);
					for (const auto& Set : Initializer.PipelineLayout.BindingLayouts)
						Bytes += Set.BindingLayouts.size() * sizeof(FBindingLayoutItem);
				}
				else Bytes += sizeof(FEntry) + GetPipelineKeyOwnedBytes(*Key);
				auto Metadata = Queue.ReserveCacheMetadata(Bytes);
				auto Pipeline = std::shared_ptr<TPipeline>(new TPipeline(std::move(Observation), Metadata));
				FEntry Entry{Pipeline, std::move(Metadata)};
				if constexpr (Graphics)
					GraphicsEntries.emplace(Initializer, FGraphicsEntry{std::move(Entry), Initializer.BoundShaders.VertexShader,
						Initializer.BoundShaders.FragmentShader, Initializer.VertexDeclaration});
				else ComputeEntries.emplace(std::move(*Key), std::move(Entry));
				return Pipeline;
			}
			catch (const std::bad_alloc&)
			{
				RHITerminateOnOutOfMemory("RHI pipeline metadata");
			}
		}

		FRHICapabilities Capabilities;
		FPipelineCompileQueue Queue;
		std::mutex Mutex;
		std::unordered_map<FGraphicsPipelineStateInitializer, FGraphicsEntry, FGraphicsHash> GraphicsEntries;
		std::unordered_map<FComputePipelineStateKey, FEntry, FComputePipelineStateKeyHasher> ComputeEntries;
		std::chrono::steady_clock::time_point LastPrune = std::chrono::steady_clock::now();
		bool Closed = false;
	};

	FRHIPipelineStateCache::FRHIPipelineStateCache(const FRHICapabilities& Capabilities, FRHIPipelineCompileBackend Backend)
		: State(std::make_unique<FState>(Capabilities, std::move(Backend))) {}
	FRHIPipelineStateCache::~FRHIPipelineStateCache() { Shutdown(); }
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

	auto FRHIPipelineStateCache::Shutdown() -> void
	{
		{
			std::lock_guard Lock(State->Mutex);
			State->Closed = true;
			State->GraphicsEntries.clear();
			State->ComputeEntries.clear();
		}
		State->Queue.Shutdown();
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
			if constexpr (Graphics)
				requiref(Initializer.BoundShaders.VertexShader && Initializer.BoundShaders.FragmentShader && Initializer.VertexDeclaration
					&& Initializer.BlendState && Initializer.DepthStencilState && Initializer.RasterizerState,
					"Graphics PSO needs vertex and fragment shaders and a vertex declaration with fixed states.");
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
			CommandList.RecordPipelineWait(Pipeline);
			return Pipeline;
		}
		auto GetAndOrCreateComputePipelineState(FRHICommandListBase& CommandList, const FComputePipelineStateInitializer& Initializer, FName Name) -> FComputePipelineStateRef
		{
			auto Pipeline = AcquireRequired(Initializer, Name);
			CommandList.RecordPipelineWait(Pipeline);
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
