#pragma once

#include "RenderCoreAPI.h"
#include "RHIResources.h"

#include <functional>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Durin
{
	class FRHICommandListImmediate;
	class FRenderGraphBuilder;
	class FCompiledRenderGraph;
	class FRenderGraphResourceBackings;

	// Selects the command domain used by a declared graph pass.
	enum class ERenderGraphPassType : uint8
	{
		Graphics,
		Compute,
		Copy,
	};

	// Describes whether a pass observes or replaces one declared resource range.
	enum class ERenderGraphUse : uint8
	{
		Read,
		Write,
		ReadWrite,
	};

	// Selects the stable resource category exposed by graph diagnostics.
	enum class ERenderGraphResourceKind : uint8
	{
		Texture,
		Buffer,
		Token,
	};

	// Distinguishes semantic value reachability from execution-only ordering.
	enum class ERenderGraphDependencyKind : uint8
	{
		Value,
		Execution,
		Explicit,
	};

	// Describes a graph-created texture without requiring physical backing.
	struct FRenderGraphTextureDesc final
	{
		FRHITextureDesc Texture;
		std::string BackingClass = "transient";
	};

	// Describes a graph-created buffer without requiring physical backing.
	struct FRenderGraphBufferDesc final
	{
		FRHIBufferDesc Buffer;
		std::string BackingClass = "transient";
	};

	// Identifies one texture registered in a single builder lifetime.
	class FRenderGraphTextureHandle final
	{
	public:
		FRenderGraphTextureHandle() = default;
		auto IsValid() const -> bool { return Owner != 0; }
		auto operator==(const FRenderGraphTextureHandle&) const -> bool = default;

	private:
		friend class FRenderGraphBuilder;
		friend class FCompiledRenderGraph;
		friend class FRenderGraphPassResources;
		friend class FRenderGraphResourceBackings;
		FRenderGraphTextureHandle(uint64 InOwner, uint32 InIndex)
			: Owner(InOwner), Index(InIndex) {}
		uint64 Owner = 0;
		uint32 Index = 0;
	};

	// Identifies one buffer registered in a single builder lifetime.
	class FRenderGraphBufferHandle final
	{
	public:
		FRenderGraphBufferHandle() = default;
		auto IsValid() const -> bool { return Owner != 0; }
		auto operator==(const FRenderGraphBufferHandle&) const -> bool = default;

	private:
		friend class FRenderGraphBuilder;
		friend class FCompiledRenderGraph;
		friend class FRenderGraphPassResources;
		friend class FRenderGraphResourceBackings;
		FRenderGraphBufferHandle(uint64 InOwner, uint32 InIndex)
			: Owner(InOwner), Index(InIndex) {}
		uint64 Owner = 0;
		uint32 Index = 0;
	};

	// Identifies one pass registered in a single builder lifetime.
	class FRenderGraphPassHandle final
	{
	public:
		FRenderGraphPassHandle() = default;
		auto IsValid() const -> bool { return Owner != 0; }
		auto operator==(const FRenderGraphPassHandle&) const -> bool = default;

	private:
		friend class FRenderGraphBuilder;
		FRenderGraphPassHandle(uint64 InOwner, uint32 InIndex)
			: Owner(InOwner), Index(InIndex) {}
		uint64 Owner = 0;
		uint32 Index = 0;
	};

	// Identifies one logical scheduling value with no physical RHI ownership.
	class FRenderGraphTokenHandle final
	{
	public:
		FRenderGraphTokenHandle() = default;
		auto IsValid() const -> bool { return Owner != 0; }
		auto operator==(const FRenderGraphTokenHandle&) const -> bool = default;

	private:
		friend class FRenderGraphBuilder;
		FRenderGraphTokenHandle(uint64 InOwner, uint32 InIndex)
			: Owner(InOwner), Index(InIndex) {}
		uint64 Owner = 0;
		uint32 Index = 0;
	};

	// Exposes only resources declared by the executing graph to pass callbacks.
	class RENDERCORE_API FRenderGraphPassResources final
	{
	public:
		auto GetTexture(FRenderGraphTextureHandle Handle) const -> FRHITexture*;
		auto GetBuffer(FRenderGraphBufferHandle Handle) const -> FRHIBuffer*;

	private:
		friend class FCompiledRenderGraph;
		explicit FRenderGraphPassResources(const FCompiledRenderGraph& InGraph,
			uint32 InPassIndex)
			: Graph(InGraph), PassIndex(InPassIndex)
		{
		}

		const FCompiledRenderGraph& Graph;
		uint32 PassIndex = 0;
	};

	using FRenderGraphExecute = std::function<void(
		FRHICommandListImmediate&, const FRenderGraphPassResources&)>;
	using FRenderGraphPrepare = std::function<bool(std::string&)>;

	// Names one retained logical resource that requires physical backing.
	struct FRenderGraphPreparationRequest final
	{
		uint32 ResourceId = 0;
		std::string Name;
		ERenderGraphResourceKind Kind = ERenderGraphResourceKind::Texture;
		FRenderGraphTextureHandle Texture;
		FRenderGraphBufferHandle Buffer;
		FRHITextureDesc TextureDesc;
		FRHIBufferDesc BufferDesc;
		std::string BackingClass;
		uint32 FirstPass = 0;
		uint32 LastPass = 0;
	};

	// Collects one candidate complete backing publication during preparation.
	class RENDERCORE_API FRenderGraphResourceBackings final
	{
	public:
		auto SetTexture(FRenderGraphTextureHandle Handle, FRHITexture* Texture)
			-> bool;
		auto SetBuffer(FRenderGraphBufferHandle Handle, FRHIBuffer* Buffer)
			-> bool;

	private:
		friend class FCompiledRenderGraph;
		explicit FRenderGraphResourceBackings(uint64 InOwner, uint32 Count);
		uint64 Owner = 0;
		std::vector<FRHITexture*> Textures;
		std::vector<FRHIBuffer*> Buffers;
	};

	using FRenderGraphBackingResolver = std::function<bool(
		std::span<const FRenderGraphPreparationRequest>,
		FRenderGraphResourceBackings&, std::string&)>;

	// Records one immutable dependency edge in compiler diagnostics.
	struct FRenderGraphDependency final
	{
		uint32 BeforePass = 0;
		uint32 AfterPass = 0;
		std::string Cause;
		ERenderGraphDependencyKind Kind = ERenderGraphDependencyKind::Execution;

		auto operator==(const FRenderGraphDependency&) const -> bool = default;
	};

	// Records one pointer-free declared resource and its preparation outcome.
	struct FRenderGraphResourceCapture final
	{
		uint32 ResourceId = 0;
		std::string Name;
		ERenderGraphResourceKind Kind = ERenderGraphResourceKind::Texture;
		bool bImported = false;
		std::string BackingClass;
		std::string Preparation;
		EPixelFormat TextureFormat = EPixelFormat::Unknown;
		FIntPoint TextureExtent{0, 0};
		uint16 TextureArraySize = 0;
		uint8 TextureMips = 0;
		uint64 BufferSize = 0;
		uint32 BufferStride = 0;
	};

	// Records one exact pointer-free pass use after range normalization.
	struct FRenderGraphUseCapture final
	{
		uint32 PassDeclarationIndex = 0;
		uint32 ResourceId = 0;
		ERenderGraphUse Use = ERenderGraphUse::Read;
		ERHIAccess Access = ERHIAccess::None;
		FRHITextureSubresourceRange TextureRange{};
		uint64 BufferOffset = 0;
		uint64 BufferSize = 0;
		uint32 Version = 0;
		bool bDiscard = false;
		bool bStore = true;
	};

	// Records one exact pointer-free transition at a pass or graph boundary.
	struct FRenderGraphTransitionCapture final
	{
		uint32 ResourceId = 0;
		uint32 PassIndex = std::numeric_limits<uint32>::max();
		ERHIAccess Before = ERHIAccess::None;
		ERHIAccess After = ERHIAccess::None;
		FRHITextureSubresourceRange TextureRange{};
		uint64 BufferOffset = 0;
		uint64 BufferSize = 0;
		bool bFinal = false;
	};

	// Reports the retained scheduled interval of one declared resource.
	struct FRenderGraphResourceLifetime final
	{
		std::string Name;
		uint32 FirstPass = 0;
		uint32 LastPass = 0;
		bool bImported = false;
		bool bCulled = false;
	};

	// Explains whether one declared pass survived explicit-root reachability.
	struct FRenderGraphCullingDecision final
	{
		std::string Name;
		bool bCulled = false;
		std::string Reason;
	};

	// Freezes deterministic graph-shape gates and observational CPU thresholds.
	struct FRenderGraphBudget final
	{
		uint32 MaxPasses = std::numeric_limits<uint32>::max();
		uint32 MaxDependencies = std::numeric_limits<uint32>::max();
		uint32 MaxBufferTransitions = std::numeric_limits<uint32>::max();
		uint32 MaxTextureTransitions = std::numeric_limits<uint32>::max();
		uint64 MaxCompileMicroseconds = std::numeric_limits<uint64>::max();
		uint64 MaxExecuteMicroseconds = std::numeric_limits<uint64>::max();
	};

	// Reports graph shape and CPU cost without affecting execution correctness.
	struct FRenderGraphStatistics final
	{
		uint32 DeclaredPasses = 0;
		uint32 ScheduledPasses = 0;
		uint32 CulledPasses = 0;
		uint32 Dependencies = 0;
		uint32 BufferTransitions = 0;
		uint32 TextureTransitions = 0;
		uint64 CompileMicroseconds = 0;
		uint64 ExecuteMicroseconds = 0;
		bool bCompileBudgetExceeded = false;
		bool bExecuteBudgetExceeded = false;
	};

	// Pointer-free pass record suitable for persistence and tooling.
	struct FRenderGraphPassCapture final
	{
		std::string Name;
		ERenderGraphPassType Type = ERenderGraphPassType::Graphics;
		uint32 DeclarationIndex = 0;
		uint32 BufferTransitions = 0;
		uint32 TextureTransitions = 0;
	};

	// Owns an immutable diagnostic snapshot independent of graph/RHI lifetimes.
	struct FRenderGraphCapture final
	{
		FRenderGraphStatistics Statistics;
		std::vector<FRenderGraphPassCapture> Passes;
		std::vector<FRenderGraphResourceCapture> Resources;
		std::vector<FRenderGraphUseCapture> Uses;
		std::vector<FRenderGraphTransitionCapture> Transitions;
		std::vector<FRenderGraphDependency> Dependencies;
		std::vector<FRenderGraphResourceLifetime> ResourceLifetimes;
		std::vector<FRenderGraphCullingDecision> CullingDecisions;
		std::string Dump;
	};

	// Owns the compiled pass order and transition batches for one graph.
	struct FCompiledRenderGraphPass final
	{
		std::string Name;
		ERenderGraphPassType Type = ERenderGraphPassType::Graphics;
		uint32 DeclarationIndex = 0;
		std::vector<FRHIBufferTransition> BufferTransitions;
		std::vector<FRHITextureTransition> TextureTransitions;
	};

	// Immutable executable result produced only after complete graph validation.
	class RENDERCORE_API FCompiledRenderGraph final
	{
	public:
		FCompiledRenderGraph(FCompiledRenderGraph&&) noexcept;
		auto operator=(FCompiledRenderGraph&&) noexcept
			-> FCompiledRenderGraph&;
		~FCompiledRenderGraph();

		FCompiledRenderGraph(const FCompiledRenderGraph&) = delete;
		auto operator=(const FCompiledRenderGraph&)
			-> FCompiledRenderGraph& = delete;

		auto GetPasses() const -> std::span<const FCompiledRenderGraphPass>;
		auto GetDependencies() const -> std::span<const FRenderGraphDependency>;
		auto GetResourceLifetimes() const
			-> std::span<const FRenderGraphResourceLifetime>;
		auto GetCullingDecisions() const
			-> std::span<const FRenderGraphCullingDecision>;
		auto GetFinalBufferTransitions() const
			-> std::span<const FRHIBufferTransition>;
		auto GetFinalTextureTransitions() const
			-> std::span<const FRHITextureTransition>;
		auto GetCompileMicroseconds() const -> uint64;
		auto GetStatistics() const -> FRenderGraphStatistics;
		auto Capture() const -> FRenderGraphCapture;
		auto Dump() const -> std::string;
		// Runs complete resource preparation before recording any pass command.
		auto Execute(FRHICommandListImmediate& CommandList,
			std::string* OutError = nullptr) const -> bool;

	private:
		friend class FRenderGraphBuilder;
		friend class FRenderGraphPassResources;
		struct FState;
		explicit FCompiledRenderGraph(std::unique_ptr<FState> InState);
		std::unique_ptr<FState> State;
	};

	// Publishes either one complete graph or one deterministic compile failure.
	struct FRenderGraphCompileResult final
	{
		std::unique_ptr<FCompiledRenderGraph> Graph;
		std::string Error;

		auto IsSuccess() const -> bool { return Graph != nullptr; }
	};

	// Collects frame-local resources and passes before deterministic compilation.
	class RENDERCORE_API FRenderGraphBuilder final
	{
	public:
		FRenderGraphBuilder();
		~FRenderGraphBuilder();

		FRenderGraphBuilder(const FRenderGraphBuilder&) = delete;
		auto operator=(const FRenderGraphBuilder&)
			-> FRenderGraphBuilder& = delete;
		FRenderGraphBuilder(FRenderGraphBuilder&&) = delete;
		auto operator=(FRenderGraphBuilder&&)
			-> FRenderGraphBuilder& = delete;

		auto ImportTexture(std::string_view Name, FRHITexture* Texture,
			ERHIAccess InitialAccess, ERHIAccess FinalAccess)
			-> FRenderGraphTextureHandle;
		auto CreateTexture(std::string_view Name, FRHITexture* Texture,
			ERHIAccess FinalAccess = ERHIAccess::None)
			-> FRenderGraphTextureHandle;
		auto CreateTexture(std::string_view Name,
			const FRenderGraphTextureDesc& Desc,
			ERHIAccess FinalAccess = ERHIAccess::None)
			-> FRenderGraphTextureHandle;
		auto ImportBuffer(std::string_view Name, FRHIBuffer* Buffer,
			ERHIAccess InitialAccess, ERHIAccess FinalAccess)
			-> FRenderGraphBufferHandle;
		auto CreateBuffer(std::string_view Name, FRHIBuffer* Buffer,
			ERHIAccess FinalAccess = ERHIAccess::None)
			-> FRenderGraphBufferHandle;
		auto CreateBuffer(std::string_view Name,
			const FRenderGraphBufferDesc& Desc,
			ERHIAccess FinalAccess = ERHIAccess::None)
			-> FRenderGraphBufferHandle;
		auto CreateToken(std::string_view Name) -> FRenderGraphTokenHandle;

		auto AddPass(std::string_view Name, ERenderGraphPassType Type,
			FRenderGraphExecute Execute = {}) -> FRenderGraphPassHandle;
		auto AddDependency(FRenderGraphPassHandle Pass,
			FRenderGraphPassHandle Prerequisite) -> void;
		auto MarkPassRoot(FRenderGraphPassHandle Pass,
			std::string_view Reason = "side-effect") -> void;
		auto EnablePassCulling() -> void;
		auto SetExecutionPreparation(FRenderGraphPrepare Prepare) -> void;
		auto SetBackingResolver(FRenderGraphBackingResolver Resolver) -> void;
		auto SetBudget(const FRenderGraphBudget& Budget) -> void;

		auto UseTexture(FRenderGraphPassHandle Pass,
			FRenderGraphTextureHandle Texture,
			const FRHITextureSubresourceRange& Range, ERenderGraphUse Use,
			ERHIAccess Access, bool bDiscard = false) -> void;
		auto UseBuffer(FRenderGraphPassHandle Pass,
			FRenderGraphBufferHandle Buffer, uint64 Offset, uint64 Size,
			ERenderGraphUse Use, ERHIAccess Access,
			bool bDiscard = false) -> void;
		auto UseColorAttachment(FRenderGraphPassHandle Pass,
			FRenderGraphTextureHandle Texture,
			const FRHITextureSubresourceRange& Range,
			ERHIRenderTargetLoadAction LoadAction,
			ERHIRenderTargetStoreAction StoreAction) -> void;
		auto UseDepthStencilAttachment(FRenderGraphPassHandle Pass,
			FRenderGraphTextureHandle Texture,
			const FRHITextureSubresourceRange& Range,
			ERHIRenderTargetLoadAction LoadAction,
			ERHIRenderTargetStoreAction StoreAction) -> void;
		// Declares an attachment whose render-pass body performs its own RHI
		// entry/final layout transitions and publishes ResultAccess on exit.
		auto UseManagedColorAttachment(FRenderGraphPassHandle Pass,
			FRenderGraphTextureHandle Texture,
			const FRHITextureSubresourceRange& Range,
			ERHIRenderTargetLoadAction LoadAction,
			ERHIRenderTargetStoreAction StoreAction,
			ERHIAccess ResultAccess) -> void;
		auto UseManagedDepthStencilAttachment(FRenderGraphPassHandle Pass,
			FRenderGraphTextureHandle Texture,
			const FRHITextureSubresourceRange& Range,
			ERHIRenderTargetLoadAction LoadAction,
			ERHIRenderTargetStoreAction StoreAction,
			ERHIAccess ResultAccess) -> void;
		auto UseManagedTexture(FRenderGraphPassHandle Pass,
			FRenderGraphTextureHandle Texture,
			const FRHITextureSubresourceRange& Range, ERenderGraphUse Use,
			ERHIAccess EntryAccess, ERHIAccess ResultAccess,
			bool bDiscard = false) -> void;
		auto UseToken(FRenderGraphPassHandle Pass, FRenderGraphTokenHandle Token,
			ERenderGraphUse Use) -> void;

		auto Compile() const -> FRenderGraphCompileResult;

	private:
		struct FState;
		std::unique_ptr<FState> State;
	};
} // namespace Durin
