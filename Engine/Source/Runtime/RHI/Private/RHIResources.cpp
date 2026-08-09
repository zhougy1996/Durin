#include "RHIResources.h"

#include "Math/Operations.h"

#include "Math/Vector.h"

namespace Durin
{
	namespace
	{
		constexpr ERHIAccess ReadAccessMask = ERHIAccess::VertexBufferRead
			| ERHIAccess::IndexBufferRead
			| ERHIAccess::GraphicsUniformRead
			| ERHIAccess::ComputeUniformRead
			| ERHIAccess::GraphicsShaderRead
			| ERHIAccess::ComputeShaderRead
			| ERHIAccess::TransferRead
			| ERHIAccess::HostRead;

		constexpr ERHIAccess ExclusiveAccessMask = ERHIAccess::ColorAttachmentReadWrite
			| ERHIAccess::DepthStencilReadWrite
			| ERHIAccess::GraphicsShaderReadWrite
			| ERHIAccess::ComputeShaderReadWrite
			| ERHIAccess::TransferWrite
			| ERHIAccess::HostWrite
			| ERHIAccess::Present;

		auto IsSingleBit(ERHIAccess Access) -> bool
		{
			const uint32 Value = static_cast<uint32>(Access);
			return Value != 0 && (Value & (Value - 1)) == 0;
		}

		auto IsValidAccessShape(ERHIAccess Access, bool bExpected, std::string& OutError) -> bool
		{
			auto Fail = [&OutError](const char* Message) {
				OutError = Message;
				return false;
			};
			if (Access == ERHIAccess::Discard)
			{
				return bExpected || Fail("Discard access is valid only as expected-before state.");
			}
			if (Access == ERHIAccess::None)
			{
				return bExpected || Fail("Required-after access must not be None.");
			}
			if (EnumHasAnyFlags(Access, ERHIAccess::Discard))
			{
				return Fail("Discard access must not be combined with another state.");
			}
			const ERHIAccess KnownMask = ReadAccessMask | ExclusiveAccessMask;
			if ((static_cast<uint32>(Access) & ~static_cast<uint32>(KnownMask)) != 0)
			{
				return Fail("Access contains an unknown state bit.");
			}
			if (EnumHasAnyFlags(Access, ExclusiveAccessMask) && !IsSingleBit(Access))
			{
				return Fail("Write-capable and presentation access states are exclusive.");
			}
			return true;
		}

		auto BufferUsageAdmits(EBufferUsageFlags Usage, ERHIAccess Access) -> bool
		{
			if (Access == ERHIAccess::None || Access == ERHIAccess::Discard) return true;
			if (EnumHasAnyFlags(Access, ERHIAccess::VertexBufferRead)
				&& !EnumHasAnyFlags(Usage, EBufferUsageFlags::VertexBuffer)) return false;
			if (EnumHasAnyFlags(Access, ERHIAccess::IndexBufferRead)
				&& !EnumHasAnyFlags(Usage, EBufferUsageFlags::IndexBuffer)) return false;
			if (EnumHasAnyFlags(Access, ERHIAccess::GraphicsUniformRead | ERHIAccess::ComputeUniformRead)
				&& !EnumHasAnyFlags(Usage, EBufferUsageFlags::UniformBuffer)) return false;
			if (EnumHasAnyFlags(Access, ERHIAccess::GraphicsShaderRead | ERHIAccess::ComputeShaderRead)
				&& !EnumHasAnyFlags(Usage, EBufferUsageFlags::ShaderResource | EBufferUsageFlags::StructuredBuffer
					| EBufferUsageFlags::ByteAddressBuffer | EBufferUsageFlags::UnorderedAccess)) return false;
			if (EnumHasAnyFlags(Access, ERHIAccess::TransferRead)
				&& !EnumHasAnyFlags(Usage, EBufferUsageFlags::SourceCopy)) return false;
			if (EnumHasAnyFlags(Access, ERHIAccess::HostRead)
				&& !EnumHasAnyFlags(Usage, EBufferUsageFlags::KeepCPUAccessible)) return false;
			if (EnumHasAnyFlags(Access, ERHIAccess::GraphicsShaderReadWrite | ERHIAccess::ComputeShaderReadWrite)
				&& !EnumHasAnyFlags(Usage, EBufferUsageFlags::UnorderedAccess)) return false;
			if (EnumHasAnyFlags(Access, ERHIAccess::TransferWrite)
				&& !EnumHasAnyFlags(Usage, EBufferUsageFlags::Static | EBufferUsageFlags::Dynamic)) return false;
			if (EnumHasAnyFlags(Access, ERHIAccess::HostWrite)
				&& !EnumHasAnyFlags(Usage, EBufferUsageFlags::Dynamic | EBufferUsageFlags::KeepCPUAccessible)) return false;
			return !EnumHasAnyFlags(Access, ERHIAccess::ColorAttachmentReadWrite
				| ERHIAccess::DepthStencilReadWrite | ERHIAccess::Present);
		}

		auto TextureUsageAdmits(const FRHITexture& Texture, ERHIAccess Access) -> bool
		{
			if (Access == ERHIAccess::None || Access == ERHIAccess::Discard) return true;
			const ETextureCreateFlags Usage = Texture.GetFlags();
			constexpr ERHIAccess TextureReadMask = ERHIAccess::GraphicsShaderRead
				| ERHIAccess::ComputeShaderRead;
			if (EnumHasAnyFlags(Access, ReadAccessMask & ~TextureReadMask)
				&& Access != ERHIAccess::TransferRead && Access != ERHIAccess::HostRead) return false;
			if (EnumHasAnyFlags(Access, TextureReadMask)
				&& !EnumHasAnyFlags(Usage, ETextureCreateFlags::ShaderResource | ETextureCreateFlags::Storage)) return false;
			if (Access == ERHIAccess::TransferRead || Access == ERHIAccess::HostRead)
				return EnumHasAnyFlags(Usage, ETextureCreateFlags::CPUReadback);
			if (Access == ERHIAccess::ColorAttachmentReadWrite)
				return EnumHasAnyFlags(Usage, ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ResolveTargetable);
			if (Access == ERHIAccess::DepthStencilReadWrite)
				return EnumHasAnyFlags(Usage, ETextureCreateFlags::DepthStencilTargetable);
			if (Access == ERHIAccess::GraphicsShaderReadWrite || Access == ERHIAccess::ComputeShaderReadWrite)
				return EnumHasAnyFlags(Usage, ETextureCreateFlags::Storage);
			if (Access == ERHIAccess::TransferWrite) return true;
			if (Access == ERHIAccess::HostWrite) return false;
			if (Access == ERHIAccess::Present)
				return EnumHasAnyFlags(Usage, ETextureCreateFlags::RenderTargetable);
			return true;
		}

		auto RangesOverlap(uint64 FirstOffset, uint64 FirstSize, uint64 SecondOffset, uint64 SecondSize) -> bool
		{
			return FirstOffset < SecondOffset + SecondSize && SecondOffset < FirstOffset + FirstSize;
		}

		auto TextureAspectsAdmit(ERHITextureAspect Aspects, ERHIAccess Access) -> bool
		{
			if (EnumHasAnyFlags(Aspects, ERHITextureAspect::Color)
				&& Access == ERHIAccess::DepthStencilReadWrite) return false;
			if (EnumHasAnyFlags(Aspects, ERHITextureAspect::Depth | ERHITextureAspect::Stencil)
				&& (Access == ERHIAccess::ColorAttachmentReadWrite || Access == ERHIAccess::Present)) return false;
			return true;
		}
	}

	auto GetTextureAspects(EPixelFormat Format) -> ERHITextureAspect
	{
		const FPixelFormatInfo& Info = GetPixelFormatInfo(Format);
		ERHITextureAspect Result = ERHITextureAspect::None;
		if (Info.bHasDepth) Result |= ERHITextureAspect::Depth;
		if (Info.bHasStencil) Result |= ERHITextureAspect::Stencil;
		return Result == ERHITextureAspect::None ? ERHITextureAspect::Color : Result;
	}

	auto GetTextureLayoutForAccess(ERHIAccess Access, ERHITextureLayout& OutLayout) -> bool
	{
		if (Access == ERHIAccess::None || Access == ERHIAccess::Discard) OutLayout = ERHITextureLayout::Undefined;
		else if (Access == ERHIAccess::ColorAttachmentReadWrite) OutLayout = ERHITextureLayout::ColorAttachment;
		else if (Access == ERHIAccess::DepthStencilReadWrite) OutLayout = ERHITextureLayout::DepthStencilAttachment;
		else if (Access == ERHIAccess::GraphicsShaderRead || Access == ERHIAccess::ComputeShaderRead
			|| Access == (ERHIAccess::GraphicsShaderRead | ERHIAccess::ComputeShaderRead)) OutLayout = ERHITextureLayout::ShaderReadOnly;
		else if (Access == ERHIAccess::TransferRead) OutLayout = ERHITextureLayout::TransferSource;
		else if (Access == ERHIAccess::TransferWrite) OutLayout = ERHITextureLayout::TransferDestination;
		else if (Access == ERHIAccess::HostRead || Access == ERHIAccess::HostWrite
			|| Access == ERHIAccess::GraphicsShaderReadWrite || Access == ERHIAccess::ComputeShaderReadWrite) OutLayout = ERHITextureLayout::General;
		else if (Access == ERHIAccess::Present) OutLayout = ERHITextureLayout::Present;
		else return false;
		return true;
	}

	auto FRHIBufferTransition::Whole(FRHIBuffer* Buffer, ERHIAccess ExpectedBefore,
		ERHIAccess RequiredAfter) -> FRHIBufferTransition
	{
		return {.Buffer = Buffer, .Offset = 0, .Size = Buffer ? Buffer->GetSize() : 0,
			.ExpectedBefore = ExpectedBefore, .RequiredAfter = RequiredAfter};
	}

	auto FRHITextureTransition::Whole(FRHITexture* Texture, ERHIAccess ExpectedBefore,
		ERHIAccess RequiredAfter) -> FRHITextureTransition
	{
		return {.Texture = Texture,
			.Range = {.Aspects = Texture ? GetTextureAspects(Texture->GetFormat()) : ERHITextureAspect::None,
				.FirstMip = 0, .NumMips = Texture ? static_cast<uint32>(Texture->GetNumMips()) : 0u,
				.FirstArrayLayer = 0, .NumArrayLayers = Texture ? static_cast<uint32>(Texture->GetArraySize()) : 0u},
			.ExpectedBefore = ExpectedBefore, .RequiredAfter = RequiredAfter};
	}

	auto ValidateBufferTransition(const FRHIBufferTransition& Transition, std::string& OutError) -> bool
	{
		auto Fail = [&OutError](const char* Message) { OutError = Message; return false; };
		if (Transition.Buffer == nullptr) return Fail("Buffer transition resource is null.");
		if (Transition.Buffer->GetResourceType() != ERHIResourceType::Buffer) return Fail("Buffer transition resource type is invalid.");
		if (Transition.Size == 0) return Fail("Buffer transition size must be nonzero.");
		const uint64 ResourceSize = Transition.Buffer->GetSize();
		if (Transition.Offset > ResourceSize || Transition.Size > ResourceSize - Transition.Offset)
			return Fail("Buffer transition range exceeds the resource size.");
		if (!IsValidAccessShape(Transition.ExpectedBefore, true, OutError)
			|| !IsValidAccessShape(Transition.RequiredAfter, false, OutError)) return false;
		if (!BufferUsageAdmits(Transition.Buffer->GetUsage(), Transition.ExpectedBefore)
			|| !BufferUsageAdmits(Transition.Buffer->GetUsage(), Transition.RequiredAfter))
			return Fail("Buffer transition access is incompatible with resource usage.");
		OutError.clear();
		return true;
	}

	auto ValidateTextureTransition(const FRHITextureTransition& Transition, std::string& OutError) -> bool
	{
		auto Fail = [&OutError](const char* Message) { OutError = Message; return false; };
		if (Transition.Texture == nullptr) return Fail("Texture transition resource is null.");
		if (Transition.Texture->GetResourceType() != ERHIResourceType::Texture) return Fail("Texture transition resource type is invalid.");
		if (Transition.Range.Aspects == ERHITextureAspect::None) return Fail("Texture transition aspects must be nonempty.");
		constexpr ERHITextureAspect KnownAspects = ERHITextureAspect::Color | ERHITextureAspect::Depth | ERHITextureAspect::Stencil;
		if ((static_cast<uint8>(Transition.Range.Aspects) & ~static_cast<uint8>(KnownAspects)) != 0
			|| !EnumHasAllFlags(GetTextureAspects(Transition.Texture->GetFormat()), Transition.Range.Aspects))
			return Fail("Texture transition aspects are unsupported by the pixel format.");
		if (Transition.Range.NumMips == 0 || Transition.Range.NumArrayLayers == 0)
			return Fail("Texture transition mip and layer counts must be nonzero.");
		if (Transition.Range.FirstMip > Transition.Texture->GetNumMips()
			|| Transition.Range.NumMips > Transition.Texture->GetNumMips() - Transition.Range.FirstMip)
			return Fail("Texture transition mip range exceeds the resource.");
		if (Transition.Range.FirstArrayLayer > Transition.Texture->GetArraySize()
			|| Transition.Range.NumArrayLayers > Transition.Texture->GetArraySize() - Transition.Range.FirstArrayLayer)
			return Fail("Texture transition layer range exceeds the resource.");
		if (!IsValidAccessShape(Transition.ExpectedBefore, true, OutError)
			|| !IsValidAccessShape(Transition.RequiredAfter, false, OutError)) return false;
		ERHITextureLayout IgnoredLayout;
		if (!GetTextureLayoutForAccess(Transition.ExpectedBefore, IgnoredLayout)
			|| !GetTextureLayoutForAccess(Transition.RequiredAfter, IgnoredLayout))
			return Fail("Texture transition access has no deterministic texture layout.");
		if (!TextureUsageAdmits(*Transition.Texture, Transition.ExpectedBefore)
			|| !TextureUsageAdmits(*Transition.Texture, Transition.RequiredAfter))
			return Fail("Texture transition access is incompatible with resource usage.");
		if (!TextureAspectsAdmit(Transition.Range.Aspects, Transition.ExpectedBefore)
			|| !TextureAspectsAdmit(Transition.Range.Aspects, Transition.RequiredAfter))
			return Fail("Texture transition access is incompatible with the selected aspects.");
		OutError.clear();
		return true;
	}

	auto ValidateBufferTransitions(std::span<const FRHIBufferTransition> Transitions, std::string& OutError) -> bool
	{
		for (size_t Index = 0; Index < Transitions.size(); ++Index)
		{
			if (!ValidateBufferTransition(Transitions[Index], OutError)) return false;
			for (size_t OtherIndex = 0; OtherIndex < Index; ++OtherIndex)
			{
				if (Transitions[Index].Buffer == Transitions[OtherIndex].Buffer
					&& RangesOverlap(Transitions[Index].Offset, Transitions[Index].Size,
						Transitions[OtherIndex].Offset, Transitions[OtherIndex].Size))
				{
					OutError = "Buffer transition batch contains overlapping ranges.";
					return false;
				}
			}
		}
		OutError.clear();
		return true;
	}

	auto ValidateTextureTransitions(std::span<const FRHITextureTransition> Transitions, std::string& OutError) -> bool
	{
		for (size_t Index = 0; Index < Transitions.size(); ++Index)
		{
			if (!ValidateTextureTransition(Transitions[Index], OutError)) return false;
			for (size_t OtherIndex = 0; OtherIndex < Index; ++OtherIndex)
			{
				const auto& A = Transitions[Index];
				const auto& B = Transitions[OtherIndex];
				const bool bAspectOverlap = EnumHasAnyFlags(A.Range.Aspects, B.Range.Aspects);
				const bool bMipOverlap = RangesOverlap(A.Range.FirstMip, A.Range.NumMips, B.Range.FirstMip, B.Range.NumMips);
				const bool bLayerOverlap = RangesOverlap(A.Range.FirstArrayLayer, A.Range.NumArrayLayers,
					B.Range.FirstArrayLayer, B.Range.NumArrayLayers);
				if (A.Texture == B.Texture && bAspectOverlap && bMipOverlap && bLayerOverlap)
				{
					OutError = "Texture transition batch contains overlapping subresource ranges.";
					return false;
				}
			}
		}
		OutError.clear();
		return true;
	}

	auto ValidateTextureCreateDesc(const FRHITextureCreateDesc& CreateDesc, std::string& OutError) -> bool
	{
		auto Fail = [&OutError](std::string Message) {
			OutError = std::move(Message);
			return false;
		};

		if (CreateDesc.Extent.x <= 0 || CreateDesc.Extent.y <= 0) return Fail("Texture extent must be nonzero.");
		if (CreateDesc.Depth == 0) return Fail("Texture depth must be nonzero.");
		if (CreateDesc.ArraySize == 0) return Fail("Texture array size must be nonzero.");
		if (CreateDesc.NumMips == 0) return Fail("Texture mip count must be nonzero.");
		if (CreateDesc.NumSamples == 0) return Fail("Texture sample count must be nonzero.");
		if (CreateDesc.Format == EPixelFormat::Unknown) return Fail("Texture pixel format must be specified.");
		if (CreateDesc.NumSamples != 1 && CreateDesc.NumSamples != 2
			&& CreateDesc.NumSamples != 4 && CreateDesc.NumSamples != 8
			&& CreateDesc.NumSamples != 16)
		{
			return Fail("Texture sample count must be one of 1, 2, 4, 8, or 16.");
		}

		switch (CreateDesc.Dimension)
		{
		case ETextureDimension::Texture2D:
			if (CreateDesc.Depth != 1) return Fail("Texture2D depth must be one.");
			if (CreateDesc.ArraySize != 1) return Fail("Texture2D array size must be one.");
			break;
		case ETextureDimension::Texture2DArray:
			if (CreateDesc.Depth != 1) return Fail("Texture2DArray depth must be one.");
			break;
		case ETextureDimension::Texture3D:
			if (CreateDesc.ArraySize != 1) return Fail("Texture3D array size must be one.");
			break;
		case ETextureDimension::TextureCube:
			if (CreateDesc.Extent.x != CreateDesc.Extent.y) return Fail("TextureCube width and height must be equal.");
			if (CreateDesc.ArraySize != TextureCubeFaceCount) return Fail("TextureCube must contain exactly six array layers.");
			if (CreateDesc.Depth != 1) return Fail("TextureCube depth must be one.");
			break;
		case ETextureDimension::TextureCubeArray:
			if (CreateDesc.Extent.x != CreateDesc.Extent.y) return Fail("TextureCubeArray width and height must be equal.");
			if (CreateDesc.Depth != 1) return Fail("TextureCubeArray depth must be one.");
			if (CreateDesc.ArraySize % TextureCubeFaceCount != 0) return Fail("TextureCubeArray layer count must be divisible by six.");
			break;
		default:
			return Fail("Texture dimension is invalid.");
		}

		const uint32 MaxDimension = CreateDesc.Dimension == ETextureDimension::Texture3D
			? std::max({static_cast<uint32>(CreateDesc.Extent.x), static_cast<uint32>(CreateDesc.Extent.y), static_cast<uint32>(CreateDesc.Depth)})
			: std::max(static_cast<uint32>(CreateDesc.Extent.x), static_cast<uint32>(CreateDesc.Extent.y));
		uint32 MaximumMipCount = 1;
		for (uint32 Remaining = MaxDimension; Remaining > 1; Remaining >>= 1) ++MaximumMipCount;
		if (CreateDesc.NumMips > MaximumMipCount) return Fail("Texture mip count exceeds the complete mip chain for its extent.");
		if (CreateDesc.NumSamples != 1 && CreateDesc.NumMips != 1) return Fail("Multisampled textures must have exactly one mip.");
		if ((CreateDesc.Dimension == ETextureDimension::Texture3D
			|| CreateDesc.Dimension == ETextureDimension::TextureCube
			|| CreateDesc.Dimension == ETextureDimension::TextureCubeArray)
			&& CreateDesc.NumSamples != 1)
		{
			return Fail("Texture3D, TextureCube, and TextureCubeArray must be single-sampled.");
		}

		const bool bDepthStencil = EnumHasAnyFlags(CreateDesc.Flags, ETextureCreateFlags::DepthStencilTargetable);
		const bool bColorOrResolve = EnumHasAnyFlags(CreateDesc.Flags,
			ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ResolveTargetable);
		if (bDepthStencil && (bColorOrResolve || EnumHasAnyFlags(CreateDesc.Flags, ETextureCreateFlags::Storage)))
			return Fail("Depth-stencil texture usage is mutually exclusive with color, resolve, and storage usage.");
		if (CreateDesc.NumSamples != 1 && EnumHasAnyFlags(CreateDesc.Flags,
			ETextureCreateFlags::Storage | ETextureCreateFlags::CPUReadback | ETextureCreateFlags::ResolveTargetable))
			return Fail("Storage, CPU-readback, and resolve textures must be single-sampled.");

		const uint64 SubresourceCount = static_cast<uint64>(CreateDesc.NumMips) * CreateDesc.ArraySize;
		if (SubresourceCount > std::numeric_limits<uint32>::max()) return Fail("Texture subresource count exceeds the portable range.");

		OutError.clear();
		return true;
	}

	auto ValidateTexture2DUpdate(
		const FRHITextureDesc& TextureDesc,
		uint32 MipIndex,
		uint32 ArraySlice,
		const FUpdateTextureRegion2D& UpdateRegion,
		uint32 SourcePitch,
		std::string& OutError
	) -> bool
	{
		auto Fail = [&OutError](std::string Message) {
			OutError = std::move(Message);
			return false;
		};

		if (MipIndex >= TextureDesc.NumMips) return Fail("Texture upload mip index is outside the texture mip range.");
		if (ArraySlice >= TextureDesc.ArraySize) return Fail("Texture upload array slice is outside the texture layer range.");
		if (UpdateRegion.SrcX < 0 || UpdateRegion.SrcY < 0) return Fail("Texture upload source offsets must be nonnegative.");
		if (UpdateRegion.Width == 0 || UpdateRegion.Height == 0) return Fail("Texture upload region must be nonzero.");

		const uint32 MipWidth = std::max(1u, static_cast<uint32>(TextureDesc.Extent.x) >> MipIndex);
		const uint32 MipHeight = std::max(1u, static_cast<uint32>(TextureDesc.Extent.y) >> MipIndex);
		if (static_cast<uint64>(UpdateRegion.DestX) + UpdateRegion.Width > MipWidth
			|| static_cast<uint64>(UpdateRegion.DestY) + UpdateRegion.Height > MipHeight)
		{
			return Fail("Texture upload destination region exceeds the selected mip.");
		}

		const FPixelFormatInfo& FormatInfo = GetPixelFormatInfo(TextureDesc.Format);
		if (FormatInfo.BytesPerBlock == 0 || FormatInfo.BlockSize == 0)
		{
			return Fail("Texture upload requires a valid pixel format layout.");
		}

		const uint32 BlockSize = FormatInfo.BlockSize;
		if (UpdateRegion.DestX % BlockSize != 0 || UpdateRegion.DestY % BlockSize != 0
			|| static_cast<uint32>(UpdateRegion.SrcX) % BlockSize != 0
			|| static_cast<uint32>(UpdateRegion.SrcY) % BlockSize != 0)
		{
			return Fail("Texture upload source and destination offsets must be block-aligned.");
		}
		if ((UpdateRegion.Width % BlockSize != 0 && UpdateRegion.DestX + UpdateRegion.Width != MipWidth)
			|| (UpdateRegion.Height % BlockSize != 0 && UpdateRegion.DestY + UpdateRegion.Height != MipHeight))
		{
			return Fail("Texture upload dimensions must be block-aligned unless the region reaches the mip edge.");
		}

		const uint64 SourceBlockX = static_cast<uint32>(UpdateRegion.SrcX) / BlockSize;
		const uint64 RegionBlocksWide = (static_cast<uint64>(UpdateRegion.Width) + BlockSize - 1) / BlockSize;
		const uint64 RequiredPitch = (SourceBlockX + RegionBlocksWide) * FormatInfo.BytesPerBlock;
		if (RequiredPitch > SourcePitch) return Fail("Texture upload source pitch is too small for the requested source region.");

		OutError.clear();
		return true;
	}

	auto ResolveTextureCubeFaceUv(const FVector3& Direction, ETextureCubeFace& OutFace, FVector2f& OutUv) -> bool
	{
		const double AbsX = std::abs(Direction.x);
		const double AbsY = std::abs(Direction.y);
		const double AbsZ = std::abs(Direction.z);
		const double MajorAxis = std::max({AbsX, AbsY, AbsZ});
		if (!std::isfinite(MajorAxis) || MajorAxis <= 0.0) return false;

		double Sc = 0.0;
		double Tc = 0.0;
		if (AbsX >= AbsY && AbsX >= AbsZ)
		{
			if (Direction.x >= 0.0)
			{
				OutFace = ETextureCubeFace::PositiveX;
				Sc = -Direction.z;
				Tc = -Direction.y;
			}
			else
			{
				OutFace = ETextureCubeFace::NegativeX;
				Sc = Direction.z;
				Tc = -Direction.y;
			}
		}
		else if (AbsY >= AbsZ)
		{
			if (Direction.y >= 0.0)
			{
				OutFace = ETextureCubeFace::PositiveY;
				Sc = Direction.x;
				Tc = Direction.z;
			}
			else
			{
				OutFace = ETextureCubeFace::NegativeY;
				Sc = Direction.x;
				Tc = -Direction.z;
			}
		}
		else if (Direction.z >= 0.0)
		{
			OutFace = ETextureCubeFace::PositiveZ;
			Sc = Direction.x;
			Tc = -Direction.y;
		}
		else
		{
			OutFace = ETextureCubeFace::NegativeZ;
			Sc = -Direction.x;
			Tc = -Direction.y;
		}

		OutUv = FVector2f(
			static_cast<float>((Sc / MajorAxis + 1.0) * 0.5),
			static_cast<float>((Tc / MajorAxis + 1.0) * 0.5)
		);
		return true;
	}

	auto ResolveTextureCubeFacePixelDirection(ETextureCubeFace Face, uint32 PixelX, uint32 PixelY,
		uint32 FaceDimension, FVector3& OutDirection) -> bool
	{
		if (FaceDimension == 0 || PixelX >= FaceDimension || PixelY >= FaceDimension) return false;
		const double A = 2.0 * (static_cast<double>(PixelX) + 0.5) / FaceDimension - 1.0;
		const double B = 2.0 * (static_cast<double>(PixelY) + 0.5) / FaceDimension - 1.0;
		switch (Face)
		{
		case ETextureCubeFace::PositiveX: OutDirection = FVector3(1.0, -B, -A); break;
		case ETextureCubeFace::NegativeX: OutDirection = FVector3(-1.0, -B, A); break;
		case ETextureCubeFace::PositiveY: OutDirection = FVector3(A, 1.0, B); break;
		case ETextureCubeFace::NegativeY: OutDirection = FVector3(A, -1.0, -B); break;
		case ETextureCubeFace::PositiveZ: OutDirection = FVector3(A, -B, 1.0); break;
		case ETextureCubeFace::NegativeZ: OutDirection = FVector3(-A, -B, -1.0); break;
		default: return false;
		}
		const double Length = Math::Length(OutDirection);
		if (!std::isfinite(Length) || Length <= 0.0) return false;
		OutDirection /= Length;
		return true;
	}

	// May use a multiple producer single consumer queue here if the contention is high, but currently we don't have that many threads creating resources, so a simple vector with mutex should be fine.
	std::vector<FRHIResource*> PendingDeletes;
	std::mutex PendingDeletesMutex;

#if DO_CHECK
	// This pointer will be set before any FRHIResource being deleted, then it will be checked and reset in the destructor of FRHIResource.
	// This is to catch any unexpected deletion, such as deleting a resource manually without calling DeleteResources.
	thread_local const FRHIResource* CurrentDeleting = nullptr;
#endif

	FRHIResource::FRHIResource(ERHIResourceType InResourceType)
		: ResourceType(InResourceType)
	{
	}

	FRHIResource::~FRHIResource()
	{
#if DO_CHECK
		// A derived constructor may fail before the resource is ever referenced.
		// Published resources must still arrive through DeleteResources.
		check(IsEngineExitRequested() || CurrentDeleting == this
			|| AtomicFlags.IsUnpublished(std::memory_order_relaxed));
		if (CurrentDeleting == this)
		{
			CurrentDeleting = nullptr;
		}
#endif
	}

	auto FRHIResource::EnqueueForDelete() const -> void
	{
		std::lock_guard<std::mutex> lock(PendingDeletesMutex);
		PendingDeletes.push_back(const_cast<FRHIResource*>(this));
	}

	auto FRHIResource::DeleteResources(const std::vector<FRHIResource*>& ResourcesToDelete) -> void
	{
		for (FRHIResource* Resource : ResourcesToDelete)
		{
			const bool bBeganDeleting = Resource->AtomicFlags.BeginDelete();
			if (!bBeganDeleting)
			{
				checkf(false,
					"Deferred RHI resource was not in the pending-delete state.");
				std::terminate();
			}
#if DO_CHECK
			CurrentDeleting = Resource;
#endif
			delete Resource;
#if DO_CHECK
			check(CurrentDeleting == nullptr);
#endif
		}
	}

	auto FRHIResource::GatherResourcesToDelete(std::vector<FRHIResource*>& OutResourcesToDelete) -> void
	{
		std::vector<FRHIResource*> LocalTemp;
		{
			std::lock_guard<std::mutex> Lock(PendingDeletesMutex);
			LocalTemp.swap(PendingDeletes);
		}
		OutResourcesToDelete.insert(
			OutResourcesToDelete.end(),
			std::make_move_iterator(LocalTemp.begin()),
			std::make_move_iterator(LocalTemp.end())
		);
	}

	auto FRHIResource::GetNumPendingDeletes() -> size_t
	{
		std::lock_guard<std::mutex> Lock(PendingDeletesMutex);
		return PendingDeletes.size();
	}

} // namespace Durin
