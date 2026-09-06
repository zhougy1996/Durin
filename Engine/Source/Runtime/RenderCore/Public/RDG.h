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

	// Carries a graph-local texture handle and its exact runtime subresource range.
	struct FRDGTextureParameter final
	{
		FRDGTextureHandle Texture;
		FRHITextureSubresourceRange Range{};
	};

	// Carries a graph-local buffer handle and its exact runtime byte range.
	struct FRDGBufferParameter final
	{
		FRDGBufferHandle Buffer;
		uint64 Offset = 0;
		uint64 Size = 0;
	};

	// Carries one graph-local logical scheduling value.
	struct FRDGTokenParameter final
	{
		FRDGTokenHandle Token;
	};

	// Declares const read access to one graph-owned typed value.
	template<typename T>
	struct TRDGValueRead final
	{
		TRDGValueHandle<T> Value;
	};

	// Declares mutable write access to one graph-owned typed value.
	template<typename T>
	struct TRDGValueWrite final
	{
		TRDGValueHandle<T> Value;
	};

	// Carries a graph-local color attachment and its exact runtime range.
	struct FRDGColorAttachmentParameter final
	{
		FRDGTextureHandle Texture;
		FRHITextureSubresourceRange Range{};
	};

	// Carries a graph-local depth/stencil attachment and its runtime range.
	struct FRDGDepthStencilAttachmentParameter final
	{
		FRDGTextureHandle Texture;
		FRHITextureSubresourceRange Range{};
	};

	// Carries a graph-local texture whose entry/exit transitions are pass-managed.
	struct FRDGManagedTextureParameter final
	{
		FRDGTextureHandle Texture;
		FRHITextureSubresourceRange Range{};
	};

	struct FRDGParametersMetadata;

	// Describes one parameter field in stable declaration order.
	struct FRDGParameterMemberMetadata final
	{
		const char* Name = nullptr;
		uint32 Offset = 0;
		uint32 ElementSize = 0;
		uint32 ArraySize = 1;
		bool bOptional = false;
		ERDGParameterMemberKind Kind =
			ERDGParameterMemberKind::Texture;
		ERDGResourceKind ResourceKind = ERDGResourceKind::Texture;
		ERDGParameterRangeKind RangeKind =
			ERDGParameterRangeKind::None;
		ERDGUse Use = ERDGUse::Read;
		ERHIAccess Access = ERHIAccess::None;
		bool bDiscard = false;
		ERHIRenderTargetLoadAction LoadAction =
			ERHIRenderTargetLoadAction::Load;
		ERHIRenderTargetStoreAction StoreAction =
			ERHIRenderTargetStoreAction::Store;
		bool bPassManagedTransition = false;
		ERHIAccess ResultAccess = ERHIAccess::None;
		const FRDGParametersMetadata* NestedParameters = nullptr;
		const void* ValueTypeIdentity = nullptr;
		bool (*ReadValueHandle)(const void*, uint64&, uint32&) = nullptr;
		const void* (*ReadOptionalValueAddress)(const void*) = nullptr;
		// When enabled, this exact graph member also supplies one reflected
		// shader resource binding. Reflection still owns descriptor coordinates.
		bool bShaderBinding = false;
		const char* ShaderBindingName = nullptr;
		ERHIBindingType ShaderBindingType = ERHIBindingType::Texture;
	};

	// Describes a complete graph-parameter structure without owning its members.
	struct FRDGParametersMetadata final
	{
		const char* StructName = nullptr;
		uint32 StructSize = 0;
		uint32 StructAlignment = 0;
		std::span<const FRDGParameterMemberMetadata> Members;
	};

	// Describes one non-nested member occurrence in a validated parameter type.
	struct FRDGParameterLayoutLeaf final
	{
		const FRDGParameterMemberMetadata* Metadata = nullptr;
		uint32 Offset = 0;
		uint32 FirstElementIndex = 0;
		std::string Path;
	};

	// Describes one wrapper instance in deterministic metadata order.
	struct FRDGParameterLayoutElement final
	{
		uint32 Offset = 0;
		uint32 LeafIndex = 0;
		uint32 ArrayElementIndex = 0;
		std::string FieldPath;
	};

	// Maps one reflected shader name to its validated graph leaf group.
	struct FRDGParameterShaderBinding final
	{
		std::string Name;
		uint32 LeafIndex = 0;
	};

	// Owns the immutable flattened interpretation shared by one typed parameter
	// declaration. Runtime values and graph-local handles remain outside it.
	struct FRDGParameterLayout final
	{
		const FRDGParametersMetadata* Metadata = nullptr;
		std::vector<FRDGParameterLayoutLeaf> Leaves;
		std::vector<FRDGParameterLayoutElement> Elements;
		std::vector<uint32> OffsetIndex;
		std::vector<uint32> TextureElements;
		std::vector<uint32> BufferElements;
		std::vector<uint32> ValueElements;
		std::vector<uint32> TokenElements;
		std::vector<uint32> AttachmentElements;
		std::vector<FRDGParameterShaderBinding> ShaderBindings;
	};

	// Caches either the immutable layout or its deterministic validation error.
	struct FRDGParameterLayoutBuildResult final
	{
		std::unique_ptr<const FRDGParameterLayout> Layout;
		std::string Error;
	};

	RENDERCORE_API auto BuildRDGParameterLayout(
		const FRDGParametersMetadata* Metadata,
		uint32 ExpectedSize, uint32 ExpectedAlignment)
		-> FRDGParameterLayoutBuildResult;

	template<typename ParameterStruct, size_t N>
	constexpr auto MakeInlineRDGParametersMetadata(
		std::string_view StructName,
		const std::array<FRDGParameterMemberMetadata, N>& Members)
		-> FRDGParametersMetadata
	{
		return FRDGParametersMetadata{
			.StructName = StructName.data(),
			.StructSize = static_cast<uint32>(sizeof(ParameterStruct)),
			.StructAlignment = static_cast<uint32>(alignof(ParameterStruct)),
			.Members = Members,
		};
	}

	template<typename ParameterStruct>
	concept CRDGParameters = requires
	{
		{ ParameterStruct::GetRDGParametersMetadata() }
			-> std::same_as<const FRDGParametersMetadata*>;
	};

	template<CRDGParameters ParameterStruct>
	auto GetRDGParameterLayoutBuildResult()
		-> const FRDGParameterLayoutBuildResult&
	{
		static const FRDGParameterLayoutBuildResult Result =
			BuildRDGParameterLayout(ParameterStruct::GetRDGParametersMetadata(),
				static_cast<uint32>(sizeof(ParameterStruct)),
				static_cast<uint32>(alignof(ParameterStruct)));
		return Result;
	}

	template<CRDGParameters ParameterStruct>
	auto GetRDGParameterLayout() -> const FRDGParameterLayout*
	{
		return GetRDGParameterLayoutBuildResult<ParameterStruct>().Layout.get();
	}

	template<typename MemberType>
	struct TRDGParameterMemberTraits
	{
		using ValueType = MemberType;
		static constexpr bool bOptional = false;
		static constexpr uint32 ArraySize = 1;
		static constexpr uint32 ElementSize = sizeof(MemberType);
	};

	template<typename Value>
	struct TRDGParameterMemberTraits<std::optional<Value>>
		: TRDGParameterMemberTraits<Value>
	{
		using ValueType = Value;
		static constexpr bool bOptional = true;
		static constexpr uint32 ElementSize = sizeof(std::optional<Value>);
	};

	template<typename Value, size_t Count>
	struct TRDGParameterMemberTraits<std::array<Value, Count>>
		: TRDGParameterMemberTraits<Value>
	{
		static_assert(Count > 0, "Render graph parameter arrays cannot be empty");
		using ValueType = typename TRDGParameterMemberTraits<Value>::ValueType;
		static constexpr uint32 ArraySize = static_cast<uint32>(Count);
		static constexpr uint32 ElementSize = sizeof(Value);
	};

	template<typename ParameterStruct, typename MemberType, typename ExpectedType>
	constexpr auto MakeRDGResourceParameterMemberMetadata(
		const char* Name, uint32 Offset, ERDGParameterMemberKind Kind,
		ERDGResourceKind ResourceKind,
		ERDGParameterRangeKind RangeKind, ERDGUse Use,
		ERHIAccess Access, bool bDiscard = false,
		ERHIRenderTargetLoadAction LoadAction =
			ERHIRenderTargetLoadAction::Load,
		ERHIRenderTargetStoreAction StoreAction =
			ERHIRenderTargetStoreAction::Store,
		bool bPassManagedTransition = false,
		ERHIAccess ResultAccess = ERHIAccess::None)
		-> FRDGParameterMemberMetadata
	{
		using FTraits = TRDGParameterMemberTraits<MemberType>;
		constexpr bool bExpectedWrapper =
			std::same_as<ExpectedType, FRDGTextureParameter>
			|| std::same_as<ExpectedType, FRDGBufferParameter>
			|| std::same_as<ExpectedType, FRDGTokenParameter>
			|| std::same_as<ExpectedType, FRDGColorAttachmentParameter>
			|| std::same_as<ExpectedType,
				FRDGDepthStencilAttachmentParameter>
			|| std::same_as<ExpectedType, FRDGManagedTextureParameter>;
		static_assert(bExpectedWrapper,
			"Render graph resource metadata requires a typed graph wrapper");
		static_assert(std::same_as<typename FTraits::ValueType, ExpectedType>,
			"Render graph parameter member type does not match its declaration");
		static_assert(std::is_standard_layout_v<ParameterStruct>,
			"Render graph parameter structs must use standard layout");
		auto Metadata = FRDGParameterMemberMetadata{
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
		if constexpr (FTraits::bOptional)
			Metadata.ReadOptionalValueAddress = [](const void* Element) {
				const auto& Optional = *static_cast<const std::optional<
					ExpectedType>*>(Element);
				return Optional ? static_cast<const void*>(&*Optional) : nullptr;
			};
		return Metadata;
	}

	// Composes a shader SRV/UAV role onto the exact graph resource declaration.
	template<typename ParameterStruct, typename MemberType, typename ExpectedType>
	constexpr auto MakeRDGShaderResourceParameterMemberMetadata(
		const char* Name, uint32 Offset, ERDGParameterMemberKind Kind,
		ERDGResourceKind ResourceKind,
		ERDGParameterRangeKind RangeKind, ERDGUse Use,
		ERHIAccess Access, ERHIBindingType BindingType,
		const char* ShaderBindingName = nullptr, bool bDiscard = false)
		-> FRDGParameterMemberMetadata
	{
		auto Metadata = MakeRDGResourceParameterMemberMetadata<
			ParameterStruct, MemberType, ExpectedType>(Name, Offset, Kind,
			ResourceKind, RangeKind, Use, Access, bDiscard);
		Metadata.bShaderBinding = true;
		Metadata.ShaderBindingName = ShaderBindingName != nullptr
			? ShaderBindingName : Name;
		Metadata.ShaderBindingType = BindingType;
		return Metadata;
	}

	template<typename ParameterStruct, typename MemberType>
	constexpr auto MakeRDGNestedParameterMemberMetadata(
		const char* Name, uint32 Offset,
		const FRDGParametersMetadata* NestedParameters)
		-> FRDGParameterMemberMetadata
	{
		using FTraits = TRDGParameterMemberTraits<MemberType>;
		static_assert(!FTraits::bOptional,
			"Only graph resource wrappers can be optional");
		static_assert(CRDGParameters<typename FTraits::ValueType>,
			"Nested graph parameter members must have registered metadata");
		static_assert(std::is_standard_layout_v<ParameterStruct>,
			"Render graph parameter structs must use standard layout");
		return {
			.Name = Name,
			.Offset = Offset,
			.ElementSize = FTraits::ElementSize,
			.ArraySize = FTraits::ArraySize,
			.Kind = ERDGParameterMemberKind::Nested,
			.NestedParameters = NestedParameters,
		};
	}

	template<typename ParameterStruct, typename MemberType, typename T>
	constexpr auto MakeRDGValueParameterMemberMetadata(
		const char* Name, uint32 Offset)
		-> FRDGParameterMemberMetadata
	{
		using FTraits = TRDGParameterMemberTraits<MemberType>;
		constexpr bool bRead = std::same_as<typename FTraits::ValueType,
			TRDGValueRead<T>>;
		constexpr bool bWrite = std::same_as<typename FTraits::ValueType,
			TRDGValueWrite<T>>;
		static_assert(bRead || bWrite,
			"Render graph value member type does not match its declaration");
		static_assert(std::is_standard_layout_v<ParameterStruct>,
			"Render graph parameter structs must use standard layout");
		auto Metadata = FRDGParameterMemberMetadata{
			.Name = Name,
			.Offset = Offset,
			.ElementSize = FTraits::ElementSize,
			.ArraySize = FTraits::ArraySize,
			.bOptional = FTraits::bOptional,
			.Kind = bRead
				? ERDGParameterMemberKind::ValueRead
				: ERDGParameterMemberKind::ValueWrite,
			.ResourceKind = ERDGResourceKind::Token,
			.Use = bRead ? ERDGUse::Read : ERDGUse::Write,
			.bDiscard = bWrite,
			.ValueTypeIdentity =
				&RDGPrivate::GValueTypeIdentity<std::remove_cv_t<T>>,
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
		if constexpr (FTraits::bOptional)
			Metadata.ReadOptionalValueAddress = [](const void* Element) {
				const auto& Optional = *static_cast<const std::optional<
					typename FTraits::ValueType>*>(Element);
				return Optional ? static_cast<const void*>(&*Optional) : nullptr;
			};
		return Metadata;
	}

	// A move-only mutable capability for one builder-owned parameter allocation.
	template<typename ParameterStruct>
	class TRDGParametersRef final
	{
	public:
		TRDGParametersRef() = default;
		TRDGParametersRef(TRDGParametersRef&& Other) noexcept
			: Data(std::exchange(Other.Data, nullptr)),
			  Lifetime(std::move(Other.Lifetime)),
			  Layout(std::exchange(Other.Layout, nullptr))
		{
		}
		auto operator=(TRDGParametersRef&& Other) noexcept
			-> TRDGParametersRef&
		{
			if (this != &Other)
			{
				Data = std::exchange(Other.Data, nullptr);
				Lifetime = std::move(Other.Lifetime);
				Layout = std::exchange(Other.Layout, nullptr);
			}
			return *this;
		}

		TRDGParametersRef(const TRDGParametersRef&) = delete;
		auto operator=(const TRDGParametersRef&)
			-> TRDGParametersRef& = delete;

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
		friend class FRDGBuilder;
		TRDGParametersRef(ParameterStruct* InData,
			std::weak_ptr<void> InLifetime, const FRDGParameterLayout* InLayout)
			: Data(InData), Lifetime(std::move(InLifetime)), Layout(InLayout)
		{
		}

		ParameterStruct* Data = nullptr;
		std::weak_ptr<void> Lifetime;
		const FRDGParameterLayout* Layout = nullptr;
	};

	// Exposes only resources declared by the executing graph to pass callbacks.
	class FRDGPassResources final
	{
	public:
		FRDGPassResources(const FRDGPassResources&) = delete;
		auto operator=(const FRDGPassResources&) -> FRDGPassResources& = delete;
		RENDERCORE_API auto GetTexture(FRDGTextureHandle Handle) const -> FRHITexture*;
		RENDERCORE_API auto GetBuffer(FRDGBufferHandle Handle) const -> FRHIBuffer*;
		template<typename T>
		auto ReadValue(TRDGValueHandle<T> Handle) const -> const T&
		{
			return *static_cast<const T*>(ResolveValue(Handle.Owner, Handle.Index,
				&RDGPrivate::GValueTypeIdentity<std::remove_cv_t<T>>, false));
		}
		template<typename T>
		auto WriteValue(TRDGValueHandle<T> Handle) const -> T&
		{
			return *static_cast<T*>(ResolveValue(Handle.Owner, Handle.Index,
				&RDGPrivate::GValueTypeIdentity<std::remove_cv_t<T>>, true));
		}

	private:
		friend class FRDGBuilder;
		explicit FRDGPassResources(const FRDGBuilder& InGraph,
			uint32 InPassIndex)
			: Graph(InGraph), PassIndex(InPassIndex)
		{
		}
		RENDERCORE_API auto ResolveValue(uint64 Owner, uint32 Index, const void* TypeIdentity,
			bool bWrite) const -> void*;

		const FRDGBuilder& Graph;
		uint32 PassIndex = 0;
	};

	// Carries the physical texture and immutable declaration details for one
	// color or depth/stencil attachment parameter.
	struct FRDGAttachmentView final
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
	class RENDERCORE_API FRDGParameterResolver final
	{
	public:
		FRDGParameterResolver(const FRDGParameterResolver&) = delete;
		auto operator=(const FRDGParameterResolver&)
			-> FRDGParameterResolver& = delete;
		FRDGParameterResolver(FRDGParameterResolver&&) = delete;
		auto operator=(FRDGParameterResolver&&)
			-> FRDGParameterResolver& = delete;

		auto GetTexture(const FRDGTextureParameter& Parameter) const
			-> FRHITexture*;
		auto GetTexture(
			const std::optional<FRDGTextureParameter>& Parameter) const
			-> FRHITexture*;
		auto GetTexture(const FRDGManagedTextureParameter& Parameter) const
			-> FRHITexture*;
		auto GetTexture(
			const std::optional<FRDGManagedTextureParameter>& Parameter) const
			-> FRHITexture*;
		auto GetBuffer(const FRDGBufferParameter& Parameter) const
			-> FRHIBuffer*;
		auto GetBuffer(
			const std::optional<FRDGBufferParameter>& Parameter) const
			-> FRHIBuffer*;
		auto GetColorAttachment(
			const FRDGColorAttachmentParameter& Parameter) const
			-> FRDGAttachmentView;
		auto GetColorAttachment(const std::optional<
			FRDGColorAttachmentParameter>& Parameter) const
			-> FRDGAttachmentView;
		auto GetDepthStencilAttachment(
			const FRDGDepthStencilAttachmentParameter& Parameter) const
			-> FRDGAttachmentView;
		auto GetDepthStencilAttachment(const std::optional<
			FRDGDepthStencilAttachmentParameter>& Parameter) const
			-> FRDGAttachmentView;
		auto GetPassName() const -> std::string_view { return PassName; }
		auto GetPassType() const -> ERDGPassType { return PassType; }
		template<CRDGParameters ParameterStruct>
		auto GetShaderParameters(const ParameterStruct& InParameters) const
			-> FRDGShaderParameterScope;
		template<typename T>
		auto ReadValue(const TRDGValueRead<T>& Parameter) const -> const T&
		{
			FindMember(&Parameter, ERDGParameterMemberKind::ValueRead,
				ERDGParameterMemberKind::ValueRead, false);
			return Resources.ReadValue(Parameter.Value);
		}
		template<typename T>
		auto WriteValue(const TRDGValueWrite<T>& Parameter) const -> T&
		{
			FindMember(&Parameter, ERDGParameterMemberKind::ValueWrite,
				ERDGParameterMemberKind::ValueWrite, false);
			return Resources.WriteValue(Parameter.Value);
		}
		template<typename T>
		auto ReadValue(const std::optional<TRDGValueRead<T>>& Parameter) const
			-> const T*
		{
			FindMember(&Parameter, ERDGParameterMemberKind::ValueRead,
				ERDGParameterMemberKind::ValueRead, true);
			return Parameter ? &Resources.ReadValue(Parameter->Value) : nullptr;
		}
		template<typename T>
		auto WriteValue(
			const std::optional<TRDGValueWrite<T>>& Parameter) const -> T*
		{
			FindMember(&Parameter, ERDGParameterMemberKind::ValueWrite,
				ERDGParameterMemberKind::ValueWrite, true);
			return Parameter ? &Resources.WriteValue(Parameter->Value) : nullptr;
		}

	private:
		friend class FRDGBuilder;
		friend class FRDGShaderParameterScope;
		explicit FRDGParameterResolver(
			const FRDGPassResources& InResources,
			const FRDGParameterLayout* InLayout,
			std::span<const std::pair<uint32, uint32>> InOptionalAliases,
			const void* InParameters, std::string_view InPassName,
			ERDGPassType InPassType)
			: Resources(InResources), Layout(InLayout),
			  OptionalAliases(InOptionalAliases), Parameters(InParameters),
			  PassName(InPassName), PassType(InPassType)
		{
		}
		auto ValidateShaderParametersIdentity(const void* Data,
			const FRDGParametersMetadata* InMetadata) const -> void;
		auto FindMember(const void* Address,
			ERDGParameterMemberKind ExpectedKind,
			ERDGParameterMemberKind AlternateKind,
			bool bOptional) const -> const FRDGParameterMemberMetadata&;

		const FRDGPassResources& Resources;
		const FRDGParameterLayout* Layout = nullptr;
		std::span<const std::pair<uint32, uint32>> OptionalAliases;
		const void* Parameters = nullptr;
		std::string_view PassName;
		ERDGPassType PassType = ERDGPassType::Graphics;
	};

	// A non-copyable callback-lifetime view of the exact immutable pass object.
	// It is the only object accepted by composed shader submission.
	class RENDERCORE_API FRDGShaderParameterScope final
	{
	public:
		FRDGShaderParameterScope(const FRDGShaderParameterScope&) = delete;
		auto operator=(const FRDGShaderParameterScope&)
			-> FRDGShaderParameterScope& = delete;
		FRDGShaderParameterScope(FRDGShaderParameterScope&&) = delete;
		auto operator=(FRDGShaderParameterScope&&)
			-> FRDGShaderParameterScope& = delete;

		auto GetResolver() const -> const FRDGParameterResolver&
		{
			return Resolver;
		}
		auto GetData() const -> const void* { return Data; }
		auto GetMetadata() const -> const FRDGParametersMetadata*
		{
			return Layout != nullptr ? Layout->Metadata : nullptr;
		}
		auto GetLayout() const -> const FRDGParameterLayout* { return Layout; }

	private:
		friend class FRDGParameterResolver;
		FRDGShaderParameterScope(const FRDGParameterResolver& InResolver,
			const void* InData, const FRDGParameterLayout* InLayout)
			: Resolver(InResolver), Data(InData), Layout(InLayout)
		{
		}

		const FRDGParameterResolver& Resolver;
		const void* Data = nullptr;
		const FRDGParameterLayout* Layout = nullptr;
	};

	template<CRDGParameters ParameterStruct>
	auto FRDGParameterResolver::GetShaderParameters(
		const ParameterStruct& InParameters) const
		-> FRDGShaderParameterScope
	{
		const auto* InMetadata = ParameterStruct::GetRDGParametersMetadata();
		ValidateShaderParametersIdentity(&InParameters, InMetadata);
		return FRDGShaderParameterScope(*this, &InParameters, Layout);
	}

	using FRDGPassExecute = std::function<void(
		FRHICommandListImmediate&, const FRDGPassResources&)>;
	using FRDGParameterizedPassExecute = std::function<void(
		FRHICommandListImmediate&, const FRDGParameterResolver&)>;
	// Describes one retained graph-created resource for execution allocation.
	// Diagnostic names are deliberately absent from allocation identity.
	struct FRDGAllocationRequest final
	{
		uint32 ResourceId = 0;
		ERDGResourceKind Kind = ERDGResourceKind::Texture;
		FRHITextureDesc TextureDesc;
		FRHIBufferDesc BufferDesc;
		uint32 FirstPass = 0;
		uint32 LastPass = 0;
		uint32 ObservationTag = 0;
		// Exported allocations must leave the reusable pool before Allocate returns.
		// Their counted references own them thereafter; no implicit pool return.
		bool bExtracted = false;
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
		friend class FRDGBuilder;
		explicit FRDGAllocatedResources(uint32 Count);
		std::vector<FTextureRHIRef> Textures;
		std::vector<FBufferRHIRef> Buffers;
		std::vector<uint64> AllocationIds;
		std::vector<std::string> AllocationDispositions;
		FRDGAllocationStatistics Statistics;
	};

	// Allocates one retained batch atomically. Implementations must not publish a
	// partial result when returning failure. bExtracted resources transfer out of
	// reusable ownership even if subsequent recording fails. Other allocations
	// are borrowed for one ordered Execute; retaining a reference is not an export.
	// Reuse requires ordered GPU uses on the same RHI timeline, or explicit GPU
	// completion/synchronization across timelines. CPU return is not GPU completion.
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

	// Records one immutable dependency edge in compiler diagnostics.
	struct FRDGDependency final
	{
		uint32 BeforePass = 0;
		uint32 AfterPass = 0;
		std::string Cause;
		ERDGDependencyKind Kind = ERDGDependencyKind::Execution;

		auto operator==(const FRDGDependency&) const -> bool = default;
	};

	// Records one pointer-free declared resource and its preparation outcome.
	struct FRDGResourceCapture final
	{
		uint32 ResourceId = 0;
		std::string Name;
		ERDGResourceKind Kind = ERDGResourceKind::Texture;
		bool bExternal = false;
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
	struct FRDGUseCapture final
	{
		uint32 PassDeclarationIndex = 0;
		uint32 ResourceId = 0;
		ERDGUse Use = ERDGUse::Read;
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
	struct FRDGParameterCapture final
	{
		uint32 PassDeclarationIndex = 0;
		std::string FieldPath;
		ERDGParameterMemberKind Kind =
			ERDGParameterMemberKind::Texture;
		ERDGResourceKind ResourceKind =
			ERDGResourceKind::Texture;
		bool bPresent = false;
		// Absent optional fields use max uint32 and never name a synthetic resource.
		uint32 ResourceId = std::numeric_limits<uint32>::max();
		ERDGUse Use = ERDGUse::Read;
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
	struct FRDGTransitionCapture final
	{
		uint32 ResourceId = 0;
		uint32 PassIndex = std::numeric_limits<uint32>::max();
		ERHIAccess Before = ERHIAccess::None;
		ERHIAccess After = ERHIAccess::None;
		FRHITextureSubresourceRange TextureRange{};
		uint64 BufferOffset = 0;
		uint64 BufferSize = 0;
		bool bFinal = false;
		bool bDiscardContents = false;
	};

	// Reports the retained scheduled interval of one declared resource.
	struct FRDGResourceLifetime final
	{
		std::string Name;
		uint32 FirstPass = 0;
		uint32 LastPass = 0;
		bool bExternal = false;
		bool bCulled = false;
	};

	// Explains whether one declared pass survived explicit-root reachability.
	struct FRDGCullingDecision final
	{
		std::string Name;
		bool bCulled = false;
		std::string Reason;
	};

	// Separates catastrophic graph-shape safety limits from observational budgets.
	struct FRDGBudget final
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
	struct FRDGStatistics final
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
	struct FRDGPassCapture final
	{
		std::string Name;
		ERDGPassType Type = ERDGPassType::Graphics;
		uint32 DeclarationIndex = 0;
		std::string ParameterStructName;
		uint32 BufferTransitions = 0;
		uint32 TextureTransitions = 0;
	};

	// Owns an immutable diagnostic snapshot independent of graph/RHI lifetimes.
	struct FRDGCapture final
	{
		// False before execution or after compilation failure; compiled arrays are empty.
		bool bCompiled = false;
		FRDGBudget Budget;
		FRDGStatistics Statistics;
		FRDGAllocationStatistics AllocationStatistics;
		std::vector<FRDGPassCapture> Passes;
		std::vector<FRDGResourceCapture> Resources;
		std::vector<FRDGParameterCapture> Parameters;
		std::vector<FRDGUseCapture> Uses;
		std::vector<FRDGTransitionCapture> Transitions;
		std::vector<FRDGDependency> Dependencies;
		std::vector<FRDGResourceLifetime> ResourceLifetimes;
		std::vector<FRDGCullingDecision> CullingDecisions;
		std::string Dump;
	};

	// Owns the compiled pass order and transition batches for one graph.
	struct FRDGCompiledPass final
	{
		std::string Name;
		ERDGPassType Type = ERDGPassType::Graphics;
		uint32 DeclarationIndex = 0;
		std::string ParameterStructName;
		std::vector<FRHIBufferTransition> BufferTransitions;
		std::vector<FRHITextureTransition> TextureTransitions;
	};

	// CPU recording outcome; Recorded does not imply GPU completion.
	enum class ERDGExecutionStatus : uint8
	{
		CompileFailed, PreparationFailed, Recorded, InvalidState
	};

	// Thread-confined, single-use graph lifecycle; failure is terminal.
	enum class ERDGBuilderState : uint8
	{
		Building, Compiling, Preparing, Recording, Recorded, Failed
	};

	// Owns the recoverable result of one execution attempt.
	struct FRDGExecutionResult final
	{
		ERDGExecutionStatus Status = ERDGExecutionStatus::InvalidState;
		std::string Error;
		auto IsSuccess() const -> bool { return Status == ERDGExecutionStatus::Recorded; }
	};

	// Owns declarations, storage and private compilation records for one graph execution.
	// Thread-confined; all declaration methods require Building in every configuration.
	class FRDGBuilder final
	{
	public:
		RENDERCORE_API FRDGBuilder();
		RENDERCORE_API ~FRDGBuilder();

		FRDGBuilder(const FRDGBuilder&) = delete;
		auto operator=(const FRDGBuilder&)
			-> FRDGBuilder& = delete;
		FRDGBuilder(FRDGBuilder&&) = delete;
		auto operator=(FRDGBuilder&&)
			-> FRDGBuilder& = delete;

		RENDERCORE_API auto RegisterExternalTexture(const FTextureRHIRef& Texture,
			std::string_view Name, ERHIAccess InitialAccess,
			ERHIAccess FinalAccess) -> FRDGTextureHandle;
		RENDERCORE_API auto CreateTexture(const FRDGTextureDesc& Desc,
			std::string_view Name,
			ERHIAccess FinalAccess = ERHIAccess::None)
			-> FRDGTextureHandle;
		RENDERCORE_API auto RegisterExternalBuffer(const FBufferRHIRef& Buffer,
			std::string_view Name, ERHIAccess InitialAccess,
			ERHIAccess FinalAccess) -> FRDGBufferHandle;
		RENDERCORE_API auto CreateBuffer(const FRDGBufferDesc& Desc,
			std::string_view Name,
			ERHIAccess FinalAccess = ERHIAccess::None)
			-> FRDGBufferHandle;
		RENDERCORE_API auto CreateToken(std::string_view Name) -> FRDGTokenHandle;
		// Exports the complete resource through a terminal consumer. Every subresource
		// must have valid stored contents; Destination is published only after success.
		RENDERCORE_API auto QueueTextureExtraction(FRDGTextureHandle Texture,
			FTextureRHIRef* Destination, ERHIAccess FinalAccess) -> void;
		// Requires valid contents across the entire buffer and publishes only after success.
		RENDERCORE_API auto QueueBufferExtraction(FRDGBufferHandle Buffer,
			FBufferRHIRef* Destination, ERHIAccess FinalAccess) -> void;
		template<typename T, typename... Args>
		requires std::constructible_from<T, Args...> && std::destructible<T>
		auto CreateValue(std::string_view Name, std::string_view StableTypeName,
			Args&&... ConstructorArgs) -> TRDGValueHandle<T>
		{
			RequireBuilding();
			static_assert(std::is_object_v<T> && !std::is_const_v<T>
				&& !std::is_volatile_v<T>,
				"Render graph values require an unqualified object type");
			uint32 Index = 0;
			void* Storage = AllocateValueStorage(Name, StableTypeName,
				&RDGPrivate::GValueTypeIdentity<T>, sizeof(T), alignof(T),
				[](void* Value) { std::destroy_at(static_cast<T*>(Value)); }, Index);
			if (Storage == nullptr) return {};
			FStorageConstructionScope Construction(*this);
			std::construct_at(static_cast<T*>(Storage),
				std::forward<Args>(ConstructorArgs)...);
			MarkValueStorageConstructed(Index);
			return {StateOwner(), Index};
		}

		RENDERCORE_API auto AddPass(std::string_view Name, ERDGPassType Type,
			FRDGPassExecute Execute = {}) -> FRDGPassHandle;
		template<typename ParameterStruct>
		requires CRDGParameters<ParameterStruct>
		auto AddPass(std::string_view Name, ERDGPassType Type,
			TRDGParametersRef<ParameterStruct>&& Parameters,
			FRDGPassExecute Execute = {}) -> FRDGPassHandle
		{
			RequireBuilding();
			auto Lifetime = Parameters.Lifetime.lock();
			void* Data = std::exchange(Parameters.Data, nullptr);
			const FRDGParameterLayout* Layout =
				std::exchange(Parameters.Layout, nullptr);
			if (Layout == nullptr)
				Layout = GetRDGParameterLayout<ParameterStruct>();
			Parameters.Lifetime.reset();
			return AddParameterizedPass(Name, Type,
				Layout, Data,
				std::move(Lifetime), std::move(Execute), {});
		}
		template<typename ParameterStruct, typename Execute>
		requires CRDGParameters<ParameterStruct>
			&& std::invocable<Execute&, FRHICommandListImmediate&,
				const ParameterStruct&, const FRDGParameterResolver&>
		auto AddPass(std::string_view Name, ERDGPassType Type,
			TRDGParametersRef<ParameterStruct>&& Parameters,
			Execute&& ExecuteCallback) -> FRDGPassHandle
		{
			RequireBuilding();
			ParameterStruct* TypedData = Parameters.Data;
			FRDGParameterizedPassExecute ErasedExecute =
				[TypedData, Callback = std::forward<Execute>(ExecuteCallback)](
					FRHICommandListImmediate& CommandList,
					const FRDGParameterResolver& Resolver) mutable {
					std::invoke(Callback, CommandList,
						static_cast<const ParameterStruct&>(*TypedData), Resolver);
				};
			RequireBuilding();
			auto Lifetime = Parameters.Lifetime.lock();
			void* Data = std::exchange(Parameters.Data, nullptr);
			const FRDGParameterLayout* Layout =
				std::exchange(Parameters.Layout, nullptr);
			if (Layout == nullptr)
				Layout = GetRDGParameterLayout<ParameterStruct>();
			Parameters.Lifetime.reset();
			return AddParameterizedPass(Name, Type,
				Layout, Data,
				std::move(Lifetime), {}, std::move(ErasedExecute));
		}
		RENDERCORE_API auto AddDependency(FRDGPassHandle Pass,
			FRDGPassHandle Prerequisite) -> void;
		RENDERCORE_API auto MarkPassRoot(FRDGPassHandle Pass,
			std::string_view Reason = "side-effect") -> void;
		RENDERCORE_API auto EnablePassCulling() -> void;
		RENDERCORE_API auto SetBudget(const FRDGBudget& Budget) -> void;

		template<typename ParameterStruct>
		requires CRDGParameters<ParameterStruct>
		auto AllocParameters() -> TRDGParametersRef<ParameterStruct>
		{
			RequireBuilding();
			static_assert(std::is_standard_layout_v<ParameterStruct>,
				"Render graph parameter structs must use standard layout");
			static_assert(std::default_initializable<ParameterStruct>,
				"Render graph parameter structs must be default constructible");
			static_assert(std::destructible<ParameterStruct>,
				"Render graph parameter structs must be destructible");
			std::weak_ptr<void> Lifetime;
			const auto& LayoutResult =
				GetRDGParameterLayoutBuildResult<ParameterStruct>();
			void* Storage = AllocateParameterStorage(sizeof(ParameterStruct),
				alignof(ParameterStruct),
				ParameterStruct::GetRDGParametersMetadata(),
				LayoutResult,
				[](void* Value) { std::destroy_at(
					static_cast<ParameterStruct*>(Value)); }, Lifetime);
			if (Storage == nullptr) return {};
			FStorageConstructionScope Construction(*this);
			auto* Parameters = std::construct_at(
				static_cast<ParameterStruct*>(Storage));
			MarkParameterStorageConstructed(Storage);
			return {Parameters, std::move(Lifetime), LayoutResult.Layout.get()};
		}

		RENDERCORE_API auto UseTexture(FRDGPassHandle Pass,
			FRDGTextureHandle Texture,
			const FRHITextureSubresourceRange& Range, ERDGUse Use,
			ERHIAccess Access, bool bDiscard = false) -> void;
		RENDERCORE_API auto UseBuffer(FRDGPassHandle Pass,
			FRDGBufferHandle Buffer, uint64 Offset, uint64 Size,
			ERDGUse Use, ERHIAccess Access,
			bool bDiscard = false) -> void;
		RENDERCORE_API auto UseColorAttachment(FRDGPassHandle Pass,
			FRDGTextureHandle Texture,
			const FRHITextureSubresourceRange& Range,
			ERHIRenderTargetLoadAction LoadAction,
			ERHIRenderTargetStoreAction StoreAction) -> void;
		RENDERCORE_API auto UseDepthStencilAttachment(FRDGPassHandle Pass,
			FRDGTextureHandle Texture,
			const FRHITextureSubresourceRange& Range,
			ERHIRenderTargetLoadAction LoadAction,
			ERHIRenderTargetStoreAction StoreAction) -> void;
		// Declares an attachment whose render-pass body performs its own RHI
		// entry/final layout transitions and publishes ResultAccess on exit.
		RENDERCORE_API auto UseManagedColorAttachment(FRDGPassHandle Pass,
			FRDGTextureHandle Texture,
			const FRHITextureSubresourceRange& Range,
			ERHIRenderTargetLoadAction LoadAction,
			ERHIRenderTargetStoreAction StoreAction,
			ERHIAccess ResultAccess) -> void;
		RENDERCORE_API auto UseManagedDepthStencilAttachment(FRDGPassHandle Pass,
			FRDGTextureHandle Texture,
			const FRHITextureSubresourceRange& Range,
			ERHIRenderTargetLoadAction LoadAction,
			ERHIRenderTargetStoreAction StoreAction,
			ERHIAccess ResultAccess) -> void;
		RENDERCORE_API auto UseManagedTexture(FRDGPassHandle Pass,
			FRDGTextureHandle Texture,
			const FRHITextureSubresourceRange& Range, ERDGUse Use,
			ERHIAccess EntryAccess, ERHIAccess ResultAccess,
			bool bDiscard = false) -> void;
		RENDERCORE_API auto UseToken(FRDGPassHandle Pass, FRDGTokenHandle Token,
			ERDGUse Use) -> void;
		template<typename T>
		auto UseValue(FRDGPassHandle Pass,
			TRDGValueHandle<T> Value, ERDGUse Use) -> void
		{
			UseValueErased(Pass, Value.Owner, Value.Index,
				&RDGPrivate::GValueTypeIdentity<std::remove_cv_t<T>>, Use);
		}

		// Consumes this builder even on failure. Retrying requires a newly authored graph.
		RENDERCORE_API auto Execute(FRHICommandListImmediate& CommandList,
			FRDGExecutionContext* Context = nullptr) -> FRDGExecutionResult;
		RENDERCORE_API auto GetState() const -> ERDGBuilderState;
		// Duplicate execution leaves this original report unchanged.
		RENDERCORE_API auto GetExecutionResult() const -> const FRDGExecutionResult&;
		RENDERCORE_API auto HasCompiledPlan() const -> bool;
		RENDERCORE_API auto GetPasses() const -> std::span<const FRDGCompiledPass>;
		RENDERCORE_API auto GetDependencies() const -> std::span<const FRDGDependency>;
		RENDERCORE_API auto GetResourceLifetimes() const
			-> std::span<const FRDGResourceLifetime>;
		RENDERCORE_API auto GetCullingDecisions() const
			-> std::span<const FRDGCullingDecision>;
		RENDERCORE_API auto GetFinalBufferTransitions() const
			-> std::span<const FRHIBufferTransition>;
		RENDERCORE_API auto GetFinalTextureTransitions() const
			-> std::span<const FRHITextureTransition>;
		RENDERCORE_API auto GetCompileMicroseconds() const -> uint64;
		RENDERCORE_API auto GetBudget() const -> const FRDGBudget&;
		RENDERCORE_API auto GetStatistics() const -> FRDGStatistics;
		// Owning pointer-free evidence survives builder destruction and preparation failure.
		RENDERCORE_API auto Capture() const -> FRDGCapture;
		RENDERCORE_API auto Dump() const -> std::string;

	private:
		friend class FRDGBuilderTestAccessor;
		friend class FRDGPassResources;
		RENDERCORE_API auto RequireBuilding() const -> void;
		auto Compile() -> std::string;
		RENDERCORE_API auto CompileForTesting() -> std::string;
		auto Record(FRHICommandListImmediate& CommandList,
			FRDGExecutionContext* Context, std::string* OutError) -> bool;
		struct FCompiledState;
		std::unique_ptr<FCompiledState> Compiled;

		RENDERCORE_API auto BeginStorageConstruction() -> void;
		RENDERCORE_API auto EndStorageConstruction() -> void;
		// User constructors may call back into the builder before storage is ready.
		struct FStorageConstructionScope final
		{
			explicit FStorageConstructionScope(FRDGBuilder& InBuilder) : Builder(InBuilder)
			{ Builder.BeginStorageConstruction(); }
			~FStorageConstructionScope() { Builder.EndStorageConstruction(); }
			FRDGBuilder& Builder;
		};

		RENDERCORE_API auto AddParameterizedPass(std::string_view Name,
			ERDGPassType Type,
			const FRDGParameterLayout* Layout, void* Parameters,
			std::shared_ptr<void> Lifetime, FRDGPassExecute Execute,
			FRDGParameterizedPassExecute ParameterizedExecute)
			-> FRDGPassHandle;
		RENDERCORE_API auto CanDeclareManualUse(FRDGPassHandle Pass,
			std::string_view InvalidHandleError) -> bool;
		RENDERCORE_API auto AllocateParameterStorage(size_t Size, size_t Alignment,
			const FRDGParametersMetadata* Metadata,
			const FRDGParameterLayoutBuildResult& LayoutResult,
			void (*Destroy)(void*), std::weak_ptr<void>& OutLifetime) -> void*;
		RENDERCORE_API auto MarkParameterStorageConstructed(void* Storage) -> void;
		RENDERCORE_API auto AllocateValueStorage(std::string_view Name,
			std::string_view StableTypeName, const void* TypeIdentity, size_t Size,
			size_t Alignment, void (*Destroy)(void*), uint32& OutIndex) -> void*;
		RENDERCORE_API auto MarkValueStorageConstructed(uint32 ResourceIndex) -> void;
		RENDERCORE_API auto UseValueErased(FRDGPassHandle Pass, uint64 Owner,
			uint32 Index, const void* TypeIdentity, ERDGUse Use) -> void;
		RENDERCORE_API auto StateOwner() const -> uint64;
		struct FState;
		std::unique_ptr<FState> State;
	};
} // namespace Durin
