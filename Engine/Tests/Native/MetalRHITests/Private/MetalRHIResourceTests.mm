#import <Metal/Metal.h>

#include "DynamicRHI.h"
#include "MetalBuffer.h"
#include "MetalSampler.h"
#include "MetalTexture.h"
#include "RHICommandList.h"
#include "RHIGlobals.h"

#include <gtest/gtest.h>

namespace
{
	class FScopedEnvironmentVariable
	{
	public:
		FScopedEnvironmentVariable(const char* InName, const char* Value) : Name(InName)
		{
			if (const char* Previous = std::getenv(InName)) Original = Previous;
			setenv(InName, Value, 1);
		}
		~FScopedEnvironmentVariable()
		{
			if (Original) setenv(Name, Original->c_str(), 1);
			else unsetenv(Name);
		}
	private:
		const char* Name;
		std::optional<std::string> Original;
	};

	struct FScopedRHIExit
	{
		~FScopedRHIExit()
		{
			if (Durin::GDynamicRHI) Durin::RHIExit();
		}
	};
}

TEST(FMetalRHIBufferTests, UploadThenCopyRetainsBuffersUntilGPUCompletion)
{
	@autoreleasepool
	{
		FScopedEnvironmentVariable Backend("DURIN_RHI_BACKEND", "metal");
		for (const char* Mode : {"inline", "threaded"})
		{
			SCOPED_TRACE(Mode);
			FScopedEnvironmentVariable Execution("DURIN_RHI_EXECUTION", Mode);
			ASSERT_TRUE(Durin::RHIInit(Durin::FRHIInitializationContext::Headless()));
			FScopedRHIExit Exit;
			auto& Commands = Durin::FRHICommandListImmediate::Get();
			constexpr uint32_t Size = 16;
			const uint8_t Initial[Size] = {17, 18, 19, 20, 21, 22, 23, 24,
				25, 26, 27, 28, 29, 30, 31, 32};
			auto SourceDesc = Durin::FRHIBufferCreateDesc::Create("Metal source", Size, 1,
				Durin::EBufferUsageFlags::SourceCopy | Durin::EBufferUsageFlags::DestinationCopy);
			SourceDesc.InitialData = {.Data = Initial, .Size = Size};
			const auto DestinationDesc = Durin::FRHIBufferCreateDesc::Create("Metal destination", Size, 1,
				Durin::EBufferUsageFlags::DestinationCopy | Durin::EBufferUsageFlags::KeepCPUAccessible);
			auto SourceResult = Durin::GDynamicRHI->RHITryCreateBuffer(Commands, SourceDesc);
			auto DestinationResult = Durin::GDynamicRHI->RHITryCreateBuffer(Commands, DestinationDesc);
			ASSERT_TRUE(SourceResult.has_value());
			ASSERT_TRUE(DestinationResult.has_value());
			auto Source = std::move(*SourceResult);
			auto Destination = std::move(*DestinationResult);
			const auto* InitialBytes = static_cast<const uint8_t*>(
				[static_cast<Durin::FMetalBuffer*>(Source.GetReference())->GetHandle() contents]);
			ASSERT_NE(InitialBytes, nullptr);
			EXPECT_EQ(std::memcmp(InitialBytes, Initial, Size), 0);
			const uint8_t Pattern[Size] = {1, 3, 5, 7, 9, 11, 13, 15,
				2, 4, 6, 8, 10, 12, 14, 16};
			const auto Signal = Commands.BeginGPUSubmission(
				{.Queue = Durin::GDynamicRHI->RHIGetQueueCapabilities().Graphics});
			Commands.WriteBuffer(Source.GetReference(), Pattern, Size, 0);
			const Durin::FRHIBufferCopyRegion Copy{.SourceOffset = 0,
				.DestinationOffset = 0, .Size = Size};
			Commands.CopyBuffer(Source.GetReference(), Destination.GetReference(), {&Copy, 1});
			Commands.EndGPUSubmission();
			Source = nullptr;
			Commands.ImmediateFlush(Durin::EImmediateFlushType::FlushRHIThread);
			EXPECT_EQ(Signal.GetState(), Durin::ERHIGPUSubmissionState::Pending);
			Commands.ImmediateFlush(Durin::EImmediateFlushType::FlushRHIThread,
				Durin::ERHISubmitFlags::SubmitToGPU);
			ASSERT_EQ(Durin::GDynamicRHI->RHIWaitForCompletion(Signal, 1'000'000'000),
				Durin::ERHIGPUWaitResult::Complete);
			const auto* Bytes = static_cast<const uint8_t*>(
				[static_cast<Durin::FMetalBuffer*>(Destination.GetReference())->GetHandle() contents]);
			ASSERT_NE(Bytes, nullptr);
			EXPECT_EQ(std::memcmp(Bytes, Pattern, Size), 0);
			Destination = nullptr;
			Durin::RHIExit();
		}
	}
}

TEST(FMetalRHIBufferTests, InvalidDescriptorsReturnRecoverableErrors)
{
	@autoreleasepool
	{
		FScopedEnvironmentVariable Backend("DURIN_RHI_BACKEND", "metal");
		ASSERT_TRUE(Durin::RHIInit(Durin::FRHIInitializationContext::Headless()));
		FScopedRHIExit Exit;
		auto& Commands = Durin::FRHICommandListImmediate::Get();
		const auto Empty = Durin::FRHIBufferCreateDesc::Create("Empty", 0, 1,
			Durin::EBufferUsageFlags::SourceCopy);
		const auto Result = Durin::GDynamicRHI->RHITryCreateBuffer(Commands, Empty);
		ASSERT_FALSE(Result.has_value());
		EXPECT_EQ(Result.error().Failure,
			Durin::ERHIResourceCreationFailure::UnsupportedDescriptor);
		Durin::RHIExit();
	}
}

TEST(FMetalRHIBufferTests, SamplersCreateWithSupportedState)
{
	@autoreleasepool
	{
		FScopedEnvironmentVariable Backend("DURIN_RHI_BACKEND", "metal");
		ASSERT_TRUE(Durin::RHIInit(Durin::FRHIInitializationContext::Headless()));
		FScopedRHIExit Exit;
		for (auto Desc : {Durin::FRHISamplerDesc::PointClamp(),
			Durin::FRHISamplerDesc::LinearRepeat(),
			Durin::FRHISamplerDesc::AnisotropicClamp()})
		{
			auto Sampler = Durin::GDynamicRHI->RHICreateSampler(Desc);
			ASSERT_NE(Sampler.GetReference(), nullptr);
			EXPECT_TRUE(Sampler->IsImmutable());
			EXPECT_NE(static_cast<Durin::FMetalSampler*>(Sampler.GetReference())->GetHandle(), nil);
		}
		auto CompareDesc = Durin::FRHISamplerDesc::PointClamp();
		CompareDesc.bEnableCompare = true;
		CompareDesc.CompareOp = Durin::ESamplerCompareOp::LessOrEqual;
		CompareDesc.AddressU = Durin::ESamplerAddressMode::ClampToBorder;
		EXPECT_NE(Durin::GDynamicRHI->RHICreateSampler(CompareDesc).GetReference(), nullptr);
		auto InvalidDesc = Durin::FRHISamplerDesc::PointClamp();
		InvalidDesc.bUnnormalizedCoordinates = true;
		EXPECT_EQ(Durin::GDynamicRHI->RHICreateSampler(InvalidDesc).GetReference(), nullptr);
		Durin::RHIExit();
	}
}

TEST(FMetalRHIBufferTests, BufferViewsValidateRangesAndRetainParent)
{
	@autoreleasepool
	{
		FScopedEnvironmentVariable Backend("DURIN_RHI_BACKEND", "metal");
		ASSERT_TRUE(Durin::RHIInit(Durin::FRHIInitializationContext::Headless()));
		FScopedRHIExit Exit;
		auto& Commands = Durin::FRHICommandListImmediate::Get();
		const auto Desc = Durin::FRHIBufferCreateDesc::Create("Uniform", 64, 16,
			Durin::EBufferUsageFlags::UniformBuffer);
		auto BufferResult = Durin::GDynamicRHI->RHITryCreateBuffer(Commands, Desc);
		ASSERT_TRUE(BufferResult.has_value());
		auto Buffer = std::move(*BufferResult);
		const Durin::FRHIBufferViewDesc ViewDesc{
			.Offset = 16, .Size = 32, .Type = Durin::ERHIBufferViewType::Uniform};
		auto View = Durin::GDynamicRHI->RHICreateBufferView(Buffer.GetReference(), ViewDesc);
		ASSERT_NE(View.GetReference(), nullptr);
		EXPECT_EQ(View->GetDesc(), ViewDesc);
		const Durin::FRHIBufferViewDesc InvalidDesc{
			.Offset = 60, .Size = 16, .Type = Durin::ERHIBufferViewType::Uniform};
		EXPECT_EQ(Durin::GDynamicRHI->RHICreateBufferView(Buffer.GetReference(), InvalidDesc)
			.GetReference(), nullptr);
		Buffer = nullptr;
		ASSERT_NE(View->GetBuffer(), nullptr);
		EXPECT_NE(static_cast<Durin::FMetalBuffer*>(View->GetBuffer())->GetHandle(), nil);
		View = nullptr;
		Durin::RHIExit();
	}
}

TEST(FMetalRHITextureTests, RecordedBufferTextureRoundTripCompletesOnGPU)
{
	@autoreleasepool
	{
		FScopedEnvironmentVariable Backend("DURIN_RHI_BACKEND", "metal");
		for (const char* Mode : {"inline", "threaded"})
		{
			SCOPED_TRACE(Mode);
			FScopedEnvironmentVariable Execution("DURIN_RHI_EXECUTION", Mode);
			ASSERT_TRUE(Durin::RHIInit(Durin::FRHIInitializationContext::Headless()));
			FScopedRHIExit Exit;
			auto& Commands = Durin::FRHICommandListImmediate::Get();
			constexpr uint32_t Width = 16;
			constexpr uint32_t Height = 16;
			constexpr uint32_t ByteCount = Width * Height * 4;
			uint8_t Pattern[ByteCount];
			for (uint32_t Index = 0; Index < ByteCount; ++Index)
				Pattern[Index] = static_cast<uint8_t>(Index * 13 + 7);
			auto TextureDesc = Durin::FRHITextureCreateDesc::Create2D(
				"Metal transfer texture", Width, Height, Durin::EPixelFormat::RGBA8_UNORM);
			TextureDesc.SetFlags(Durin::ETextureCreateFlags::SourceCopy
				| Durin::ETextureCreateFlags::DestinationCopy
				| Durin::ETextureCreateFlags::CPUReadback);
			ASSERT_TRUE(Durin::GDynamicRHI->RHIIsTextureSupported(TextureDesc));
			auto UnsupportedDesc = TextureDesc;
			UnsupportedDesc.AddFlags(Durin::ETextureCreateFlags::ShaderResource);
			EXPECT_FALSE(Durin::GDynamicRHI->RHIIsTextureSupported(UnsupportedDesc));
			const auto Unsupported = Durin::GDynamicRHI->RHITryCreateTexture(Commands, UnsupportedDesc);
			ASSERT_FALSE(Unsupported.has_value());
			EXPECT_EQ(Unsupported.error().Failure,
				Durin::ERHIResourceCreationFailure::UnsupportedDescriptor);
			auto FirstTextureResult = Durin::GDynamicRHI->RHITryCreateTexture(Commands, TextureDesc);
			auto SecondTextureResult = Durin::GDynamicRHI->RHITryCreateTexture(Commands, TextureDesc);
			ASSERT_TRUE(FirstTextureResult.has_value());
			ASSERT_TRUE(SecondTextureResult.has_value());
			auto FirstTexture = std::move(*FirstTextureResult);
			auto SecondTexture = std::move(*SecondTextureResult);
			const Durin::FRHITextureViewDesc ViewDesc{
				.Usage = Durin::ERHITextureViewUsage::TransferSource,
				.Dimension = Durin::ERHITextureViewDimension::Texture2D,
				.Format = Durin::EPixelFormat::RGBA8_UNORM,
				.Range = {Durin::ERHITextureAspect::Color, 0, 1, 0, 1}};
			auto View = Durin::GDynamicRHI->RHICreateTextureView(
				FirstTexture.GetReference(), ViewDesc);
			ASSERT_NE(View.GetReference(), nullptr);
			auto SampledViewDesc = ViewDesc;
			SampledViewDesc.Usage = Durin::ERHITextureViewUsage::Sampled;
			EXPECT_EQ(Durin::GDynamicRHI->RHICreateTextureView(
				FirstTexture.GetReference(), SampledViewDesc).GetReference(), nullptr);
			auto SourceDesc = Durin::FRHIBufferCreateDesc::Create("Texture source", ByteCount, 1,
				Durin::EBufferUsageFlags::SourceCopy);
			SourceDesc.InitialData = {.Data = Pattern, .Size = ByteCount};
			const auto DestinationDesc = Durin::FRHIBufferCreateDesc::Create("Texture readback", ByteCount, 1,
				Durin::EBufferUsageFlags::DestinationCopy | Durin::EBufferUsageFlags::KeepCPUAccessible);
			auto SourceResult = Durin::GDynamicRHI->RHITryCreateBuffer(Commands, SourceDesc);
			auto DestinationResult = Durin::GDynamicRHI->RHITryCreateBuffer(Commands, DestinationDesc);
			ASSERT_TRUE(SourceResult.has_value());
			ASSERT_TRUE(DestinationResult.has_value());
			auto Source = std::move(*SourceResult);
			auto Destination = std::move(*DestinationResult);
			const Durin::FRHIBufferTextureCopyRegion BufferRegion{
				.TextureExtent = {.Width = Width, .Height = Height, .Depth = 1}};
			const Durin::FRHITextureCopyRegion TextureRegion{
				.Extent = {.Width = Width, .Height = Height, .Depth = 1}};
			const auto Signal = Commands.BeginGPUSubmission(
				{.Queue = Durin::GDynamicRHI->RHIGetQueueCapabilities().Graphics});
			Commands.InitializeTexture(FirstTexture.GetReference());
			Commands.InitializeTexture(SecondTexture.GetReference());
			Commands.CopyBufferToTexture(Source.GetReference(), FirstTexture.GetReference(),
				{&BufferRegion, 1});
			Commands.CopyTexture(FirstTexture.GetReference(), SecondTexture.GetReference(),
				{&TextureRegion, 1});
			Commands.CopyTextureToBuffer(SecondTexture.GetReference(), Destination.GetReference(),
				{&BufferRegion, 1});
			auto AsyncReadback = Commands.EnqueueTextureReadback(
				SecondTexture.GetReference());
			Commands.EndGPUSubmission();
			Source = nullptr;
			FirstTexture = nullptr;
			EXPECT_NE(View->GetTexture(), nullptr);
			View = nullptr;
			Commands.ImmediateFlush(Durin::EImmediateFlushType::FlushRHIThread,
				Durin::ERHISubmitFlags::SubmitToGPU);
			ASSERT_EQ(Durin::GDynamicRHI->RHIWaitForCompletion(Signal, 1'000'000'000),
				Durin::ERHIGPUWaitResult::Complete);
			ASSERT_EQ(AsyncReadback->GetState(), Durin::ERHITextureReadbackState::Ready);
			Durin::FByteBuffer AsyncPixels;
			ASSERT_TRUE(AsyncReadback->TakePixels(AsyncPixels));
			ASSERT_EQ(AsyncPixels.size(), ByteCount);
			EXPECT_EQ(std::memcmp(AsyncPixels.data(), Pattern, ByteCount), 0);
			const auto* Bytes = static_cast<const uint8_t*>(
				[static_cast<Durin::FMetalBuffer*>(Destination.GetReference())->GetHandle() contents]);
			ASSERT_NE(Bytes, nullptr);
			EXPECT_EQ(std::memcmp(Bytes, Pattern, ByteCount), 0);
			Durin::FByteBuffer Readback;
			ASSERT_TRUE(Durin::GDynamicRHI->RHIReadTexture2D(
				Commands, SecondTexture.GetReference(), 0, 0, Readback));
			ASSERT_EQ(Readback.size(), ByteCount);
			EXPECT_EQ(std::memcmp(Readback.data(), Pattern, ByteCount), 0);
			SecondTexture = nullptr;
			Destination = nullptr;
			Durin::RHIExit();
		}
	}
}

TEST(FMetalRHITextureTests, PitchedUploadWorksInsideAndOutsideSubmission)
{
	@autoreleasepool
	{
		FScopedEnvironmentVariable Backend("DURIN_RHI_BACKEND", "metal");
		for (const char* Mode : {"inline", "threaded"})
		{
			SCOPED_TRACE(Mode);
			FScopedEnvironmentVariable Execution("DURIN_RHI_EXECUTION", Mode);
			ASSERT_TRUE(Durin::RHIInit(Durin::FRHIInitializationContext::Headless()));
			FScopedRHIExit Exit;
			auto& Commands = Durin::FRHICommandListImmediate::Get();
			auto Desc = Durin::FRHITextureCreateDesc::Create2D(
				"Pitched upload", 5, 3, Durin::EPixelFormat::RGBA8_UNORM);
			Desc.SetFlags(Durin::ETextureCreateFlags::DestinationCopy
				| Durin::ETextureCreateFlags::CPUReadback);
			auto TextureResult = Durin::GDynamicRHI->RHITryCreateTexture(Commands, Desc);
			ASSERT_TRUE(TextureResult.has_value());
			auto Texture = std::move(*TextureResult);
			constexpr uint32_t SourcePitch = 8 * 4;
			uint8_t Source[SourcePitch * 5];
			const Durin::FUpdateTextureRegion2D Region(0, 0, 2, 1, 5, 3);
			for (bool bExplicitSubmission : {false, true})
			{
				SCOPED_TRACE(bExplicitSubmission);
				for (uint32_t Index = 0; Index < sizeof(Source); ++Index)
					Source[Index] = static_cast<uint8_t>(Index * 3 + (bExplicitSubmission ? 11 : 5));
				Durin::FByteBuffer Expected(5 * 3 * 4);
				for (uint32_t Row = 0; Row < 3; ++Row)
					std::memcpy(Expected.data() + Row * 5 * 4,
						Source + (Row + 1) * SourcePitch + 2 * 4, 5 * 4);
				Durin::FRHIGPUSyncPointRef Signal;
				if (bExplicitSubmission)
					Signal = Commands.BeginGPUSubmission(
						{.Queue = Durin::GDynamicRHI->RHIGetQueueCapabilities().Graphics});
				Durin::GDynamicRHI->RHIUpdateTexture2D(Commands, Texture.GetReference(),
					0, 0, Region, SourcePitch, std::as_bytes(std::span{Source}));
				if (bExplicitSubmission)
				{
					Commands.EndGPUSubmission();
					Commands.ImmediateFlush(Durin::EImmediateFlushType::FlushRHIThread,
						Durin::ERHISubmitFlags::SubmitToGPU);
					ASSERT_EQ(Durin::GDynamicRHI->RHIWaitForCompletion(Signal, 1'000'000'000),
						Durin::ERHIGPUWaitResult::Complete);
				}
				Durin::FByteBuffer Actual;
				ASSERT_TRUE(Durin::GDynamicRHI->RHIReadTexture2D(
					Commands, Texture.GetReference(), 0, 0, Actual));
				EXPECT_EQ(Actual, Expected);
				auto AsyncReadback = Commands.EnqueueTextureReadback(Texture.GetReference());
				Commands.BlockUntilGPUIdle();
				ASSERT_EQ(AsyncReadback->GetState(), Durin::ERHITextureReadbackState::Ready);
				Durin::FByteBuffer AsyncPixels;
				ASSERT_TRUE(AsyncReadback->TakePixels(AsyncPixels));
				EXPECT_EQ(AsyncPixels, Expected);
			}
			Texture = nullptr;
			Durin::RHIExit();
		}
	}
}

TEST(FMetalRHITextureTests, ShutdownCancelsUnsubmittedReadback)
{
	@autoreleasepool
	{
		FScopedEnvironmentVariable Backend("DURIN_RHI_BACKEND", "metal");
		ASSERT_TRUE(Durin::RHIInit(Durin::FRHIInitializationContext::Headless()));
		FScopedRHIExit Exit;
		auto& Commands = Durin::FRHICommandListImmediate::Get();
		auto Desc = Durin::FRHITextureCreateDesc::Create2D(
			"Canceled readback", 4, 4, Durin::EPixelFormat::RGBA8_UNORM);
		Desc.SetFlags(Durin::ETextureCreateFlags::DestinationCopy
			| Durin::ETextureCreateFlags::CPUReadback);
		auto TextureResult = Durin::GDynamicRHI->RHITryCreateTexture(Commands, Desc);
		ASSERT_TRUE(TextureResult.has_value());
		auto Texture = std::move(*TextureResult);
		const auto Signal = Commands.BeginGPUSubmission(
			{.Queue = Durin::GDynamicRHI->RHIGetQueueCapabilities().Graphics});
		auto Request = Commands.EnqueueTextureReadback(Texture.GetReference());
		Commands.EndGPUSubmission();
		Texture = nullptr;
		Commands.ImmediateFlush(Durin::EImmediateFlushType::FlushRHIThread);
		EXPECT_EQ(Request->GetState(), Durin::ERHITextureReadbackState::Pending);
		Durin::RHIExit();
		EXPECT_EQ(Signal.GetState(), Durin::ERHIGPUSubmissionState::Canceled);
		EXPECT_EQ(Request->GetState(), Durin::ERHITextureReadbackState::Canceled);
	}
}

TEST(FMetalRHITextureTests, BufferTextureCopiesRespectOffsetAndRowPitch)
{
	@autoreleasepool
	{
		FScopedEnvironmentVariable Backend("DURIN_RHI_BACKEND", "metal");
		ASSERT_TRUE(Durin::RHIInit(Durin::FRHIInitializationContext::Headless()));
		FScopedRHIExit Exit;
		auto& Commands = Durin::FRHICommandListImmediate::Get();
		constexpr uint32_t RowPitch = 8 * 4;
		constexpr uint32_t BufferSize = 4 + RowPitch * 5;
		uint8_t Input[BufferSize]{};
		for (uint32_t Row = 0; Row < 3; ++Row)
			for (uint32_t Byte = 0; Byte < 5 * 4; ++Byte)
				Input[4 + Row * RowPitch + Byte] = static_cast<uint8_t>(Row * 41 + Byte);
		auto SourceDesc = Durin::FRHIBufferCreateDesc::Create("Pitched source", BufferSize, 1,
			Durin::EBufferUsageFlags::SourceCopy);
		SourceDesc.InitialData = {.Data = Input, .Size = BufferSize};
		const auto DestinationDesc = Durin::FRHIBufferCreateDesc::Create("Pitched destination", BufferSize, 1,
			Durin::EBufferUsageFlags::DestinationCopy | Durin::EBufferUsageFlags::KeepCPUAccessible);
		auto TextureDesc = Durin::FRHITextureCreateDesc::Create2D(
			"Pitched buffer texture", 5, 3, Durin::EPixelFormat::RGBA8_UNORM);
		TextureDesc.SetFlags(Durin::ETextureCreateFlags::SourceCopy
			| Durin::ETextureCreateFlags::DestinationCopy);
		auto SourceResult = Durin::GDynamicRHI->RHITryCreateBuffer(Commands, SourceDesc);
		auto DestinationResult = Durin::GDynamicRHI->RHITryCreateBuffer(Commands, DestinationDesc);
		auto TextureResult = Durin::GDynamicRHI->RHITryCreateTexture(Commands, TextureDesc);
		ASSERT_TRUE(SourceResult.has_value());
		ASSERT_TRUE(DestinationResult.has_value());
		ASSERT_TRUE(TextureResult.has_value());
		auto Source = std::move(*SourceResult);
		auto Destination = std::move(*DestinationResult);
		auto Texture = std::move(*TextureResult);
		const Durin::FRHIBufferTextureCopyRegion Region{
			.BufferOffset = 4, .BufferRowLength = 8, .BufferImageHeight = 5,
			.TextureExtent = {.Width = 5, .Height = 3, .Depth = 1}};
		const auto Signal = Commands.BeginGPUSubmission(
			{.Queue = Durin::GDynamicRHI->RHIGetQueueCapabilities().Graphics});
		Commands.CopyBufferToTexture(Source.GetReference(), Texture.GetReference(), {&Region, 1});
		Commands.CopyTextureToBuffer(Texture.GetReference(), Destination.GetReference(), {&Region, 1});
		Commands.EndGPUSubmission();
		Source = nullptr;
		Texture = nullptr;
		Commands.ImmediateFlush(Durin::EImmediateFlushType::FlushRHIThread,
			Durin::ERHISubmitFlags::SubmitToGPU);
		ASSERT_EQ(Durin::GDynamicRHI->RHIWaitForCompletion(Signal, 1'000'000'000),
			Durin::ERHIGPUWaitResult::Complete);
		const auto* Output = static_cast<const uint8_t*>(
			[static_cast<Durin::FMetalBuffer*>(Destination.GetReference())->GetHandle() contents]);
		ASSERT_NE(Output, nullptr);
		for (uint32_t Row = 0; Row < 3; ++Row)
			EXPECT_EQ(std::memcmp(Output + 4 + Row * RowPitch,
				Input + 4 + Row * RowPitch, 5 * 4), 0);
		Destination = nullptr;
		Durin::RHIExit();
	}
}

TEST(FMetalRHITextureTests, RequiredColorFormatsPreserveTransferBytes)
{
	@autoreleasepool
	{
		FScopedEnvironmentVariable Backend("DURIN_RHI_BACKEND", "metal");
		constexpr Durin::EPixelFormat Formats[] = {
			Durin::EPixelFormat::R8_UNORM,
			Durin::EPixelFormat::RG8_UNORM,
			Durin::EPixelFormat::R16_FLOAT,
			Durin::EPixelFormat::RGBA8_UNORM,
			Durin::EPixelFormat::BGRA8_UNORM,
			Durin::EPixelFormat::SRGBA8_UNORM,
			Durin::EPixelFormat::SBGRA8_UNORM,
			Durin::EPixelFormat::R11G11B10_FLOAT,
			Durin::EPixelFormat::RGBA16_FLOAT,
			Durin::EPixelFormat::RGBA32_FLOAT,
			Durin::EPixelFormat::RG32_UINT};
		for (const char* Mode : {"inline", "threaded"})
		{
			SCOPED_TRACE(Mode);
			FScopedEnvironmentVariable Execution("DURIN_RHI_EXECUTION", Mode);
			ASSERT_TRUE(Durin::RHIInit(Durin::FRHIInitializationContext::Headless()));
			FScopedRHIExit Exit;
			auto& Commands = Durin::FRHICommandListImmediate::Get();
			for (auto Format : Formats)
			{
				SCOPED_TRACE(static_cast<uint32_t>(Format));
				constexpr uint32_t Width = 16;
				constexpr uint32_t Height = 4;
				const uint32_t ByteCount = Width * Height
					* Durin::GetPixelFormatInfo(Format).BytesPerBlock;
				std::vector<uint8_t> Pattern(ByteCount);
				for (uint32_t Index = 0; Index < ByteCount; ++Index)
					Pattern[Index] = static_cast<uint8_t>(Index * 17 + 3);
				auto TextureDesc = Durin::FRHITextureCreateDesc::Create2D(
					"Color format transfer", Width, Height, Format);
				TextureDesc.SetFlags(Durin::ETextureCreateFlags::SourceCopy
					| Durin::ETextureCreateFlags::DestinationCopy
					| Durin::ETextureCreateFlags::CPUReadback);
				ASSERT_TRUE(Durin::GDynamicRHI->RHIIsTextureSupported(TextureDesc));
				auto TextureResult = Durin::GDynamicRHI->RHITryCreateTexture(Commands, TextureDesc);
				ASSERT_TRUE(TextureResult.has_value());
				auto Texture = std::move(*TextureResult);
				auto SourceDesc = Durin::FRHIBufferCreateDesc::Create("Color source", ByteCount,
					1, Durin::EBufferUsageFlags::SourceCopy);
				SourceDesc.InitialData = {.Data = Pattern.data(), .Size = ByteCount};
				const auto DestinationDesc = Durin::FRHIBufferCreateDesc::Create(
					"Color destination", ByteCount, 1,
					Durin::EBufferUsageFlags::DestinationCopy
						| Durin::EBufferUsageFlags::KeepCPUAccessible);
				auto SourceResult = Durin::GDynamicRHI->RHITryCreateBuffer(Commands, SourceDesc);
				auto DestinationResult = Durin::GDynamicRHI->RHITryCreateBuffer(Commands, DestinationDesc);
				ASSERT_TRUE(SourceResult.has_value());
				ASSERT_TRUE(DestinationResult.has_value());
				auto Source = std::move(*SourceResult);
				auto Destination = std::move(*DestinationResult);
				const Durin::FRHIBufferTextureCopyRegion Region{
					.TextureExtent = {.Width = Width, .Height = Height, .Depth = 1}};
				const auto Signal = Commands.BeginGPUSubmission(
					{.Queue = Durin::GDynamicRHI->RHIGetQueueCapabilities().Graphics});
				Commands.CopyBufferToTexture(Source.GetReference(), Texture.GetReference(),
					{&Region, 1});
				Commands.CopyTextureToBuffer(Texture.GetReference(), Destination.GetReference(),
					{&Region, 1});
				Commands.EndGPUSubmission();
				Commands.ImmediateFlush(Durin::EImmediateFlushType::FlushRHIThread,
					Durin::ERHISubmitFlags::SubmitToGPU);
				ASSERT_EQ(Durin::GDynamicRHI->RHIWaitForCompletion(Signal, 1'000'000'000),
					Durin::ERHIGPUWaitResult::Complete);
				const auto* Bytes = static_cast<const uint8_t*>(
					[static_cast<Durin::FMetalBuffer*>(Destination.GetReference())->GetHandle() contents]);
				ASSERT_NE(Bytes, nullptr);
				EXPECT_EQ(std::memcmp(Bytes, Pattern.data(), ByteCount), 0);
				Durin::FByteBuffer Readback;
				ASSERT_TRUE(Durin::GDynamicRHI->RHIReadTexture2D(
					Commands, Texture.GetReference(), 0, 0, Readback));
				ASSERT_EQ(Readback.size(), ByteCount);
				EXPECT_EQ(std::memcmp(Readback.data(), Pattern.data(), ByteCount), 0);
				Source = nullptr;
				Destination = nullptr;
				Texture = nullptr;
			}
			Durin::RHIExit();
		}
	}
}

TEST(FMetalRHITextureTests, CubeFaceMipCopiesPreserveTwoLayers)
{
	@autoreleasepool
	{
		FScopedEnvironmentVariable Backend("DURIN_RHI_BACKEND", "metal");
		for (const char* Mode : {"inline", "threaded"})
		for (const auto Format : {Durin::EPixelFormat::RGBA8_UNORM,
			Durin::EPixelFormat::RGBA16_FLOAT})
		{
			SCOPED_TRACE(Mode);
			SCOPED_TRACE(Format == Durin::EPixelFormat::RGBA16_FLOAT ? "RGBA16F" : "RGBA8");
			FScopedEnvironmentVariable Execution("DURIN_RHI_EXECUTION", Mode);
			ASSERT_TRUE(Durin::RHIInit(Durin::FRHIInitializationContext::Headless()));
			FScopedRHIExit Exit;
			auto& Commands = Durin::FRHICommandListImmediate::Get();
			auto Desc = Durin::FRHITextureCreateDesc::CreateCube("Cube transfer")
				.SetExtent(8).SetNumMips(2).SetFormat(Format)
				.SetFlags(Durin::ETextureCreateFlags::SourceCopy
					| Durin::ETextureCreateFlags::DestinationCopy
					| Durin::ETextureCreateFlags::CPUReadback);
			ASSERT_TRUE(Durin::GDynamicRHI->RHIIsTextureSupported(Desc));
			auto FirstResult = Durin::GDynamicRHI->RHITryCreateTexture(Commands, Desc);
			auto SecondResult = Durin::GDynamicRHI->RHITryCreateTexture(Commands, Desc);
			ASSERT_TRUE(FirstResult.has_value());
			ASSERT_TRUE(SecondResult.has_value());
			auto First = std::move(*FirstResult);
			auto Second = std::move(*SecondResult);
			const Durin::FRHITextureViewDesc CubeViewDesc{
				.Usage = Durin::ERHITextureViewUsage::TransferSource,
				.Dimension = Durin::ERHITextureViewDimension::TextureCube,
				.Format = Format,
				.Range = {Durin::ERHITextureAspect::Color, 1, 1, 0, 6}};
			EXPECT_NE(Durin::GDynamicRHI->RHICreateTextureView(
				First.GetReference(), CubeViewDesc).GetReference(), nullptr);
			const uint32_t FaceBytes = 4 * 4 * Durin::GetPixelFormatInfo(Format).BytesPerBlock;
			std::vector<uint8_t> Pattern(FaceBytes * 2);
			for (uint32_t Index = 0; Index < Pattern.size(); ++Index)
				Pattern[Index] = static_cast<uint8_t>(Index * 7 + 9);
			auto SourceDesc = Durin::FRHIBufferCreateDesc::Create("Cube source", Pattern.size(), 1,
				Durin::EBufferUsageFlags::SourceCopy);
			SourceDesc.InitialData = {.Data = Pattern.data(),
				.Size = static_cast<uint32>(Pattern.size())};
			const auto DestinationDesc = Durin::FRHIBufferCreateDesc::Create(
				"Cube destination", Pattern.size(), 1,
				Durin::EBufferUsageFlags::DestinationCopy
					| Durin::EBufferUsageFlags::KeepCPUAccessible);
			auto SourceResult = Durin::GDynamicRHI->RHITryCreateBuffer(Commands, SourceDesc);
			auto DestinationResult = Durin::GDynamicRHI->RHITryCreateBuffer(Commands, DestinationDesc);
			ASSERT_TRUE(SourceResult.has_value());
			ASSERT_TRUE(DestinationResult.has_value());
			auto Source = std::move(*SourceResult);
			auto Destination = std::move(*DestinationResult);
			const Durin::FRHIBufferTextureCopyRegion Upload{
				.TextureMip = 1, .TextureFirstArrayLayer = 2, .TextureNumArrayLayers = 2,
				.TextureExtent = {.Width = 4, .Height = 4, .Depth = 1}};
			const Durin::FRHITextureCopyRegion Copy{
				.SourceMip = 1, .SourceFirstArrayLayer = 2,
				.DestinationMip = 1, .DestinationFirstArrayLayer = 4,
				.NumArrayLayers = 2,
				.Extent = {.Width = 4, .Height = 4, .Depth = 1}};
			const Durin::FRHIBufferTextureCopyRegion Download{
				.TextureMip = 1, .TextureFirstArrayLayer = 4, .TextureNumArrayLayers = 2,
				.TextureExtent = {.Width = 4, .Height = 4, .Depth = 1}};
			const auto Signal = Commands.BeginGPUSubmission(
				{.Queue = Durin::GDynamicRHI->RHIGetQueueCapabilities().Graphics});
			Commands.CopyBufferToTexture(Source.GetReference(), First.GetReference(), {&Upload, 1});
			Commands.CopyTexture(First.GetReference(), Second.GetReference(), {&Copy, 1});
			Commands.CopyTextureToBuffer(Second.GetReference(), Destination.GetReference(),
				{&Download, 1});
			Commands.EndGPUSubmission();
			Source = nullptr;
			First = nullptr;
			Commands.ImmediateFlush(Durin::EImmediateFlushType::FlushRHIThread,
				Durin::ERHISubmitFlags::SubmitToGPU);
			ASSERT_EQ(Durin::GDynamicRHI->RHIWaitForCompletion(Signal, 1'000'000'000),
				Durin::ERHIGPUWaitResult::Complete);
			const auto* Bytes = static_cast<const uint8_t*>(
				[static_cast<Durin::FMetalBuffer*>(Destination.GetReference())->GetHandle() contents]);
			ASSERT_NE(Bytes, nullptr);
			EXPECT_EQ(std::memcmp(Bytes, Pattern.data(), Pattern.size()), 0);
			for (uint32_t Face = 0; Face < 2; ++Face)
			{
				Durin::FByteBuffer Readback;
				ASSERT_TRUE(Durin::GDynamicRHI->RHIReadTexture2D(
					Commands, Second.GetReference(), 1, 4 + Face, Readback));
				ASSERT_EQ(Readback.size(), FaceBytes);
				EXPECT_EQ(std::memcmp(Readback.data(), Pattern.data() + Face * FaceBytes, FaceBytes), 0);
			}
			Destination = nullptr;
			Second = nullptr;
			Durin::RHIExit();
		}
	}
}

TEST(FMetalRHITextureTests, ArrayLayersCopyAcrossCubeBoundary)
{
	@autoreleasepool
	{
		FScopedEnvironmentVariable Backend("DURIN_RHI_BACKEND", "metal");
		for (const char* Mode : {"inline", "threaded"})
		for (const bool bCubeArray : {false, true})
		{
			SCOPED_TRACE(Mode);
			SCOPED_TRACE(bCubeArray ? "cube array" : "2D array");
			FScopedEnvironmentVariable Execution("DURIN_RHI_EXECUTION", Mode);
			ASSERT_TRUE(Durin::RHIInit(Durin::FRHIInitializationContext::Headless()));
			FScopedRHIExit Exit;
			auto& Commands = Durin::FRHICommandListImmediate::Get();
			auto Desc = bCubeArray
			? Durin::FRHITextureCreateDesc::CreateCubeArray("Array transfer")
			: Durin::FRHITextureCreateDesc::Create2DArray("Array transfer");
			Desc.SetExtent(8, 8).SetArraySize(bCubeArray ? 12 : 4)
				.SetFormat(Durin::EPixelFormat::RGBA8_UNORM)
				.SetFlags(Durin::ETextureCreateFlags::SourceCopy
					| Durin::ETextureCreateFlags::DestinationCopy
					| Durin::ETextureCreateFlags::CPUReadback);
			ASSERT_TRUE(Durin::GDynamicRHI->RHIIsTextureSupported(Desc));
			auto FirstResult = Durin::GDynamicRHI->RHITryCreateTexture(Commands, Desc);
			auto SecondResult = Durin::GDynamicRHI->RHITryCreateTexture(Commands, Desc);
			ASSERT_TRUE(FirstResult.has_value());
			ASSERT_TRUE(SecondResult.has_value());
			auto First = std::move(*FirstResult);
			auto Second = std::move(*SecondResult);
			const auto View = Durin::MakeDefaultTextureViewDesc(
				*First, Durin::ERHITextureViewUsage::TransferSource);
			EXPECT_EQ(View.Dimension, bCubeArray
				? Durin::ERHITextureViewDimension::TextureCubeArray
				: Durin::ERHITextureViewDimension::Texture2DArray);
			EXPECT_NE(Durin::GDynamicRHI->RHICreateTextureView(
				First.GetReference(), View).GetReference(), nullptr);
			constexpr uint32_t LayerBytes = 4 * 4 * 4;
			uint8_t Pattern[LayerBytes * 2];
			for (uint32_t Index = 0; Index < sizeof(Pattern); ++Index)
				Pattern[Index] = static_cast<uint8_t>(Index * 11 + 3);
			auto SourceDesc = Durin::FRHIBufferCreateDesc::Create("Array source",
				sizeof(Pattern), 1, Durin::EBufferUsageFlags::SourceCopy);
			SourceDesc.InitialData = {.Data = Pattern, .Size = sizeof(Pattern)};
			const auto DestinationDesc = Durin::FRHIBufferCreateDesc::Create(
				"Array destination", sizeof(Pattern), 1,
				Durin::EBufferUsageFlags::DestinationCopy
					| Durin::EBufferUsageFlags::KeepCPUAccessible);
			auto SourceResult = Durin::GDynamicRHI->RHITryCreateBuffer(Commands, SourceDesc);
			auto DestinationResult = Durin::GDynamicRHI->RHITryCreateBuffer(Commands, DestinationDesc);
			ASSERT_TRUE(SourceResult.has_value());
			ASSERT_TRUE(DestinationResult.has_value());
			auto Source = std::move(*SourceResult);
			auto Destination = std::move(*DestinationResult);
			const uint32_t FirstLayer = bCubeArray ? 5 : 1;
			const uint32_t LastLayer = bCubeArray ? 8 : 2;
			const Durin::FRHIBufferTextureCopyRegion Upload{
				.TextureFirstArrayLayer = FirstLayer, .TextureNumArrayLayers = 2,
				.TextureExtent = {4, 4, 1}};
			const Durin::FRHITextureCopyRegion Copy{
				.SourceFirstArrayLayer = FirstLayer,
				.DestinationFirstArrayLayer = LastLayer,
				.NumArrayLayers = 2, .Extent = {4, 4, 1}};
			const Durin::FRHIBufferTextureCopyRegion Download{
				.TextureFirstArrayLayer = LastLayer, .TextureNumArrayLayers = 2,
				.TextureExtent = {4, 4, 1}};
			const auto Signal = Commands.BeginGPUSubmission(
				{.Queue = Durin::GDynamicRHI->RHIGetQueueCapabilities().Graphics});
			Commands.CopyBufferToTexture(Source.GetReference(), First.GetReference(), {&Upload, 1});
			Commands.CopyTexture(First.GetReference(), Second.GetReference(), {&Copy, 1});
			Commands.CopyTextureToBuffer(Second.GetReference(), Destination.GetReference(),
				{&Download, 1});
			Commands.EndGPUSubmission();
			Commands.ImmediateFlush(Durin::EImmediateFlushType::FlushRHIThread,
				Durin::ERHISubmitFlags::SubmitToGPU);
			ASSERT_EQ(Durin::GDynamicRHI->RHIWaitForCompletion(Signal, 1'000'000'000),
				Durin::ERHIGPUWaitResult::Complete);
			const auto* Bytes = static_cast<const uint8_t*>(
				[static_cast<Durin::FMetalBuffer*>(Destination.GetReference())->GetHandle() contents]);
			ASSERT_NE(Bytes, nullptr);
			EXPECT_EQ(std::memcmp(Bytes, Pattern, sizeof(Pattern)), 0);
			for (uint32_t Layer = 0; Layer < 2; ++Layer)
			{
				Durin::FByteBuffer Readback;
				ASSERT_TRUE(Durin::GDynamicRHI->RHIReadTexture2D(
					Commands, Second.GetReference(), 0, LastLayer + Layer, Readback));
				ASSERT_EQ(Readback.size(), 8u * 8u * 4u);
				for (uint32_t Row = 0; Row < 4; ++Row)
					EXPECT_EQ(std::memcmp(Readback.data() + Row * 8 * 4,
						Pattern + Layer * LayerBytes + Row * 4 * 4, 4 * 4), 0);
			}
			auto PlaneDesc = Durin::FRHITextureCreateDesc::Create2D(
				"Array layer source", 4, 4, Durin::EPixelFormat::RGBA8_UNORM)
				.SetFlags(Durin::ETextureCreateFlags::SourceCopy
					| Durin::ETextureCreateFlags::DestinationCopy);
			auto PlaneResult = Durin::GDynamicRHI->RHITryCreateTexture(Commands, PlaneDesc);
			ASSERT_TRUE(PlaneResult.has_value());
			auto Plane = std::move(*PlaneResult);
			const Durin::FRHIBufferTextureCopyRegion PlaneUpload{
				.TextureExtent = {4, 4, 1}};
			const Durin::FRHITextureCopyRegion CrossDimensionCopy{
				.DestinationFirstArrayLayer = 0,
				.Extent = {4, 4, 1}};
			const Durin::FRHIBufferTextureCopyRegion PlaneDownload{
				.TextureExtent = {4, 4, 1}};
			const auto PlaneSignal = Commands.BeginGPUSubmission(
				{.Queue = Durin::GDynamicRHI->RHIGetQueueCapabilities().Graphics});
			Commands.CopyBufferToTexture(Source.GetReference(), Plane.GetReference(),
				{&PlaneUpload, 1});
			Commands.CopyTexture(Plane.GetReference(), Second.GetReference(),
				{&CrossDimensionCopy, 1});
			Commands.CopyTextureToBuffer(Second.GetReference(), Destination.GetReference(),
				{&PlaneDownload, 1});
			Commands.EndGPUSubmission();
			Commands.ImmediateFlush(Durin::EImmediateFlushType::FlushRHIThread,
				Durin::ERHISubmitFlags::SubmitToGPU);
			ASSERT_EQ(Durin::GDynamicRHI->RHIWaitForCompletion(PlaneSignal, 1'000'000'000),
				Durin::ERHIGPUWaitResult::Complete);
			EXPECT_EQ(std::memcmp(Bytes, Pattern, LayerBytes), 0);
			Durin::RHIExit();
		}
	}
}

TEST(FMetalRHITextureTests, VolumeMipCopiesPreserveDepthSlices)
{
	@autoreleasepool
	{
		FScopedEnvironmentVariable Backend("DURIN_RHI_BACKEND", "metal");
		for (const char* Mode : {"inline", "threaded"})
		{
			SCOPED_TRACE(Mode);
			FScopedEnvironmentVariable Execution("DURIN_RHI_EXECUTION", Mode);
			ASSERT_TRUE(Durin::RHIInit(Durin::FRHIInitializationContext::Headless()));
			FScopedRHIExit Exit;
			auto& Commands = Durin::FRHICommandListImmediate::Get();
			auto Desc = Durin::FRHITextureCreateDesc::Create3D("Volume transfer")
				.SetExtent(4, 4).SetDepth(4).SetNumMips(2)
				.SetFormat(Durin::EPixelFormat::RGBA8_UNORM)
				.SetFlags(Durin::ETextureCreateFlags::SourceCopy
					| Durin::ETextureCreateFlags::DestinationCopy);
			ASSERT_TRUE(Durin::GDynamicRHI->RHIIsTextureSupported(Desc));
			auto FirstResult = Durin::GDynamicRHI->RHITryCreateTexture(Commands, Desc);
			auto SecondResult = Durin::GDynamicRHI->RHITryCreateTexture(Commands, Desc);
			ASSERT_TRUE(FirstResult.has_value());
			ASSERT_TRUE(SecondResult.has_value());
			auto First = std::move(*FirstResult);
			auto Second = std::move(*SecondResult);
			const Durin::FRHITextureViewDesc VolumeViewDesc{
				.Usage = Durin::ERHITextureViewUsage::TransferSource,
				.Dimension = Durin::ERHITextureViewDimension::Texture3D,
				.Format = Durin::EPixelFormat::RGBA8_UNORM,
				.Range = {Durin::ERHITextureAspect::Color, 1, 1, 0, 1}};
			EXPECT_NE(Durin::GDynamicRHI->RHICreateTextureView(
				First.GetReference(), VolumeViewDesc).GetReference(), nullptr);
			constexpr uint32_t ByteCount = 2 * 2 * 2 * 4;
			uint8_t Pattern[ByteCount];
			for (uint32_t Index = 0; Index < ByteCount; ++Index)
				Pattern[Index] = static_cast<uint8_t>(Index * 11 + 4);
			auto SourceDesc = Durin::FRHIBufferCreateDesc::Create("Volume source", ByteCount, 1,
				Durin::EBufferUsageFlags::SourceCopy);
			SourceDesc.InitialData = {.Data = Pattern, .Size = ByteCount};
			const auto DestinationDesc = Durin::FRHIBufferCreateDesc::Create("Volume destination",
				ByteCount, 1, Durin::EBufferUsageFlags::DestinationCopy
					| Durin::EBufferUsageFlags::KeepCPUAccessible);
			auto SourceResult = Durin::GDynamicRHI->RHITryCreateBuffer(Commands, SourceDesc);
			auto DestinationResult = Durin::GDynamicRHI->RHITryCreateBuffer(Commands, DestinationDesc);
			ASSERT_TRUE(SourceResult.has_value());
			ASSERT_TRUE(DestinationResult.has_value());
			auto Source = std::move(*SourceResult);
			auto Destination = std::move(*DestinationResult);
			const Durin::FRHIBufferTextureCopyRegion Region{
				.TextureMip = 1,
				.TextureExtent = {.Width = 2, .Height = 2, .Depth = 2}};
			const Durin::FRHITextureCopyRegion Copy{
				.SourceMip = 1, .DestinationMip = 1,
				.Extent = {.Width = 2, .Height = 2, .Depth = 2}};
			const auto Signal = Commands.BeginGPUSubmission(
				{.Queue = Durin::GDynamicRHI->RHIGetQueueCapabilities().Graphics});
			Commands.CopyBufferToTexture(Source.GetReference(), First.GetReference(), {&Region, 1});
			Commands.CopyTexture(First.GetReference(), Second.GetReference(), {&Copy, 1});
			Commands.CopyTextureToBuffer(Second.GetReference(), Destination.GetReference(),
				{&Region, 1});
			Commands.EndGPUSubmission();
			Source = nullptr;
			First = nullptr;
			Second = nullptr;
			Commands.ImmediateFlush(Durin::EImmediateFlushType::FlushRHIThread,
				Durin::ERHISubmitFlags::SubmitToGPU);
			ASSERT_EQ(Durin::GDynamicRHI->RHIWaitForCompletion(Signal, 1'000'000'000),
				Durin::ERHIGPUWaitResult::Complete);
			const auto* Bytes = static_cast<const uint8_t*>(
				[static_cast<Durin::FMetalBuffer*>(Destination.GetReference())->GetHandle() contents]);
			ASSERT_NE(Bytes, nullptr);
			EXPECT_EQ(std::memcmp(Bytes, Pattern, ByteCount), 0);
			Destination = nullptr;
			Durin::RHIExit();
		}
	}
}

TEST(FMetalRHITextureTests, PitchedVolumeUploadPreservesVoxelBox)
{
	@autoreleasepool
	{
		FScopedEnvironmentVariable Backend("DURIN_RHI_BACKEND", "metal");
		for (const char* Mode : {"inline", "threaded"})
		{
			SCOPED_TRACE(Mode);
			FScopedEnvironmentVariable Execution("DURIN_RHI_EXECUTION", Mode);
			ASSERT_TRUE(Durin::RHIInit(Durin::FRHIInitializationContext::Headless()));
			FScopedRHIExit Exit;
			auto& Commands = Durin::FRHICommandListImmediate::Get();
			auto Desc = Durin::FRHITextureCreateDesc::Create3D("Pitched volume")
				.SetExtent(4, 4).SetDepth(4).SetFormat(Durin::EPixelFormat::RGBA8_UNORM)
				.SetFlags(Durin::ETextureCreateFlags::SourceCopy
					| Durin::ETextureCreateFlags::DestinationCopy);
			auto TextureResult = Durin::GDynamicRHI->RHITryCreateTexture(Commands, Desc);
			ASSERT_TRUE(TextureResult.has_value());
			auto Texture = std::move(*TextureResult);
			constexpr uint32_t RowPitch = 4 * 4;
			constexpr uint32_t DepthPitch = RowPitch * 4;
			constexpr uint32_t OutputSize = 2 * 2 * 2 * 4;
			uint8_t Source[DepthPitch * 4];
			const Durin::FUpdateTextureRegion3D Update(
				1, 1, 1, 1, 1, 1, 2, 2, 2);
			const Durin::FRHIBufferTextureCopyRegion Copy{
				.TextureOffset = {1, 1, 1},
				.TextureExtent = {.Width = 2, .Height = 2, .Depth = 2}};
			for (bool bExplicitSubmission : {false, true})
			{
				SCOPED_TRACE(bExplicitSubmission);
				for (uint32_t Index = 0; Index < sizeof(Source); ++Index)
					Source[Index] = static_cast<uint8_t>(Index * 5 + (bExplicitSubmission ? 17 : 3));
				uint8_t Expected[OutputSize];
				for (uint32_t Z = 0; Z < 2; ++Z)
					for (uint32_t Y = 0; Y < 2; ++Y)
						std::memcpy(Expected + Z * 2 * 2 * 4 + Y * 2 * 4,
							Source + (Z + 1) * DepthPitch + (Y + 1) * RowPitch + 4,
							2 * 4);
				const auto BufferDesc = Durin::FRHIBufferCreateDesc::Create(
					"Volume upload readback", OutputSize, 1,
					Durin::EBufferUsageFlags::DestinationCopy
						| Durin::EBufferUsageFlags::KeepCPUAccessible);
				auto BufferResult = Durin::GDynamicRHI->RHITryCreateBuffer(Commands, BufferDesc);
				ASSERT_TRUE(BufferResult.has_value());
				auto Buffer = std::move(*BufferResult);
				Durin::FRHIGPUSyncPointRef Signal;
				if (bExplicitSubmission)
					Signal = Commands.BeginGPUSubmission(
						{.Queue = Durin::GDynamicRHI->RHIGetQueueCapabilities().Graphics});
				Durin::GDynamicRHI->RHIUpdateTexture3D(Commands, Texture.GetReference(),
					0, Update, RowPitch, DepthPitch, std::as_bytes(std::span{Source}));
				if (!bExplicitSubmission)
					Signal = Commands.BeginGPUSubmission(
						{.Queue = Durin::GDynamicRHI->RHIGetQueueCapabilities().Graphics});
				Commands.CopyTextureToBuffer(Texture.GetReference(), Buffer.GetReference(),
					{&Copy, 1});
				Commands.EndGPUSubmission();
				Commands.ImmediateFlush(Durin::EImmediateFlushType::FlushRHIThread,
					Durin::ERHISubmitFlags::SubmitToGPU);
				ASSERT_EQ(Durin::GDynamicRHI->RHIWaitForCompletion(Signal, 1'000'000'000),
					Durin::ERHIGPUWaitResult::Complete);
				const auto* Output = static_cast<const uint8_t*>(
					[static_cast<Durin::FMetalBuffer*>(Buffer.GetReference())->GetHandle() contents]);
				ASSERT_NE(Output, nullptr);
				EXPECT_EQ(std::memcmp(Output, Expected, OutputSize), 0);
				Buffer = nullptr;
			}
			Texture = nullptr;
			Durin::RHIExit();
		}
	}
}
