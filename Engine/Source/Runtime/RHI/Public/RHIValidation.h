#pragma once

#include <expected>

#include "RHIAPI.h"

namespace Durin
{
	enum class ERHIBindingType : uint8;

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
		InvalidAccess,
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
		InvalidAccess,
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
		InvalidBlockLayout,
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

	// Context is local to the operation that needs it. Scalar failures use enums.
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

	struct FRHIBufferTransitionError
	{
		ERHIBufferTransitionError Code;
		std::optional<uint32> Index;
		std::optional<uint32> OtherIndex;
	};

	struct FRHITextureTransitionError
	{
		ERHITextureTransitionError Code;
		std::optional<uint32> Index;
		std::optional<uint32> OtherIndex;
	};

	struct FRHIBufferCopyError
	{
		ERHIBufferCopyError Code;
		std::optional<uint32> Index;
		std::optional<uint32> OtherIndex;
	};

	struct FRHIBufferTextureCopyError
	{
		using FCode = std::variant<ERHIBufferTextureCopyError, ERHITextureCopyRegionError, ERHICopyFootprintError>;
		FCode Code;
		std::optional<uint32> Index;
	};

	struct FRHITextureCopyError
	{
		using FCode = std::variant<ERHITextureCopyError, ERHITextureCopyRegionError>;
		FCode Code;
		std::optional<uint32> Index;
	};

	// Presentation boundaries format only the error types they consume.
	RHI_API auto FormatRHIError(ERHIShaderBindingError Error) -> std::string;
	RHI_API auto FormatRHIError(ERHIGraphicsPipelineError Error) -> std::string;
	RHI_API auto FormatRHIError(ERHIComputePipelineError Error) -> std::string;
	RHI_API auto FormatRHIError(ERHIBufferTransitionError Error) -> std::string;
	RHI_API auto FormatRHIError(ERHITextureTransitionError Error) -> std::string;
	RHI_API auto FormatRHIError(ERHIBufferViewError Error) -> std::string;
	RHI_API auto FormatRHIError(ERHITextureViewError Error) -> std::string;
	RHI_API auto FormatRHIError(ERHITextureCopyRegionError Error) -> std::string;
	RHI_API auto FormatRHIError(ERHICopyFootprintError Error) -> std::string;
	RHI_API auto FormatRHIError(ERHIBufferCopyError Error) -> std::string;
	RHI_API auto FormatRHIError(ERHIBufferTextureCopyError Error) -> std::string;
	RHI_API auto FormatRHIError(ERHITextureCopyError Error) -> std::string;
	RHI_API auto FormatRHIError(ERHITextureCreateError Error) -> std::string;
	RHI_API auto FormatRHIError(ERHITextureUploadError Error) -> std::string;
	RHI_API auto FormatRHIError(ERHIVolumeUploadError Error) -> std::string;
	RHI_API auto FormatRHIError(const FRHIShaderBindingError& Error) -> std::string;
	RHI_API auto FormatRHIError(const FRHIBufferTransitionError& Error) -> std::string;
	RHI_API auto FormatRHIError(const FRHITextureTransitionError& Error) -> std::string;
	RHI_API auto FormatRHIError(const FRHIBufferCopyError& Error) -> std::string;
	RHI_API auto FormatRHIError(const FRHIBufferTextureCopyError& Error) -> std::string;
	RHI_API auto FormatRHIError(const FRHITextureCopyError& Error) -> std::string;
}
