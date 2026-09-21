#pragma once

#include "RDGAllocator.h"
#include "RDGDiagnostics.h"
#include "RDGExecution.h"
#include "RDGParameters.h"
#include <concepts>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace Durin
{
	// Owns declarations, storage and private compilation records for one graph execution.
	// Thread-confined; all declaration methods require Building in every configuration.
	class FRDGBuilder final
	{
	public:
		RENDERCORE_API FRDGBuilder();
		RENDERCORE_API ~FRDGBuilder();

		FRDGBuilder(const FRDGBuilder&) = delete;
		auto operator=(const FRDGBuilder&)
			-> FRDGBuilder& = delete;
		FRDGBuilder(FRDGBuilder&&) = delete;
		auto operator=(FRDGBuilder&&)
			-> FRDGBuilder& = delete;

		RENDERCORE_API auto RegisterExternalTexture(const FTextureRHIRef& Texture,
			std::string_view Name, ERHIAccess InitialAccess,
			ERHIAccess FinalAccess) -> FRDGTextureHandle;
		RENDERCORE_API auto CreateTexture(const FRDGTextureDesc& Desc,
			std::string_view Name,
			ERHIAccess FinalAccess = ERHIAccess::None)
			-> FRDGTextureHandle;
		RENDERCORE_API auto RegisterExternalBuffer(const FBufferRHIRef& Buffer,
			std::string_view Name, ERHIAccess InitialAccess,
			ERHIAccess FinalAccess) -> FRDGBufferHandle;
		RENDERCORE_API auto CreateBuffer(const FRDGBufferDesc& Desc,
			std::string_view Name,
			ERHIAccess FinalAccess = ERHIAccess::None)
			-> FRDGBufferHandle;
		RENDERCORE_API auto CreateToken(std::string_view Name) -> FRDGTokenHandle;
		// Exports the complete resource through a terminal consumer. Every subresource
		// must have valid stored contents; Destination is published only after success.
		RENDERCORE_API auto QueueTextureExtraction(FRDGTextureHandle Texture,
			FTextureRHIRef* Destination, ERHIAccess FinalAccess) -> void;
		// Requires prior stored writes or imported contents covering every byte.
		// Partial writes retain earlier producers; publication occurs only after success.
		RENDERCORE_API auto QueueBufferExtraction(FRDGBufferHandle Buffer,
			FBufferRHIRef* Destination, ERHIAccess FinalAccess) -> void;
		template<typename T, typename... Args>
		requires std::constructible_from<T, Args...> && std::destructible<T>
		auto CreateValue(std::string_view Name, std::string_view StableTypeName,
			Args&&... ConstructorArgs) -> TRDGValueHandle<T>
		{
			RequireBuilding();
			static_assert(std::is_object_v<T> && !std::is_const_v<T>
				&& !std::is_volatile_v<T>,
				"Render graph values require an unqualified object type");
			uint32 Index = 0;
			void* Storage = AllocateValueStorage(Name, StableTypeName,
				&RDGPrivate::GValueTypeIdentity<T>, sizeof(T), alignof(T),
				[](void* Value) { std::destroy_at(static_cast<T*>(Value)); }, Index);
			if (Storage == nullptr) return {};
			FStorageConstructionScope Construction(*this);
			std::construct_at(static_cast<T*>(Storage),
				std::forward<Args>(ConstructorArgs)...);
			MarkValueStorageConstructed(Index);
			return {StateOwner(), Index};
		}

		template<typename ParameterStruct, typename Execute>
		requires CRDGParameters<ParameterStruct>
			&& std::invocable<Execute&, FRHICommandListImmediate&,
				const ParameterStruct&, const FRDGParameterResolver&>
		auto AddPass(std::string_view Name, ERDGPassType Type,
			TRDGParametersRef<ParameterStruct>&& Parameters,
			Execute&& ExecuteCallback) -> FRDGPassHandle
		{
			RequireBuilding();
			ParameterStruct* TypedData = Parameters.Data;
			FRDGParameterizedPassExecute ErasedExecute =
				[TypedData, Callback = std::forward<Execute>(ExecuteCallback)](
					FRHICommandListImmediate& CommandList,
					const FRDGParameterResolver& Resolver) mutable {
					std::invoke(Callback, CommandList,
						static_cast<const ParameterStruct&>(*TypedData), Resolver);
				};
			RequireBuilding();
			auto Lifetime = Parameters.Lifetime.lock();
			void* Data = std::exchange(Parameters.Data, nullptr);
			const FRDGParameterLayout* Layout =
				std::exchange(Parameters.Layout, nullptr);
			if (Layout == nullptr)
				Layout = GetRDGParameterLayout<ParameterStruct>();
			Parameters.Lifetime.reset();
			const size_t AllocationIndex = std::exchange(Parameters.AllocationIndex,
				TRDGParametersRef<ParameterStruct>::InvalidAllocationIndex);
			return AddParameterizedPass(Name, Type,
				Layout, Data, AllocationIndex,
				std::move(Lifetime), std::move(ErasedExecute));
		}
		// Building only: Producer must precede Consumer in this builder. Retaining
		// Consumer retains Producer; invalid declarations fail compilation.
		RENDERCORE_API auto AddPassDependency(FRDGPassHandle Producer,
			FRDGPassHandle Consumer) -> void;
		RENDERCORE_API auto MarkPassRoot(FRDGPassHandle Pass,
			std::string_view Reason = "side-effect") -> void;
		RENDERCORE_API auto EnablePassCulling() -> void;
		// Eligibility is an author declaration; enabling the policy does not imply
		// that an independent physical queue is available during execution.
		RENDERCORE_API auto SetPassAsyncComputeEligible(FRDGPassHandle Pass, bool bEligible = true) -> void;
		RENDERCORE_API auto SetAsyncComputeEnabled(bool bEnabled) -> void;
		RENDERCORE_API auto SetBudget(const FRDGBudget& Budget) -> void;

		template<typename ParameterStruct>
		requires CRDGParameters<ParameterStruct>
		auto AllocParameters() -> TRDGParametersRef<ParameterStruct>
		{
			RequireBuilding();
			static_assert(std::is_standard_layout_v<ParameterStruct>,
				"Render graph parameter structs must use standard layout");
			static_assert(std::default_initializable<ParameterStruct>,
				"Render graph parameter structs must be default constructible");
			static_assert(std::destructible<ParameterStruct>,
				"Render graph parameter structs must be destructible");
			std::weak_ptr<void> Lifetime;
			size_t AllocationIndex = TRDGParametersRef<ParameterStruct>::InvalidAllocationIndex;
			const auto& LayoutResult =
				GetRDGParameterLayoutBuildResult<ParameterStruct>();
			void* Storage = AllocateParameterStorage(sizeof(ParameterStruct),
				alignof(ParameterStruct),
				ParameterStruct::GetRDGParametersMetadata(),
				LayoutResult,
				[](void* Value) { std::destroy_at(
					static_cast<ParameterStruct*>(Value)); }, Lifetime, AllocationIndex);
			if (Storage == nullptr) return {};
			FStorageConstructionScope Construction(*this);
			auto* Parameters = std::construct_at(
				static_cast<ParameterStruct*>(Storage));
			MarkParameterStorageConstructed(AllocationIndex);
			return {Parameters, std::move(Lifetime), LayoutResult->get(), AllocationIndex};
		}

		// Consumes this builder even on failure. Retrying requires a newly authored graph.
		RENDERCORE_API auto Execute(FRHICommandListImmediate& CommandList,
			FRDGAllocator* Allocator = nullptr) -> FRDGExecutionResult;
		RENDERCORE_API auto GetState() const -> ERDGBuilderState;
		// Absent before Execute. Duplicate execution leaves the original report unchanged.
		RENDERCORE_API auto GetExecutionResult() const -> const std::optional<FRDGExecutionResult>&;
		RENDERCORE_API auto HasCompiledPlan() const -> bool;
		RENDERCORE_API auto GetPasses() const -> std::span<const FRDGCompiledPass>;
		RENDERCORE_API auto GetDependencies() const -> std::span<const FRDGDependency>;
		RENDERCORE_API auto GetResourceLifetimes() const
			-> std::span<const FRDGResourceLifetime>;
		RENDERCORE_API auto GetCullingDecisions() const
			-> std::span<const FRDGCullingDecision>;
		RENDERCORE_API auto GetFinalBarriers() const -> const FRDGBarrierBatch&;
		RENDERCORE_API auto GetExecutionPlan() const -> const FRDGExecutionPlan&;
		RENDERCORE_API auto GetBudget() const -> const FRDGBudget&;
		RENDERCORE_API auto GetStatistics() const -> FRDGStatistics;
		// Detailed evidence is materialized on first inspection and cached outside the execution plan.
		// Owning pointer-free snapshots survive builder destruction and preparation failure.
		RENDERCORE_API auto Capture() const -> FRDGCapture;
		RENDERCORE_API auto Dump() const -> std::string;

	private:
		// Raw declaration injection is restricted to native compiler fixtures.
		RENDERCORE_API auto AddTestPass(std::string_view Name, ERDGPassType Type,
			FRDGPassExecute Execute = {}) -> FRDGPassHandle;
		template<typename ParameterStruct>
		requires CRDGParameters<ParameterStruct>
		auto AddTestPass(std::string_view Name, ERDGPassType Type,
			TRDGParametersRef<ParameterStruct>&& Parameters,
			FRDGPassExecute Execute = {}) -> FRDGPassHandle
		{
			return AddPass(Name, Type, std::move(Parameters),
				[Callback = std::move(Execute)](FRHICommandListImmediate& CommandList,
					const ParameterStruct&, const FRDGParameterResolver& Resolver) {
					if (Callback) Callback(CommandList, Resolver.Resources);
				});
		}

		RENDERCORE_API auto UseTexture(FRDGPassHandle Pass,
			FRDGTextureHandle Texture,
			const FRHITextureSubresourceRange& Range, ERDGUse Use,
			ERHIAccess Access, bool bDiscard = false) -> void;
		RENDERCORE_API auto UseBuffer(FRDGPassHandle Pass,
			FRDGBufferHandle Buffer, uint64 Offset, uint64 Size,
			ERDGUse Use, ERHIAccess Access,
			bool bDiscard = false) -> void;
		RENDERCORE_API auto UseColorAttachment(FRDGPassHandle Pass,
			FRDGTextureHandle Texture,
			const FRHITextureSubresourceRange& Range,
			ERHIRenderTargetLoadAction LoadAction,
			ERHIRenderTargetStoreAction StoreAction) -> void;
		RENDERCORE_API auto UseDepthStencilAttachment(FRDGPassHandle Pass,
			FRDGTextureHandle Texture,
			const FRHITextureSubresourceRange& Range,
			ERHIRenderTargetLoadAction LoadAction,
			ERHIRenderTargetStoreAction StoreAction) -> void;
		// Declares an attachment whose render-pass body performs its own RHI
		// entry/final layout transitions and publishes ResultAccess on exit.
		RENDERCORE_API auto UseManagedColorAttachment(FRDGPassHandle Pass,
			FRDGTextureHandle Texture,
			const FRHITextureSubresourceRange& Range,
			ERHIRenderTargetLoadAction LoadAction,
			ERHIRenderTargetStoreAction StoreAction,
			ERHIAccess ResultAccess) -> void;
		RENDERCORE_API auto UseManagedDepthStencilAttachment(FRDGPassHandle Pass,
			FRDGTextureHandle Texture,
			const FRHITextureSubresourceRange& Range,
			ERHIRenderTargetLoadAction LoadAction,
			ERHIRenderTargetStoreAction StoreAction,
			ERHIAccess ResultAccess) -> void;
		RENDERCORE_API auto UseManagedTexture(FRDGPassHandle Pass,
			FRDGTextureHandle Texture,
			const FRHITextureSubresourceRange& Range, ERDGUse Use,
			ERHIAccess EntryAccess, ERHIAccess ResultAccess,
			bool bDiscard = false) -> void;
		RENDERCORE_API auto UseToken(FRDGPassHandle Pass, FRDGTokenHandle Token,
			ERDGUse Use) -> void;
		template<typename T>
		auto UseValue(FRDGPassHandle Pass,
			TRDGValueHandle<T> Value, ERDGUse Use) -> void
		{
			UseValueErased(Pass, Value.Owner, Value.Index,
				&RDGPrivate::GValueTypeIdentity<std::remove_cv_t<T>>, Use);
		}

		friend class FRDGBuilderTestAccessor;
		friend class FRDGPassResources;
		RENDERCORE_API auto RequireBuilding() const -> void;
		// Test-only runtime evidence in logical batch order, never a graph-success proof.
		RENDERCORE_API auto GetSubmissionSyncPoints() const -> std::span<const FRHIGPUSyncPointRef>;
		auto Compile() -> FRDGCompileResult;
		auto EnsureDiagnostics() const -> void;
		RENDERCORE_API auto CompileForTesting() -> FRDGCompileResult;
		auto Record(FRHICommandListImmediate& CommandList,
			FRDGAllocator* Allocator) -> FRDGPreparationResult;
		struct FCompiledState;
		std::unique_ptr<FCompiledState> Compiled;
		struct FDiagnostics;
		mutable std::unique_ptr<FDiagnostics> Diagnostics;

		RENDERCORE_API auto BeginStorageConstruction() -> void;
		RENDERCORE_API auto EndStorageConstruction() -> void;
		// User constructors may call back into the builder before storage is ready.
		struct FStorageConstructionScope final
		{
			explicit FStorageConstructionScope(FRDGBuilder& InBuilder) : Builder(InBuilder)
			{ Builder.BeginStorageConstruction(); }
			~FStorageConstructionScope() { Builder.EndStorageConstruction(); }
			FRDGBuilder& Builder;
		};

		RENDERCORE_API auto AddParameterizedPass(std::string_view Name,
			ERDGPassType Type,
			const FRDGParameterLayout* Layout, void* Parameters, size_t AllocationIndex,
			std::shared_ptr<void> Lifetime,
			FRDGParameterizedPassExecute ParameterizedExecute)
			-> FRDGPassHandle;
		// Validates test authority once and appends a complete texture declaration.
		auto DeclareTextureUse(FRDGPassHandle Pass, FRDGTextureHandle Texture,
			const FRHITextureSubresourceRange& Range, ERDGUse Use,
			ERHIAccess Access, bool bDiscard, bool bStore = true,
			bool bPassManagedTransition = false,
			ERHIAccess ResultAccess = ERHIAccess::None) -> void;
		RENDERCORE_API auto CanDeclareManualUse(FRDGPassHandle Pass) -> bool;
		RENDERCORE_API auto AllocateParameterStorage(size_t Size, size_t Alignment,
			const FRDGParametersMetadata* Metadata,
			const FRDGParameterLayoutBuildResult& LayoutResult,
			void (*Destroy)(void*), std::weak_ptr<void>& OutLifetime,
			size_t& OutAllocationIndex) -> void*;
		RENDERCORE_API auto MarkParameterStorageConstructed(size_t AllocationIndex) -> void;
		RENDERCORE_API auto AllocateValueStorage(std::string_view Name,
			std::string_view StableTypeName, const void* TypeIdentity, size_t Size,
			size_t Alignment, void (*Destroy)(void*), uint32& OutIndex) -> void*;
		RENDERCORE_API auto MarkValueStorageConstructed(uint32 ResourceIndex) -> void;
		RENDERCORE_API auto UseValueErased(FRDGPassHandle Pass, uint64 Owner,
			uint32 Index, const void* TypeIdentity, ERDGUse Use) -> void;
		RENDERCORE_API auto StateOwner() const -> uint64;
		struct FState;
		std::unique_ptr<FState> State;
	};
} // namespace Durin
