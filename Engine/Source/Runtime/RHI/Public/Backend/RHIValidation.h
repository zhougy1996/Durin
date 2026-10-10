#pragma once

#include "CoreMinimal.h"

#include "RHIResources.h"
#include "RHIShaderParameters.h"

namespace Durin
{
	// Detailed validation belongs to RHI implementations and conformance fixtures.
	enum class ERHITextureCreateError : uint8
	{
		EmptyExtent,
		EmptyDepth,
		EmptyArray,
		EmptyMips,
		EmptySamples,
		UnknownFormat,
		InvalidSampleCount,
		Invalid2DDepth,
		Invalid2DArraySize,
		InvalidArrayDepth,
		InvalidVolumeArraySize,
		MultisampledVolume,
		UnsupportedVolumeUsage,
		InvalidVolumeFormat,
		NonSquareCube,
		InvalidCubeLayers,
		InvalidCubeDepth,
		NonSquareCubeArray,
		InvalidCubeArrayDepth,
		InvalidCubeArrayLayers,
		InvalidDimension,
		TooManyMips,
		MultisampledMips,
		MultisampledDimension,
		ConflictingDepthUsage,
		MultisampledUsage,
		TooManySubresources,
	};

	RHI_API auto ToString(ERHITextureCreateError Error) -> std::string_view;

	enum class ERHITextureUploadError : uint8
	{
		MipOutOfBounds,
		LayerOutOfBounds,
		NegativeOffset,
		EmptyExtent,
		BoxOutOfBounds,
		InvalidBlockLayout,
		OffsetAlignment,
		ExtentAlignment,
		InsufficientPitch,
	};

	RHI_API auto ToString(ERHITextureUploadError Error) -> std::string_view;

	enum class ERHIVolumeUploadError : uint8
	{
		InvalidDimension,
		MipOutOfBounds,
		NegativeOffset,
		EmptyExtent,
		BoxOutOfBounds,
		InvalidBlockLayout,
		OffsetAlignment,
		ExtentAlignment,
		InsufficientRowPitch,
		RowOffsetOverflow,
		InsufficientDepthPitch,
		FootprintOverflow,
	};

	RHI_API auto ToString(ERHIVolumeUploadError Error) -> std::string_view;

	// Validates the backend-neutral constraints required before creating a texture.
	RHI_API auto ValidateTextureCreateDesc(const FRHITextureCreateDesc& CreateDesc) -> std::expected<void, ERHITextureCreateError>;

	// Validates one two-dimensional mip/slice upload, including block-compressed alignment constraints.
	RHI_API auto ValidateTexture2DUpdate(
		const FRHITextureDesc& TextureDesc,
		uint32 MipIndex,
		uint32 ArraySlice,
		const FUpdateTextureRegion2D& UpdateRegion,
		uint32 SourcePitch) -> std::expected<void, ERHITextureUploadError>;

	// Validates one volume-mip upload, including row/depth pitches and block geometry.
	RHI_API auto ValidateTexture3DUpdate(
		const FRHITextureDesc& TextureDesc,
		uint32 MipIndex,
		const FUpdateTextureRegion3D& UpdateRegion,
		uint32 SourceRowPitch,
		uint32 SourceDepthPitch) -> std::expected<void, ERHIVolumeUploadError>;


	enum class ERHIBufferTransitionError : uint8
	{
		NullResource,
		InvalidResourceType,
		EmptyRange,
		RangeOutOfBounds,
		InvalidAccess,
		IncompatibleUsage,
		OverlappingRanges,
	};

	RHI_API auto ToString(ERHIBufferTransitionError Error) -> std::string_view;

	enum class ERHITextureTransitionError : uint8
	{
		NullResource,
		InvalidResourceType,
		EmptyAspects,
		UnsupportedAspects,
		EmptyRange,
		MipOutOfBounds,
		LayerOutOfBounds,
		IndeterminateLayout,
		InvalidAccess,
		IncompatibleUsage,
		IncompatibleAspects,
		OverlappingRanges,
	};

	RHI_API auto ToString(ERHITextureTransitionError Error) -> std::string_view;

	struct FRHIBufferTransitionError
	{
		ERHIBufferTransitionError Code;
		std::optional<uint32> Index;
		std::optional<uint32> OtherIndex;
	};

	RHI_API auto ToString(const FRHIBufferTransitionError& Error) -> std::string;

	struct FRHITextureTransitionError
	{
		ERHITextureTransitionError Code;
		std::optional<uint32> Index;
		std::optional<uint32> OtherIndex;
	};

	RHI_API auto ToString(const FRHITextureTransitionError& Error) -> std::string;

	RHI_API auto ValidateBufferTransition(const FRHIBufferTransition& Transition) -> std::expected<void, ERHIBufferTransitionError>;
	RHI_API auto ValidateTextureTransition(const FRHITextureTransition& Transition) -> std::expected<void, ERHITextureTransitionError>;
	RHI_API auto ValidateBufferTransitions(std::span<const FRHIBufferTransition> Transitions) -> std::expected<void, FRHIBufferTransitionError>;
	RHI_API auto ValidateTextureTransitions(std::span<const FRHITextureTransition> Transitions) -> std::expected<void, FRHITextureTransitionError>;


	enum class ERHIIndirectCommandError : uint8
	{
		NullBuffer,
		MissingIndirectUsage,
		MisalignedOffset,
		RangeOutOfBounds,
		Unsupported,
	};

	RHI_API auto ToString(ERHIIndirectCommandError Error) -> std::string_view;
	RHI_API auto ValidateIndirectArgumentBuffer(const FRHIBuffer* Buffer,
		uint64 Offset, uint32 RecordSize, bool bSupported)
		-> std::expected<void, ERHIIndirectCommandError>;


	enum class ERHIBufferViewError : uint8
	{
		NullParent,
		InvalidParentType,
		EmptyRange,
		RangeOutOfBounds,
		UniformFormat,
		UniformUsage,
		UniformAlignment,
		StructuredFormat,
		StructuredUsage,
		StructuredAlignment,
		ByteAddressFormat,
		ByteAddressUsage,
		ByteAddressAlignment,
		FormattedUsage,
		InvalidFormattedFormat,
		FormattedAlignment,
		InvalidType,
	};

	RHI_API auto ToString(ERHIBufferViewError Error) -> std::string_view;

	enum class ERHITextureViewError : uint8
	{
		NullParent,
		InvalidParentType,
		FormatMismatch,
		EmptyAspects,
		UnsupportedAspects,
		EmptyRange,
		MipOutOfBounds,
		LayerOutOfBounds,
		InvalidCubeRange,
		InvalidVolumeRange,
		Invalid2DRange,
		InvalidArrayParent,
		UnsupportedDimension,
		SampledUsage,
		StorageUsage,
		InvalidStorageRange,
		ColorAttachmentUsage,
		InvalidColorAttachmentRange,
		DepthAttachmentUsage,
		InvalidDepthAttachmentRange,
		TransferSourceUsage,
		TransferDestinationUsage,
		InvalidUsage,
	};

	RHI_API auto ToString(ERHITextureViewError Error) -> std::string_view;

	RHI_API auto ValidateBufferViewDesc(
		const FRHIBuffer* Buffer,
		const FRHIBufferViewDesc& Desc) -> std::expected<void, ERHIBufferViewError>;
	RHI_API auto ValidateBufferViewDesc(
		const FRHIBufferDesc& Buffer,
		const FRHIBufferViewDesc& Desc) -> std::expected<void, ERHIBufferViewError>;
	RHI_API auto ValidateTextureViewDesc(
		const FRHITexture* Texture,
		const FRHITextureViewDesc& Desc) -> std::expected<void, ERHITextureViewError>;


	enum class ERHITextureCopyRegionError : uint8
	{
		UnsupportedAspect,
		UnsupportedDepthStencil,
		MipOutOfBounds,
		LayerOutOfBounds,
		NegativeOffset,
		EmptyExtent,
		UnsupportedDimension,
		InvalidVolumeLayer,
		Invalid2DDepth,
		BoxOutOfBounds,
		InvalidBlockLayout,
		BlockAlignment,
	};

	RHI_API auto ToString(ERHITextureCopyRegionError Error) -> std::string_view;

	enum class ERHICopyFootprintError : uint8
	{
		InvalidBlockLayout,
		LayoutTooSmall,
		BlockAlignment,
		OffsetAlignment,
		ImagePitchOverflow,
		FootprintOverflow,
		EmptyFootprint,
	};

	RHI_API auto ToString(ERHICopyFootprintError Error) -> std::string_view;

	enum class ERHIBufferCopyError : uint8
	{
		NullResource,
		SourceUsage,
		DestinationUsage,
		EmptyRange,
		SourceOutOfBounds,
		DestinationOutOfBounds,
		OverlappingDestinations,
		AliasedRanges,
	};

	RHI_API auto ToString(ERHIBufferCopyError Error) -> std::string_view;

	enum class ERHIBufferTextureCopyError : uint8
	{
		NullResource,
		MultisampledTexture,
		BufferSourceUsage,
		TextureDestinationUsage,
		TextureSourceUsage,
		BufferDestinationUsage,
		BufferOutOfBounds,
		OverlappingBufferDestinations,
		OverlappingTextureDestinations,
	};

	RHI_API auto ToString(ERHIBufferTextureCopyError Error) -> std::string_view;

	enum class ERHITextureCopyError : uint8
	{
		NullResource,
		SourceUsage,
		DestinationUsage,
		FormatMismatch,
		MultisampledTexture,
		AspectMismatch,
		OverlappingDestinations,
		AliasedRegions,
	};

	RHI_API auto ToString(ERHITextureCopyError Error) -> std::string_view;

	struct FRHIBufferCopyError
	{
		ERHIBufferCopyError Code;
		std::optional<uint32> Index;
		std::optional<uint32> OtherIndex;
	};

	RHI_API auto ToString(const FRHIBufferCopyError& Error) -> std::string;

	struct FRHIBufferTextureCopyError
	{
		using FCode = std::variant<ERHIBufferTextureCopyError, ERHITextureCopyRegionError, ERHICopyFootprintError>;
		FCode Code;
		std::optional<uint32> Index;
	};

	RHI_API auto ToString(const FRHIBufferTextureCopyError& Error) -> std::string;

	struct FRHITextureCopyError
	{
		using FCode = std::variant<ERHITextureCopyError, ERHITextureCopyRegionError>;
		FCode Code;
		std::optional<uint32> Index;
	};

	RHI_API auto ToString(const FRHITextureCopyError& Error) -> std::string;

	RHI_API auto ValidateBufferCopies(FRHIBuffer* Source, FRHIBuffer* Destination,
		std::span<const FRHIBufferCopyRegion> Regions) -> std::expected<void, FRHIBufferCopyError>;
	RHI_API auto ValidateBufferToTextureCopies(FRHIBuffer* Source, FRHITexture* Destination,
		std::span<const FRHIBufferTextureCopyRegion> Regions) -> std::expected<void, FRHIBufferTextureCopyError>;
	RHI_API auto ValidateTextureToBufferCopies(FRHITexture* Source, FRHIBuffer* Destination,
		std::span<const FRHIBufferTextureCopyRegion> Regions) -> std::expected<void, FRHIBufferTextureCopyError>;
	RHI_API auto ValidateTextureCopies(FRHITexture* Source, FRHITexture* Destination,
		std::span<const FRHITextureCopyRegion> Regions) -> std::expected<void, FRHITextureCopyError>;
	RHI_API auto GetBufferTextureCopyFootprint(const FRHITexture& Texture,
		const FRHIBufferTextureCopyRegion& Region) -> std::expected<uint64, ERHICopyFootprintError>;


	enum class ERHIShaderBindingError : uint8
	{
		InvalidStage,
		SetOutOfRange,
		IncompatibleBinding,
		MissingBinding,
		UnexpectedBinding,
		NullResource,
		TypeMismatch,
		ArrayElementOutOfRange,
		StageMismatch,
	};

	RHI_API auto ToString(ERHIShaderBindingError Error) -> std::string_view;

	struct FRHIShaderBindingError
	{
		ERHIShaderBindingError Code;
		std::optional<uint32> Index;
		std::optional<uint32> SetIndex;
		std::optional<uint32> BindingIndex;
		std::optional<uint32> ArrayElement;
		std::optional<ERHIBindingType> ExpectedBindingType;
		std::optional<ERHIBindingType> ActualBindingType;
	};

	RHI_API auto ToString(const FRHIShaderBindingError& Error) -> std::string;

	RHI_API auto ValidateShaderParameterUpdate(const FPipelineLayoutDesc& Layout,
		EShaderStageFlags ShaderStage,
		std::span<const FRHIShaderParameterResource> Resources) -> std::expected<void, FRHIShaderBindingError>;
	RHI_API auto ValidateShaderBindingCompleteness(const FPipelineLayoutDesc& Layout,
		std::span<const FRHIShaderParameterResource> Resources) -> std::expected<void, FRHIShaderBindingError>;
} // namespace Durin
