#include "RHIResources.h"

#include <gtest/gtest.h>

namespace Durin
{
	TEST(FRHIResourceViewValidationTests, ValidatesBufferRangesKindsAndDefaults)
	{
		FRHIBuffer Uniform(FRHIBufferCreateDesc::Create(
			"Uniform", 256, 16, EBufferUsageFlags::UniformBuffer));
		const FRHIBufferViewDesc Default = MakeDefaultBufferViewDesc(
			Uniform, ERHIBufferViewType::Uniform);
		EXPECT_EQ(Default, (FRHIBufferViewDesc{0, 256, ERHIBufferViewType::Uniform, EPixelFormat::Unknown}));
		std::string Error;
		EXPECT_TRUE(ValidateBufferViewDesc(&Uniform, Default, Error)) << Error;

		FRHIBufferViewDesc Invalid = Default;
		Invalid.Size = 0;
		EXPECT_FALSE(ValidateBufferViewDesc(&Uniform, Invalid, Error));
		Invalid = Default;
		Invalid.Offset = 16;
		Invalid.Size = 256;
		EXPECT_FALSE(ValidateBufferViewDesc(&Uniform, Invalid, Error));
		Invalid = Default;
		Invalid.Offset = 1;
		Invalid.Size = 16;
		EXPECT_FALSE(ValidateBufferViewDesc(&Uniform, Invalid, Error));

		FRHIBuffer Structured(FRHIBufferCreateDesc::Create(
			"Structured", 96, 12, EBufferUsageFlags::StructuredBuffer));
		EXPECT_TRUE(ValidateBufferViewDesc(&Structured,
			{12, 36, ERHIBufferViewType::StructuredStorage, EPixelFormat::Unknown}, Error)) << Error;
		EXPECT_FALSE(ValidateBufferViewDesc(&Structured,
			{4, 36, ERHIBufferViewType::StructuredStorage, EPixelFormat::Unknown}, Error));

		FRHIBuffer Formatted(FRHIBufferCreateDesc::Create(
			"Formatted", 64, 0, EBufferUsageFlags::FormattedBuffer));
		EXPECT_TRUE(ValidateBufferViewDesc(&Formatted,
			{4, 16, ERHIBufferViewType::Formatted, EPixelFormat::R32_FLOAT}, Error)) << Error;
		EXPECT_FALSE(ValidateBufferViewDesc(&Formatted,
			{0, 16, ERHIBufferViewType::Formatted, EPixelFormat::BC1_UNORM}, Error));
	}

	TEST(FRHIResourceViewValidationTests, ValidatesTextureUsageDimensionAndSubresources)
	{
		FRHITexture Texture(FRHITextureCreateDesc::Create2D(
			"Sampled", 64, 32, EPixelFormat::RGBA8_UNORM)
			.SetNumMips(4)
			.SetFlags(ETextureCreateFlags::ShaderResource | ETextureCreateFlags::DestinationCopy));
		const FRHITextureViewDesc Default = MakeDefaultTextureViewDesc(
			Texture, ERHITextureViewUsage::Sampled);
		EXPECT_EQ(Default.Range, (FRHITextureSubresourceRange{
			ERHITextureAspect::Color, 0, 4, 0, 1}));
		std::string Error;
		EXPECT_TRUE(ValidateTextureViewDesc(&Texture, Default, Error)) << Error;

		FRHITextureViewDesc Invalid = Default;
		Invalid.Format = EPixelFormat::BGRA8_UNORM;
		EXPECT_FALSE(ValidateTextureViewDesc(&Texture, Invalid, Error));
		Invalid = Default;
		Invalid.Range.NumMips = 0;
		EXPECT_FALSE(ValidateTextureViewDesc(&Texture, Invalid, Error));
		Invalid = Default;
		Invalid.Range.Aspects = ERHITextureAspect::Depth;
		EXPECT_FALSE(ValidateTextureViewDesc(&Texture, Invalid, Error));

		FRHITexture Cube(FRHITextureCreateDesc::CreateCube("Cube")
			.SetExtent(16)
			.SetNumMips(2)
			.SetFormat(EPixelFormat::RGBA8_UNORM)
			.SetFlags(ETextureCreateFlags::ShaderResource));
		const FRHITextureViewDesc CubeDefault = MakeDefaultTextureViewDesc(
			Cube, ERHITextureViewUsage::Sampled);
		EXPECT_EQ(CubeDefault.Dimension, ERHITextureViewDimension::TextureCube);
		EXPECT_TRUE(ValidateTextureViewDesc(&Cube, CubeDefault, Error)) << Error;
		FRHITextureViewDesc Face = CubeDefault;
		Face.Dimension = ERHITextureViewDimension::Texture2D;
		Face.Range.FirstArrayLayer = 5;
		Face.Range.NumArrayLayers = 1;
		EXPECT_TRUE(ValidateTextureViewDesc(&Cube, Face, Error)) << Error;
	}

	TEST(FRHIResourceViewValidationTests, RetainsParentResources)
	{
		TRefCountPtr<FRHIBuffer> Buffer = new FRHIBuffer(FRHIBufferCreateDesc::Create(
			"Lifetime", 64, 16, EBufferUsageFlags::UniformBuffer));
		FRHIBuffer* Parent = Buffer.GetReference();
		TRefCountPtr<FRHIBufferView> View = new FRHIBufferView(
			Parent, MakeDefaultBufferViewDesc(*Parent, ERHIBufferViewType::Uniform));
		EXPECT_EQ(Parent->GetRefCount(), 2u);
		Buffer = nullptr;
		EXPECT_EQ(Parent->GetRefCount(), 1u);
		EXPECT_EQ(View->GetBuffer(), Parent);
		View = nullptr;

		std::vector<FRHIResource*> Pending;
		FRHIResource::GatherResourcesToDelete(Pending);
		FRHIResource::DeleteResources(Pending);
		Pending.clear();
		FRHIResource::GatherResourcesToDelete(Pending);
		FRHIResource::DeleteResources(Pending);
	}
} // namespace Durin
