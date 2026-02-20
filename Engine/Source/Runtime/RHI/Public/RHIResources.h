#pragma once

#include "RHIConstants.h"
#include "RHIDefinitions.h"

class FRHICommandListImmediate;

class RHI_API FRHITexture{};

class RHI_API FRHIViewport
{
public:
	virtual auto Tick(float DeltaTime) -> void {};
	virtual auto GetBackBuffer(FRHICommandListImmediate& RHICmdList) -> TSharedPtr<FRHITexture> = 0;
	virtual auto WaitForLastFrameCompletion() -> void = 0;
	virtual auto GetImageFormat() const -> EPixelFormat = 0;
};

struct RHI_API FRHIRenderTargetsInfo
{
	FRHITexture* ColorRenderTargets[kMaxSimultaneousRenderTargets];
	int32 NumColorRenderTargets;
	bool bClearColor;
};

struct RHI_API FRHIRenderPassInfo
{
	FRHITexture* ColorRenderTargets[kMaxSimultaneousRenderTargets];
};

class RHI_API FGraphicsPipelineStateInitializer{

};

struct FRHIBufferDesc
{
	uint32 Size{};
	uint32 Stride{};
	EBufferUsageFlags Usage{};

	FRHIBufferDesc() = default;
	FRHIBufferDesc(uint32 InSize, uint32 InStride, EBufferUsageFlags InUsage)
		: Size(InSize)
		, Stride(InStride)
		, Usage(InUsage)
	{
	}

	static auto Null() -> FRHIBufferDesc
	{
		return FRHIBufferDesc(0, 0, BUF_NullResource);
	}

	auto IsNull() const -> bool
	{
		if (EnumHasAnyFlags(Usage, BUF_NullResource))
		{
			// The null resource descriptor should have its other fields zeroed, and no additional flags.
			check(Size == 0 && Stride == 0 && Usage == BUF_NullResource);
			return true;
		}

		return false;
	}
};

class FRHIBuffer
{
public:
	FRHIBuffer(FRHIBufferDesc const& InDesc)
		: Desc_(InDesc)
	{
	}

	FRHIBufferDesc const& GetDesc() const { return Desc_; }

	/** @return The number of bytes in the buffer. */
	uint32 GetSize() const { return Desc_.Size; }

	/** @return The stride in bytes of the buffer. */
	uint32 GetStride() const { return Desc_.Stride; }

	/** @return The usage flags used to create the buffer. */
	EBufferUsageFlags GetUsage() const { return Desc_.Usage; }

private:
	FRHIBufferDesc Desc_;
};