#include "RHIResources.h"

#include <gtest/gtest.h>
#include <thread>

namespace Durin
{
	TEST(FRHIResourceViewValidationTests, ConcurrentFinalReleaseDefersParentsToDeletionOwner)
	{
		const auto Owner = std::this_thread::get_id();
		uint32 Destroyed = 0;
		class FTrackedResource : public FRHIResource
		{
		public:
			FTrackedResource(std::thread::id InOwner, uint32& InDestroyed,
				TRefCountPtr<FRHIResource> InParent = {})
				: FRHIResource(ERHIResourceType::PipelineState), Owner(InOwner),
				Destroyed(InDestroyed), Parent(std::move(InParent)) {}
			~FTrackedResource() override
			{
				EXPECT_EQ(std::this_thread::get_id(), Owner);
				++Destroyed;
			}
		private:
			std::thread::id Owner;
			uint32& Destroyed;
			TRefCountPtr<FRHIResource> Parent;
		};
		std::vector<FRHIResource*> Pending;
		while (FRHIResource::GetNumPendingDeletes())
		{
			FRHIResource::GatherResourcesToDelete(Pending);
			FRHIResource::DeleteResources(Pending);
			Pending.clear();
		}
		std::array<TRefCountPtr<FRHIResource>, 256> Children;
		for (auto& Child : Children)
		{
			auto Parent = MakeRefCount<FTrackedResource>(Owner, Destroyed);
			Child = MakeRefCount<FTrackedResource>(Owner, Destroyed, Parent);
		}
		{
			std::array<std::jthread, 4> Workers;
			for (size_t Worker = 0; Worker < Workers.size(); ++Worker)
				Workers[Worker] = std::jthread([&, Worker] {
					for (size_t Index = Worker; Index < Children.size(); Index += Workers.size())
						Children[Index] = nullptr;
				});
		}
		EXPECT_EQ(Destroyed, 0u);
		EXPECT_EQ(FRHIResource::GetNumPendingDeletes(), Children.size());
		FRHIResource::GatherResourcesToDelete(Pending);
		EXPECT_EQ(Pending.size(), Children.size());
		FRHIResource::DeleteResources(Pending);
		EXPECT_EQ(Destroyed, Children.size());
		EXPECT_EQ(FRHIResource::GetNumPendingDeletes(), Children.size());
		Pending.clear();
		FRHIResource::GatherResourcesToDelete(Pending);
		FRHIResource::DeleteResources(Pending);
		EXPECT_EQ(Destroyed, Children.size() * 2);
		EXPECT_EQ(FRHIResource::GetNumPendingDeletes(), 0u);
	}

	TEST(FRHIResourceViewValidationTests, ValidatesBufferRangesKindsAndDefaults)
	{
		FRHIBuffer Uniform(FRHIBufferCreateDesc::Create(
			"Uniform", 256, 16, EBufferUsageFlags::UniformBuffer));
		const FRHIBufferViewDesc Default = MakeDefaultBufferViewDesc(
			Uniform, ERHIBufferViewType::Uniform);
		EXPECT_EQ(Default, (FRHIBufferViewDesc{0, 256, ERHIBufferViewType::Uniform, EPixelFormat::Unknown}));
		const auto BufferViewDescResult = ValidateBufferViewDesc(&Uniform, Default);
		EXPECT_TRUE(BufferViewDescResult) << FormatRHIError(BufferViewDescResult.error());

		FRHIBufferViewDesc Invalid = Default;
		Invalid.Size = 0;
		const auto BufferViewDescResult2 = ValidateBufferViewDesc(&Uniform, Invalid);
		ASSERT_FALSE(BufferViewDescResult2);
		Invalid = Default;
		Invalid.Offset = 16;
		Invalid.Size = 256;
		const auto BufferViewDescResult3 = ValidateBufferViewDesc(&Uniform, Invalid);
		ASSERT_FALSE(BufferViewDescResult3);
		Invalid = Default;
		Invalid.Offset = 1;
		Invalid.Size = 16;
		const auto BufferViewDescResult4 = ValidateBufferViewDesc(&Uniform, Invalid);
		ASSERT_FALSE(BufferViewDescResult4);

		FRHIBuffer Structured(FRHIBufferCreateDesc::Create(
			"Structured", 96, 12, EBufferUsageFlags::StructuredBuffer));
		const auto BufferViewDescResult5 = ValidateBufferViewDesc(&Structured,
			{12, 36, ERHIBufferViewType::StructuredStorage, EPixelFormat::Unknown});
		EXPECT_TRUE(BufferViewDescResult5) << FormatRHIError(BufferViewDescResult5.error());
		const auto BufferViewDescResult6 = ValidateBufferViewDesc(&Structured,
			{4, 36, ERHIBufferViewType::StructuredStorage, EPixelFormat::Unknown});
		ASSERT_FALSE(BufferViewDescResult6);

		FRHIBuffer Formatted(FRHIBufferCreateDesc::Create(
			"Formatted", 64, 0, EBufferUsageFlags::FormattedBuffer));
		const auto BufferViewDescResult7 = ValidateBufferViewDesc(&Formatted,
			{4, 16, ERHIBufferViewType::Formatted, EPixelFormat::R32_FLOAT});
		EXPECT_TRUE(BufferViewDescResult7) << FormatRHIError(BufferViewDescResult7.error());
		const auto BufferViewDescResult8 = ValidateBufferViewDesc(&Formatted,
			{0, 16, ERHIBufferViewType::Formatted, EPixelFormat::BC1_UNORM});
		ASSERT_FALSE(BufferViewDescResult8);
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
		const auto TextureViewDescResult = ValidateTextureViewDesc(&Texture, Default);
		EXPECT_TRUE(TextureViewDescResult) << FormatRHIError(TextureViewDescResult.error());

		FRHITextureViewDesc Invalid = Default;
		Invalid.Format = EPixelFormat::BGRA8_UNORM;
		const auto TextureViewDescResult2 = ValidateTextureViewDesc(&Texture, Invalid);
		ASSERT_FALSE(TextureViewDescResult2);
		Invalid = Default;
		Invalid.Range.NumMips = 0;
		const auto TextureViewDescResult3 = ValidateTextureViewDesc(&Texture, Invalid);
		ASSERT_FALSE(TextureViewDescResult3);
		Invalid = Default;
		Invalid.Range.Aspects = ERHITextureAspect::Depth;
		const auto TextureViewDescResult4 = ValidateTextureViewDesc(&Texture, Invalid);
		ASSERT_FALSE(TextureViewDescResult4);

		FRHITexture Cube(FRHITextureCreateDesc::CreateCube("Cube")
			.SetExtent(16)
			.SetNumMips(2)
			.SetFormat(EPixelFormat::RGBA8_UNORM)
			.SetFlags(ETextureCreateFlags::ShaderResource));
		const FRHITextureViewDesc CubeDefault = MakeDefaultTextureViewDesc(
			Cube, ERHITextureViewUsage::Sampled);
		EXPECT_EQ(CubeDefault.Dimension, ERHITextureViewDimension::TextureCube);
		const auto TextureViewDescResult5 = ValidateTextureViewDesc(&Cube, CubeDefault);
		EXPECT_TRUE(TextureViewDescResult5) << FormatRHIError(TextureViewDescResult5.error());
		FRHITextureViewDesc Face = CubeDefault;
		Face.Dimension = ERHITextureViewDimension::Texture2D;
		Face.Range.FirstArrayLayer = 5;
		Face.Range.NumArrayLayers = 1;
		const auto TextureViewDescResult6 = ValidateTextureViewDesc(&Cube, Face);
		EXPECT_TRUE(TextureViewDescResult6) << FormatRHIError(TextureViewDescResult6.error());
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

	TEST(FRHIResourceViewValidationTests,
		ValidatesDirectionalShadowDepthAttachmentAndSampledViews)
	{
		FRHITexture Shadow(FRHITextureCreateDesc::Create2DArray(
			"DirectionalShadowArray")
			.SetExtent(2048, 2048)
			.SetArraySize(3)
			.SetFormat(EPixelFormat::D32)
			.SetFlags(ETextureCreateFlags::DepthStencilTargetable
				| ETextureCreateFlags::ShaderResource));
		const FRHITextureViewDesc Sampled = MakeDefaultTextureViewDesc(
			Shadow, ERHITextureViewUsage::Sampled);
		EXPECT_EQ(Sampled.Range, (FRHITextureSubresourceRange{
			ERHITextureAspect::Depth, 0, 1, 0, 3}));
		EXPECT_EQ(Sampled.Dimension, ERHITextureViewDimension::Texture2DArray);
		const auto TextureViewDescResult = ValidateTextureViewDesc(&Shadow, Sampled);
		EXPECT_TRUE(TextureViewDescResult) << FormatRHIError(TextureViewDescResult.error());
		for (uint32 Layer = 0; Layer < 3; ++Layer)
		{
			FRHITextureViewDesc Attachment = MakeDefaultTextureViewDesc(
				Shadow, ERHITextureViewUsage::DepthStencilAttachment);
			Attachment.Dimension = ERHITextureViewDimension::Texture2D;
			Attachment.Range.FirstArrayLayer = Layer;
			Attachment.Range.NumArrayLayers = 1;
			EXPECT_EQ(Attachment.Range, (FRHITextureSubresourceRange{
				ERHITextureAspect::Depth, 0, 1, Layer, 1}));
			const auto TextureViewDescResult2 = ValidateTextureViewDesc(&Shadow, Attachment);
			EXPECT_TRUE(TextureViewDescResult2) << FormatRHIError(TextureViewDescResult2.error());
		}

		FRHITexture SampleOnly(FRHITextureCreateDesc::Create2D(
			"SampleOnlyDepth", 16, 16, EPixelFormat::D32)
			.SetFlags(ETextureCreateFlags::ShaderResource));
		const auto TextureViewDescResult3 = ValidateTextureViewDesc(
			&SampleOnly,
			MakeDefaultTextureViewDesc(
				SampleOnly, ERHITextureViewUsage::DepthStencilAttachment));
		ASSERT_FALSE(TextureViewDescResult3);
	}
} // namespace Durin
