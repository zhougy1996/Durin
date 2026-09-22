#pragma once

#include "RDG/RDGDefinitions.h"
#include <array>
#include <concepts>
#include <expected>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace Durin
{
	enum class ERDGMetadataError : uint8
	{
		MetadataNull,
		MetadataNameEmpty,
		MetadataLayoutMismatch,
		MetadataNestingLimit,
		MemberNameEmpty,
		MemberNameDuplicate,
		MemberLayoutEmpty,
		MemberOffsetInvalid,
		NestedMetadataInvalid,
		UnexpectedNestedMetadata,
		WrapperLayoutMismatch,
		OptionalLayoutMismatch,
		DeclarationSemanticsInvalid,
		ShaderBindingNameEmpty,
		ShaderBindingDuplicate,
		ShaderDeclarationIncompatible,
		ShaderBindingAuthorityMissing,
		NestedShaderBindingDuplicate,
		ParameterLayoutMismatch,
	};
	RENDERCORE_API auto ToString(ERDGMetadataError Error) -> std::string_view;

	// Diagnostic records own names and value descriptions; no graph or RHI pointers.
	struct FRDGMetadataErrorContext
	{
		std::string StructName, MemberName, OtherMemberName, BindingName;
		uint64 MemberIndex = 0, ExpectedSize = 0, ActualSize = 0;
		uint64 ExpectedAlignment = 0, ActualAlignment = 0, Offset = 0, ArraySize = 0, Depth = 0;
	};

	// Each reason enum belongs to one diagnostic domain.
	struct FRDGMetadataError
	{
		ERDGMetadataError Reason;
		FRDGMetadataErrorContext Context{};
	};
	RENDERCORE_API auto ToString(const FRDGMetadataError& Error) -> std::string;

	using FRDGMetadataResult = std::expected<void, FRDGMetadataError>;

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
	using FRDGParameterLayoutBuildResult =
		std::expected<std::unique_ptr<const FRDGParameterLayout>, FRDGMetadataError>;

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
		const auto& Result = GetRDGParameterLayoutBuildResult<ParameterStruct>();
		return Result ? Result->get() : nullptr;
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

	// Common texture roles fix wrapper, range, use, and access as one contract.
	template<typename ParameterStruct, typename MemberType, ERDGPassType Domain = ERDGPassType::Graphics>
	constexpr auto MakeRDGTextureReadMetadata(const char* Name, uint32 Offset)
		-> FRDGParameterMemberMetadata
	{
		static_assert(Domain == ERDGPassType::Graphics || Domain == ERDGPassType::Compute);
		return MakeRDGResourceParameterMemberMetadata<ParameterStruct, MemberType,
			FRDGTextureParameter>(Name, Offset, ERDGParameterMemberKind::Texture,
			ERDGResourceKind::Texture, ERDGParameterRangeKind::TextureSubresource,
			ERDGUse::Read, Domain == ERDGPassType::Graphics
				? ERHIAccess::GraphicsShaderRead : ERHIAccess::ComputeShaderRead);
	}

	template<typename ParameterStruct, typename MemberType>
	constexpr auto MakeRDGComputeTextureWriteMetadata(const char* Name, uint32 Offset)
		-> FRDGParameterMemberMetadata
	{
		return MakeRDGResourceParameterMemberMetadata<ParameterStruct, MemberType,
			FRDGTextureParameter>(Name, Offset, ERDGParameterMemberKind::Texture,
			ERDGResourceKind::Texture, ERDGParameterRangeKind::TextureSubresource,
			ERDGUse::Write, ERHIAccess::ComputeShaderReadWrite, true);
	}

	// The pass owns internal transitions; RDG tracks the declared entry and exit.
	template<typename ParameterStruct, typename MemberType>
	constexpr auto MakeRDGManagedTextureMetadata(const char* Name, uint32 Offset,
		ERHIAccess EntryAccess, bool bDiscard, ERHIAccess ResultAccess)
		-> FRDGParameterMemberMetadata
	{
		return MakeRDGResourceParameterMemberMetadata<ParameterStruct, MemberType,
			FRDGManagedTextureParameter>(Name, Offset, ERDGParameterMemberKind::ManagedTexture,
			ERDGResourceKind::Texture, ERDGParameterRangeKind::TextureSubresource,
			ERDGUse::ReadWrite, EntryAccess, bDiscard, ERHIRenderTargetLoadAction::Load,
			ERHIRenderTargetStoreAction::Store, true, ResultAccess);
	}

	// A non-None exit access delegates attachment transitions to the render pass.
	template<typename ParameterStruct, typename MemberType>
	constexpr auto MakeRDGAttachmentMetadata(const char* Name, uint32 Offset,
		ERHIRenderTargetLoadAction LoadAction, ERHIRenderTargetStoreAction StoreAction,
		ERHIAccess ResultAccess = ERHIAccess::None) -> FRDGParameterMemberMetadata
	{
		using FWrapper = typename TRDGParameterMemberTraits<MemberType>::ValueType;
		constexpr bool bDepth = std::same_as<FWrapper, FRDGDepthStencilAttachmentParameter>;
		static_assert(bDepth || std::same_as<FWrapper, FRDGColorAttachmentParameter>);
		const bool bManaged = ResultAccess != ERHIAccess::None;
		return MakeRDGResourceParameterMemberMetadata<ParameterStruct, MemberType, FWrapper>(
			Name, Offset, bDepth
				? (bManaged ? ERDGParameterMemberKind::ManagedDepthStencilAttachment
					: ERDGParameterMemberKind::DepthStencilAttachment)
				: (bManaged ? ERDGParameterMemberKind::ManagedColorAttachment
					: ERDGParameterMemberKind::ColorAttachment),
			ERDGResourceKind::Texture, ERDGParameterRangeKind::TextureSubresource,
			ERDGUse::ReadWrite, bDepth ? ERHIAccess::DepthStencilReadWrite
				: ERHIAccess::ColorAttachmentReadWrite,
			LoadAction != ERHIRenderTargetLoadAction::Load, LoadAction, StoreAction,
			bManaged, ResultAccess);
	}

	// Shader annotations decorate an existing semantic declaration without changing its use.
	constexpr auto WithRDGShaderBinding(FRDGParameterMemberMetadata Metadata,
		ERHIBindingType BindingType, const char* BindingName = nullptr)
		-> FRDGParameterMemberMetadata
	{
		Metadata.bShaderBinding = true;
		Metadata.ShaderBindingName = BindingName != nullptr ? BindingName : Metadata.Name;
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
			  Layout(std::exchange(Other.Layout, nullptr)),
			  AllocationIndex(std::exchange(Other.AllocationIndex, InvalidAllocationIndex))
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
				AllocationIndex = std::exchange(Other.AllocationIndex, InvalidAllocationIndex);
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
			std::weak_ptr<void> InLifetime, const FRDGParameterLayout* InLayout,
			size_t InAllocationIndex)
			: Data(InData), Lifetime(std::move(InLifetime)), Layout(InLayout),
			  AllocationIndex(InAllocationIndex)
		{
		}

		ParameterStruct* Data = nullptr;
		std::weak_ptr<void> Lifetime;
		const FRDGParameterLayout* Layout = nullptr;
		static constexpr size_t InvalidAllocationIndex = std::numeric_limits<size_t>::max();
		// Builder allocations are append-only, so indices survive vector growth.
		size_t AllocationIndex = InvalidAllocationIndex;
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
	class FRHICommandList;
	enum class ERDGRecordingPolicy : uint8 { Serial, Parallel };
	using FRDGRecordingPassExecute = std::function<void(
		FRHICommandList&, const FRDGParameterResolver&)>;
} // namespace Durin
