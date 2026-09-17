#include "RHIValidation.h"
#include "RHICreationError.h"

namespace Durin
{
	namespace
	{
		auto DescribeError(std::monostate) -> std::string_view { return {}; }

		auto DescribeError(ERHIAccessError Code) -> std::string_view
		{
			switch (Code)
			{
			case ERHIAccessError::DiscardAfter: return "Discard access is valid only as expected-before state.";
			case ERHIAccessError::NoneAfter: return "Required-after access must not be None.";
			case ERHIAccessError::CombinedDiscard: return "Discard access must not be combined with another state.";
			case ERHIAccessError::UnknownBits: return "Access contains an unknown state bit.";
			case ERHIAccessError::ExclusiveState: return "Write-capable and presentation access states are exclusive.";
			}
			return {};
		}

		auto DescribeError(ERHIShaderBindingError Code) -> std::string_view
		{
			switch (Code)
			{
			case ERHIShaderBindingError::InvalidStage: return "Shader parameter update stage is invalid.";
			case ERHIShaderBindingError::SetOutOfRange: return "Shader parameter set is outside the active pipeline layout.";
			case ERHIShaderBindingError::IncompatibleBinding: return "Shader parameter location, element, type, or stage is incompatible with the active pipeline layout.";
			case ERHIShaderBindingError::MissingBinding: return "Shader binding validation failed: MissingBinding.";
			case ERHIShaderBindingError::UnexpectedBinding: return "Shader binding validation failed: UnexpectedBinding.";
			case ERHIShaderBindingError::NullResource: return "Shader binding validation failed: NullResource.";
			case ERHIShaderBindingError::TypeMismatch: return "Shader binding validation failed: TypeMismatch.";
			case ERHIShaderBindingError::ArrayElementOutOfRange: return "Shader binding validation failed: ArrayElementOutOfRange.";
			case ERHIShaderBindingError::StageMismatch: return "Shader binding validation failed: StageMismatch.";
			}
			return {};
		}

		auto DescribeError(ERHIGraphicsPipelineError Code) -> std::string_view
		{
			switch (Code)
			{
			case ERHIGraphicsPipelineError::InvalidFixedState: return "Graphics pipeline contains an invalid fixed-state value.";
			case ERHIGraphicsPipelineError::InvalidBlendState: return "Graphics pipeline contains an invalid blend-state value.";
			case ERHIGraphicsPipelineError::MissingShaders: return "Graphics pipeline requires vertex and fragment shaders.";
			case ERHIGraphicsPipelineError::ShaderStageMismatch: return "Graphics pipeline shader stages do not match their slots.";
			case ERHIGraphicsPipelineError::InvalidReflectedLayout: return "Graphics pipeline reflected layout is structurally invalid.";
			case ERHIGraphicsPipelineError::InvalidPushConstants: return "Graphics pipeline push-constant layout is structurally invalid.";
			case ERHIGraphicsPipelineError::InvalidRenderTargets: return "Graphics pipeline render-target layout is invalid.";
			case ERHIGraphicsPipelineError::SampleCountMismatch: return "Graphics pipeline raster samples do not match the render-target layout.";
			case ERHIGraphicsPipelineError::MissingDepthAttachment: return "Graphics pipeline depth/stencil state requires a depth attachment.";
			case ERHIGraphicsPipelineError::MissingStencilAttachment: return "Graphics pipeline stencil state requires a stencil-capable attachment.";
			case ERHIGraphicsPipelineError::MissingVertexDeclaration: return "Graphics pipeline requires a vertex declaration.";
			case ERHIGraphicsPipelineError::InvalidVertexDeclaration: return "Graphics pipeline vertex declaration is structurally invalid.";
			case ERHIGraphicsPipelineError::InconsistentVertexStream: return "Graphics pipeline vertex stream stride or input rate is inconsistent.";
			case ERHIGraphicsPipelineError::OverlappingVertexElements: return "Graphics pipeline vertex elements overlap within a stream.";
			case ERHIGraphicsPipelineError::TooManyColorAttachments: return "Graphics pipeline color attachment count exceeds the device limit.";
			case ERHIGraphicsPipelineError::UnsupportedSampleCount: return "Graphics pipeline sample count is unsupported by the device.";
			case ERHIGraphicsPipelineError::UnsupportedFillMode: return "Graphics pipeline non-solid fill is unsupported by the device.";
			case ERHIGraphicsPipelineError::UnsupportedDepthClamp: return "Graphics pipeline depth clamp is unsupported by the device.";
			case ERHIGraphicsPipelineError::UnsupportedWideLines: return "Graphics pipeline wide lines are unsupported by the device.";
			}
			return {};
		}

		auto DescribeError(ERHIComputePipelineError Code) -> std::string_view
		{
			switch (Code)
			{
			case ERHIComputePipelineError::MissingShader: return "Compute pipeline requires a compute shader.";
			case ERHIComputePipelineError::ShaderStageMismatch: return "Compute pipeline shader stage does not match its slot.";
			case ERHIComputePipelineError::InvalidReflectedLayout: return "Compute pipeline reflected layout is structurally invalid.";
			case ERHIComputePipelineError::InvalidPushConstants: return "Compute pipeline push-constant layout is structurally invalid.";
			case ERHIComputePipelineError::OverlappingPushConstants: return "Compute pipeline push-constant ranges overlap.";
			case ERHIComputePipelineError::MissingDispatchLimits: return "Compute dispatch limits are unavailable.";
			}
			return {};
		}

		auto DescribeError(ERHIBufferTransitionError Code) -> std::string_view
		{
			switch (Code)
			{
			case ERHIBufferTransitionError::NullResource: return "Buffer transition resource is null.";
			case ERHIBufferTransitionError::InvalidResourceType: return "Buffer transition resource type is invalid.";
			case ERHIBufferTransitionError::EmptyRange: return "Buffer transition size must be nonzero.";
			case ERHIBufferTransitionError::RangeOutOfBounds: return "Buffer transition range exceeds the resource size.";
			case ERHIBufferTransitionError::IncompatibleUsage: return "Buffer transition access is incompatible with resource usage.";
			case ERHIBufferTransitionError::OverlappingRanges: return "BufferTransition batch contains overlapping ranges.";
			}
			return {};
		}

		auto DescribeError(ERHITextureTransitionError Code) -> std::string_view
		{
			switch (Code)
			{
			case ERHITextureTransitionError::NullResource: return "Texture transition resource is null.";
			case ERHITextureTransitionError::InvalidResourceType: return "Texture transition resource type is invalid.";
			case ERHITextureTransitionError::EmptyAspects: return "Texture transition aspects must be nonempty.";
			case ERHITextureTransitionError::UnsupportedAspects: return "Texture transition aspects are unsupported by the pixel format.";
			case ERHITextureTransitionError::EmptyRange: return "Texture transition mip and layer counts must be nonzero.";
			case ERHITextureTransitionError::MipOutOfBounds: return "Texture transition mip range exceeds the resource.";
			case ERHITextureTransitionError::LayerOutOfBounds: return "Texture transition layer range exceeds the resource.";
			case ERHITextureTransitionError::IndeterminateLayout: return "Texture transition access has no deterministic texture layout.";
			case ERHITextureTransitionError::IncompatibleUsage: return "Texture transition access is incompatible with resource usage.";
			case ERHITextureTransitionError::IncompatibleAspects: return "Texture transition access is incompatible with the selected aspects.";
			case ERHITextureTransitionError::OverlappingRanges: return "TextureTransition batch contains overlapping ranges.";
			}
			return {};
		}

		auto DescribeError(ERHIBufferViewError Code) -> std::string_view
		{
			switch (Code)
			{
			case ERHIBufferViewError::NullParent: return "Buffer view parent is null.";
			case ERHIBufferViewError::InvalidParentType: return "Buffer view parent type is invalid.";
			case ERHIBufferViewError::EmptyRange: return "Buffer view size must be nonzero.";
			case ERHIBufferViewError::RangeOutOfBounds: return "Buffer view range exceeds the parent buffer.";
			case ERHIBufferViewError::UniformFormat: return "Uniform buffer views cannot specify a format.";
			case ERHIBufferViewError::UniformUsage: return "Uniform buffer view requires UniformBuffer usage.";
			case ERHIBufferViewError::UniformAlignment: return "Uniform buffer view offset and size must be 16-byte aligned.";
			case ERHIBufferViewError::StructuredFormat: return "Structured buffer views cannot specify a format.";
			case ERHIBufferViewError::StructuredUsage: return "Structured storage view requires StructuredBuffer or UnorderedAccess usage.";
			case ERHIBufferViewError::StructuredAlignment: return "Structured buffer view range must align to the parent stride.";
			case ERHIBufferViewError::ByteAddressFormat: return "Byte-address buffer views cannot specify a format.";
			case ERHIBufferViewError::ByteAddressUsage: return "Byte-address storage view requires ByteAddressBuffer usage.";
			case ERHIBufferViewError::ByteAddressAlignment: return "Byte-address buffer view range must be four-byte aligned.";
			case ERHIBufferViewError::FormattedUsage: return "Formatted buffer view requires FormattedBuffer usage.";
			case ERHIBufferViewError::InvalidFormattedFormat: return "Formatted buffer view requires an uncompressed color format.";
			case ERHIBufferViewError::FormattedAlignment: return "Formatted buffer view range must align to its texel size.";
			case ERHIBufferViewError::InvalidType: return "Buffer view type is invalid.";
			}
			return {};
		}

		auto DescribeError(ERHITextureViewError Code) -> std::string_view
		{
			switch (Code)
			{
			case ERHITextureViewError::NullParent: return "Texture view parent is null.";
			case ERHITextureViewError::InvalidParentType: return "Texture view parent type is invalid.";
			case ERHITextureViewError::FormatMismatch: return "Texture view format must exactly match its parent.";
			case ERHITextureViewError::EmptyAspects: return "Texture view aspects must be nonempty.";
			case ERHITextureViewError::UnsupportedAspects: return "Texture view aspects are unsupported by the parent format.";
			case ERHITextureViewError::EmptyRange: return "Texture view mip and layer counts must be nonzero.";
			case ERHITextureViewError::MipOutOfBounds: return "Texture view mip range exceeds the parent texture.";
			case ERHITextureViewError::LayerOutOfBounds: return "Texture view layer range exceeds the parent texture.";
			case ERHITextureViewError::InvalidCubeRange: return "Cube texture views require all six faces of a cube parent.";
			case ERHITextureViewError::InvalidVolumeRange: return "Texture3D views require the sole layer of a 3D parent.";
			case ERHITextureViewError::Invalid2DRange: return "Texture2D views require one layer of a 2D, array, or cube parent.";
			case ERHITextureViewError::InvalidArrayParent: return "Texture2DArray views require a 2D-array parent.";
			case ERHITextureViewError::UnsupportedDimension: return "Texture view dimension is unsupported.";
			case ERHITextureViewError::SampledUsage: return "Sampled texture view requires ShaderResource usage.";
			case ERHITextureViewError::StorageUsage: return "Storage texture view requires Storage usage.";
			case ERHITextureViewError::InvalidStorageRange: return "Storage texture views require a single-sampled 2D or 3D color range.";
			case ERHITextureViewError::ColorAttachmentUsage: return "Color attachment view requires render-target or resolve usage.";
			case ERHITextureViewError::InvalidColorAttachmentRange: return "Color attachment views require one 2D color mip and layer.";
			case ERHITextureViewError::DepthAttachmentUsage: return "Depth/stencil attachment view requires DepthStencilTargetable usage.";
			case ERHITextureViewError::InvalidDepthAttachmentRange: return "Depth/stencil attachment views require one 2D depth/stencil mip and layer.";
			case ERHITextureViewError::TransferSourceUsage: return "Transfer-source view requires SourceCopy usage.";
			case ERHITextureViewError::TransferDestinationUsage: return "Transfer-destination view requires DestinationCopy usage.";
			case ERHITextureViewError::InvalidUsage: return "Texture view usage is invalid.";
			}
			return {};
		}

		auto DescribeError(ERHITextureCopyRegionError Code) -> std::string_view
		{
			switch (Code)
			{
			case ERHITextureCopyRegionError::UnsupportedAspect: return "Texture copy aspect is unsupported by the texture format.";
			case ERHITextureCopyRegionError::UnsupportedDepthStencil: return "Depth and stencil copies are deferred by the current transfer contract.";
			case ERHITextureCopyRegionError::MipOutOfBounds: return "Texture copy mip exceeds the texture.";
			case ERHITextureCopyRegionError::LayerOutOfBounds: return "Texture copy layer range exceeds the texture.";
			case ERHITextureCopyRegionError::NegativeOffset: return "Texture copy offsets must be nonnegative.";
			case ERHITextureCopyRegionError::EmptyExtent: return "Texture copy extent must be nonempty.";
			case ERHITextureCopyRegionError::UnsupportedDimension: return "Texture copy dimension is unsupported.";
			case ERHITextureCopyRegionError::InvalidVolumeLayer: return "Texture3D copies use the sole mip subresource layer.";
			case ERHITextureCopyRegionError::Invalid2DDepth: return "Texture2D and cube copies require Z zero and depth one.";
			case ERHITextureCopyRegionError::BoxOutOfBounds: return "Texture copy box exceeds the selected mip.";
			case ERHITextureCopyRegionError::InvalidBlockLayout: return "Texture copy format has no block layout.";
			case ERHITextureCopyRegionError::BlockAlignment: return "Texture copy box is not aligned to compressed blocks or a mip edge.";
			}
			return {};
		}

		auto DescribeError(ERHICopyFootprintError Code) -> std::string_view
		{
			switch (Code)
			{
			case ERHICopyFootprintError::LayoutTooSmall: return "Buffer-texture copy layout is smaller than the texture extent.";
			case ERHICopyFootprintError::BlockAlignment: return "Buffer-texture row length and image height must align to format blocks.";
			case ERHICopyFootprintError::OffsetAlignment: return "Buffer-texture offset must align to the texel block size.";
			case ERHICopyFootprintError::RowPitchOverflow: return "Buffer-texture row pitch overflows.";
			case ERHICopyFootprintError::ImagePitchOverflow: return "Buffer-texture image pitch overflows.";
			case ERHICopyFootprintError::FootprintOverflow: return "Buffer-texture image footprint overflows.";
			case ERHICopyFootprintError::EmptyFootprint: return "Buffer-texture footprint must be nonzero.";
			}
			return {};
		}

		auto DescribeError(ERHIBufferCopyError Code) -> std::string_view
		{
			switch (Code)
			{
			case ERHIBufferCopyError::NullResource: return "Buffer copy resources must be nonnull.";
			case ERHIBufferCopyError::SourceUsage: return "Buffer copy source requires SourceCopy usage.";
			case ERHIBufferCopyError::DestinationUsage: return "Buffer copy destination requires DestinationCopy usage.";
			case ERHIBufferCopyError::EmptyRange: return "Buffer copy size must be nonzero.";
			case ERHIBufferCopyError::SourceOutOfBounds: return "Buffer copy source range exceeds the resource.";
			case ERHIBufferCopyError::DestinationOutOfBounds: return "Buffer copy destination range exceeds the resource.";
			case ERHIBufferCopyError::OverlappingDestinations: return "Buffer copy batch contains overlapping destinations.";
			case ERHIBufferCopyError::AliasedRanges: return "Same-buffer copy source and destination ranges overlap.";
			}
			return {};
		}

		auto DescribeError(ERHIBufferTextureCopyError Code) -> std::string_view
		{
			switch (Code)
			{
			case ERHIBufferTextureCopyError::NullResource: return "Buffer-texture copy resources must be nonnull.";
			case ERHIBufferTextureCopyError::MultisampledTexture: return "Buffer-texture copies require single-sampled textures.";
			case ERHIBufferTextureCopyError::BufferSourceUsage: return "Buffer-to-texture source requires SourceCopy usage.";
			case ERHIBufferTextureCopyError::TextureDestinationUsage: return "Buffer-to-texture destination requires DestinationCopy usage.";
			case ERHIBufferTextureCopyError::TextureSourceUsage: return "Texture-to-buffer source requires SourceCopy usage.";
			case ERHIBufferTextureCopyError::BufferDestinationUsage: return "Texture-to-buffer destination requires DestinationCopy usage.";
			case ERHIBufferTextureCopyError::BufferOutOfBounds: return "Buffer-texture copy footprint exceeds the buffer.";
			case ERHIBufferTextureCopyError::OverlappingBufferDestinations: return "Texture-to-buffer copy batch contains overlapping destinations.";
			case ERHIBufferTextureCopyError::OverlappingTextureDestinations: return "Buffer-to-texture copy batch contains overlapping destinations.";
			}
			return {};
		}

		auto DescribeError(ERHITextureCopyError Code) -> std::string_view
		{
			switch (Code)
			{
			case ERHITextureCopyError::NullResource: return "Texture copy resources must be nonnull.";
			case ERHITextureCopyError::SourceUsage: return "Texture copy source requires SourceCopy usage.";
			case ERHITextureCopyError::DestinationUsage: return "Texture copy destination requires DestinationCopy usage.";
			case ERHITextureCopyError::FormatMismatch: return "Texture copies require identical formats.";
			case ERHITextureCopyError::MultisampledTexture: return "Texture copies require sample count one.";
			case ERHITextureCopyError::AspectMismatch: return "Texture copy source and destination aspects must match.";
			case ERHITextureCopyError::OverlappingDestinations: return "Texture copy batch contains overlapping destinations.";
			case ERHITextureCopyError::AliasedRegions: return "Same-texture copy source and destination regions overlap.";
			}
			return {};
		}

		auto DescribeError(ERHITextureCreateError Code) -> std::string_view
		{
			switch (Code)
			{
			case ERHITextureCreateError::EmptyExtent: return "Texture extent must be nonzero.";
			case ERHITextureCreateError::EmptyDepth: return "Texture depth must be nonzero.";
			case ERHITextureCreateError::EmptyArray: return "Texture array size must be nonzero.";
			case ERHITextureCreateError::EmptyMips: return "Texture mip count must be nonzero.";
			case ERHITextureCreateError::EmptySamples: return "Texture sample count must be nonzero.";
			case ERHITextureCreateError::UnknownFormat: return "Texture pixel format must be specified.";
			case ERHITextureCreateError::InvalidSampleCount: return "Texture sample count must be one of 1, 2, 4, 8, or 16.";
			case ERHITextureCreateError::Invalid2DDepth: return "Texture2D depth must be one.";
			case ERHITextureCreateError::Invalid2DArraySize: return "Texture2D array size must be one.";
			case ERHITextureCreateError::InvalidArrayDepth: return "Texture2DArray depth must be one.";
			case ERHITextureCreateError::InvalidVolumeArraySize: return "Texture3D array size must be one.";
			case ERHITextureCreateError::MultisampledVolume: return "Texture3D must be single-sampled.";
			case ERHITextureCreateError::UnsupportedVolumeUsage: return "Texture3D supports only sampled, storage, source-copy, and destination-copy usage.";
			case ERHITextureCreateError::InvalidVolumeFormat: return "Texture3D requires a color format.";
			case ERHITextureCreateError::NonSquareCube: return "TextureCube width and height must be equal.";
			case ERHITextureCreateError::InvalidCubeLayers: return "TextureCube must contain exactly six array layers.";
			case ERHITextureCreateError::InvalidCubeDepth: return "TextureCube depth must be one.";
			case ERHITextureCreateError::NonSquareCubeArray: return "TextureCubeArray width and height must be equal.";
			case ERHITextureCreateError::InvalidCubeArrayDepth: return "TextureCubeArray depth must be one.";
			case ERHITextureCreateError::InvalidCubeArrayLayers: return "TextureCubeArray layer count must be divisible by six.";
			case ERHITextureCreateError::InvalidDimension: return "Texture dimension is invalid.";
			case ERHITextureCreateError::TooManyMips: return "Texture mip count exceeds the complete mip chain for its extent.";
			case ERHITextureCreateError::MultisampledMips: return "Multisampled textures must have exactly one mip.";
			case ERHITextureCreateError::MultisampledDimension: return "Texture3D, TextureCube, and TextureCubeArray must be single-sampled.";
			case ERHITextureCreateError::ConflictingDepthUsage: return "Depth-stencil texture usage is mutually exclusive with color, resolve, and storage usage.";
			case ERHITextureCreateError::MultisampledUsage: return "Storage, CPU-readback, and resolve textures must be single-sampled.";
			case ERHITextureCreateError::TooManySubresources: return "Texture subresource count exceeds the portable range.";
			}
			return {};
		}

		auto DescribeError(ERHITextureUploadError Code) -> std::string_view
		{
			switch (Code)
			{
			case ERHITextureUploadError::MipOutOfBounds: return "Texture upload mip index is outside the texture mip range.";
			case ERHITextureUploadError::LayerOutOfBounds: return "Texture upload array slice is outside the texture layer range.";
			case ERHITextureUploadError::NegativeOffset: return "Texture upload source offsets must be nonnegative.";
			case ERHITextureUploadError::EmptyExtent: return "Texture upload region must be nonzero.";
			case ERHITextureUploadError::BoxOutOfBounds: return "Texture upload destination region exceeds the selected mip.";
			case ERHITextureUploadError::InvalidBlockLayout: return "Texture upload requires a valid pixel format layout.";
			case ERHITextureUploadError::OffsetAlignment: return "Texture upload source and destination offsets must be block-aligned.";
			case ERHITextureUploadError::ExtentAlignment: return "Texture upload dimensions must be block-aligned unless the region reaches the mip edge.";
			case ERHITextureUploadError::InsufficientPitch: return "Texture upload source pitch is too small for the requested source region.";
			}
			return {};
		}

		auto DescribeError(ERHIVolumeUploadError Code) -> std::string_view
		{
			switch (Code)
			{
			case ERHIVolumeUploadError::InvalidDimension: return "Texture3D upload requires a Texture3D resource.";
			case ERHIVolumeUploadError::MipOutOfBounds: return "Texture3D upload mip index is outside the texture mip range.";
			case ERHIVolumeUploadError::NegativeOffset: return "Texture3D upload source offsets must be nonnegative.";
			case ERHIVolumeUploadError::EmptyExtent: return "Texture3D upload region must be nonzero.";
			case ERHIVolumeUploadError::BoxOutOfBounds: return "Texture3D upload destination region exceeds the selected mip.";
			case ERHIVolumeUploadError::InvalidBlockLayout: return "Texture3D upload requires a valid pixel format layout.";
			case ERHIVolumeUploadError::OffsetAlignment: return "Texture3D upload X/Y offsets must be block-aligned.";
			case ERHIVolumeUploadError::ExtentAlignment: return "Texture3D upload X/Y dimensions must align to blocks unless reaching a mip edge.";
			case ERHIVolumeUploadError::InsufficientRowPitch: return "Texture3D upload row pitch is too small for the source region.";
			case ERHIVolumeUploadError::RowOffsetOverflow: return "Texture3D upload source row offset overflows.";
			case ERHIVolumeUploadError::InsufficientDepthPitch: return "Texture3D upload depth pitch is too small for the source region.";
			case ERHIVolumeUploadError::FootprintOverflow: return "Texture3D upload source depth footprint overflows.";
			}
			return {};
		}
	}

	auto FormatRHIError(const FRHIError& Error) -> std::string
	{
		std::string Text(std::visit([](auto Code) { return DescribeError(Code); }, Error.Code));
		if (Error.Index) Text += std::format(" index={}", *Error.Index);
		if (Error.OtherIndex) Text += std::format(" other={}", *Error.OtherIndex);
		if (Error.SetIndex) Text += std::format(" set={}", *Error.SetIndex);
		if (Error.BindingIndex) Text += std::format(" binding={}", *Error.BindingIndex);
		if (Error.ArrayElement) Text += std::format(" element={}", *Error.ArrayElement);
		if (Error.ExpectedBindingType) Text += std::format(" expected-type={}", static_cast<uint32>(*Error.ExpectedBindingType));
		if (Error.ActualBindingType) Text += std::format(" actual-type={}", static_cast<uint32>(*Error.ActualBindingType));
		return Text;
	}
	auto FormatRHICreationError(const FRHICreationError& Error) -> std::string
	{
		if (!Error.HasError()) return {};
		std::string_view Reason;
		switch (Error.Failure)
		{
		case ERHIResourceCreationFailure::None: return {};
		case ERHIResourceCreationFailure::Unknown: Reason = "unknown creation failure"; break;
		case ERHIResourceCreationFailure::OutOfMemory: Reason = "out of memory"; break;
		case ERHIResourceCreationFailure::ResourceExhausted: Reason = "resource capacity exhausted"; break;
		case ERHIResourceCreationFailure::UnsupportedDescriptor: Reason = "unsupported resource descriptor"; break;
		}
		std::string_view Source;
		switch (Error.Source)
		{
		case ERHICreationFailureSource::None: Source = "unspecified"; break;
		case ERHICreationFailureSource::NativeBackend: Source = "native backend"; break;
		case ERHICreationFailureSource::MetadataBudget: Source = "pipeline CPU metadata budget"; break;
		case ERHICreationFailureSource::GraphicsPipelineCache: Source = "graphics pipeline cache"; break;
		case ERHICreationFailureSource::ComputePipelineCache: Source = "compute pipeline cache"; break;
		case ERHICreationFailureSource::PipelineLayoutCache: Source = "pipeline layout cache"; break;
		case ERHICreationFailureSource::DescriptorLayoutCache: Source = "descriptor layout cache"; break;
		case ERHICreationFailureSource::RenderPassCache: Source = "render-pass cache"; break;
		case ERHICreationFailureSource::RequestNotAdmitted: Source = "pipeline request not admitted"; break;
		case ERHICreationFailureSource::ObserverFailed: Source = "pipeline observer failed"; break;
		case ERHICreationFailureSource::BackendReturnedNull: Source = "backend returned no pipeline"; break;
		}
		std::string Text = std::format("{}: {}", Source, Reason);
		if (Error.NativeCode) Text += std::format(" native={}", *Error.NativeCode);
		return Text;
	}
}
