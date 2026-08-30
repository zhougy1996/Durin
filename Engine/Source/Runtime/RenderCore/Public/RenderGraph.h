#pragma once

#include "RenderCoreAPI.h"
#include "RHIResources.h"

#include <array>
#include <concepts>
#include <cstddef>
#include <functional>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace Durin
{
	class FRHICommandListImmediate;
	class FRenderGraphBuilder;
	class FCompiledRenderGraph;
	class FRenderGraphResourceBackings;
	class FRenderGraphParameterResolver;
	class FRenderGraphShaderParameters;

	// Selects the command domain used by a declared graph pass.
	enum class ERenderGraphPassType : uint8
	{
		Graphics,
		Compute,
		Copy,
	};

	// Describes whether a pass observes or replaces one declared resource range.
	enum class ERenderGraphUse : uint8
	{
		Read,
		Write,
		ReadWrite,
	};

	// Selects the stable resource category exposed by graph diagnostics.
	enum class ERenderGraphResourceKind : uint8
	{
		Texture,
		Buffer,
		Token,
	};

	// Selects how one graph-parameter member is lowered into a canonical use.
	enum class ERenderGraphParameterMemberKind : uint8
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
	enum class ERenderGraphParameterRangeKind : uint8
	{
		None,
		TextureSubresource,
		BufferBytes,
	};

	// Distinguishes semantic value reachability from execution-only ordering.
	enum class ERenderGraphDependencyKind : uint8
	{
		Value,
		Execution,
		Explicit,
	};

	// Describes a graph-created texture without requiring physical backing.
	struct FRenderGraphTextureDesc final
	{
		FRHITextureDesc Texture;
		std::string BackingClass = "transient";
		uint32 ObservationTag = 0;
	};

	// Describes a graph-created buffer without requiring physical backing.
	struct FRenderGraphBufferDesc final
	{
		FRHIBufferDesc Buffer;
		std::string BackingClass = "transient";
		uint32 ObservationTag = 0;
	};

	// Identifies one texture registered in a single builder lifetime.
	class FRenderGraphTextureHandle final
	{
	public:
		FRenderGraphTextureHandle() = default;
		auto IsValid() const -> bool { return Owner != 0; }
		auto operator==(const FRenderGraphTextureHandle&) const -> bool = default;

	private:
		friend class FRenderGraphBuilder;
		friend class FCompiledRenderGraph;
		friend class FRenderGraphPassResources;
		friend class FRenderGraphResourceBackings;
		FRenderGraphTextureHandle(uint64 InOwner, uint32 InIndex)
			: Owner(InOwner), Index(InIndex) {}
		uint64 Owner = 0;
		uint32 Index = 0;
	};

	// Identifies one buffer registered in a single builder lifetime.
	class FRenderGraphBufferHandle final
	{
	public:
		FRenderGraphBufferHandle() = default;
		auto IsValid() const -> bool { return Owner != 0; }
		auto operator==(const FRenderGraphBufferHandle&) const -> bool = default;

	private:
		friend class FRenderGraphBuilder;
		friend class FCompiledRenderGraph;
		friend class FRenderGraphPassResources;
		friend class FRenderGraphResourceBackings;
		FRenderGraphBufferHandle(uint64 InOwner, uint32 InIndex)
			: Owner(InOwner), Index(InIndex) {}
		uint64 Owner = 0;
		uint32 Index = 0;
	};

	// Identifies one pass registered in a single builder lifetime.
	class FRenderGraphPassHandle final
	{
	public:
		FRenderGraphPassHandle() = default;
		auto IsValid() const -> bool { return Owner != 0; }
		auto operator==(const FRenderGraphPassHandle&) const -> bool = default;

	private:
		friend class FRenderGraphBuilder;
		FRenderGraphPassHandle(uint64 InOwner, uint32 InIndex)
			: Owner(InOwner), Index(InIndex) {}
		uint64 Owner = 0;
		uint32 Index = 0;
	};

	// Identifies one logical scheduling value with no physical RHI ownership.
	class FRenderGraphTokenHandle final
	{
	public:
		FRenderGraphTokenHandle() = default;
		auto IsValid() const -> bool { return Owner != 0; }
		auto operator==(const FRenderGraphTokenHandle&) const -> bool = default;

	private:
		friend class FRenderGraphBuilder;
		FRenderGraphTokenHandle(uint64 InOwner, uint32 InIndex)
			: Owner(InOwner), Index(InIndex) {}
		uint64 Owner = 0;
		uint32 Index = 0;
	};

	namespace RenderGraphPrivate
	{
		template<typename T>
		inline constexpr uint8 GValueTypeIdentity = 0;
	}

	// Identifies one graph-owned payload with a compile-time C++ type.
	template<typename T>
	class TRenderGraphValueHandle final
	{
	public:
		TRenderGraphValueHandle() = default;
		auto IsValid() const -> bool { return Owner != 0; }
		auto operator==(const TRenderGraphValueHandle&) const -> bool = default;
		auto OwnerForValidation() const -> uint64 { return Owner; }
		auto IndexForValidation() const -> uint32 { return Index; }

	private:
		friend class FRenderGraphBuilder;
		friend class FRenderGraphPassResources;
		friend class FRenderGraphParameterResolver;
		TRenderGraphValueHandle(uint64 InOwner, uint32 InIndex)
			: Owner(InOwner), Index(InIndex) {}
		uint64 Owner = 0;
		uint32 Index = 0;
	};

	// Carries a graph-local texture handle and its exact runtime subresource range.
	struct FRenderGraphTextureParameter final
	{
		FRenderGraphTextureHandle Texture;
		FRHITextureSubresourceRange Range{};
	};

	// Carries a graph-local buffer handle and its exact runtime byte range.
	struct FRenderGraphBufferParameter final
	{
		FRenderGraphBufferHandle Buffer;
		uint64 Offset = 0;
		uint64 Size = 0;
	};

	// Carries one graph-local logical scheduling value.
	struct FRenderGraphTokenParameter final
	{
		FRenderGraphTokenHandle Token;
	};

	// Declares const read access to one graph-owned typed value.
	template<typename T>
	struct TRenderGraphValueRead final
	{
		TRenderGraphValueHandle<T> Value;
	};

	// Declares mutable write access to one graph-owned typed value.
	template<typename T>
	struct TRenderGraphValueWrite final
	{
		TRenderGraphValueHandle<T> Value;
	};

	// Carries a graph-local color attachment and its exact runtime range.
	struct FRenderGraphColorAttachmentParameter final
	{
		FRenderGraphTextureHandle Texture;
		FRHITextureSubresourceRange Range{};
	};

	// Carries a graph-local depth/stencil attachment and its runtime range.
	struct FRenderGraphDepthStencilAttachmentParameter final
	{
		FRenderGraphTextureHandle Texture;
		FRHITextureSubresourceRange Range{};
	};

	// Carries a graph-local texture whose entry/exit transitions are pass-managed.
	struct FRenderGraphManagedTextureParameter final
	{
		FRenderGraphTextureHandle Texture;
		FRHITextureSubresourceRange Range{};
	};

	struct FRenderGraphParametersMetadata;

	// Describes one parameter field in stable declaration order.
	struct FRenderGraphParameterMemberMetadata final
	{
		const char* Name = nullptr;
		uint32 Offset = 0;
		uint32 ElementSize = 0;
		uint32 ArraySize = 1;
		bool bOptional = false;
		ERenderGraphParameterMemberKind Kind =
			ERenderGraphParameterMemberKind::Texture;
		ERenderGraphResourceKind ResourceKind = ERenderGraphResourceKind::Texture;
		ERenderGraphParameterRangeKind RangeKind =
			ERenderGraphParameterRangeKind::None;
		ERenderGraphUse Use = ERenderGraphUse::Read;
		ERHIAccess Access = ERHIAccess::None;
		bool bDiscard = false;
		ERHIRenderTargetLoadAction LoadAction =
			ERHIRenderTargetLoadAction::Load;
		ERHIRenderTargetStoreAction StoreAction =
			ERHIRenderTargetStoreAction::Store;
		bool bPassManagedTransition = false;
		ERHIAccess ResultAccess = ERHIAccess::None;
		const FRenderGraphParametersMetadata* NestedParameters = nullptr;
		const void* ValueTypeIdentity = nullptr;
		bool (*ReadValueHandle)(const void*, uint64&, uint32&) = nullptr;
		// When enabled, this exact graph member also supplies one reflected
		// shader resource binding. Reflection still owns descriptor coordinates.
		bool bShaderBinding = false;
		const char* ShaderBindingName = nullptr;
		ERHIBindingType ShaderBindingType = ERHIBindingType::Texture;
	};

	// Describes a complete graph-parameter structure without owning its members.
	struct FRenderGraphParametersMetadata final
	{
		const char* StructName = nullptr;
		uint32 StructSize = 0;
		uint32 StructAlignment = 0;
		std::span<const FRenderGraphParameterMemberMetadata> Members;
	};

	template<typename ParameterStruct, size_t N>
	constexpr auto MakeInlineRenderGraphParametersMetadata(
		std::string_view StructName,
		const std::array<FRenderGraphParameterMemberMetadata, N>& Members)
		-> FRenderGraphParametersMetadata
	{
		return {
			.StructName = StructName.data(),
			.StructSize = static_cast<uint32>(sizeof(ParameterStruct)),
			.StructAlignment = static_cast<uint32>(alignof(ParameterStruct)),
			.Members = Members,
		};
	}

	template<typename ParameterStruct>
	concept CRenderGraphParameters = requires
	{
		{ ParameterStruct::GetRenderGraphParametersMetadata() }
			-> std::same_as<const FRenderGraphParametersMetadata*>;
	};

	template<typename MemberType>
	struct TRenderGraphParameterMemberTraits
	{
		using ValueType = MemberType;
		static constexpr bool bOptional = false;
		static constexpr uint32 ArraySize = 1;
		static constexpr uint32 ElementSize = sizeof(MemberType);
	};

	template<typename Value>
	struct TRenderGraphParameterMemberTraits<std::optional<Value>>
		: TRenderGraphParameterMemberTraits<Value>
	{
		using ValueType = Value;
		static constexpr bool bOptional = true;
		static constexpr uint32 ElementSize = sizeof(std::optional<Value>);
	};

	template<typename Value, size_t Count>
	struct TRenderGraphParameterMemberTraits<std::array<Value, Count>>
		: TRenderGraphParameterMemberTraits<Value>
	{
		static_assert(Count > 0, "Render graph parameter arrays cannot be empty");
		using ValueType = typename TRenderGraphParameterMemberTraits<Value>::ValueType;
		static constexpr uint32 ArraySize = static_cast<uint32>(Count);
		static constexpr uint32 ElementSize = sizeof(Value);
	};

	template<typename ParameterStruct, typename MemberType, typename ExpectedType>
	constexpr auto MakeRenderGraphResourceParameterMemberMetadata(
		const char* Name, uint32 Offset, ERenderGraphParameterMemberKind Kind,
		ERenderGraphResourceKind ResourceKind,
		ERenderGraphParameterRangeKind RangeKind, ERenderGraphUse Use,
		ERHIAccess Access, bool bDiscard = false,
		ERHIRenderTargetLoadAction LoadAction =
			ERHIRenderTargetLoadAction::Load,
		ERHIRenderTargetStoreAction StoreAction =
			ERHIRenderTargetStoreAction::Store,
		bool bPassManagedTransition = false,
		ERHIAccess ResultAccess = ERHIAccess::None)
		-> FRenderGraphParameterMemberMetadata
	{
		using FTraits = TRenderGraphParameterMemberTraits<MemberType>;
		constexpr bool bExpectedWrapper =
			std::same_as<ExpectedType, FRenderGraphTextureParameter>
			|| std::same_as<ExpectedType, FRenderGraphBufferParameter>
			|| std::same_as<ExpectedType, FRenderGraphTokenParameter>
			|| std::same_as<ExpectedType, FRenderGraphColorAttachmentParameter>
			|| std::same_as<ExpectedType,
				FRenderGraphDepthStencilAttachmentParameter>
			|| std::same_as<ExpectedType, FRenderGraphManagedTextureParameter>;
		static_assert(bExpectedWrapper,
			"Render graph resource metadata requires a typed graph wrapper");
		static_assert(std::same_as<typename FTraits::ValueType, ExpectedType>,
			"Render graph parameter member type does not match its declaration");
		static_assert(std::is_standard_layout_v<ParameterStruct>,
			"Render graph parameter structs must use standard layout");
		return {
			.Name = Name,
			.Offset = Offset,
			.ElementSize = FTraits::ElementSize,
			.ArraySize = FTraits::ArraySize,
			.bOptional = FTraits::bOptional,
			.Kind = Kind,
			.ResourceKind = ResourceKind,
			.RangeKind = RangeKind,
			.Use = Use,
			.Access = Access,
			.bDiscard = bDiscard,
			.LoadAction = LoadAction,
			.StoreAction = StoreAction,
			.bPassManagedTransition = bPassManagedTransition,
			.ResultAccess = ResultAccess,
		};
	}

	// Composes a shader SRV/UAV role onto the exact graph resource declaration.
	template<typename ParameterStruct, typename MemberType, typename ExpectedType>
	constexpr auto MakeRenderGraphShaderResourceParameterMemberMetadata(
		const char* Name, uint32 Offset, ERenderGraphParameterMemberKind Kind,
		ERenderGraphResourceKind ResourceKind,
		ERenderGraphParameterRangeKind RangeKind, ERenderGraphUse Use,
		ERHIAccess Access, ERHIBindingType BindingType,
		const char* ShaderBindingName = nullptr, bool bDiscard = false)
		-> FRenderGraphParameterMemberMetadata
	{
		auto Metadata = MakeRenderGraphResourceParameterMemberMetadata<
			ParameterStruct, MemberType, ExpectedType>(Name, Offset, Kind,
			ResourceKind, RangeKind, Use, Access, bDiscard);
		Metadata.bShaderBinding = true;
		Metadata.ShaderBindingName = ShaderBindingName != nullptr
			? ShaderBindingName : Name;
		Metadata.ShaderBindingType = BindingType;
		return Metadata;
	}

	template<typename ParameterStruct, typename MemberType>
	constexpr auto MakeRenderGraphNestedParameterMemberMetadata(
		const char* Name, uint32 Offset,
		const FRenderGraphParametersMetadata* NestedParameters)
		-> FRenderGraphParameterMemberMetadata
	{
		using FTraits = TRenderGraphParameterMemberTraits<MemberType>;
		static_assert(!FTraits::bOptional,
			"Only graph resource wrappers can be optional");
		static_assert(CRenderGraphParameters<typename FTraits::ValueType>,
			"Nested graph parameter members must have registered metadata");
		static_assert(std::is_standard_layout_v<ParameterStruct>,
			"Render graph parameter structs must use standard layout");
		return {
			.Name = Name,
			.Offset = Offset,
			.ElementSize = FTraits::ElementSize,
			.ArraySize = FTraits::ArraySize,
			.Kind = ERenderGraphParameterMemberKind::Nested,
			.NestedParameters = NestedParameters,
		};
	}

	template<typename ParameterStruct, typename MemberType, typename T>
	constexpr auto MakeRenderGraphValueParameterMemberMetadata(
		const char* Name, uint32 Offset)
		-> FRenderGraphParameterMemberMetadata
	{
		using FTraits = TRenderGraphParameterMemberTraits<MemberType>;
		constexpr bool bRead = std::same_as<typename FTraits::ValueType,
			TRenderGraphValueRead<T>>;
		constexpr bool bWrite = std::same_as<typename FTraits::ValueType,
			TRenderGraphValueWrite<T>>;
		static_assert(bRead || bWrite,
			"Render graph value member type does not match its declaration");
		static_assert(std::is_standard_layout_v<ParameterStruct>,
			"Render graph parameter structs must use standard layout");
		return {
			.Name = Name,
			.Offset = Offset,
			.ElementSize = FTraits::ElementSize,
			.ArraySize = FTraits::ArraySize,
			.bOptional = FTraits::bOptional,
			.Kind = bRead
				? ERenderGraphParameterMemberKind::ValueRead
				: ERenderGraphParameterMemberKind::ValueWrite,
			.ResourceKind = ERenderGraphResourceKind::Token,
			.Use = bRead ? ERenderGraphUse::Read : ERenderGraphUse::Write,
			.bDiscard = bWrite,
			.ValueTypeIdentity =
				&RenderGraphPrivate::GValueTypeIdentity<std::remove_cv_t<T>>,
			.ReadValueHandle = [](const void* Element, uint64& Owner,
				uint32& Index) -> bool {
				const typename FTraits::ValueType* Wrapper = nullptr;
				if constexpr (FTraits::bOptional)
				{
					const auto& Optional = *static_cast<const std::optional<
						typename FTraits::ValueType>*>(Element);
					if (!Optional) return false;
					Wrapper = &*Optional;
				}
				else Wrapper = static_cast<const typename FTraits::ValueType*>(Element);
				Owner = Wrapper->Value.OwnerForValidation();
				Index = Wrapper->Value.IndexForValidation();
				return true;
			},
		};
	}

	// A move-only mutable capability for one builder-owned parameter allocation.
	template<typename ParameterStruct>
	class TRenderGraphParametersRef final
	{
	public:
		TRenderGraphParametersRef() = default;
		TRenderGraphParametersRef(TRenderGraphParametersRef&& Other) noexcept
			: Data(std::exchange(Other.Data, nullptr)),
			  Lifetime(std::move(Other.Lifetime))
		{
		}
		auto operator=(TRenderGraphParametersRef&& Other) noexcept
			-> TRenderGraphParametersRef&
		{
			if (this != &Other)
			{
				Data = std::exchange(Other.Data, nullptr);
				Lifetime = std::move(Other.Lifetime);
			}
			return *this;
		}

		TRenderGraphParametersRef(const TRenderGraphParametersRef&) = delete;
		auto operator=(const TRenderGraphParametersRef&)
			-> TRenderGraphParametersRef& = delete;

		auto IsValid() const -> bool { return Data != nullptr && !Lifetime.expired(); }
		explicit operator bool() const { return IsValid(); }
		auto Get() -> ParameterStruct& { return *Data; }
		auto Get() const -> const ParameterStruct& { return *Data; }
		auto operator->() -> ParameterStruct* { return IsValid() ? Data : nullptr; }
		auto operator->() const -> const ParameterStruct*
		{
			return IsValid() ? Data : nullptr;
		}

	private:
		friend class FRenderGraphBuilder;
		TRenderGraphParametersRef(ParameterStruct* InData,
			std::weak_ptr<void> InLifetime)
			: Data(InData), Lifetime(std::move(InLifetime))
		{
		}

		ParameterStruct* Data = nullptr;
		std::weak_ptr<void> Lifetime;
	};

	// Exposes only resources declared by the executing graph to pass callbacks.
	class RENDERCORE_API FRenderGraphPassResources final
	{
	public:
		auto GetTexture(FRenderGraphTextureHandle Handle) const -> FRHITexture*;
		auto GetBuffer(FRenderGraphBufferHandle Handle) const -> FRHIBuffer*;
		template<typename T>
		auto ReadValue(TRenderGraphValueHandle<T> Handle) const -> const T&
		{
			return *static_cast<const T*>(ResolveValue(Handle.Owner, Handle.Index,
				&RenderGraphPrivate::GValueTypeIdentity<std::remove_cv_t<T>>, false));
		}
		template<typename T>
		auto WriteValue(TRenderGraphValueHandle<T> Handle) const -> T&
		{
			return *static_cast<T*>(ResolveValue(Handle.Owner, Handle.Index,
				&RenderGraphPrivate::GValueTypeIdentity<std::remove_cv_t<T>>, true));
		}

	private:
		friend class FCompiledRenderGraph;
		explicit FRenderGraphPassResources(const FCompiledRenderGraph& InGraph,
			uint32 InPassIndex)
			: Graph(InGraph), PassIndex(InPassIndex)
		{
		}
		auto ResolveValue(uint64 Owner, uint32 Index, const void* TypeIdentity,
			bool bWrite) const -> void*;

		const FCompiledRenderGraph& Graph;
		uint32 PassIndex = 0;
	};

	// Carries the physical texture and immutable declaration details for one
	// color or depth/stencil attachment parameter.
	struct FRenderGraphAttachmentView final
	{
		FRHITexture* Texture = nullptr;
		FRHITextureSubresourceRange Range{};
		ERHIRenderTargetLoadAction LoadAction =
			ERHIRenderTargetLoadAction::Load;
		ERHIRenderTargetStoreAction StoreAction =
			ERHIRenderTargetStoreAction::Store;
		bool bPassManagedTransition = false;
		ERHIAccess ResultAccess = ERHIAccess::None;

		explicit operator bool() const { return Texture != nullptr; }
	};

	// Resolves only wrapper objects that are members of the executing pass's
	// immutable parameter allocation. Raw graph handles are intentionally absent.
	class RENDERCORE_API FRenderGraphParameterResolver final
	{
	public:
		FRenderGraphParameterResolver(const FRenderGraphParameterResolver&) = delete;
		auto operator=(const FRenderGraphParameterResolver&)
			-> FRenderGraphParameterResolver& = delete;
		FRenderGraphParameterResolver(FRenderGraphParameterResolver&&) = delete;
		auto operator=(FRenderGraphParameterResolver&&)
			-> FRenderGraphParameterResolver& = delete;

		auto GetTexture(const FRenderGraphTextureParameter& Parameter) const
			-> FRHITexture*;
		auto GetTexture(
			const std::optional<FRenderGraphTextureParameter>& Parameter) const
			-> FRHITexture*;
		auto GetTexture(const FRenderGraphManagedTextureParameter& Parameter) const
			-> FRHITexture*;
		auto GetTexture(
			const std::optional<FRenderGraphManagedTextureParameter>& Parameter) const
			-> FRHITexture*;
		auto GetBuffer(const FRenderGraphBufferParameter& Parameter) const
			-> FRHIBuffer*;
		auto GetBuffer(
			const std::optional<FRenderGraphBufferParameter>& Parameter) const
			-> FRHIBuffer*;
		auto GetColorAttachment(
			const FRenderGraphColorAttachmentParameter& Parameter) const
			-> FRenderGraphAttachmentView;
		auto GetColorAttachment(const std::optional<
			FRenderGraphColorAttachmentParameter>& Parameter) const
			-> FRenderGraphAttachmentView;
		auto GetDepthStencilAttachment(
			const FRenderGraphDepthStencilAttachmentParameter& Parameter) const
			-> FRenderGraphAttachmentView;
		auto GetDepthStencilAttachment(const std::optional<
			FRenderGraphDepthStencilAttachmentParameter>& Parameter) const
			-> FRenderGraphAttachmentView;
		auto GetPassName() const -> std::string_view { return PassName; }
		auto GetPassType() const -> ERenderGraphPassType { return PassType; }
		template<CRenderGraphParameters ParameterStruct>
		auto GetShaderParameters(const ParameterStruct& InParameters) const
			-> FRenderGraphShaderParameters;
		template<typename T>
		auto ReadValue(const TRenderGraphValueRead<T>& Parameter) const -> const T&
		{
			FindMember(&Parameter, ERenderGraphParameterMemberKind::ValueRead,
				ERenderGraphParameterMemberKind::ValueRead, false);
			return Resources.ReadValue(Parameter.Value);
		}
		template<typename T>
		auto WriteValue(const TRenderGraphValueWrite<T>& Parameter) const -> T&
		{
			FindMember(&Parameter, ERenderGraphParameterMemberKind::ValueWrite,
				ERenderGraphParameterMemberKind::ValueWrite, false);
			return Resources.WriteValue(Parameter.Value);
		}
		template<typename T>
		auto ReadValue(const std::optional<TRenderGraphValueRead<T>>& Parameter) const
			-> const T*
		{
			FindMember(&Parameter, ERenderGraphParameterMemberKind::ValueRead,
				ERenderGraphParameterMemberKind::ValueRead, true);
			return Parameter ? &Resources.ReadValue(Parameter->Value) : nullptr;
		}
		template<typename T>
		auto WriteValue(
			const std::optional<TRenderGraphValueWrite<T>>& Parameter) const -> T*
		{
			FindMember(&Parameter, ERenderGraphParameterMemberKind::ValueWrite,
				ERenderGraphParameterMemberKind::ValueWrite, true);
			return Parameter ? &Resources.WriteValue(Parameter->Value) : nullptr;
		}

	private:
		friend class FCompiledRenderGraph;
		friend class FRenderGraphShaderParameters;
		explicit FRenderGraphParameterResolver(
			const FRenderGraphPassResources& InResources,
			const FRenderGraphParametersMetadata* InMetadata,
			const void* InParameters, std::string_view InPassName,
			ERenderGraphPassType InPassType)
			: Resources(InResources), Metadata(InMetadata), Parameters(InParameters),
			  PassName(InPassName), PassType(InPassType)
		{
		}
		auto ValidateShaderParametersIdentity(const void* Data,
			const FRenderGraphParametersMetadata* InMetadata) const -> void;
		auto FindMember(const void* Address,
			ERenderGraphParameterMemberKind ExpectedKind,
			ERenderGraphParameterMemberKind AlternateKind,
			bool bOptional) const -> const FRenderGraphParameterMemberMetadata&;

		const FRenderGraphPassResources& Resources;
		const FRenderGraphParametersMetadata* Metadata = nullptr;
		const void* Parameters = nullptr;
		std::string_view PassName;
		ERenderGraphPassType PassType = ERenderGraphPassType::Graphics;
	};

	// A non-copyable callback-lifetime view of the exact immutable pass object.
	// It is the only object accepted by composed shader submission.
	class RENDERCORE_API FRenderGraphShaderParameters final
	{
	public:
		FRenderGraphShaderParameters(const FRenderGraphShaderParameters&) = delete;
		auto operator=(const FRenderGraphShaderParameters&)
			-> FRenderGraphShaderParameters& = delete;
		FRenderGraphShaderParameters(FRenderGraphShaderParameters&&) = delete;
		auto operator=(FRenderGraphShaderParameters&&)
			-> FRenderGraphShaderParameters& = delete;

		auto GetResolver() const -> const FRenderGraphParameterResolver&
		{
			return Resolver;
		}
		auto GetData() const -> const void* { return Data; }
		auto GetMetadata() const -> const FRenderGraphParametersMetadata*
		{
			return Metadata;
		}

	private:
		friend class FRenderGraphParameterResolver;
		FRenderGraphShaderParameters(const FRenderGraphParameterResolver& InResolver,
			const void* InData, const FRenderGraphParametersMetadata* InMetadata)
			: Resolver(InResolver), Data(InData), Metadata(InMetadata)
		{
		}

		const FRenderGraphParameterResolver& Resolver;
		const void* Data = nullptr;
		const FRenderGraphParametersMetadata* Metadata = nullptr;
	};

	template<CRenderGraphParameters ParameterStruct>
	auto FRenderGraphParameterResolver::GetShaderParameters(
		const ParameterStruct& InParameters) const
		-> FRenderGraphShaderParameters
	{
		const auto* InMetadata =
			ParameterStruct::GetRenderGraphParametersMetadata();
		ValidateShaderParametersIdentity(&InParameters, InMetadata);
		return FRenderGraphShaderParameters(*this, &InParameters, InMetadata);
	}

	using FRenderGraphExecute = std::function<void(
		FRHICommandListImmediate&, const FRenderGraphPassResources&)>;
	using FRenderGraphParameterizedExecute = std::function<void(
		FRHICommandListImmediate&, const FRenderGraphParameterResolver&)>;
	using FRenderGraphPrepare = std::function<bool(std::string&)>;

	// Describes one retained graph-created resource for execution allocation.
	// Diagnostic names are deliberately absent from allocation identity.
	struct FRDGAllocationRequest final
	{
		uint32 ResourceId = 0;
		ERenderGraphResourceKind Kind = ERenderGraphResourceKind::Texture;
		FRHITextureDesc TextureDesc;
		FRHIBufferDesc BufferDesc;
		uint32 FirstPass = 0;
		uint32 LastPass = 0;
		uint32 ObservationTag = 0;
	};

	struct FRDGAllocationStatistics final
	{
		uint32 ActiveResources = 0;
		uint32 RetainedResources = 0;
		uint64 ActiveBytes = 0;
		uint64 RetainedBytes = 0;
		uint64 PeakActiveBytes = 0;
		uint64 ReuseHits = 0;
		uint64 ReuseMisses = 0;
		uint64 Evictions = 0;
		uint64 Failures = 0;
	};

	// Owns one complete candidate allocation result until graph execution retires.
	class RENDERCORE_API FRDGAllocatedResources final
	{
	public:
		auto SetTexture(uint32 ResourceId, FTextureRHIRef Texture,
			uint64 AllocationId = 0,
			std::string_view Disposition = "allocated") -> bool;
		auto SetBuffer(uint32 ResourceId, FBufferRHIRef Buffer,
			uint64 AllocationId = 0,
			std::string_view Disposition = "allocated") -> bool;
		auto SetStatistics(const FRDGAllocationStatistics& InStatistics) -> void
		{
			Statistics = InStatistics;
		}

	private:
		friend class FCompiledRenderGraph;
		explicit FRDGAllocatedResources(uint32 Count);
		std::vector<FTextureRHIRef> Textures;
		std::vector<FBufferRHIRef> Buffers;
		std::vector<uint64> AllocationIds;
		std::vector<std::string> AllocationDispositions;
		FRDGAllocationStatistics Statistics;
	};

	// Allocates one retained batch atomically. Implementations must not publish a
	// partial result when returning failure.
	class RENDERCORE_API FRDGAllocator
	{
	public:
		virtual ~FRDGAllocator() = default;
		virtual auto Allocate(std::span<const FRDGAllocationRequest> Requests,
			FRDGAllocatedResources& OutResources, std::string& OutError)
			-> bool = 0;
	};

	struct FRDGExecutionContext final
	{
		FRDGAllocator& Allocator;
	};

	// Names one retained logical resource that requires physical backing.
	struct FRenderGraphPreparationRequest final
	{
		uint32 ResourceId = 0;
		std::string Name;
		ERenderGraphResourceKind Kind = ERenderGraphResourceKind::Texture;
		FRenderGraphTextureHandle Texture;
		FRenderGraphBufferHandle Buffer;
		FRHITextureDesc TextureDesc;
		FRHIBufferDesc BufferDesc;
		std::string BackingClass;
		uint32 FirstPass = 0;
		uint32 LastPass = 0;
	};

	// Collects one candidate complete backing publication during preparation.
	class RENDERCORE_API FRenderGraphResourceBackings final
	{
	public:
		auto SetTexture(FRenderGraphTextureHandle Handle, FRHITexture* Texture)
			-> bool;
		auto SetBuffer(FRenderGraphBufferHandle Handle, FRHIBuffer* Buffer)
			-> bool;

	private:
		friend class FCompiledRenderGraph;
		explicit FRenderGraphResourceBackings(uint64 InOwner, uint32 Count);
		uint64 Owner = 0;
		std::vector<FRHITexture*> Textures;
		std::vector<FRHIBuffer*> Buffers;
	};

	using FRenderGraphBackingResolver = std::function<bool(
		std::span<const FRenderGraphPreparationRequest>,
		FRenderGraphResourceBackings&, std::string&)>;

	// Records one immutable dependency edge in compiler diagnostics.
	struct FRenderGraphDependency final
	{
		uint32 BeforePass = 0;
		uint32 AfterPass = 0;
		std::string Cause;
		ERenderGraphDependencyKind Kind = ERenderGraphDependencyKind::Execution;

		auto operator==(const FRenderGraphDependency&) const -> bool = default;
	};

	// Records one pointer-free declared resource and its preparation outcome.
	struct FRenderGraphResourceCapture final
	{
		uint32 ResourceId = 0;
		std::string Name;
		ERenderGraphResourceKind Kind = ERenderGraphResourceKind::Texture;
		bool bImported = false;
		std::string BackingClass;
		std::string Preparation;
		std::string AllocationDisposition;
		uint64 PhysicalAllocationId = 0;
		std::string ValueType;
		EPixelFormat TextureFormat = EPixelFormat::Unknown;
		FIntPoint TextureExtent{0, 0};
		uint16 TextureArraySize = 0;
		uint8 TextureMips = 0;
		uint64 BufferSize = 0;
		uint32 BufferStride = 0;
	};

	// Records one exact pointer-free pass use after range normalization.
	struct FRenderGraphUseCapture final
	{
		uint32 PassDeclarationIndex = 0;
		uint32 ResourceId = 0;
		ERenderGraphUse Use = ERenderGraphUse::Read;
		ERHIAccess Access = ERHIAccess::None;
		FRHITextureSubresourceRange TextureRange{};
		uint64 BufferOffset = 0;
		uint64 BufferSize = 0;
		uint32 Version = 0;
		bool bDiscard = false;
		bool bStore = true;
		std::string ParameterPath;
		std::string ShaderBindingName;
		ERHIBindingType ShaderBindingType = ERHIBindingType::Texture;
	};

	// Records one submitted leaf parameter capability, including optional absence.
	struct FRenderGraphParameterCapture final
	{
		uint32 PassDeclarationIndex = 0;
		std::string FieldPath;
		ERenderGraphParameterMemberKind Kind =
			ERenderGraphParameterMemberKind::Texture;
		ERenderGraphResourceKind ResourceKind =
			ERenderGraphResourceKind::Texture;
		bool bPresent = false;
		// Absent optional fields use max uint32 and never name a synthetic resource.
		uint32 ResourceId = std::numeric_limits<uint32>::max();
		ERenderGraphUse Use = ERenderGraphUse::Read;
		ERHIAccess Access = ERHIAccess::None;
		FRHITextureSubresourceRange TextureRange{};
		uint64 BufferOffset = 0;
		uint64 BufferSize = 0;
		bool bDiscard = false;
		bool bStore = true;
		bool bPassManagedTransition = false;
		ERHIAccess ResultAccess = ERHIAccess::None;
		std::string ShaderBindingName;
		ERHIBindingType ShaderBindingType = ERHIBindingType::Texture;
	};

	// Records one exact pointer-free transition at a pass or graph boundary.
	struct FRenderGraphTransitionCapture final
	{
		uint32 ResourceId = 0;
		uint32 PassIndex = std::numeric_limits<uint32>::max();
		ERHIAccess Before = ERHIAccess::None;
		ERHIAccess After = ERHIAccess::None;
		FRHITextureSubresourceRange TextureRange{};
		uint64 BufferOffset = 0;
		uint64 BufferSize = 0;
		bool bFinal = false;
	};

	// Reports the retained scheduled interval of one declared resource.
	struct FRenderGraphResourceLifetime final
	{
		std::string Name;
		uint32 FirstPass = 0;
		uint32 LastPass = 0;
		bool bImported = false;
		bool bCulled = false;
	};

	// Explains whether one declared pass survived explicit-root reachability.
	struct FRenderGraphCullingDecision final
	{
		std::string Name;
		bool bCulled = false;
		std::string Reason;
	};

	// Separates catastrophic graph-shape safety limits from observational budgets.
	struct FRenderGraphBudget final
	{
		uint32 MaxPasses = std::numeric_limits<uint32>::max();
		uint32 MaxDependencies = std::numeric_limits<uint32>::max();
		uint32 MaxBufferTransitions = std::numeric_limits<uint32>::max();
		uint32 MaxTextureTransitions = std::numeric_limits<uint32>::max();
		uint32 RegressionMaxPasses = std::numeric_limits<uint32>::max();
		uint32 RegressionMaxDependencies = std::numeric_limits<uint32>::max();
		uint32 RegressionMaxBufferTransitions =
			std::numeric_limits<uint32>::max();
		uint32 RegressionMaxTextureTransitions =
			std::numeric_limits<uint32>::max();
		uint64 MaxCompileMicroseconds = std::numeric_limits<uint64>::max();
		uint64 MaxExecuteMicroseconds = std::numeric_limits<uint64>::max();
	};

	// Reports graph shape and CPU cost without affecting execution correctness.
	struct FRenderGraphStatistics final
	{
		uint32 DeclaredPasses = 0;
		uint32 ScheduledPasses = 0;
		uint32 CulledPasses = 0;
		uint32 Dependencies = 0;
		uint32 BufferTransitions = 0;
		uint32 TextureTransitions = 0;
		uint64 CompileMicroseconds = 0;
		uint64 ExecuteMicroseconds = 0;
		bool bPassRegressionBudgetExceeded = false;
		bool bDependencyRegressionBudgetExceeded = false;
		bool bBufferTransitionRegressionBudgetExceeded = false;
		bool bTextureTransitionRegressionBudgetExceeded = false;
		bool bCompileBudgetExceeded = false;
		bool bExecuteBudgetExceeded = false;

		auto IsStructuralRegressionBudgetExceeded() const -> bool
		{
			return bPassRegressionBudgetExceeded
				|| bDependencyRegressionBudgetExceeded
				|| bBufferTransitionRegressionBudgetExceeded
				|| bTextureTransitionRegressionBudgetExceeded;
		}
	};

	// Pointer-free pass record suitable for persistence and tooling.
	struct FRenderGraphPassCapture final
	{
		std::string Name;
		ERenderGraphPassType Type = ERenderGraphPassType::Graphics;
		uint32 DeclarationIndex = 0;
		std::string ParameterStructName;
		uint32 BufferTransitions = 0;
		uint32 TextureTransitions = 0;
	};

	// Owns an immutable diagnostic snapshot independent of graph/RHI lifetimes.
	struct FRenderGraphCapture final
	{
		FRenderGraphBudget Budget;
		FRenderGraphStatistics Statistics;
		FRDGAllocationStatistics AllocationStatistics;
		std::vector<FRenderGraphPassCapture> Passes;
		std::vector<FRenderGraphResourceCapture> Resources;
		std::vector<FRenderGraphParameterCapture> Parameters;
		std::vector<FRenderGraphUseCapture> Uses;
		std::vector<FRenderGraphTransitionCapture> Transitions;
		std::vector<FRenderGraphDependency> Dependencies;
		std::vector<FRenderGraphResourceLifetime> ResourceLifetimes;
		std::vector<FRenderGraphCullingDecision> CullingDecisions;
		std::string Dump;
	};

	// Owns the compiled pass order and transition batches for one graph.
	struct FCompiledRenderGraphPass final
	{
		std::string Name;
		ERenderGraphPassType Type = ERenderGraphPassType::Graphics;
		uint32 DeclarationIndex = 0;
		std::string ParameterStructName;
		std::vector<FRHIBufferTransition> BufferTransitions;
		std::vector<FRHITextureTransition> TextureTransitions;
	};

	// Immutable executable result produced only after complete graph validation.
	class RENDERCORE_API FCompiledRenderGraph final
	{
	public:
		FCompiledRenderGraph(FCompiledRenderGraph&&) noexcept;
		auto operator=(FCompiledRenderGraph&&) noexcept
			-> FCompiledRenderGraph&;
		~FCompiledRenderGraph();

		FCompiledRenderGraph(const FCompiledRenderGraph&) = delete;
		auto operator=(const FCompiledRenderGraph&)
			-> FCompiledRenderGraph& = delete;

		auto GetPasses() const -> std::span<const FCompiledRenderGraphPass>;
		auto GetDependencies() const -> std::span<const FRenderGraphDependency>;
		auto GetResourceLifetimes() const
			-> std::span<const FRenderGraphResourceLifetime>;
		auto GetCullingDecisions() const
			-> std::span<const FRenderGraphCullingDecision>;
		auto GetFinalBufferTransitions() const
			-> std::span<const FRHIBufferTransition>;
		auto GetFinalTextureTransitions() const
			-> std::span<const FRHITextureTransition>;
		auto GetCompileMicroseconds() const -> uint64;
		auto GetBudget() const -> const FRenderGraphBudget&;
		auto GetStatistics() const -> FRenderGraphStatistics;
		auto Capture() const -> FRenderGraphCapture;
		auto Dump() const -> std::string;
		// Runs complete resource preparation before recording any pass command.
		auto Execute(FRHICommandListImmediate& CommandList,
			std::string* OutError = nullptr) const -> bool;
		auto Execute(FRHICommandListImmediate& CommandList,
			FRDGExecutionContext& Context,
			std::string* OutError = nullptr) const -> bool;

	private:
		friend class FRenderGraphBuilder;
		friend class FRenderGraphPassResources;
		struct FState;
		explicit FCompiledRenderGraph(std::unique_ptr<FState> InState);
		auto ExecuteInternal(FRHICommandListImmediate& CommandList,
			FRDGExecutionContext* Context, std::string* OutError) const -> bool;
		std::unique_ptr<FState> State;
	};

	// Publishes either one complete graph or one deterministic compile failure.
	struct FRenderGraphCompileResult final
	{
		std::unique_ptr<FCompiledRenderGraph> Graph;
		std::string Error;

		auto IsSuccess() const -> bool { return Graph != nullptr; }
	};

	// Collects frame-local resources and passes before deterministic compilation.
	class RENDERCORE_API FRenderGraphBuilder final
	{
	public:
		FRenderGraphBuilder();
		~FRenderGraphBuilder();

		FRenderGraphBuilder(const FRenderGraphBuilder&) = delete;
		auto operator=(const FRenderGraphBuilder&)
			-> FRenderGraphBuilder& = delete;
		FRenderGraphBuilder(FRenderGraphBuilder&&) = delete;
		auto operator=(FRenderGraphBuilder&&)
			-> FRenderGraphBuilder& = delete;

		auto ImportTexture(std::string_view Name, FRHITexture* Texture,
			ERHIAccess InitialAccess, ERHIAccess FinalAccess)
			-> FRenderGraphTextureHandle;
		auto RegisterExternalTexture(const FTextureRHIRef& Texture,
			std::string_view Name, ERHIAccess InitialAccess,
			ERHIAccess FinalAccess) -> FRenderGraphTextureHandle;
		auto CreateTexture(const FRenderGraphTextureDesc& Desc,
			std::string_view Name,
			ERHIAccess FinalAccess = ERHIAccess::None)
			-> FRenderGraphTextureHandle;
		auto CreateTexture(std::string_view Name, FRHITexture* Texture,
			ERHIAccess FinalAccess = ERHIAccess::None)
			-> FRenderGraphTextureHandle;
		auto CreateTexture(std::string_view Name,
			const FRenderGraphTextureDesc& Desc,
			ERHIAccess FinalAccess = ERHIAccess::None)
			-> FRenderGraphTextureHandle;
		auto ImportBuffer(std::string_view Name, FRHIBuffer* Buffer,
			ERHIAccess InitialAccess, ERHIAccess FinalAccess)
			-> FRenderGraphBufferHandle;
		auto RegisterExternalBuffer(const FBufferRHIRef& Buffer,
			std::string_view Name, ERHIAccess InitialAccess,
			ERHIAccess FinalAccess) -> FRenderGraphBufferHandle;
		auto CreateBuffer(const FRenderGraphBufferDesc& Desc,
			std::string_view Name,
			ERHIAccess FinalAccess = ERHIAccess::None)
			-> FRenderGraphBufferHandle;
		auto CreateBuffer(std::string_view Name, FRHIBuffer* Buffer,
			ERHIAccess FinalAccess = ERHIAccess::None)
			-> FRenderGraphBufferHandle;
		auto CreateBuffer(std::string_view Name,
			const FRenderGraphBufferDesc& Desc,
			ERHIAccess FinalAccess = ERHIAccess::None)
			-> FRenderGraphBufferHandle;
		auto CreateToken(std::string_view Name) -> FRenderGraphTokenHandle;
		auto QueueTextureExtraction(FRenderGraphTextureHandle Texture,
			FTextureRHIRef* Destination, ERHIAccess FinalAccess) -> void;
		auto QueueBufferExtraction(FRenderGraphBufferHandle Buffer,
			FBufferRHIRef* Destination, ERHIAccess FinalAccess) -> void;
		template<typename T, typename... Args>
		requires std::constructible_from<T, Args...> && std::destructible<T>
		auto CreateValue(std::string_view Name, std::string_view StableTypeName,
			Args&&... ConstructorArgs) -> TRenderGraphValueHandle<T>
		{
			static_assert(std::is_object_v<T> && !std::is_const_v<T>
				&& !std::is_volatile_v<T>,
				"Render graph values require an unqualified object type");
			uint32 Index = 0;
			void* Storage = AllocateValueStorage(Name, StableTypeName,
				&RenderGraphPrivate::GValueTypeIdentity<T>, sizeof(T), alignof(T),
				[](void* Value) { std::destroy_at(static_cast<T*>(Value)); }, Index);
			if (Storage == nullptr) return {};
			std::construct_at(static_cast<T*>(Storage),
				std::forward<Args>(ConstructorArgs)...);
			MarkValueStorageConstructed(Index);
			return {StateOwner(), Index};
		}

		auto AddPass(std::string_view Name, ERenderGraphPassType Type,
			FRenderGraphExecute Execute = {}) -> FRenderGraphPassHandle;
		template<typename ParameterStruct>
		requires CRenderGraphParameters<ParameterStruct>
		auto AddPass(std::string_view Name, ERenderGraphPassType Type,
			TRenderGraphParametersRef<ParameterStruct>&& Parameters,
			FRenderGraphExecute Execute = {}) -> FRenderGraphPassHandle
		{
			auto Lifetime = Parameters.Lifetime.lock();
			void* Data = std::exchange(Parameters.Data, nullptr);
			Parameters.Lifetime.reset();
			return AddParameterizedPass(Name, Type,
				ParameterStruct::GetRenderGraphParametersMetadata(), Data,
				std::move(Lifetime), std::move(Execute), {});
		}
		template<typename ParameterStruct, typename Execute>
		requires CRenderGraphParameters<ParameterStruct>
			&& std::invocable<Execute&, FRHICommandListImmediate&,
				const ParameterStruct&, const FRenderGraphParameterResolver&>
		auto AddPass(std::string_view Name, ERenderGraphPassType Type,
			TRenderGraphParametersRef<ParameterStruct>&& Parameters,
			Execute&& ExecuteCallback) -> FRenderGraphPassHandle
		{
			ParameterStruct* TypedData = Parameters.Data;
			FRenderGraphParameterizedExecute ErasedExecute =
				[TypedData, Callback = std::forward<Execute>(ExecuteCallback)](
					FRHICommandListImmediate& CommandList,
					const FRenderGraphParameterResolver& Resolver) mutable {
					std::invoke(Callback, CommandList,
						static_cast<const ParameterStruct&>(*TypedData), Resolver);
				};
			auto Lifetime = Parameters.Lifetime.lock();
			void* Data = std::exchange(Parameters.Data, nullptr);
			Parameters.Lifetime.reset();
			return AddParameterizedPass(Name, Type,
				ParameterStruct::GetRenderGraphParametersMetadata(), Data,
				std::move(Lifetime), {}, std::move(ErasedExecute));
		}
		auto AddDependency(FRenderGraphPassHandle Pass,
			FRenderGraphPassHandle Prerequisite) -> void;
		auto MarkPassRoot(FRenderGraphPassHandle Pass,
			std::string_view Reason = "side-effect") -> void;
		auto EnablePassCulling() -> void;
		auto SetExecutionPreparation(FRenderGraphPrepare Prepare) -> void;
		auto SetBackingResolver(FRenderGraphBackingResolver Resolver) -> void;
		auto SetBudget(const FRenderGraphBudget& Budget) -> void;

		template<typename ParameterStruct>
		requires CRenderGraphParameters<ParameterStruct>
		auto AllocParameters() -> TRenderGraphParametersRef<ParameterStruct>
		{
			static_assert(std::is_standard_layout_v<ParameterStruct>,
				"Render graph parameter structs must use standard layout");
			static_assert(std::default_initializable<ParameterStruct>,
				"Render graph parameter structs must be default constructible");
			static_assert(std::destructible<ParameterStruct>,
				"Render graph parameter structs must be destructible");
			std::weak_ptr<void> Lifetime;
			void* Storage = AllocateParameterStorage(sizeof(ParameterStruct),
				alignof(ParameterStruct),
				ParameterStruct::GetRenderGraphParametersMetadata(),
				[](void* Value) { std::destroy_at(
					static_cast<ParameterStruct*>(Value)); }, Lifetime);
			if (Storage == nullptr) return {};
			auto* Parameters = std::construct_at(
				static_cast<ParameterStruct*>(Storage));
			MarkParameterStorageConstructed(Storage);
			return {Parameters, std::move(Lifetime)};
		}

		auto UseTexture(FRenderGraphPassHandle Pass,
			FRenderGraphTextureHandle Texture,
			const FRHITextureSubresourceRange& Range, ERenderGraphUse Use,
			ERHIAccess Access, bool bDiscard = false) -> void;
		auto UseBuffer(FRenderGraphPassHandle Pass,
			FRenderGraphBufferHandle Buffer, uint64 Offset, uint64 Size,
			ERenderGraphUse Use, ERHIAccess Access,
			bool bDiscard = false) -> void;
		auto UseColorAttachment(FRenderGraphPassHandle Pass,
			FRenderGraphTextureHandle Texture,
			const FRHITextureSubresourceRange& Range,
			ERHIRenderTargetLoadAction LoadAction,
			ERHIRenderTargetStoreAction StoreAction) -> void;
		auto UseDepthStencilAttachment(FRenderGraphPassHandle Pass,
			FRenderGraphTextureHandle Texture,
			const FRHITextureSubresourceRange& Range,
			ERHIRenderTargetLoadAction LoadAction,
			ERHIRenderTargetStoreAction StoreAction) -> void;
		// Declares an attachment whose render-pass body performs its own RHI
		// entry/final layout transitions and publishes ResultAccess on exit.
		auto UseManagedColorAttachment(FRenderGraphPassHandle Pass,
			FRenderGraphTextureHandle Texture,
			const FRHITextureSubresourceRange& Range,
			ERHIRenderTargetLoadAction LoadAction,
			ERHIRenderTargetStoreAction StoreAction,
			ERHIAccess ResultAccess) -> void;
		auto UseManagedDepthStencilAttachment(FRenderGraphPassHandle Pass,
			FRenderGraphTextureHandle Texture,
			const FRHITextureSubresourceRange& Range,
			ERHIRenderTargetLoadAction LoadAction,
			ERHIRenderTargetStoreAction StoreAction,
			ERHIAccess ResultAccess) -> void;
		auto UseManagedTexture(FRenderGraphPassHandle Pass,
			FRenderGraphTextureHandle Texture,
			const FRHITextureSubresourceRange& Range, ERenderGraphUse Use,
			ERHIAccess EntryAccess, ERHIAccess ResultAccess,
			bool bDiscard = false) -> void;
		auto UseToken(FRenderGraphPassHandle Pass, FRenderGraphTokenHandle Token,
			ERenderGraphUse Use) -> void;
		template<typename T>
		auto UseValue(FRenderGraphPassHandle Pass,
			TRenderGraphValueHandle<T> Value, ERenderGraphUse Use) -> void
		{
			UseValueErased(Pass, Value.Owner, Value.Index,
				&RenderGraphPrivate::GValueTypeIdentity<std::remove_cv_t<T>>, Use);
		}

		auto Compile() const -> FRenderGraphCompileResult;

	private:
		auto AddParameterizedPass(std::string_view Name,
			ERenderGraphPassType Type,
			const FRenderGraphParametersMetadata* Metadata, void* Parameters,
			std::shared_ptr<void> Lifetime, FRenderGraphExecute Execute,
			FRenderGraphParameterizedExecute ParameterizedExecute)
			-> FRenderGraphPassHandle;
		auto CanDeclareManualUse(FRenderGraphPassHandle Pass,
			std::string_view InvalidHandleError) -> bool;
		auto AllocateParameterStorage(size_t Size, size_t Alignment,
			const FRenderGraphParametersMetadata* Metadata,
			void (*Destroy)(void*), std::weak_ptr<void>& OutLifetime) -> void*;
		auto MarkParameterStorageConstructed(void* Storage) -> void;
		auto AllocateValueStorage(std::string_view Name,
			std::string_view StableTypeName, const void* TypeIdentity, size_t Size,
			size_t Alignment, void (*Destroy)(void*), uint32& OutIndex) -> void*;
		auto MarkValueStorageConstructed(uint32 ResourceIndex) -> void;
		auto UseValueErased(FRenderGraphPassHandle Pass, uint64 Owner,
			uint32 Index, const void* TypeIdentity, ERenderGraphUse Use) -> void;
		auto StateOwner() const -> uint64;
		struct FState;
		std::unique_ptr<FState> State;
	};
} // namespace Durin
