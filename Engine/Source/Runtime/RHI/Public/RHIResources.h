#pragma once

#include "RHIConstants.h"
#include "RHIDefinitions.h"

#include "Hash/XxHash.h"

namespace Doge
{
	class FRHICommandListImmediate;

	enum class ERHIResourceType : uint8
	{
		VertexShader,
		PixelShader,
		ComputeShader,
	};

	class FRHIResource
	{
	public:
		FRHIResource() = delete;
		RHI_API FRHIResource(ERHIResourceType InResourceType);

	protected:
		// The destructor is protected to prevent deletion directly.
		RHI_API virtual ~FRHIResource();

	private:
		RHI_API auto MarkForDelete() const-> void;

	public:
		FORCEINLINE auto AddRef() const -> uint32
		{
			return AtomicFlags.AddRef(std::memory_order_acquire);
		}

		FORCEINLINE auto Release() const -> uint32
		{
			const uint32 NewRefCount = AtomicFlags.Release(std::memory_order_release);

			if (NewRefCount == 0)
			{
				MarkForDelete();
			}
			return NewRefCount;
		}

		FORCEINLINE auto GetRefCount() const -> uint32
		{
			return AtomicFlags.GetRefCount(std::memory_order_relaxed);
		}

		FORCEINLINE auto GetResourceType() const -> ERHIResourceType { return ResourceType; }

		static RHI_API auto DeleteResources(const std::vector<FRHIResource*>& ResourcesToDelete) -> void;
		static RHI_API auto GatherResourcesToDelete(std::vector<FRHIResource*>& OutResourcesToDelete) -> void;

	private:
		class FAtomicFlags
		{
			static constexpr uint32 MarkedForDeleteBit = 1 << 30;
			static constexpr uint32 DeletingBit = 1 << 31;
			static constexpr uint32 NumRefsMask = ~(MarkedForDeleteBit | DeletingBit);

			std::atomic_uint Packed = {0};

		public:
			auto AddRef(std::memory_order MemoryOrder) -> uint32
			{
				const uint32 OldPacked = Packed.fetch_add(1, MemoryOrder);
				check((OldPacked & DeletingBit) == 0); // Resource is being deleted, cannot add reference
				return (OldPacked & NumRefsMask) + 1;
			}

			auto Release(std::memory_order MemoryOrder) -> uint32
			{
				const uint32 OldPacked = Packed.fetch_sub(1, MemoryOrder);
				check((OldPacked & DeletingBit) == 0); // Resource is being deleted
				return (OldPacked & NumRefsMask) - 1;
			}

			auto MarkForDelete(std::memory_order MemoryOrder) -> bool
			{
				uint32 OldPacked = Packed.fetch_or(MarkedForDeleteBit, MemoryOrder);
				check((OldPacked & DeletingBit) == 0);
				return (OldPacked & MarkedForDeleteBit) != 0; // Return whether the resource was already marked for delete before this call
			}

			auto GetRefCount(std::memory_order MemoryOrder) const -> uint32
			{
				return Packed.load(MemoryOrder) & NumRefsMask;
			}

			auto Deleting() const -> bool
			{
				const uint32 OldPacked = Packed.load(std::memory_order_acquire);
				return (OldPacked & DeletingBit) != 0;
			}

			auto IsValid(std::memory_order MemoryOrder) const -> bool
			{
				const uint32 LocalPacked = Packed.load(MemoryOrder);
				return (LocalPacked & MarkedForDeleteBit) == 0 && (LocalPacked & NumRefsMask) != 0;
			}
		};

		mutable FAtomicFlags AtomicFlags;

		ERHIResourceType ResourceType;
	};

	class FRHIShader : public FRHIResource
	{
	public:
		FRHIShader() = delete;
		RHI_API FRHIShader(ERHIResourceType InResourceType, EShaderFrequency InFrequency);

		FORCEINLINE auto GetFrequency() const -> EShaderFrequency { return Frequency; }

		auto SetHash(const FXxHash64& InHash) -> void { Hash = InHash; }

		auto GetHash() const -> FXxHash64 { return Hash; }

	protected:
		FXxHash64 Hash;
		EShaderFrequency Frequency;
	};

	class FRHIVertexShader : public FRHIShader
	{
	public:
		FRHIVertexShader()
			: FRHIShader(ERHIResourceType::VertexShader, EShaderFrequency::Vertex)
		{
		}
	};

	class FRHIPixelShader : public FRHIShader
	{
	public:
		FRHIPixelShader()
			: FRHIShader(ERHIResourceType::PixelShader, EShaderFrequency::Pixel)
		{
		}
	};

	class RHI_API FRHITexture
	{
	public:
		auto GetSizeX() const -> uint32 { return SizeX; }
		auto GetSizeY() const -> uint32 { return SizeY; }

	protected:
		uint32 SizeX = 0;
		uint32 SizeY = 0;
	};

	class RHI_API FRHIViewport
	{
	public:
		virtual ~FRHIViewport() = default;
		virtual auto Tick(float DeltaTime) -> void {};
		virtual auto GetBackBuffer(FRHICommandListImmediate& RHICmdList) -> TSharedPtr<FRHITexture> = 0;
		virtual auto WaitForLastFrameCompletion() -> void = 0;
		virtual auto GetFormat() const -> EPixelFormat = 0;
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

	class RHI_API FGraphicsPipelineStateInitializer
	{
	public:
		FName RenderPassName;

		EPixelFormat PixelFormat;
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
		explicit FRHIBuffer(const FRHIBufferDesc& InDesc)
			: Desc(InDesc)
		{
		}

		auto GetDesc() const -> FRHIBufferDesc const& { return Desc; }

		/** @return The number of bytes in the buffer. */
		auto GetSize() const -> uint32 { return Desc.Size; }

		/** @return The stride in bytes of the buffer. */
		auto GetStride() const -> uint32 { return Desc.Stride; }

		/** @return The usage flags used to create the buffer. */
		auto GetUsage() const -> EBufferUsageFlags { return Desc.Usage; }

	private:
		FRHIBufferDesc Desc;
	};
} // namespace Doge