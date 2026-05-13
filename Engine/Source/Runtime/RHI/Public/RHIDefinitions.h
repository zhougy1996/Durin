#pragma once


namespace Doge
{
	constexpr uint32 kFrameInFlight = 2;
	constexpr uint32 MaxSimultaneousRenderTargets = 8U;

	constexpr uint32 MaxVertexElementCount = 17U;
	constexpr uint8 MaxVertexElementCount_NumBits = 5U;
	static_assert(MaxVertexElementCount <= (1U << MaxVertexElementCount_NumBits), "MaxVertexElementCount exceeds the number of bits allocated for it.");

	enum class ERHIInterface
	{
		OpenGL,
		Vulkan,
		D3D11,
		D3D12,
		Metal
	};

	enum class EShaderFrequency
	{
		Vertex,
		Pixel,
		Compute,
		RayGen,
		RayHitGroup,
		RayMiss,
	};

	enum class ERHIPipeline : uint8
	{
		Graphics = 1 << 0,
		Compute = 1 << 1,

		None = 0,
		All = Graphics | Compute,
		Num = 2
	};

	enum class EGpuVendorId
	{
		Unknown = -1,
		NotQueried = 0, // Not queried yet

		Amd = 0x1002,
		ImgTec = 0x1010,
		Nvidia = 0x10DE,
		Arm = 0x13B5,
		Broadcom = 0x14E4,
		Qualcomm = 0x5143,
		Intel = 0x8086,
		Apple = 0x106B,
	};

	inline EGpuVendorId RHIConvertToGpuVendorId(uint32 VendorId)
	{
		switch (static_cast<EGpuVendorId>(VendorId))
		{
		case EGpuVendorId::NotQueried:
			return EGpuVendorId::NotQueried;

		case EGpuVendorId::Amd:
		case EGpuVendorId::ImgTec:
		case EGpuVendorId::Nvidia:
		case EGpuVendorId::Arm:
		case EGpuVendorId::Broadcom:
		case EGpuVendorId::Qualcomm:
		case EGpuVendorId::Intel:
		case EGpuVendorId::Apple:
			return static_cast<EGpuVendorId>(VendorId);

		default:
			return EGpuVendorId::Unknown;
		}
	}

	enum class EVertexElementType : uint8
	{
		None = 0,
		Float1,
		Float2,
		Float3,
		Float4,
		PackedNormal,
		UByte4,
		UByte4N,
		Color,
		Short2,
		Short4,
		Short2N,
		Half2,
		Half4,
		Short4N,
		UShort2,
		UShort4,
		UShort2N,
		UShort4N,
		URGB10A2N,
		UInt,

		Count
	};

	enum class ETextureDimension : uint8
	{
		Texture2D,
		Texture2DArray,
		Texture3D,
		TextureCube,
		TextureCubeArray
	};

	enum class ETextureCreateFlags : uint64
	{
		None = 0,
		// Texture can be used as a render target
		RenderTargetable = 1ull << 0,
		// Texture can be used as a resolve target (MSAA back buffer, or for manual resolves of multisampled render targets).
		ResolveTargetable = 1ull << 1,
		// Texture can be used as a depth-stencil target
		DepthStencilTargetable = 1ull << 2,
		// Texture can be used as a shader resource.
		ShaderResource = 1ull << 3
	};
	ENUM_CLASS_FLAGS(ETextureCreateFlags);

	/**
	 *	Resource usage flags - for vertex and index buffers.
	 */
	enum class EBufferUsageFlags : uint32
	{
		None = 0,

		/** The buffer will be written to once. */
		Static = 1 << 0,

		/** The buffer will be written to occasionally, GPU read only, CPU write only.  The data lifetime is until the next update, or the buffer is destroyed. */
		Dynamic = 1 << 1,

		/** The buffer's data will have a lifetime of one frame.  It MUST be written to each frame, or a new one created each frame. */
		Volatile = 1 << 2,

		/** Allows an unordered access view to be created for the buffer. */
		UnorderedAccess = 1 << 3,

		/** Create a byte address buffer, which is basically a structured buffer with a uint32 type. */
		ByteAddressBuffer = 1 << 4,

		/** Buffer that the GPU will use as a source for a copy. */
		SourceCopy = 1 << 5,

		/** Create a buffer which contains the arguments used by DispatchIndirect or DrawIndirect. */
		DrawIndirect = 1 << 7,

		/**
		 * Create a buffer that can be bound as a shader resource.
		 * This is only needed for buffer types which wouldn't ordinarily be used as a shader resource, like a vertex buffer.
		 */
		ShaderResource = 1 << 8,

		/** Request that this buffer is directly CPU accessible. */
		KeepCPUAccessible = 1 << 9,

		/** Buffer should go in fast vram (hint only). Requires BUF_Transient */
		FastVRAM = 1 << 10,

		/** Create a buffer that can be shared with an external RHI or process. */
		Shared = 1 << 12,

		/**
		 * Buffer contains opaque ray tracing acceleration structure data.
		 * Resources with this flag can't be bound directly to any shader stage and only can be used with ray tracing APIs.
		 * This flag is mutually exclusive with all other buffer flags except Static and ReservedResource.
		 */
		AccelerationStructure = 1 << 13,

		VertexBuffer = 1 << 14,
		IndexBuffer = 1 << 15,
		StructuredBuffer = 1 << 16,

		/** Buffer memory is allocated independently for multiple GPUs, rather than shared via driver aliasing */
		MultiGPUAllocate = 1 << 17,

		/**
		 * Tells the render graph to not bother transferring across GPUs in multi-GPU scenarios.  Useful for cases where
		 * a buffer is read back to the CPU (such as streaming request buffers), or written to each frame by CPU (such
		 * as indirect arg buffers), and the other GPU doesn't actually care about the data.
		 */
		MultiGPUGraphIgnore = 1 << 18,

		/** Allows buffer to be used as a scratch buffer for building ray tracing acceleration structure,
		 * which implies unordered access. Only changes the buffer alignment and can be combined with other flags.
		 **/
		RayTracingScratch = (1 << 19) | UnorderedAccess,

		/** The buffer is a placeholder for streaming, and does not contain an underlying GPU resource. */
		NullResource = 1 << 20,

		/** Buffer can be used as uniform buffer on platforms that do support uniform buffer objects. */
		UniformBuffer = 1 << 21,

		/**
		 * EXPERIMENTAL: Allow the buffer to be created as a reserved (AKA tiled/sparse/virtual) resource internally, without physical memory backing.
		 * May not be used with Dynamic and other buffer flags that prevent the resource from being allocated in local GPU memory.
		 */
		ReservedResource = 1 << 22,

		// Helper bit-masks
		AnyDynamic = (Dynamic | Volatile),
	};

	ENUM_CLASS_FLAGS(EBufferUsageFlags);

	enum class EResourceLockMode
	{
		ReadOnly,
		WriteOnly,
		WriteOnly_NoOverwrite
	};

} // namespace Doge