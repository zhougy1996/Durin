#pragma once

#include "RenderCoreAPI.h"
#include "RHIResources.h"

namespace Durin
{
	class FRHICommandListImmediate;
	class FRDGBuilder;
	class FRDGBuilderTestAccessor;
	class FRDGParameterResolver;
	class FRDGShaderParameterScope;

	// Selects the command domain used by a declared graph pass.
	enum class ERDGPassType : uint8
	{
		Graphics,
		Compute,
		Copy,
	};

	// Logical scheduling role, independent of physical backend topology.
	enum class ERDGQueueAssignment : uint8 { Graphics, AsyncCompute };

	// Describes whether a pass observes or replaces one declared resource range.
	enum class ERDGUse : uint8
	{
		Read,
		Write,
		ReadWrite,
	};

	// Selects the stable resource category exposed by graph diagnostics.
	enum class ERDGResourceKind : uint8
	{
		Texture,
		Buffer,
		Token,
	};

	// Selects how one graph-parameter member is lowered into a canonical use.
	enum class ERDGParameterMemberKind : uint8
	{
		Texture,
		Buffer,
		Token,
		ColorAttachment,
		DepthStencilAttachment,
		ManagedColorAttachment,
		ManagedDepthStencilAttachment,
		ManagedTexture,
		ValueRead,
		ValueWrite,
		Nested,
	};

	// Identifies where an exact runtime range is stored by a parameter wrapper.
	enum class ERDGParameterRangeKind : uint8
	{
		None,
		TextureSubresource,
		BufferBytes,
	};

	// Distinguishes semantic value reachability from execution-only ordering.
	enum class ERDGDependencyKind : uint8
	{
		Value,
		Execution,
		Explicit,
	};

	// Describes a graph-created texture without requiring physical backing.
	struct FRDGTextureDesc final
	{
		FRHITextureDesc Texture;
		uint32 ObservationTag = 0;
	};

	// Describes a graph-created buffer without requiring physical backing.
	struct FRDGBufferDesc final
	{
		FRHIBufferDesc Buffer;
		uint32 ObservationTag = 0;
	};

	// Identifies one texture registered in a single builder lifetime.
	class FRDGTextureHandle final
	{
	public:
		FRDGTextureHandle() = default;
		auto IsValid() const -> bool { return Owner != 0; }
		auto operator==(const FRDGTextureHandle&) const -> bool = default;

	private:
		friend class FRDGBuilder;
		friend class FRDGPassResources;
		FRDGTextureHandle(uint64 InOwner, uint32 InIndex)
			: Owner(InOwner), Index(InIndex) {}
		uint64 Owner = 0;
		uint32 Index = 0;
	};

	// Identifies one buffer registered in a single builder lifetime.
	class FRDGBufferHandle final
	{
	public:
		FRDGBufferHandle() = default;
		auto IsValid() const -> bool { return Owner != 0; }
		auto operator==(const FRDGBufferHandle&) const -> bool = default;

	private:
		friend class FRDGBuilder;
		friend class FRDGPassResources;
		FRDGBufferHandle(uint64 InOwner, uint32 InIndex)
			: Owner(InOwner), Index(InIndex) {}
		uint64 Owner = 0;
		uint32 Index = 0;
	};

	// Identifies one pass registered in a single builder lifetime.
	class FRDGPassHandle final
	{
	public:
		FRDGPassHandle() = default;
		auto IsValid() const -> bool { return Owner != 0; }
		auto operator==(const FRDGPassHandle&) const -> bool = default;

	private:
		friend class FRDGBuilder;
		FRDGPassHandle(uint64 InOwner, uint32 InIndex)
			: Owner(InOwner), Index(InIndex) {}
		uint64 Owner = 0;
		uint32 Index = 0;
	};

	// Identifies one logical scheduling value with no physical RHI ownership.
	class FRDGTokenHandle final
	{
	public:
		FRDGTokenHandle() = default;
		auto IsValid() const -> bool { return Owner != 0; }
		auto operator==(const FRDGTokenHandle&) const -> bool = default;

	private:
		friend class FRDGBuilder;
		FRDGTokenHandle(uint64 InOwner, uint32 InIndex)
			: Owner(InOwner), Index(InIndex) {}
		uint64 Owner = 0;
		uint32 Index = 0;
	};

	namespace RDGPrivate
	{
		template<typename T>
		inline constexpr uint8 GValueTypeIdentity = 0;
	}

	// Identifies one graph-owned payload with a compile-time C++ type.
	template<typename T>
	class TRDGValueHandle final
	{
	public:
		TRDGValueHandle() = default;
		auto IsValid() const -> bool { return Owner != 0; }
		auto operator==(const TRDGValueHandle&) const -> bool = default;
		auto OwnerForValidation() const -> uint64 { return Owner; }
		auto IndexForValidation() const -> uint32 { return Index; }

	private:
		friend class FRDGBuilder;
		friend class FRDGPassResources;
		friend class FRDGParameterResolver;
		TRDGValueHandle(uint64 InOwner, uint32 InIndex)
			: Owner(InOwner), Index(InIndex) {}
		uint64 Owner = 0;
		uint32 Index = 0;
	};
} // namespace Durin
