#pragma once

#include "RHIFeatureLevel.h"
#include "RHIResources.h"

class FRHICommandList;

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
	static RENDERCORE_API void ReleaseRHIForAllResources();

	static RENDERCORE_API void InitPreRHIResources();

	RENDERCORE_API FRenderResource();

	/** Constructor when we know what feature level this resource should support */
	RENDERCORE_API FRenderResource(ERHIFeatureLevel InFeatureLevel);

	RENDERCORE_API virtual ~FRenderResource();

	/**
	 * Initializes the RHI resources used by this resource.
	 * Called when entering the state where both the resource and the RHI have been initialized.
	 * This is only called by the rendering thread.
	 */
	virtual void InitRHI(FRHICommandList& RHICmdList) {}

	/**
	 * Releases the RHI resources used by this resource.
	 * Called when leaving the state where both the resource and the RHI have been initialized.
	 * This is only called by the rendering thread.
	 */
	virtual void ReleaseRHI() {}

	/**
	 * Initializes the resource.
	 * This is only called by the rendering thread.
	 */
	RENDERCORE_API virtual void InitResource(FRHICommandList& RHICmdList);

	/**
	 * Prepares the resource for deletion.
	 * This is only called by the rendering thread.
	 */
	RENDERCORE_API virtual void ReleaseResource();

	/**
	 * If the resource's RHI resources have been initialized, then release and reinitialize it.  Otherwise, do nothing.
	 * This is only called by the rendering thread.
	 */
	RENDERCORE_API void UpdateRHI(FRHICommandList& RHICmdList);

	FORCEINLINE bool IsInitialized() const { return ListIndex_ != static_cast<uint32>(INDEX_NONE); }

	uint32 GetListIndex() const { return ListIndex_; }

	EInitPhase GetInitPhase() const { return InitPhase_; }

	virtual FString GetFriendlyName() const { return "undefined"; }

protected:

	// Helper for submitting a resource array to RHI and freeing eligible CPU memory
	template<typename T>
	TSharedPtr<FRHIBuffer> CreateRHIBuffer(FRHICommandList& RHICmdList, T& InOutResourceObject, uint32 ResourceCount, EBufferUsageFlags InBufferUsageFlags, const TCHAR* InDebugName)
	{
		TSharedPtr<FRHIBuffer> Buffer;

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

	void SetFeatureLevel(ERHIFeatureLevel InFeatureLevel) { FeatureLevel_ = InFeatureLevel; }

	ERHIFeatureLevel GetFeatureLevel() const { return FeatureLevel_; }


private:
	uint32 ListIndex_ = static_cast<uint32>(INDEX_NONE);

	ERHIFeatureLevel FeatureLevel_ = ERHIFeatureLevel::ES3_1;

	EInitPhase InitPhase_ = EInitPhase::Default;
};

class FVertexBuffer : public FRenderResource
{
public:
	RENDERCORE_API FVertexBuffer();
	RENDERCORE_API virtual ~FVertexBuffer();

	RENDERCORE_API virtual void ReleaseRHI() override;
	virtual FString GetFriendlyName() const override { return "FVertexBuffer"; }

	const TSharedPtr<FRHIBuffer>& GetRHI() const { return VertexBufferRHI_; }

	RENDERCORE_API void SetRHI(const TSharedPtr<FRHIBuffer>& BufferRHI);

	TSharedPtr<FRHIBuffer> VertexBufferRHI_;
};