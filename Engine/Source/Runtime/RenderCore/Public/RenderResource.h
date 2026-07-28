#pragma once

#include "RenderCoreAPI.h"
#include "RHIFeatureLevel.h"
#include "RHIResources.h"

namespace Durin
{
	class FRHICommandListBase;

	// Coordinates render-thread initialization and release of an object's RHI state.
	class FRenderResource
	{
	public:
		/** Controls initialization order of render resources. Early engine resources utilize the 'Pre' phase to avoid static init ordering issues. */
		enum class EInitPhase : uint8
		{
			Pre,
			Default,
		};

		/** Release all render resources that are currently initialized. */
		static RENDERCORE_API auto ReleaseRHIForAllResources() -> void;

		/** Reinitialize the RHI state of every registered resource after a global RHI reset. */
		static RENDERCORE_API auto InitRHIForAllResources(FRHICommandListBase& RHICmdList) -> void;

		static RENDERCORE_API auto InitPreRHIResources() -> void;

		RENDERCORE_API FRenderResource();

		RENDERCORE_API explicit FRenderResource(EInitPhase InInitPhase);

		/** Constructor when we know what feature level this resource should support */
		RENDERCORE_API FRenderResource(ERHIFeatureLevel InFeatureLevel);

		RENDERCORE_API virtual ~FRenderResource();

		/**
		 * Initializes the RHI resources used by this resource.
		 * Called when entering the state where both the resource and the RHI have been initialized.
		 * This is only called by the rendering thread.
		 */
		RENDERCORE_API virtual auto InitRHI(
			FRHICommandListBase& RHICmdList) -> void;

		/**
		 * Releases the RHI resources used by this resource.
		 * Called when leaving the state where both the resource and the RHI have been initialized.
		 * This is only called by the rendering thread.
		 */
		virtual auto ReleaseRHI() -> void {}

		/**
		 * Initializes the resource.
		 * This is only called by the rendering thread.
		 */
		RENDERCORE_API virtual auto InitResource(FRHICommandListBase& RHICmdList) -> void;

		/**
		 * Prepares the resource for deletion.
		 * This is only called by the rendering thread.
		 */
		RENDERCORE_API virtual auto ReleaseResource() -> void;

		/** Queues initialization from the game thread. */
		RENDERCORE_API auto BeginInit_GameThread() -> void;

		/** Queues RHI recreation from the game thread. */
		RENDERCORE_API auto BeginUpdateRHI_GameThread() -> void;

		/** Queues release from the game thread. */
		RENDERCORE_API auto BeginRelease_GameThread() -> void;

		/**
		 * If the resource's RHI resources have been initialized, then release and reinitialize it.  Otherwise, do nothing.
		 * This is only called by the rendering thread.
		 */
		RENDERCORE_API auto UpdateRHI(FRHICommandListBase& RHICmdList) -> void;

		FORCEINLINE auto IsInitialized() const -> bool { return ListIndex != INDEX_NONE_U32; }
		auto IsRHIInitialized() const -> bool { return bRHIInitialized; }

		auto GetListIndex() const -> uint32 { return ListIndex; }

		auto GetInitPhase() const -> EInitPhase { return InitPhase; }

		virtual auto GetFriendlyName() const -> std::string { return "Undefined"; }
		RENDERCORE_API auto SetLifetimeDiagnostic(
			std::string InOwner, uint64 InRevision = 0) -> void;
		auto GetLifetimeOwner() const -> const std::string&
		{
			return LifetimeOwner;
		}
		auto GetLifetimeRevision() const -> uint64
		{
			return LifetimeRevision;
		}

#if DURIN_BUILD_DEBUG
		RENDERCORE_API auto SetDebugName(std::string InDebugName) -> void;
		auto GetDebugName() const -> const std::string&
		{
			return DebugName;
		}
#endif

	protected:

		// Helper for submitting a resource array to RHI and freeing eligible CPU memory
		template<typename T>
		auto CreateRHIBuffer(FRHICommandListBase& RHICmdList, T& InOutResourceObject, uint32 ResourceCount, EBufferUsageFlags InBufferUsageFlags, const char* InDebugName) -> FBufferRHIRef
		{
			FBufferRHIRef Buffer;

			//FResourceArrayInterface* RESTRICT ResourceArray = InOutResourceObject ? InOutResourceObject->GetResourceArray() : nullptr;
			//if (ResourceCount != 0)
			//{
			//	Buffer = CreateRHIBufferInternal(RHICmdList, InDebugName, GetOwnerName(), ResourceCount, InBufferUsageFlags, ResourceArray, InOutResourceObject == nullptr);
			//}

			//// If the buffer creation emptied the resource array, delete the containing structure as well
			//if (ShouldFreeResourceObject(InOutResourceObject, ResourceArray))
			//{
			//	delete InOutResourceObject;
			//	InOutResourceObject = nullptr;
			//}

			return Buffer;
		}

		auto SetFeatureLevel(ERHIFeatureLevel InFeatureLevel) -> void { FeatureLevel = InFeatureLevel; }

		auto GetFeatureLevel() const -> ERHIFeatureLevel { return FeatureLevel; }


	private:
		uint32 ListIndex = INDEX_NONE_U32;

		ERHIFeatureLevel FeatureLevel = ERHIFeatureLevel::ES3_1;

		EInitPhase InitPhase = EInitPhase::Default;

		bool bRHIInitialized = false;
		std::string LifetimeOwner = "<unspecified>";
		uint64 LifetimeRevision = 0;
#if DURIN_BUILD_DEBUG
		std::string DebugName;
#endif
	};

	class FDeferredRenderResourceCleanup;
	class FTextureResource;
	// Transfers unique ownership for destruction after previously accepted resource commands.
	RENDERCORE_API auto BeginCleanupRenderResource(
		FDeferredRenderResourceCleanup&& Cleanup) -> void;
	// Destroys pending cleanup objects after the command pipe has been drained.
	RENDERCORE_API auto FlushPendingRenderResourceCleanup_RenderThread() -> void;

	// Owns a released or release-pending resource until ordered render-thread destruction.
	class FDeferredRenderResourceCleanup
	{
	public:
		RENDERCORE_API explicit FDeferredRenderResourceCleanup(
			std::unique_ptr<FRenderResource> InResource);
		RENDERCORE_API ~FDeferredRenderResourceCleanup();

		FDeferredRenderResourceCleanup(const FDeferredRenderResourceCleanup&) = delete;
		auto operator=(const FDeferredRenderResourceCleanup&)
			-> FDeferredRenderResourceCleanup& = delete;
		RENDERCORE_API FDeferredRenderResourceCleanup(
			FDeferredRenderResourceCleanup&& Other) noexcept;
		RENDERCORE_API auto operator=(FDeferredRenderResourceCleanup&& Other) noexcept
			-> FDeferredRenderResourceCleanup&;
		auto GetResourceForDiagnostics() const -> const FRenderResource*
		{
			return Resource.get();
		}

	private:
		friend RENDERCORE_API auto BeginCleanupRenderResource(
			FDeferredRenderResourceCleanup&& Cleanup) -> void;
		friend RENDERCORE_API auto
			FlushPendingRenderResourceCleanup_RenderThread() -> void;

		std::unique_ptr<FRenderResource> Resource;
	};

	// Enqueues lifecycle work with non-owning pointers; callers retain ownership through completion.
	RENDERCORE_API auto BeginInitResource(FRenderResource* Resource) -> void;
	RENDERCORE_API auto BeginUpdateResourceRHI(FRenderResource* Resource) -> void;
	RENDERCORE_API auto BeginReleaseResource(FRenderResource* Resource) -> void;

	// These diagnostics are safe on the rendering thread or after a completed render-command fence.
	RENDERCORE_API auto GetNumInitializedRenderResources() -> size_t;
	RENDERCORE_API auto GetNumPendingRenderResourceCleanup() -> size_t;
	// Reports each live resource and cleanup entry when shutdown ordering is incomplete.
	RENDERCORE_API auto ValidateRenderResourceShutdown_RenderThread(
		const char* Phase) -> bool;

	// Owns one stable consumer-facing RHI texture reference for a texture asset.
	class FTextureReference : public FRenderResource
	{
	public:
		RENDERCORE_API explicit FTextureReference(
			FTextureRHIRef InFallbackTexture = {});
		RENDERCORE_API ~FTextureReference() override;

		RENDERCORE_API auto InitRHI(FRHICommandListBase& RHICmdList) -> void override;
		RENDERCORE_API auto ReleaseRHI() -> void override;
		auto GetFriendlyName() const -> std::string override
		{
			return "FTextureReference";
		}

		// The stable identity exists at construction and is safe to copy before
		// queued initialization; only its target is mutated on the rendering thread.
		auto GetTextureReferenceRHI() const
			-> const FRHITextureReferenceRef&
		{
			return TextureReferenceRHI;
		}

		RENDERCORE_API auto GetReferencedTexture_RenderThread() const
			-> FRHITexture*;
		RENDERCORE_API auto SetReferencedTexture_RenderThread(
			FTextureRHIRef InTexture) -> void;
		RENDERCORE_API auto ResetToFallback_RenderThread() -> void;
		RENDERCORE_API auto Clear_RenderThread() -> void;

	private:
		friend class FTextureResource;

		RENDERCORE_API auto ResetToFallbackIfMatches_RenderThread(
			FRHITexture* ExpectedTexture) -> void;

		FTextureRHIRef FallbackTextureRHI;
		FRHITextureReferenceRef TextureReferenceRHI;
	};

	// Owns a concrete texture allocation without owning its stable reference.
	class FTextureResource : public FRenderResource
	{
	public:
		RENDERCORE_API explicit FTextureResource(
			FTextureReference* InTextureReference);
		RENDERCORE_API ~FTextureResource() override;

		RENDERCORE_API auto ReleaseRHI() -> void override;
		auto GetFriendlyName() const -> std::string override
		{
			return "FTextureResource";
		}

		RENDERCORE_API auto GetTextureRHI_RenderThread() const
			-> const FTextureRHIRef&;
		RENDERCORE_API auto PublishTexture_RenderThread() -> void;

	protected:
		RENDERCORE_API auto SetTextureRHI_RenderThread(
			FTextureRHIRef InTexture) -> void;

	private:
		FTextureReference* TextureReference = nullptr;
		FTextureRHIRef TextureRHI;
	};

	// Owns a vertex-buffer RHI allocation through the render-resource lifecycle.
	class FVertexBuffer : public FRenderResource
	{
	public:
		RENDERCORE_API FVertexBuffer();
		RENDERCORE_API ~FVertexBuffer() override;

		RENDERCORE_API auto ReleaseRHI() -> void override;
		auto GetFriendlyName() const -> std::string override { return "FVertexBuffer"; }

		auto GetRHI() const -> const FBufferRHIRef&
		{
			return VertexBufferRHI;
		}

		RENDERCORE_API auto SetRHI(
			const FBufferRHIRef& BufferRHI) -> void;

	private:
		FBufferRHIRef VertexBufferRHI;
	};

	// Owns an index-buffer RHI allocation through the render-resource lifecycle.
	class FIndexBuffer : public FRenderResource
	{
	public:
		RENDERCORE_API FIndexBuffer();
		RENDERCORE_API ~FIndexBuffer() override;

		RENDERCORE_API auto ReleaseRHI() -> void override;
		auto GetFriendlyName() const -> std::string override
		{
			return "FIndexBuffer";
		}

		auto GetRHI() const -> const FBufferRHIRef&
		{
			return IndexBufferRHI;
		}

		RENDERCORE_API auto SetRHI(
			const FBufferRHIRef& BufferRHI) -> void;

	private:
		FBufferRHIRef IndexBufferRHI;
	};
}
