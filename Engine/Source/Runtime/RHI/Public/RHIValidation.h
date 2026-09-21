#pragma once

#include <expected>

#include "RHIAPI.h"

namespace Durin
{
	enum class ERHIBindingType : uint8;
	enum class ERHIAccessError : uint8
	{
		DiscardAfter,
		NoneAfter,
		CombinedDiscard,
		UnknownBits,
		ExclusiveState,
	};

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

	enum class ERHIGraphicsPipelineError : uint8
	{
		InvalidFixedState,
		InvalidBlendState,
		MissingShaders,
		ShaderStageMismatch,
		InvalidReflectedLayout,
		InvalidPushConstants,
		InvalidRenderTargets,
		SampleCountMismatch,
		MissingDepthAttachment,
		MissingStencilAttachment,
		MissingVertexDeclaration,
		InvalidVertexDeclaration,
		InconsistentVertexStream,
		OverlappingVertexElements,
		TooManyColorAttachments,
		UnsupportedSampleCount,
		UnsupportedFillMode,
		UnsupportedDepthClamp,
		UnsupportedWideLines,
	};

	enum class ERHIComputePipelineError : uint8
	{
		MissingShader,
		ShaderStageMismatch,
		InvalidReflectedLayout,
		InvalidPushConstants,
		OverlappingPushConstants,
		MissingDispatchLimits,
	};

	enum class ERHIBufferTransitionError : uint8
	{
		NullResource,
		InvalidResourceType,
		EmptyRange,
		RangeOutOfBounds,
		IncompatibleUsage,
		OverlappingRanges,
	};

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
		IncompatibleUsage,
		IncompatibleAspects,
		OverlappingRanges,
	};

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

	enum class ERHICopyFootprintError : uint8
	{
		LayoutTooSmall,
		BlockAlignment,
		OffsetAlignment,
		RowPitchOverflow,
		ImagePitchOverflow,
		FootprintOverflow,
		EmptyFootprint,
	};

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

	// Engine-owned failures carry semantic codes and locations, never prose.
	struct FRHIError
	{
		using FCode = std::variant<
			ERHIAccessError,
			ERHIShaderBindingError,
			ERHIGraphicsPipelineError,
			ERHIComputePipelineError,
			ERHIBufferTransitionError,
			ERHITextureTransitionError,
			ERHIBufferViewError,
			ERHITextureViewError,
			ERHITextureCopyRegionError,
			ERHICopyFootprintError,
			ERHIBufferCopyError,
			ERHIBufferTextureCopyError,
			ERHITextureCopyError,
			ERHITextureCreateError,
			ERHITextureUploadError,
			ERHIVolumeUploadError>;
		FCode Code;
		std::optional<uint32> Index;
		std::optional<uint32> OtherIndex;
		std::optional<uint32> SetIndex;
		std::optional<uint32> BindingIndex;
		std::optional<uint32> ArrayElement;
		std::optional<ERHIBindingType> ExpectedBindingType;
		std::optional<ERHIBindingType> ActualBindingType;
	};

	template<typename T = void>
	using TRHIResult = std::expected<T, FRHIError>;
	using FRHIOperationResult = TRHIResult<>;

	// Presentation boundary only: logging, assertions and user interfaces.
	RHI_API auto FormatRHIError(const FRHIError& Error) -> std::string;
}
