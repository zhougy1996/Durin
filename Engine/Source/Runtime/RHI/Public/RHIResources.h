#pragma once

#include "RHIAPI.h"
#include "RHIDefinitions.h"
#include "PixelFormat.h"

#include "Hash/XxHash.h"
#include "Math/MathFwd.h"

namespace Doge
{
	class FRHICommandListImmediate;

	enum class ERHIResourceType : uint8
	{
		Viewport,
		Buffer,
		Texture,
		Shader,
		VertexDeclaration,
		BindingSet,
		PipelineState,
	};

	class FRHIResource
	{
	public:
		FRHIResource() = delete;
		RHI_API explicit FRHIResource(ERHIResourceType InResourceType);

	protected:
		// The destructor is protected to prevent deletion directly.
		RHI_API virtual ~FRHIResource();

	private:
		RHI_API auto MarkForDelete() const -> void;

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
			static constexpr uint32 DeletingBit = 1 << 31;
			static constexpr uint32 MarkedForDeleteBit = 1 << 30;
			static constexpr uint32 NumRefsMask = ~(MarkedForDeleteBit | DeletingBit);

			std::atomic_uint Packed = {0};

		public:
			auto AddRef(std::memory_order MemoryOrder) -> uint32
			{
				const uint32 OldPacked = Packed.fetch_add(1, MemoryOrder);
				check((OldPacked & DeletingBit) == 0); // Resource is being deleted, cannot add reference
				const uint32 OldRefCount = OldPacked & NumRefsMask;
				check(OldRefCount != NumRefsMask); // Prevent overflow
				return OldRefCount + 1;
			}

			auto Release(std::memory_order MemoryOrder) -> uint32
			{
				const uint32 OldPacked = Packed.fetch_sub(1, MemoryOrder);
				check((OldPacked & DeletingBit) == 0); // Resource is being deleted
				const uint32 OldRefCount = OldPacked & NumRefsMask;
				check(OldRefCount != 0); // Prevent underflow
				return OldRefCount - 1;
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

			auto UnmarkForDelete(std::memory_order MemoryOrder) -> bool
			{
				const uint32 OldPacked = Packed.fetch_xor(MarkedForDeleteBit, MemoryOrder);
				check((OldPacked & DeletingBit) == 0);
				bool OldMarkedForDelete = (OldPacked & MarkedForDeleteBit) != 0;
				check(OldMarkedForDelete == true);
				return OldMarkedForDelete;
			}

			auto Deleting() -> bool
			{
				const uint32 LocalPacked = Packed.load(std::memory_order_acquire);
				check((LocalPacked & MarkedForDeleteBit) != 0);
				check((LocalPacked & DeletingBit) == 0);
				const uint32 NumRefs = LocalPacked & NumRefsMask;

				// Allow caches to bring dead objects back to life.
				if (NumRefs == 0)
				{
#if DO_CHECK
					Packed.fetch_or(DeletingBit, std::memory_order_acquire);
#endif
					return true;
				}
				UnmarkForDelete(std::memory_order_release);
				return false;
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

	struct FRHIShaderDesc
	{
		explicit FRHIShaderDesc(EShaderFrequency InFrequency, FXxHash128 InHash)
			: Hash(InHash)
			, Frequency(InFrequency)
		{
		}

		auto operator==(const FRHIShaderDesc& Other) const -> bool
		{
			return Frequency == Other.Frequency && Hash == Other.Hash;
		}

		auto operator!=(const FRHIShaderDesc& Other) const -> bool
		{
			return !(*this == Other);
		}

		FXxHash128 Hash;

		EShaderFrequency Frequency = EShaderFrequency::Vertex;
	};

	struct FRHIShaderCreateDesc : public FRHIShaderDesc
	{
		using FCodeView = std::span<const uint32>;

		static auto Create(const char* InDebugName, EShaderFrequency InFrequency, FCodeView InCode, FXxHash128 InHash) -> FRHIShaderCreateDesc
		{
			auto Desc = FRHIShaderCreateDesc(InDebugName, InFrequency, InCode, InHash);
			return Desc;
		}

		static auto CreateVertex(const char* InDebugName, FCodeView InCode, FXxHash128 InHash) -> FRHIShaderCreateDesc
		{
			auto Desc = FRHIShaderCreateDesc(InDebugName, EShaderFrequency::Vertex, InCode, InHash);
			return Desc;
		}

		static auto CreatePixel(const char* InDebugName, FCodeView InCode, FXxHash128 InHash) -> FRHIShaderCreateDesc
		{
			auto Desc = FRHIShaderCreateDesc(InDebugName, EShaderFrequency::Pixel, InCode, InHash);
			return Desc;
		}

		FRHIShaderCreateDesc(const char* InDebugName, EShaderFrequency InFrequency, FCodeView InCode, FXxHash128 InHash)
			: FRHIShaderDesc(InFrequency, InHash)
			, Code(InCode)
			, DebugName(InDebugName)
		{
		}

		auto SetEntryPoint(const char* InEntryPoint) -> void
		{
			EntryPoint = InEntryPoint;
		}

		FCodeView Code{};

		const char* EntryPoint = "main";

		const char* DebugName = nullptr;
	};

	class FRHIShader : public FRHIResource
	{
	public:
		FRHIShader() = delete;
		explicit FRHIShader(const FRHIShaderDesc& InCreateDesc)
			: FRHIResource(ERHIResourceType::Shader)
			, Frequency(InCreateDesc.Frequency)
			, Hash(InCreateDesc.Hash)
		{
		}

		auto GetFrequency() const -> EShaderFrequency { return Frequency; }

		auto GetHash() const -> FXxHash128 { return Hash; }

	protected:
		EShaderFrequency Frequency = EShaderFrequency::Vertex;

		FXxHash128 Hash;
	};

	enum class EClearBinding
	{
		None,  // No clear binding, the render target will not do hardware clears.
		Color, // Target has a
		DepthStencil,
	};

	struct FClearValueBinding
	{
		struct FDepthStencilValue
		{
			float Depth;
			uint32 Stencil;
		};

		FClearValueBinding()
			: Binding(EClearBinding::Color)
		{
			ClearValue.Color[0] = 0.0f;
			ClearValue.Color[1] = 0.0f;
			ClearValue.Color[2] = 0.0f;
			ClearValue.Color[3] = 0.0f;
		}

		explicit FClearValueBinding(EClearBinding NoBinding)
			: Binding(NoBinding)
		{
			check(Binding == EClearBinding::None);
			ClearValue.Color[0] = 0.0f;
			ClearValue.Color[1] = 0.0f;
			ClearValue.Color[2] = 0.0f;
			ClearValue.Color[3] = 0.0f;

			ClearValue.DSValue.Depth = 0.0f;
			ClearValue.DSValue.Stencil = 0;
		}

		union FClearValue
		{
			float Color[4];
			FDepthStencilValue DSValue{};
		} ClearValue;

		EClearBinding Binding;
	};

	struct FRHITextureDesc
	{
		FRHITextureDesc() = default;

		explicit FRHITextureDesc(ETextureDimension InDimension)
			: Dimension(InDimension)
		{
		}

		auto IsTexture2D() const -> bool { return Dimension == ETextureDimension::Texture2D || Dimension == ETextureDimension::Texture2DArray; }

		auto IsTexture3D() const -> bool { return Dimension == ETextureDimension::Texture3D; }

		auto IsTextureCube() const -> bool { return Dimension == ETextureDimension::TextureCube; }

		auto IsTextureArray() const -> bool { return Dimension == ETextureDimension::Texture2DArray || Dimension == ETextureDimension::TextureCubeArray; }

		auto IsMipChain() const -> bool { return NumMips > 1; }

		auto IsMultisample() const -> bool { return NumSamples > 1; }

		auto GetSize() const -> FIntVector
		{
			return {Extent.x, Extent.y, Depth};
		}

		ETextureDimension Dimension = ETextureDimension::Texture2D;

		ETextureCreateFlags Flags = ETextureCreateFlags::None;

		EPixelFormat Format = EPixelFormat::Unknown;

		FClearValueBinding ClearValue{};

		FIntPoint Extent = FIntPoint(1, 1);

		// Depth of the texture if the dimension is 3D
		uint16 Depth = 1;

		// The number of array elements in the texture. (Keep at 1 if dimension is 3D).
		uint16 ArraySize = 1;

		// Number of mips in the texture mip-map chain.
		uint8 NumMips = 1;

		// Number of samples in the texture.
		uint8 NumSamples = 1;
	};

	struct FRHITextureCreateDesc : public FRHITextureDesc
	{
		static auto Create(const char* InDebugName, ETextureDimension InDimension) -> FRHITextureCreateDesc
		{
			return {InDebugName, InDimension};
		}

		static auto Create2D(const char* InDebugName) -> FRHITextureCreateDesc
		{
			return {InDebugName, ETextureDimension::Texture2D};
		}

		static auto Create2DArray(const char* InDebugName) -> FRHITextureCreateDesc
		{
			return {InDebugName, ETextureDimension::Texture2DArray};
		}

		static auto Create3D(const char* InDebugName) -> FRHITextureCreateDesc
		{
			return {InDebugName, ETextureDimension::Texture3D};
		}

		static auto CreateCube(const char* InDebugName) -> FRHITextureCreateDesc
		{
			return {InDebugName, ETextureDimension::TextureCube};
		}

		static auto CreateCubeArray(const char* InDebugName) -> FRHITextureCreateDesc
		{
			return {InDebugName, ETextureDimension::TextureCubeArray};
		}

		static auto Create2D(const char* InDebugName, uint32 InWidth, uint32 InHeight, EPixelFormat InFormat) -> FRHITextureCreateDesc
		{
			return Create2D(InDebugName)
				.SetExtent(InWidth, InHeight)
				.SetFormat(InFormat);
		}

		static auto Create2D(const char* InDebugName, FIntPoint InExtent, EPixelFormat InFormat) -> FRHITextureCreateDesc
		{
			return Create2D(InDebugName)
				.SetExtent(InExtent)
				.SetFormat(InFormat);
		}

		FRHITextureCreateDesc() = default;

		FRHITextureCreateDesc(const char* InDebugName, ETextureDimension InDimension)
			: FRHITextureDesc(InDimension)
			, DebugName(InDebugName)
		{
		}

		// clang-format off
		auto SetFlags(ETextureCreateFlags InFlags) -> FRHITextureCreateDesc& { Flags = InFlags; return *this; }
		auto AddFlags(ETextureCreateFlags InFlags) -> FRHITextureCreateDesc& { Flags |= InFlags; return *this; }
		auto SetClearValue(FClearValueBinding InClearValue) -> FRHITextureCreateDesc& { ClearValue = InClearValue; return *this; }
		auto SetExtent(const FIntPoint& InExtent) -> FRHITextureCreateDesc& { Extent = InExtent; return *this; }
		auto SetExtent(uint32 InWidth, uint32 InHeight) -> FRHITextureCreateDesc& { Extent = FIntPoint(InWidth, InHeight); return *this; }
		auto SetExtent(int32 InWidth, int32 InHeight) -> FRHITextureCreateDesc& { Extent = FIntPoint(InWidth, InHeight); return *this; }
		auto SetExtent(uint32 InExtent) -> FRHITextureCreateDesc& { Extent = FIntPoint(static_cast<int32>(InExtent)); return *this; }
		auto SetDepth(uint16 InDepth) -> FRHITextureCreateDesc& { Depth = InDepth; return *this; }
		auto SetArraySize(uint16 InArraySize) -> FRHITextureCreateDesc& { ArraySize = InArraySize; return *this; }
		auto SetNumMips(uint8 InNumMips) -> FRHITextureCreateDesc& { NumMips = InNumMips; return *this; }
		auto SetNumSamples(uint8 InNumSamples) -> FRHITextureCreateDesc& { NumSamples = InNumSamples; return *this; }
		auto SetFormat(EPixelFormat InFormat) -> FRHITextureCreateDesc& { Format = InFormat; return *this; }
		auto SetDimension(ETextureDimension InDimension) -> FRHITextureCreateDesc& { Dimension = InDimension; return *this; }
		// clang-format on

		const char* DebugName = nullptr;
	};

	/** Specifies an update region for a texture */
	struct FUpdateTextureRegion2D
	{
		/** offset in texture */
		uint32 DestX = 0;
		uint32 DestY = 0;

		/** offset in source image data */
		int32 SrcX = 0;
		int32 SrcY = 0;

		/** size of region to copy */
		uint32 Width = 0;
		uint32 Height = 0;

		FUpdateTextureRegion2D() = default;
		FUpdateTextureRegion2D(uint32 InDestX, uint32 InDestY, int32 InSrcX, int32 InSrcY, uint32 InWidth, uint32 InHeight)
			: DestX(InDestX)
			, DestY(InDestY)
			, SrcX(InSrcX)
			, SrcY(InSrcY)
			, Width(InWidth)
			, Height(InHeight)
		{
		}
	};

	class FRHITexture : public FRHIResource
	{
	public:
		FRHITexture()
			: FRHIResource(ERHIResourceType::Texture)
		{
		}
		auto GetSizeX() const -> uint32 { return SizeX; }
		auto GetSizeY() const -> uint32 { return SizeY; }

	protected:
		uint32 SizeX = 0;
		uint32 SizeY = 0;
	};

	class FRHIViewport : public FRHIResource
	{
	public:
		FRHIViewport()
			: FRHIResource(ERHIResourceType::Viewport)
		{
		}
		RHI_API virtual auto Tick(float DeltaTime) -> void {};
		RHI_API virtual auto GetBackBuffer(FRHICommandListImmediate& RHICmdList) -> TRefCountPtr<FRHITexture> = 0;
		RHI_API virtual auto GetFormat() const -> EPixelFormat = 0;
	};

	struct FRHIRenderTargetsInfo
	{
		FRHITexture* ColorRenderTargets[MaxSimultaneousRenderTargets];
		int32 NumColorRenderTargets;
		bool bClearColor;
	};

	struct FRHIRenderPassInfo
	{
		FRHITexture* ColorRenderTargets[MaxSimultaneousRenderTargets];
	};

	using FVertexDeclarationElementList = std::array<struct FVertexElement, MaxVertexElementCount>;

	class FRHIVertexDeclaration : public FRHIResource
	{
	public:
		FRHIVertexDeclaration()
			: FRHIResource(ERHIResourceType::VertexDeclaration)
		{
		}

		virtual auto GetElements() const -> const FVertexDeclarationElementList& = 0;
	};

	struct FBoundShaders
	{
		FRHIShader* VertexShader = nullptr;
		FRHIShader* PixelShader = nullptr;
	};

	struct FRHIPushConstantRange
	{
		EShaderStageFlags StageFlags;
		uint32 Offset;
		uint32 Size;
	};

	struct FBindingLayoutItem
	{
		uint32 Slot;
		ERHIResourceType ResourceType;
	};

	struct FBindingLayoutDesc
	{
		std::vector<FBindingLayoutItem> BindingLayouts;
	};

	struct FBindingSetItem
	{
		FRHIResource* Resource;
		uint32 BindingSlot;
	};

	struct BindingSetDesc
	{
		std::vector<FBindingSetItem> Bindings;
	};

	class FRHIBindingSet : public FRHIResource
	{
	public:
		FRHIBindingSet()
			: FRHIResource(ERHIResourceType::BindingSet)
		{
		}
	};

	class FGraphicsPipelineStateInitializer
	{
	public:
		FBoundShaders BoundShaders;

		FName RenderPassName;

		EPixelFormat PixelFormat = EPixelFormat::Unknown;

		FRHIVertexDeclaration* VertexDeclaration = nullptr;

		std::vector<FRHIPushConstantRange> PushConstantRanges;

		std::vector<FBindingLayoutDesc> BindingLayouts;
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
			return {0, 0, EBufferUsageFlags::NullResource};
		}

		auto IsNull() const -> bool
		{
			if (EnumHasAnyFlags(Usage, EBufferUsageFlags::NullResource))
			{
				// The null resource descriptor should have its other fields zeroed, and no additional flags.
				check(Size == 0 && Stride == 0 && Usage == EBufferUsageFlags::NullResource);
				return true;
			}

			return false;
		}
	};

	struct FResourceArrayUploadInfo
	{
		const void* Data = nullptr;
		uint32 Size = 0;
	};

	struct FRHIBufferCreateDesc : public FRHIBufferDesc
	{
		static auto Create(const char* InDebugName, EBufferUsageFlags InUsage) -> FRHIBufferCreateDesc
		{
			return {InDebugName, InUsage};
		}

		static auto Create(const char* InDebugName, uint32 InSize, uint32 InStride, EBufferUsageFlags InUsage) -> FRHIBufferCreateDesc
		{
			return {InDebugName, InSize, InStride, InUsage};
		}

		static auto Create(const char* InDebugName, const FRHIBufferDesc& InDesc) -> FRHIBufferCreateDesc
		{
			return {InDebugName, InDesc};
		}

		static auto CreateNull(const char* InDebugName) -> FRHIBufferCreateDesc
		{
			return Create(InDebugName, 0, 0, EBufferUsageFlags::NullResource);
		}

		static auto CreateVertex(const char* InDebugName) -> FRHIBufferCreateDesc
		{
			return Create(InDebugName, EBufferUsageFlags::VertexBuffer);
		}

		static auto CreateVertex(const char* InDebugName, uint32 InSize) -> FRHIBufferCreateDesc
		{
			return Create(InDebugName, InSize, 0, EBufferUsageFlags::VertexBuffer);
		}

		static auto CreateIndex(const char* InDebugName) -> FRHIBufferCreateDesc
		{
			return Create(InDebugName, EBufferUsageFlags::IndexBuffer);
		}

		static auto CreateIndex(const char* InDebugName, uint32 InSize, uint32 InStride) -> FRHIBufferCreateDesc
		{
			return Create(InDebugName, InSize, InStride, EBufferUsageFlags::IndexBuffer);
		}

		FRHIBufferCreateDesc() = default;

		FRHIBufferCreateDesc(const char* InDebugName, EBufferUsageFlags InUsage)
			: DebugName(InDebugName)
		{
			Usage = InUsage;
		}

		FRHIBufferCreateDesc(const char* InDebugName, uint32 InSize, uint32 InStride, EBufferUsageFlags InUsage)
			: FRHIBufferDesc(InSize, InStride, InUsage)
			, DebugName(InDebugName)
		{
		}

		FRHIBufferCreateDesc(const char* InDebugName, const FRHIBufferDesc& InOtherDesc)
			: FRHIBufferDesc(InOtherDesc)
			, DebugName(InDebugName)
		{
		}

		FResourceArrayUploadInfo InitialData{};

		const char* DebugName = nullptr;
	};

	class FRHIBuffer : public FRHIResource
	{
	public:
		explicit FRHIBuffer(const FRHIBufferCreateDesc& InCreateDesc)
			: FRHIResource(ERHIResourceType::Buffer)
			, Desc(static_cast<FRHIBufferDesc>(InCreateDesc)) // NOLINT Slice off the DebugName and only keep the FRHIBufferDesc part of the create desc
		{
		}

		auto GetDesc() const -> FRHIBufferDesc const& { return Desc; }

		/** @return The number of bytes in the buffer. */
		auto GetSize() const -> uint32 { return Desc.Size; }

		/** @return The stride in bytes of the buffer. */
		auto GetStride() const -> uint32 { return Desc.Stride; }

		/** @return The usage flags used to create the buffer. */
		auto GetUsage() const -> EBufferUsageFlags { return Desc.Usage; }

	protected:
		FRHIBufferDesc Desc;
	};

	class FRHIGraphicsPipelineState : public FRHIResource
	{
	public:
		FRHIGraphicsPipelineState()
			: FRHIResource(ERHIResourceType::PipelineState)
		{
		}
	};

	using FBindingSetRHIRef = TRefCountPtr<FRHIBindingSet>;
	using FVertexDeclarationRHIRef = TRefCountPtr<FRHIVertexDeclaration>;
	using FViewportRHIRef = TRefCountPtr<FRHIViewport>;
	using FTextureRHIRef = TRefCountPtr<FRHITexture>;
	using FBufferRHIRef = TRefCountPtr<FRHIBuffer>;
	using FShaderRHIRef = TRefCountPtr<FRHIShader>;
	using FGraphicsPipelineStateRHIRef = TRefCountPtr<FRHIGraphicsPipelineState>;
} // namespace Doge