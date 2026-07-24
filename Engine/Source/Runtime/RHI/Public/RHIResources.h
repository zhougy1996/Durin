#pragma once

#include "RHIAPI.h"
#include "RHIDefinitions.h"
#include "PixelFormat.h"

#include "Hash/XxHash.h"
#include "Math/MathFwd.h"

namespace Durin
{
	class FRHICommandListImmediate;
	class FRHIBuffer;

	// Identifies the concrete resource category tracked by the RHI lifetime system.
	enum class ERHIResourceType : uint8
	{
		Viewport,
		Buffer,
		Texture,
		Sampler,
		Shader,
		VertexDeclaration,
		BindingSet,
		PipelineState,
	};

	// Identifies the descriptor category expected by a shader resource binding.
	enum class ERHIBindingType : uint8
	{
		UniformBuffer = 0,
		Texture = 1,
		Sampler = 2,
		UniformBufferDynamic = 3,
		StorageBuffer = 4,
		StorageImage = 5,
	};

	// Provides thread-safe intrusive lifetime tracking for backend-owned GPU resources.
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
		// Packs reference count and deferred-deletion flags into one atomic lifetime state.
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

	// Supplies the stable stage and content identity shared by shader instances.
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

	// Extends shader identity with non-owning compiled-code input used during creation.
	struct FRHIShaderCreateDesc : public FRHIShaderDesc
	{
		using FCodeView = std::span<const std::byte>;

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

		static auto CreateFragment(const char* InDebugName, FCodeView InCode, FXxHash128 InHash) -> FRHIShaderCreateDesc
		{
			auto Desc = FRHIShaderCreateDesc(InDebugName, EShaderFrequency::Fragment, InCode, InHash);
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

	// Represents backend shader code together with its stage and stable content hash.
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

	// Selects which clear-value representation is valid for an attachment.
	enum class EClearBinding
	{
		None,  // No clear binding, the render target will not do hardware clears.
		Color, // Target has a
		DepthStencil,
	};

	// Carries the typed clear value associated with a render-target attachment.
	struct FClearValueBinding
	{
		// Stores the paired depth and stencil values used by a depth attachment clear.
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

		constexpr FClearValueBinding(float R, float G, float B, float A = 1.0f)
			: Binding(EClearBinding::Color)
		{
			ClearValue.Color[0] = R;
			ClearValue.Color[1] = G;
			ClearValue.Color[2] = B;
			ClearValue.Color[3] = A;
		}

		constexpr FClearValueBinding(float InDepth, uint32 InStencil)
			: Binding(EClearBinding::DepthStencil)
		{
			ClearValue.DSValue.Depth = InDepth;
			ClearValue.DSValue.Stencil = InStencil;
		}

		union FClearValue
		{
			float Color[4];
			FDepthStencilValue DSValue{};
		} ClearValue;

		EClearBinding Binding;
	};

	// Describes the shape, format, usage, and subresource layout of a texture.
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

	// Adds diagnostic identity to the immutable texture creation contract.
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
			return FRHITextureCreateDesc(InDebugName, ETextureDimension::TextureCube)
				.SetArraySize(TextureCubeFaceCount);
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

		/** texel offset from the beginning of SourceData passed to RHIUpdateTexture2D */
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

	// Validates the backend-neutral constraints required before creating a texture.
	RHI_API auto ValidateTextureCreateDesc(const FRHITextureCreateDesc& CreateDesc, std::string& OutError) -> bool;

	// Validates one uncompressed two-dimensional mip/slice upload before backend access.
	RHI_API auto ValidateTexture2DUpdate(
		const FRHITextureDesc& TextureDesc,
		uint32 MipIndex,
		uint32 ArraySlice,
		const FUpdateTextureRegion2D& UpdateRegion,
		uint32 SourcePitch,
		std::string& OutError
	) -> bool;

	// Resolves a nonzero Durin-space direction to the documented cube face and top-left-origin image UV.
	RHI_API auto ResolveTextureCubeFaceUv(
		const FVector3& Direction,
		ETextureCubeFace& OutFace,
		FVector2f& OutUv
	) -> bool;

	// Represents a backend texture created from an immutable texture descriptor.
	class FRHITexture : public FRHIResource
	{
	public:
		FRHITexture()
			: FRHIResource(ERHIResourceType::Texture)
		{
		}

		explicit FRHITexture(const FRHITextureDesc& InDesc)
			: FRHIResource(ERHIResourceType::Texture)
			, Dimension(InDesc.Dimension)
			, SizeX(static_cast<uint32>(InDesc.Extent.x))
			, SizeY(static_cast<uint32>(InDesc.Extent.y))
			, PixelFormat(InDesc.Format)
			, ArraySize(InDesc.ArraySize)
			, NumMips(InDesc.NumMips)
			, NumSamples(InDesc.NumSamples)
		{
		}

		auto GetDimension() const -> ETextureDimension { return Dimension; }
		auto GetSizeX() const -> uint32 { return SizeX; }
		auto GetSizeY() const -> uint32 { return SizeY; }
		auto GetFormat() const -> EPixelFormat { return PixelFormat; }
		auto GetArraySize() const -> uint16 { return ArraySize; }
		auto GetNumMips() const -> uint8 { return NumMips; }
		auto GetNumSamples() const -> uint8 { return NumSamples; }

	protected:
		ETextureDimension Dimension = ETextureDimension::Texture2D;
		uint32 SizeX = 0;
		uint32 SizeY = 0;
		EPixelFormat PixelFormat = EPixelFormat::Unknown;
		uint16 ArraySize = 1;
		uint8 NumMips = 1;
		uint8 NumSamples = 1;
	};

	// Represents a window presentation surface and its backend swapchain state.
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

	// Defines how an attachment's previous contents are treated at pass start.
	enum class ERHIRenderTargetLoadAction : uint8
	{
		Load,
		Clear,
		DontCare,
	};

	// Defines whether an attachment's contents remain valid after a pass.
	enum class ERHIRenderTargetStoreAction : uint8
	{
		Store,
		DontCare,
	};

	// Describes the backend-neutral image layout expected at a pass boundary.
	enum class ERHITextureLayout : uint8
	{
		Undefined,
		ColorAttachment,
		DepthStencilAttachment,
		ShaderReadOnly,
		General,
		Present,
	};

	// Describes the backend-neutral access state expected for an attachment.
	enum class ERHIAccess : uint8
	{
		None,
		ColorAttachmentWrite,
		DepthStencilReadWrite,
		ShaderRead,
		ShaderReadWrite,
		Present,
	};

	// Describes one render-pass attachment's format and load/store transitions.
	struct FRHIAttachmentLayout
	{
		EPixelFormat Format = EPixelFormat::Unknown;
		uint8 NumSamples = 1;
		ERHIRenderTargetLoadAction LoadAction = ERHIRenderTargetLoadAction::Clear;
		ERHIRenderTargetStoreAction StoreAction = ERHIRenderTargetStoreAction::Store;
		ERHIRenderTargetLoadAction StencilLoadAction = ERHIRenderTargetLoadAction::DontCare;
		ERHIRenderTargetStoreAction StencilStoreAction = ERHIRenderTargetStoreAction::DontCare;
		ERHITextureLayout InitialLayout = ERHITextureLayout::Undefined;
		ERHITextureLayout FinalLayout = ERHITextureLayout::ColorAttachment;
		ERHIAccess InitialAccess = ERHIAccess::None;
		ERHIAccess FinalAccess = ERHIAccess::ColorAttachmentWrite;

		auto operator==(const FRHIAttachmentLayout&) const -> bool = default;
	};

	// Extends an attachment layout with optional multisample resolve state.
	struct FRHIColorAttachmentLayout
	{
		FRHIAttachmentLayout RenderTarget;
		FRHIAttachmentLayout ResolveTarget;
		bool bHasResolveTarget = false;

		auto operator==(const FRHIColorAttachmentLayout&) const -> bool = default;
	};

	// Defines the attachment compatibility contract used to create render passes and pipelines.
	struct FRHIRenderTargetLayout
	{
		std::array<FRHIColorAttachmentLayout, MaxSimultaneousRenderTargets> ColorAttachments{};
		FRHIAttachmentLayout DepthStencilAttachment{};
		uint8 NumColorRenderTargets = 0;
		bool bHasDepthStencil = false;

		auto operator==(const FRHIRenderTargetLayout&) const -> bool = default;

		auto IsValid() const -> bool
		{
			if (NumColorRenderTargets > MaxSimultaneousRenderTargets || (NumColorRenderTargets == 0 && !bHasDepthStencil))
			{
				return false;
			}
			auto IsValidAttachment = [](const FRHIAttachmentLayout& Attachment) {
				const bool bValidSamples = Attachment.NumSamples == 1 || Attachment.NumSamples == 2 || Attachment.NumSamples == 4
					|| Attachment.NumSamples == 8 || Attachment.NumSamples == 16;
				auto AccessMatchesLayout = [](ERHIAccess Access, ERHITextureLayout Layout) {
					switch (Layout)
					{
					case ERHITextureLayout::Undefined: return Access == ERHIAccess::None;
					case ERHITextureLayout::ColorAttachment: return Access == ERHIAccess::ColorAttachmentWrite;
					case ERHITextureLayout::DepthStencilAttachment: return Access == ERHIAccess::DepthStencilReadWrite;
					case ERHITextureLayout::ShaderReadOnly: return Access == ERHIAccess::ShaderRead;
					case ERHITextureLayout::General: return Access == ERHIAccess::ShaderReadWrite;
					case ERHITextureLayout::Present: return Access == ERHIAccess::Present;
					}
					return false;
				};
				return Attachment.Format != EPixelFormat::Unknown && bValidSamples
					&& (Attachment.LoadAction != ERHIRenderTargetLoadAction::Load || Attachment.InitialLayout != ERHITextureLayout::Undefined)
					&& (Attachment.StencilLoadAction != ERHIRenderTargetLoadAction::Load || Attachment.InitialLayout != ERHITextureLayout::Undefined)
					&& AccessMatchesLayout(Attachment.InitialAccess, Attachment.InitialLayout)
					&& AccessMatchesLayout(Attachment.FinalAccess, Attachment.FinalLayout);
			};

			uint8 RasterSamples = 0;
			for (uint32 Index = 0; Index < NumColorRenderTargets; ++Index)
			{
				const auto& Color = ColorAttachments[Index];
				if (!IsValidAttachment(Color.RenderTarget)) return false;
				RasterSamples = RasterSamples == 0 ? Color.RenderTarget.NumSamples : RasterSamples;
				if (RasterSamples != Color.RenderTarget.NumSamples) return false;
				if (Color.bHasResolveTarget
					&& (!IsValidAttachment(Color.ResolveTarget) || Color.RenderTarget.NumSamples == 1
						|| Color.ResolveTarget.NumSamples != 1 || Color.ResolveTarget.Format != Color.RenderTarget.Format))
				{
					return false;
				}
			}
			if (bHasDepthStencil)
			{
				if (!IsValidAttachment(DepthStencilAttachment)) return false;
				RasterSamples = RasterSamples == 0 ? DepthStencilAttachment.NumSamples : RasterSamples;
				if (RasterSamples != DepthStencilAttachment.NumSamples) return false;
			}
			return true;
		}
	};

	// Produces a stable cache hash from render-target compatibility fields.
	struct FRHIRenderTargetLayoutHasher
	{
		auto operator()(const FRHIRenderTargetLayout& Layout) const -> size_t
		{
			auto Combine = [](size_t Seed, size_t Value) {
				return Seed ^ (Value + 0x9e3779b97f4a7c15ull + (Seed << 6) + (Seed >> 2));
			};
			auto HashAttachment = [&Combine](size_t Seed, const FRHIAttachmentLayout& Attachment) {
				Seed = Combine(Seed, static_cast<size_t>(Attachment.Format));
				Seed = Combine(Seed, Attachment.NumSamples);
				Seed = Combine(Seed, static_cast<size_t>(Attachment.LoadAction));
				Seed = Combine(Seed, static_cast<size_t>(Attachment.StoreAction));
				Seed = Combine(Seed, static_cast<size_t>(Attachment.StencilLoadAction));
				Seed = Combine(Seed, static_cast<size_t>(Attachment.StencilStoreAction));
				Seed = Combine(Seed, static_cast<size_t>(Attachment.InitialLayout));
				Seed = Combine(Seed, static_cast<size_t>(Attachment.FinalLayout));
				Seed = Combine(Seed, static_cast<size_t>(Attachment.InitialAccess));
				return Combine(Seed, static_cast<size_t>(Attachment.FinalAccess));
			};

			size_t Hash = Combine(0, Layout.NumColorRenderTargets);
			for (uint32 Index = 0; Index < Layout.NumColorRenderTargets; ++Index)
			{
				const auto& Color = Layout.ColorAttachments[Index];
				Hash = HashAttachment(Hash, Color.RenderTarget);
				Hash = Combine(Hash, Color.bHasResolveTarget);
				if (Color.bHasResolveTarget) Hash = HashAttachment(Hash, Color.ResolveTarget);
			}
			Hash = Combine(Hash, Layout.bHasDepthStencil);
			return Layout.bHasDepthStencil ? HashAttachment(Hash, Layout.DepthStencilAttachment) : Hash;
		}
	};

	// Collects live render-target resources used to derive a compatible layout.
	struct FRHIRenderTargetsInfo
	{
		FRHITexture* ColorRenderTargets[MaxSimultaneousRenderTargets]{};
		FRHITexture* ColorResolveTargets[MaxSimultaneousRenderTargets]{};
		FRHITexture* DepthStencilRenderTarget = nullptr;
		int32 NumColorRenderTargets = 0;
		bool bClearColor = false;
	};

	// Binds a render-target layout to concrete attachments and clear values for one pass.
	struct FRHIRenderPassInfo
	{
		FRHIRenderTargetLayout RenderTargetLayout{};
		FRHITexture* ColorRenderTargets[MaxSimultaneousRenderTargets]{};
		FRHITexture* ColorResolveTargets[MaxSimultaneousRenderTargets]{};
		FRHITexture* DepthStencilRenderTarget = nullptr;
		FClearValueBinding ColorClearValues[MaxSimultaneousRenderTargets]{};
		FClearValueBinding DepthStencilClearValue{1.0f, 0u};
	};

	using FVertexDeclarationElementList = std::array<struct FVertexElement, MaxVertexElementCount>;

	// Represents the backend mapping from vertex streams to shader attributes.
	class FRHIVertexDeclaration : public FRHIResource
	{
	public:
		FRHIVertexDeclaration()
			: FRHIResource(ERHIResourceType::VertexDeclaration)
		{
		}

		virtual auto GetElements() const -> const FVertexDeclarationElementList& = 0;
	};

	// Groups the shader stages currently bound to a graphics pipeline.
	struct FBoundShaders
	{
		FRHIShader* VertexShader = nullptr;
		FRHIShader* FragmentShader = nullptr;
	};

	// Defines a byte range of push constants visible to selected shader stages.
	struct FPushConstantRange
	{
		EShaderStageFlags StageFlags;
		uint32 Offset;
		uint32 Size;
	};

	// Describes one resource slot within a descriptor-set layout.
	struct FBindingLayoutItem
	{
		EShaderStageFlags StageFlags;
		uint32 Slot = 0;
		ERHIBindingType Type = ERHIBindingType::UniformBuffer;
		uint32 ArraySize = 1;

		FBindingLayoutItem(EShaderStageFlags InStageFlags, uint32 InSlot, ERHIBindingType InType, uint32 InArraySize = 1)
			: StageFlags(InStageFlags)
			, Slot(InSlot)
			, Type(InType)
			, ArraySize(InArraySize)
		{
		}
	};

	// Defines all resource bindings belonging to one descriptor set.
	struct FBindingLayout
	{
		std::vector<FBindingLayoutItem> BindingLayouts;
	};

	// Describes descriptor sets and push constants shared by a graphics pipeline.
	struct FPipelineLayoutDesc
	{
		std::vector<FBindingLayout> BindingLayouts;
		std::vector<FPushConstantRange> PushConstantRanges;
	};

	// Associates an RHI resource with one binding slot in a descriptor set.
	struct FBindingSetItem
	{
		FRHIResource* Resource;
		uint32 BindingSlot;
	};

	// References a byte range within a uniform buffer, including dynamic allocations.
	struct FRHIUniformBufferRange
	{
		FRHIBuffer* Buffer = nullptr;
		uint32 Offset = 0;
		uint32 Size = 0;
	};

	// References a byte range exposed to shaders as storage.
	struct FRHIStorageBufferRange
	{
		FRHIBuffer* Buffer = nullptr;
		uint32 Offset = 0;
		uint32 Size = 0;
	};

	// Collects the concrete resources used to create or resolve a binding set.
	struct BindingSetDesc
	{
		std::vector<FBindingSetItem> Bindings;
	};

	// Represents a backend descriptor set containing concrete shader resources.
	class FRHIBindingSet : public FRHIResource
	{
	public:
		FRHIBindingSet()
			: FRHIResource(ERHIResourceType::BindingSet)
		{
		}
	};

	// Selects texel reconstruction filtering for a sampler.
	enum class ESamplerFilter : uint8
	{
		Nearest,
		Linear,
	};

	// Selects filtering between adjacent mip levels.
	enum class ESamplerMipmapMode : uint8
	{
		Nearest,
		Linear,
	};

	// Defines how texture coordinates outside the normalized range are resolved.
	enum class ESamplerAddressMode : uint8
	{
		Repeat,
		MirroredRepeat,
		ClampToEdge,
		ClampToBorder,
	};

	// Selects the comparison applied by a depth-comparison sampler.
	enum class ESamplerCompareOp : uint8
	{
		Never,
		Less,
		Equal,
		LessOrEqual,
		Greater,
		NotEqual,
		GreaterOrEqual,
		Always,
	};

	// Selects the fixed border value returned by clamp-to-border sampling.
	enum class ESamplerBorderColor : uint8
	{
		FloatTransparentBlack,
		IntTransparentBlack,
		FloatOpaqueBlack,
		IntOpaqueBlack,
		FloatOpaqueWhite,
		IntOpaqueWhite,
	};

	// Describes immutable filtering, addressing, comparison, and LOD sampler state.
	struct FRHISamplerDesc
	{
		static auto PointClamp() -> FRHISamplerDesc
		{
			FRHISamplerDesc Desc;
			Desc.MinFilter = ESamplerFilter::Nearest;
			Desc.MagFilter = ESamplerFilter::Nearest;
			Desc.MipmapMode = ESamplerMipmapMode::Nearest;
			Desc.AddressU = ESamplerAddressMode::ClampToEdge;
			Desc.AddressV = ESamplerAddressMode::ClampToEdge;
			Desc.AddressW = ESamplerAddressMode::ClampToEdge;
			return Desc;
		}

		static auto PointRepeat() -> FRHISamplerDesc
		{
			FRHISamplerDesc Desc = PointClamp();
			Desc.AddressU = ESamplerAddressMode::Repeat;
			Desc.AddressV = ESamplerAddressMode::Repeat;
			Desc.AddressW = ESamplerAddressMode::Repeat;
			return Desc;
		}

		static auto LinearClamp() -> FRHISamplerDesc
		{
			FRHISamplerDesc Desc;
			Desc.MinFilter = ESamplerFilter::Linear;
			Desc.MagFilter = ESamplerFilter::Linear;
			Desc.MipmapMode = ESamplerMipmapMode::Linear;
			Desc.AddressU = ESamplerAddressMode::ClampToEdge;
			Desc.AddressV = ESamplerAddressMode::ClampToEdge;
			Desc.AddressW = ESamplerAddressMode::ClampToEdge;
			return Desc;
		}

		static auto LinearRepeat() -> FRHISamplerDesc
		{
			FRHISamplerDesc Desc = LinearClamp();
			Desc.AddressU = ESamplerAddressMode::Repeat;
			Desc.AddressV = ESamplerAddressMode::Repeat;
			Desc.AddressW = ESamplerAddressMode::Repeat;
			return Desc;
		}

		static auto AnisotropicClamp(float InMaxAnisotropy = 8.0f) -> FRHISamplerDesc
		{
			FRHISamplerDesc Desc = LinearClamp();
			Desc.bEnableAnisotropy = true;
			Desc.MaxAnisotropy = InMaxAnisotropy;
			return Desc;
		}

		static auto AnisotropicRepeat(float InMaxAnisotropy = 8.0f) -> FRHISamplerDesc
		{
			FRHISamplerDesc Desc = LinearRepeat();
			Desc.bEnableAnisotropy = true;
			Desc.MaxAnisotropy = InMaxAnisotropy;
			return Desc;
		}

		ESamplerFilter MinFilter = ESamplerFilter::Linear;
		ESamplerFilter MagFilter = ESamplerFilter::Linear;
		ESamplerMipmapMode MipmapMode = ESamplerMipmapMode::Linear;

		ESamplerAddressMode AddressU = ESamplerAddressMode::ClampToEdge;
		ESamplerAddressMode AddressV = ESamplerAddressMode::ClampToEdge;
		ESamplerAddressMode AddressW = ESamplerAddressMode::ClampToEdge;

		float MipLodBias = 0.0f;
		bool bEnableAnisotropy = false;
		float MaxAnisotropy = 1.0f;

		bool bEnableCompare = false;
		ESamplerCompareOp CompareOp = ESamplerCompareOp::Always;

		float MinLod = 0.0f;
		float MaxLod = 1000.0f;
		ESamplerBorderColor BorderColor = ESamplerBorderColor::FloatTransparentBlack;
		bool bUnnormalizedCoordinates = false;
	};

	// Represents backend sampler state used by texture bindings.
	class FRHISampler : public FRHIResource
	{
	public:
		FRHISampler()
			: FRHIResource(ERHIResourceType::Sampler)
		{
		}

		virtual auto IsImmutable() const -> bool { return false; }
	};

	// Collects all immutable state required to create a graphics pipeline.
	class FGraphicsPipelineStateInitializer
	{
	public:
		FBoundShaders BoundShaders;

		FRHIRenderTargetLayout RenderTargetLayout{};

		FRHIVertexDeclaration* VertexDeclaration = nullptr;

		FPipelineLayoutDesc PipelineLayout;

		bool bEnableAlphaBlend = false;

		bool bEnableBackFaceCulling = true;

		bool bEnableDepthTest = false;

		bool bEnableDepthWrite = false;

		// Selects filled or line rasterization for pipeline primitives.
		enum class EPolygonMode : uint8
		{
			Fill,
			Line
		};

		EPolygonMode PolygonMode = EPolygonMode::Fill;

		// Defines how submitted vertices are assembled into primitives.
		enum class EPrimitiveTopology : uint8
		{
			TriangleList,
			LineList
		};

		EPrimitiveTopology PrimitiveTopology = EPrimitiveTopology::TriangleList;
	};

	// Describes the byte size, element stride, and allowed usages of a buffer.
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

	// References caller-owned initial buffer bytes consumed during resource creation.
	struct FResourceArrayUploadInfo
	{
		const void* Data = nullptr;
		uint32 Size = 0;
	};

	// Adds initial upload data and diagnostic identity to buffer creation.
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

	// Represents a backend buffer while retaining its immutable creation descriptor.
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

	// Represents a backend graphics pipeline compatible with a fixed render-target layout.
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
	using FSamplerRHIRef = TRefCountPtr<FRHISampler>;
	using FBufferRHIRef = TRefCountPtr<FRHIBuffer>;
	using FShaderRHIRef = TRefCountPtr<FRHIShader>;
	using FGraphicsPipelineStateRHIRef = TRefCountPtr<FRHIGraphicsPipelineState>;
} // namespace Durin
