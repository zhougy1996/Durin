#include "MetalResourceDescriptors.h"
#include "MetalAutoreleasePool.h"

namespace Durin
{
	auto IsMetalColorRenderFormat(EPixelFormat Format) -> bool
	{
		return Format == EPixelFormat::R8_UNORM
			|| Format == EPixelFormat::RGBA8_UNORM
			|| Format == EPixelFormat::BGRA8_UNORM
			|| Format == EPixelFormat::SRGBA8_UNORM
			|| Format == EPixelFormat::SBGRA8_UNORM
			|| Format == EPixelFormat::R11G11B10_FLOAT
			|| Format == EPixelFormat::RGBA16_FLOAT
			|| Format == EPixelFormat::RG32_UINT;
	}

	namespace
	{
		auto ToMetalAddressMode(ESamplerAddressMode Mode)
			-> std::optional<MTL::SamplerAddressMode>
		{
			switch (Mode)
			{
			case ESamplerAddressMode::Repeat: return MTL::SamplerAddressModeRepeat;
			case ESamplerAddressMode::MirroredRepeat: return MTL::SamplerAddressModeMirrorRepeat;
			case ESamplerAddressMode::ClampToEdge: return MTL::SamplerAddressModeClampToEdge;
			case ESamplerAddressMode::ClampToBorder: return MTL::SamplerAddressModeClampToBorderColor;
			}
			return std::nullopt;
		}

		auto ToMetalSamplerCompare(ESamplerCompareOp Op)
			-> std::optional<MTL::CompareFunction>
		{
			switch (Op)
			{
			case ESamplerCompareOp::Never: return MTL::CompareFunctionNever;
			case ESamplerCompareOp::Less: return MTL::CompareFunctionLess;
			case ESamplerCompareOp::Equal: return MTL::CompareFunctionEqual;
			case ESamplerCompareOp::LessOrEqual: return MTL::CompareFunctionLessEqual;
			case ESamplerCompareOp::Greater: return MTL::CompareFunctionGreater;
			case ESamplerCompareOp::NotEqual: return MTL::CompareFunctionNotEqual;
			case ESamplerCompareOp::GreaterOrEqual: return MTL::CompareFunctionGreaterEqual;
			case ESamplerCompareOp::Always: return MTL::CompareFunctionAlways;
			}
			return std::nullopt;
		}
	}

	auto ToMetalPixelFormat(EPixelFormat Format) -> MTL::PixelFormat
	{
		switch (Format)
		{
		case EPixelFormat::R8_UNORM: return MTL::PixelFormatR8Unorm;
		case EPixelFormat::RG8_UNORM: return MTL::PixelFormatRG8Unorm;
		case EPixelFormat::R16_FLOAT: return MTL::PixelFormatR16Float;
		case EPixelFormat::RGBA8_UNORM: return MTL::PixelFormatRGBA8Unorm;
		case EPixelFormat::BGRA8_UNORM: return MTL::PixelFormatBGRA8Unorm;
		case EPixelFormat::SRGBA8_UNORM: return MTL::PixelFormatRGBA8Unorm_sRGB;
		case EPixelFormat::SBGRA8_UNORM: return MTL::PixelFormatBGRA8Unorm_sRGB;
		case EPixelFormat::R11G11B10_FLOAT: return MTL::PixelFormatRG11B10Float;
		case EPixelFormat::RGBA16_FLOAT: return MTL::PixelFormatRGBA16Float;
		case EPixelFormat::RGBA32_FLOAT: return MTL::PixelFormatRGBA32Float;
		case EPixelFormat::RG32_UINT: return MTL::PixelFormatRG32Uint;
		case EPixelFormat::D32: return MTL::PixelFormatDepth32Float;
		case EPixelFormat::BC1_UNORM: return MTL::PixelFormatBC1_RGBA;
		case EPixelFormat::BC1_UNORM_SRGB: return MTL::PixelFormatBC1_RGBA_sRGB;
		default: return MTL::PixelFormatInvalid;
		}
	}

	auto MakeMetalTextureDescriptor(const FRHITextureCreateDesc& Desc)
		-> NS::SharedPtr<MTL::TextureDescriptor>
	{
		const FMetalAutoreleasePool Pool;
		auto Native = NS::RetainPtr(MTL::TextureDescriptor::texture2DDescriptor(
			ToMetalPixelFormat(Desc.Format), Desc.Extent.x, Desc.Extent.y, false));
		if (!Native) return {};
		Native->setMipmapLevelCount(Desc.NumMips);
		switch (Desc.Dimension)
		{
		case ETextureDimension::Texture2D: break;
		case ETextureDimension::TextureCube:
			Native->setTextureType(MTL::TextureTypeCube); break;
		case ETextureDimension::TextureCubeArray:
			Native->setTextureType(MTL::TextureTypeCubeArray);
			Native->setArrayLength(Desc.ArraySize / TextureCubeFaceCount); break;
		case ETextureDimension::Texture2DArray:
			Native->setTextureType(MTL::TextureType2DArray);
			Native->setArrayLength(Desc.ArraySize); break;
		case ETextureDimension::Texture3D:
			Native->setTextureType(MTL::TextureType3D);
			Native->setDepth(Desc.Depth); break;
		default: return {};
		}
		Native->setStorageMode(MTL::StorageModePrivate);
		MTL::TextureUsage Usage = MTL::TextureUsageUnknown;
		if (EnumHasAnyFlags(Desc.Flags, ETextureCreateFlags::RenderTargetable
			| ETextureCreateFlags::DepthStencilTargetable))
			Usage |= MTL::TextureUsageRenderTarget;
		if (EnumHasAnyFlags(Desc.Flags, ETextureCreateFlags::ShaderResource))
			Usage |= MTL::TextureUsageShaderRead;
		if (EnumHasAnyFlags(Desc.Flags, ETextureCreateFlags::Storage))
			Usage |= MTL::TextureUsageShaderWrite;
		Native->setUsage(Usage);
		return Native;
	}

	auto MakeMetalSamplerDescriptor(const FRHISamplerDesc& Desc)
		-> NS::SharedPtr<MTL::SamplerDescriptor>
	{
		const FMetalAutoreleasePool Pool;
		if (!std::isfinite(Desc.MinLod) || !std::isfinite(Desc.MaxLod)
			|| Desc.MinLod < 0.0f || Desc.MaxLod < Desc.MinLod
			|| !std::isfinite(Desc.MipLodBias) || Desc.MipLodBias < -16.0f
			|| Desc.MipLodBias > 15.999f || Desc.bUnnormalizedCoordinates
			|| (Desc.MinFilter != ESamplerFilter::Nearest
				&& Desc.MinFilter != ESamplerFilter::Linear)
			|| (Desc.MagFilter != ESamplerFilter::Nearest
				&& Desc.MagFilter != ESamplerFilter::Linear)
			|| (Desc.MipmapMode != ESamplerMipmapMode::Nearest
				&& Desc.MipmapMode != ESamplerMipmapMode::Linear)
			|| !std::isfinite(Desc.MaxAnisotropy)
			|| (Desc.bEnableAnisotropy && (Desc.MaxAnisotropy < 1.0f
				|| Desc.MaxAnisotropy > 16.0f
				|| std::floor(Desc.MaxAnisotropy) != Desc.MaxAnisotropy)))
			return {};
		const auto U = ToMetalAddressMode(Desc.AddressU);
		const auto V = ToMetalAddressMode(Desc.AddressV);
		const auto W = ToMetalAddressMode(Desc.AddressW);
		const auto Compare = ToMetalSamplerCompare(Desc.CompareOp);
		if (!U || !V || !W || !Compare) return {};
		auto Native = NS::TransferPtr(MTL::SamplerDescriptor::alloc()->init());
		if (!Native) return {};
		Native->setMinFilter(Desc.MinFilter == ESamplerFilter::Linear
			? MTL::SamplerMinMagFilterLinear : MTL::SamplerMinMagFilterNearest);
		Native->setMagFilter(Desc.MagFilter == ESamplerFilter::Linear
			? MTL::SamplerMinMagFilterLinear : MTL::SamplerMinMagFilterNearest);
		Native->setMipFilter(Desc.MipmapMode == ESamplerMipmapMode::Linear
			? MTL::SamplerMipFilterLinear : MTL::SamplerMipFilterNearest);
		Native->setSAddressMode(*U);
		Native->setTAddressMode(*V);
		Native->setRAddressMode(*W);
		Native->setMaxAnisotropy(Desc.bEnableAnisotropy
			? static_cast<NS::UInteger>(Desc.MaxAnisotropy) : 1);
		Native->setLodMinClamp(Desc.MinLod);
		Native->setLodMaxClamp(Desc.MaxLod);
		Native->setLodBias(Desc.MipLodBias);
		Native->setCompareFunction(Desc.bEnableCompare ? *Compare : MTL::CompareFunctionNever);
		switch (Desc.BorderColor)
		{
		case ESamplerBorderColor::FloatTransparentBlack:
		case ESamplerBorderColor::IntTransparentBlack:
			Native->setBorderColor(MTL::SamplerBorderColorTransparentBlack); break;
		case ESamplerBorderColor::FloatOpaqueBlack:
		case ESamplerBorderColor::IntOpaqueBlack:
			Native->setBorderColor(MTL::SamplerBorderColorOpaqueBlack); break;
		case ESamplerBorderColor::FloatOpaqueWhite:
		case ESamplerBorderColor::IntOpaqueWhite:
			Native->setBorderColor(MTL::SamplerBorderColorOpaqueWhite); break;
		default: return {};
		}
		return Native;
	}

	auto MakeMetalTextureView(MTL::Texture* Source, const FRHITextureViewDesc& Desc)
		-> NS::SharedPtr<MTL::Texture>
	{
		const FMetalAutoreleasePool Pool;
		if (!Source) return {};
		MTL::TextureType Type = MTL::TextureType2D;
		if (Desc.Usage == ERHITextureViewUsage::Storage)
		{
			if (Desc.Dimension != ERHITextureViewDimension::Texture2D) return {};
		}
		else switch (Desc.Dimension)
		{
		case ERHITextureViewDimension::Texture2D: break;
		case ERHITextureViewDimension::Texture2DArray: Type = MTL::TextureType2DArray; break;
		case ERHITextureViewDimension::TextureCube: Type = MTL::TextureTypeCube; break;
		case ERHITextureViewDimension::TextureCubeArray: Type = MTL::TextureTypeCubeArray; break;
		case ERHITextureViewDimension::Texture3D: Type = MTL::TextureType3D; break;
		default: return {};
		}
		return NS::TransferPtr(Source->newTextureView(ToMetalPixelFormat(Desc.Format), Type,
			NS::Range::Make(Desc.Range.FirstMip, Desc.Range.NumMips),
			NS::Range::Make(Desc.Range.FirstArrayLayer, Desc.Range.NumArrayLayers)));
	}
}
