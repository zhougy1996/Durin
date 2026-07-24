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

		static RENDERCORE_API auto InitPreRHIResources() -> void;

		RENDERCORE_API FRenderResource();

		/** Constructor when we know what feature level this resource should support */
		RENDERCORE_API FRenderResource(ERHIFeatureLevel InFeatureLevel);

		RENDERCORE_API virtual ~FRenderResource();

		/**
		 * Initializes the RHI resources used by this resource.
		 * Called when entering the state where both the resource and the RHI have been initialized.
		 * This is only called by the rendering thread.
		 */
		virtual auto InitRHI(FRHICommandListBase& RHICmdList) -> void;

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

		/**
		 * If the resource's RHI resources have been initialized, then release and reinitialize it.  Otherwise, do nothing.
		 * This is only called by the rendering thread.
		 */
		RENDERCORE_API auto UpdateRHI(FRHICommandListBase& RHICmdList) -> void;

		FORCEINLINE auto IsInitialized() const -> bool { return ListIndex != INDEX_NONE_U32; }

		auto GetListIndex() const -> uint32 { return ListIndex; }

		auto GetInitPhase() const -> EInitPhase { return InitPhase; }

		virtual auto GetFriendlyName() const -> std::string { return "Undefined"; }

	protected:

		// Helper for submitting a resource array to RHI and freeing eligible CPU memory
		template<typename T>
		auto CreateRHIBuffer(FRHICommandListBase& RHICmdList, T& InOutResourceObject, uint32 ResourceCount, EBufferUsageFlags InBufferUsageFlags, const char* InDebugName) -> std::shared_ptr<FRHIBuffer>
		{
			std::shared_ptr<FRHIBuffer> Buffer;

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
	};

	// Owns a vertex-buffer RHI allocation through the render-resource lifecycle.
	class FVertexBuffer : public FRenderResource
	{
	public:
		RENDERCORE_API FVertexBuffer();
		RENDERCORE_API ~FVertexBuffer() override;

		RENDERCORE_API auto ReleaseRHI() -> void override;
		auto GetFriendlyName() const -> std::string override { return "FVertexBuffer"; }

		auto GetRHI() const -> const std::shared_ptr<FRHIBuffer>& { return VertexBufferRHI; }

		RENDERCORE_API auto SetRHI(const std::shared_ptr<FRHIBuffer>& BufferRHI) -> void;

		std::shared_ptr<FRHIBuffer> VertexBufferRHI;
	};
}
