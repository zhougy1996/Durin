#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CATransaction.h>

#include "DynamicRHI.h"
#include "MacOS/MacOSPresentationTarget.h"
#include "MetalBuffer.h"
#include "MetalCppDevice.h"
#include "MetalPipeline.h"
#include "MetalSubmission.h"
#include "Threading/Task.h"
#include "CoreGlobals.h"
#include "HAL/PlatformLTS.h"
#include "MetalSampler.h"
#include "MetalTexture.h"
#include "RHICommandList.h"
#include "RHIGlobals.h"

#include <gtest/gtest.h>

static_assert(!std::is_constructible_v<Durin::FMacOSPresentationTarget, void*, void*>);
static_assert(std::is_constructible_v<Durin::FMacOSPresentationTarget, void*, CA::MetalLayer*>);

// Fault injection affects only drawable acquisition; subsequent calls use real Metal.
@interface FMetalQualificationLayer : CAMetalLayer
@property(nonatomic) BOOL FailNextDrawable;
@property(nonatomic) NSUInteger AcquisitionCount;
@property(nonatomic) NSUInteger SuccessfulAcquisitionCount;
@end

@implementation FMetalQualificationLayer
- (id<CAMetalDrawable>)nextDrawable
{
	++self.AcquisitionCount;
	if (self.FailNextDrawable)
	{
		self.FailNextDrawable = NO;
		return nil;
	}
	id<CAMetalDrawable> Drawable = [super nextDrawable];
	if (Drawable) ++self.SuccessfulAcquisitionCount;
	return Drawable;
}
@end

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
				static_cast<Durin::FMetalBuffer*>(Source.GetReference())->GetHandle()->contents());
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
				static_cast<Durin::FMetalBuffer*>(Destination.GetReference())->GetHandle()->contents());
			ASSERT_NE(Bytes, nullptr);
			EXPECT_EQ(std::memcmp(Bytes, Pattern, Size), 0);
			Destination = nullptr;
			Durin::RHIExit();
		}
	}
}

TEST(FMetalRHIBufferTests, CppCandidatesReleaseQueueWhenInitializationUnwinds)
{
	for (uint32_t Round = 0; Round < 16; ++Round)
	{
		SCOPED_TRACE(Round);
		__weak id<MTLCommandQueue> Queue = nil;
		bool bCreatedCandidate = false;
		@autoreleasepool
		{
			try
			{
				auto Candidate = Durin::CreateMetalCppDeviceAndQueue();
				ASSERT_TRUE(Candidate.Device);
				ASSERT_TRUE(Candidate.Queue);
				bCreatedCandidate = true;
				Queue = (__bridge id<MTLCommandQueue>)static_cast<void*>(Candidate.Queue.get());
				throw std::runtime_error("Simulated initialization failure after native creation");
			}
			catch (const std::runtime_error&) {}
		}
		EXPECT_TRUE(bCreatedCandidate);
		EXPECT_EQ(Queue, nil);
	}
}

TEST(FMetalRHIBufferTests, RepeatedCppOwnersSurvivePoolDrainAndReleaseAtShutdown)
{
	FScopedEnvironmentVariable Backend("DURIN_RHI_BACKEND", "metal");
	for (const char* Mode : {"inline", "threaded"})
	{
		SCOPED_TRACE(Mode);
		FScopedEnvironmentVariable Execution("DURIN_RHI_EXECUTION", Mode);
		for (uint32_t Round = 0; Round < 16; ++Round)
		{
			SCOPED_TRACE(Round);
			__weak id<MTLBuffer> SourceNative = nil;
			__weak id<MTLBuffer> DestinationNative = nil;
			@autoreleasepool
			{
				ASSERT_TRUE(Durin::RHIInit(Durin::FRHIInitializationContext::Headless()));
				FScopedRHIExit Exit;
				auto& Commands = Durin::FRHICommandListImmediate::Get();
				const std::array<uint32_t, 4> Pattern{Round, 17, 29, 43};
				Durin::FBufferRHIRef Destination;
				Durin::FRHIGPUSyncPointRef Signal;
				@autoreleasepool
				{
					const auto Desc = Durin::FRHIBufferCreateDesc::Create(
						"Metal pool lifetime", sizeof(Pattern), sizeof(uint32_t),
						Durin::EBufferUsageFlags::SourceCopy
							| Durin::EBufferUsageFlags::DestinationCopy
							| Durin::EBufferUsageFlags::KeepCPUAccessible);
					auto SourceResult = Durin::GDynamicRHI->RHITryCreateBuffer(Commands, Desc);
					auto DestinationResult = Durin::GDynamicRHI->RHITryCreateBuffer(Commands, Desc);
					ASSERT_TRUE(SourceResult);
					ASSERT_TRUE(DestinationResult);
					auto Source = std::move(*SourceResult);
					Destination = std::move(*DestinationResult);
					SourceNative = (__bridge id<MTLBuffer>)static_cast<void*>(
						static_cast<Durin::FMetalBuffer*>(Source.GetReference())->GetHandle());
					DestinationNative = (__bridge id<MTLBuffer>)static_cast<void*>(
						static_cast<Durin::FMetalBuffer*>(Destination.GetReference())->GetHandle());
					Signal = Commands.BeginGPUSubmission(
						{.Queue = Durin::GDynamicRHI->RHIGetQueueCapabilities().Graphics});
					Commands.WriteBuffer(Source.GetReference(), Pattern.data(), sizeof(Pattern), 0);
					const Durin::FRHIBufferCopyRegion Copy{.Size = sizeof(Pattern)};
					Commands.CopyBuffer(Source.GetReference(), Destination.GetReference(), {&Copy, 1});
					Commands.EndGPUSubmission();
					Source = nullptr;
					Commands.ImmediateFlush(Durin::EImmediateFlushType::FlushRHIThread);
				}
				// Pending replay has outlived its pool and caller's source reference.
				EXPECT_NE(SourceNative, nil);
				EXPECT_EQ(Signal.GetState(), Durin::ERHIGPUSubmissionState::Pending);
				Commands.ImmediateFlush(Durin::EImmediateFlushType::FlushRHIThread,
					Durin::ERHISubmitFlags::SubmitToGPU);
				ASSERT_EQ(Durin::GDynamicRHI->RHIWaitForCompletion(Signal, 1'000'000'000),
					Durin::ERHIGPUWaitResult::Complete);
				const auto* Bytes = static_cast<const uint32_t*>(
					static_cast<Durin::FMetalBuffer*>(Destination.GetReference())->GetHandle()->contents());
				ASSERT_NE(Bytes, nullptr);
				EXPECT_EQ(std::memcmp(Bytes, Pattern.data(), sizeof(Pattern)), 0);
				Destination = nullptr;
				Durin::RHIExit();
			}
			EXPECT_EQ(SourceNative, nil);
			EXPECT_EQ(DestinationNative, nil);
		}
	}
}

TEST(FMetalRHIBufferTests, DeferredUniformVersionsReachSeparateDispatches)
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
			const std::string Source = "#include <metal_stdlib>\nusing namespace metal;\n"
				"kernel void writeVersion(constant uint4& values [[buffer(0)]], "
				"device uint* output [[buffer(1)]]) { output[values.y] = values.x; }\n";
			Durin::FByteBuffer Code;
			for (char Character : Source) Code.push_back(std::byte(Character));
			auto ShaderDesc = Durin::FRHIShaderCreateDesc::Create("MetalDeferredUniform",
				Durin::EShaderFrequency::Compute, Code,
				Durin::FXxHash128::HashBuffer(Code));
			ShaderDesc.Target = Durin::MetalShaderTarget;
			ShaderDesc.CodeFormat = Durin::EShaderCodeFormat::Msl20Source;
			ShaderDesc.ComputeThreadGroupSize = {1, 1, 1};
			ShaderDesc.SetEntryPoint("writeVersion");
			ShaderDesc.MetalBindings = {
				{0, 0, Durin::ERHIBindingType::UniformBuffer, 0, 1},
				{0, 1, Durin::ERHIBindingType::StorageBuffer, 1, 1}};
			ShaderDesc.BindingRemapIdentity = Durin::ComputeMetalBindingRemapIdentity(
				Durin::EShaderFrequency::Compute, ShaderDesc.MetalBindings,
				ShaderDesc.MetalPushConstantBufferSlot);
			auto Shader = Durin::GDynamicRHI->RHICreateShader(ShaderDesc);
			ASSERT_TRUE(Shader);
			Durin::FComputePipelineStateInitializer PipelineDesc;
			PipelineDesc.ComputeShader = Shader.GetReference();
			PipelineDesc.PipelineLayout.BindingLayouts.resize(1);
			PipelineDesc.PipelineLayout.BindingLayouts[0].BindingLayouts.emplace_back(
				Durin::EShaderStageFlags::Compute, 0, Durin::ERHIBindingType::UniformBuffer);
			PipelineDesc.PipelineLayout.BindingLayouts[0].BindingLayouts.emplace_back(
				Durin::EShaderStageFlags::Compute, 1, Durin::ERHIBindingType::StorageBuffer);
			auto Pipeline = Durin::GDynamicRHI->RHICreateComputePipelineState(
				"MetalDeferredUniform", PipelineDesc);
			ASSERT_TRUE(Pipeline);
			std::array<uint32_t, 4> Values{7, 0, 0, 0};
			auto Uniform = Commands.CreateUniformBuffer({16},
				Durin::ERHIBufferLifetimeUsage::MultiFrame,
				std::as_bytes(std::span{Values}));
			ASSERT_TRUE(Uniform);
			const auto ResultDesc = Durin::FRHIBufferCreateDesc::Create(
				"Metal deferred uniform result", 8, 4,
				Durin::EBufferUsageFlags::StructuredBuffer
					| Durin::EBufferUsageFlags::UnorderedAccess
					| Durin::EBufferUsageFlags::KeepCPUAccessible);
			auto Result = Durin::GDynamicRHI->RHITryCreateBuffer(Commands, ResultDesc);
			ASSERT_TRUE(Result.has_value());
			const Durin::FRHIBufferViewDesc ViewDesc{
				.Offset = 0, .Size = 8,
				.Type = Durin::ERHIBufferViewType::StructuredStorage};
			auto View = Durin::GDynamicRHI->RHICreateBufferView(
				Result->GetReference(), ViewDesc);
			ASSERT_TRUE(View);
			const std::array<Durin::FRHIShaderParameterResource, 2> Parameters{{
				{.Resource = Uniform.GetReference(), .SetIndex = 0,
					.BindingIndex = 0, .Type = Durin::ERHIBindingType::UniformBuffer,
					.Size = 16},
				{.Resource = View.GetReference(), .SetIndex = 0,
					.BindingIndex = 1, .Type = Durin::ERHIBindingType::StorageBuffer}}};
			const auto Signal = Commands.BeginGPUSubmission(
				{.Queue = Durin::GDynamicRHI->RHIGetQueueCapabilities().Compute});
			Commands.SwitchPipeline(Durin::ERHIPipeline::Compute);
			Commands.SetComputePipelineState(*Pipeline);
			Commands.SetShaderParameters(Shader.GetReference(), Parameters);
			Commands.Dispatch(1, 1, 1);
			Values[0] = 13;
			Values[1] = 1;
			Commands.UpdateUniformBuffer(Uniform, std::as_bytes(std::span{Values}));
			Commands.Dispatch(1, 1, 1);
			Commands.EndGPUSubmission();
			Uniform = nullptr;
			View = nullptr;
			Commands.ImmediateFlush(Durin::EImmediateFlushType::FlushRHIThread,
				Durin::ERHISubmitFlags::SubmitToGPU);
			ASSERT_EQ(Durin::GDynamicRHI->RHIWaitForCompletion(Signal, 1'000'000'000),
				Durin::ERHIGPUWaitResult::Complete);
			const auto* Output = static_cast<const uint32_t*>(
				static_cast<Durin::FMetalBuffer*>(Result->GetReference())->GetHandle()->contents());
			ASSERT_NE(Output, nullptr);
			EXPECT_EQ(Output[0], 7u);
			EXPECT_EQ(Output[1], 13u);
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
			EXPECT_NE(static_cast<Durin::FMetalSampler*>(Sampler.GetReference())->GetHandle(), nullptr);
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
		EXPECT_NE(static_cast<Durin::FMetalBuffer*>(View->GetBuffer())->GetHandle(), nullptr);
		View = nullptr;
		Durin::RHIExit();
	}
}

TEST(FMetalRHIComputeTests, RecordedDispatchWritesStructuredBufferInBothExecutionModes)
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
			const std::string Source = "#include <metal_stdlib>\n"
				"using namespace metal;\n"
				"kernel void computeMain(device uint* result [[buffer(0)]], "
				"constant uint& scale [[buffer(1)]], "
				"uint id [[thread_position_in_grid]]) { result[id] = id * scale + 3; }\n";
			Durin::FByteBuffer Code;
			for (char Character : Source) Code.push_back(std::byte(Character));
			auto ShaderDesc = Durin::FRHIShaderCreateDesc::Create("ComputeMain",
				Durin::EShaderFrequency::Compute, Code,
				Durin::FXxHash128::HashBuffer(Code));
			ShaderDesc.Target = Durin::MetalShaderTarget;
			ShaderDesc.CodeFormat = Durin::EShaderCodeFormat::Msl20Source;
			ShaderDesc.ComputeThreadGroupSize = {4, 1, 1};
			ShaderDesc.SetEntryPoint("computeMain");
			ShaderDesc.MetalBindings.push_back({0, 0,
				Durin::ERHIBindingType::StorageBuffer, 0, 1});
			ShaderDesc.MetalPushConstantBufferSlot = 1;
			ShaderDesc.BindingRemapIdentity = Durin::ComputeMetalBindingRemapIdentity(
				Durin::EShaderFrequency::Compute, ShaderDesc.MetalBindings,
				ShaderDesc.MetalPushConstantBufferSlot);
			auto Shader = Durin::GDynamicRHI->RHICreateShader(ShaderDesc);
			ASSERT_TRUE(Shader);
			Durin::FComputePipelineStateInitializer Initializer;
			Initializer.ComputeShader = Shader.GetReference();
			Initializer.PipelineLayout.BindingLayouts.resize(1);
			Initializer.PipelineLayout.BindingLayouts[0].BindingLayouts.emplace_back(
				Durin::EShaderStageFlags::Compute, 0,
				Durin::ERHIBindingType::StorageBuffer);
			Initializer.PipelineLayout.PushConstantRanges.push_back(
				{Durin::EShaderStageFlags::Compute, 0, 4});
			auto Pipeline = Durin::GDynamicRHI->RHICreateComputePipelineState(
				"MetalComputeDispatch", Initializer);
			ASSERT_TRUE(Pipeline);
			const auto BufferDesc = Durin::FRHIBufferCreateDesc::Create(
				"Metal compute result", 64, 4,
				Durin::EBufferUsageFlags::StructuredBuffer
					| Durin::EBufferUsageFlags::UnorderedAccess
					| Durin::EBufferUsageFlags::KeepCPUAccessible);
			auto BufferResult = Durin::GDynamicRHI->RHITryCreateBuffer(
				Commands, BufferDesc);
			ASSERT_TRUE(BufferResult.has_value());
			auto Buffer = std::move(*BufferResult);
			const Durin::FRHIBufferViewDesc ViewDesc{.Offset = 16, .Size = 32,
				.Type = Durin::ERHIBufferViewType::StructuredStorage};
			auto View = Durin::GDynamicRHI->RHICreateBufferView(
				Buffer.GetReference(), ViewDesc);
			ASSERT_TRUE(View);
			const Durin::FRHIShaderParameterResource Parameter{
				.Resource = View.GetReference(), .SetIndex = 0,
				.BindingIndex = 0, .Type = Durin::ERHIBindingType::StorageBuffer};
			const auto Signal = Commands.BeginGPUSubmission(
				{.Queue = Durin::GDynamicRHI->RHIGetQueueCapabilities().Graphics});
			Commands.SwitchPipeline(Durin::ERHIPipeline::Compute);
			Commands.SetComputePipelineState(*Pipeline);
			Commands.SetShaderParameters(Shader.GetReference(),
				std::span(&Parameter, 1));
			const uint32_t Scale = 7;
			Commands.PushConstants(Durin::EShaderStageFlags::Compute,
				0, sizeof(Scale), &Scale);
			Commands.Dispatch(2, 1, 1);
			Commands.EndGPUSubmission();
			Commands.ImmediateFlush(Durin::EImmediateFlushType::FlushRHIThread,
				Durin::ERHISubmitFlags::SubmitToGPU);
			ASSERT_EQ(Durin::GDynamicRHI->RHIWaitForCompletion(Signal, 1'000'000'000),
				Durin::ERHIGPUWaitResult::Complete);
			const auto* Values = static_cast<const uint32_t*>(
				static_cast<Durin::FMetalBuffer*>(Buffer.GetReference())
					->GetHandle()->contents());
			ASSERT_NE(Values, nullptr);
			for (uint32_t Index = 0; Index < 8; ++Index)
				EXPECT_EQ(Values[4 + Index], Index * 7 + 3);
			const Durin::FRHIDispatchIndirectArguments DispatchArgs{2, 1, 1};
			auto ArgsDesc = Durin::FRHIBufferCreateDesc::Create(
				"Metal indirect compute arguments", sizeof(DispatchArgs),
				sizeof(uint32_t), Durin::EBufferUsageFlags::DrawIndirect);
			ArgsDesc.InitialData = {.Data = &DispatchArgs,
				.Size = sizeof(DispatchArgs)};
			auto ArgsResult = Durin::GDynamicRHI->RHITryCreateBuffer(
				Commands, ArgsDesc);
			ASSERT_TRUE(ArgsResult.has_value());
			auto ArgsBuffer = std::move(*ArgsResult);
			const auto IndirectSignal = Commands.BeginGPUSubmission(
				{.Queue = Durin::GDynamicRHI->RHIGetQueueCapabilities().Compute});
			Commands.SwitchPipeline(Durin::ERHIPipeline::Compute);
			Commands.SetComputePipelineState(*Pipeline);
			Commands.SetShaderParameters(Shader.GetReference(),
				std::span(&Parameter, 1));
			const uint32_t IndirectScale = 9;
			Commands.PushConstants(Durin::EShaderStageFlags::Compute,
				0, sizeof(IndirectScale), &IndirectScale);
			Commands.DispatchIndirect(ArgsBuffer.GetReference(), 0);
			Commands.EndGPUSubmission();
			Commands.ImmediateFlush(Durin::EImmediateFlushType::FlushRHIThread,
				Durin::ERHISubmitFlags::SubmitToGPU);
			ASSERT_EQ(Durin::GDynamicRHI->RHIWaitForCompletion(
				IndirectSignal, 1'000'000'000), Durin::ERHIGPUWaitResult::Complete);
			for (uint32_t Index = 0; Index < 8; ++Index)
				EXPECT_EQ(Values[4 + Index], Index * 9 + 3);
			Durin::RHIExit();
		}
	}
}

TEST(FMetalRHIComputeTests, StorageWriteThenSampleUsesNativeTextureAndSamplerViews)
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
			auto TextureDesc = Durin::FRHITextureCreateDesc::Create2D(
				"Metal compute image", 4, 4, Durin::EPixelFormat::RGBA8_UNORM);
			TextureDesc.SetFlags(Durin::ETextureCreateFlags::Storage
				| Durin::ETextureCreateFlags::ShaderResource
				| Durin::ETextureCreateFlags::CPUReadback);
			ASSERT_TRUE(Durin::GDynamicRHI->RHIIsTextureSupported(TextureDesc));
			auto TextureResult = Durin::GDynamicRHI->RHITryCreateTexture(
				Commands, TextureDesc);
			ASSERT_TRUE(TextureResult.has_value());
			auto Texture = std::move(*TextureResult);
			const Durin::FRHITextureViewDesc StorageDesc{
				.Usage = Durin::ERHITextureViewUsage::Storage,
				.Dimension = Durin::ERHITextureViewDimension::Texture2D,
				.Format = Durin::EPixelFormat::RGBA8_UNORM,
				.Range = {Durin::ERHITextureAspect::Color, 0, 1, 0, 1}};
			auto StorageView = Durin::GDynamicRHI->RHICreateTextureView(
				Texture.GetReference(), StorageDesc);
			ASSERT_TRUE(StorageView);
			auto SampledDesc = StorageDesc;
			SampledDesc.Usage = Durin::ERHITextureViewUsage::Sampled;
			auto SampledView = Durin::GDynamicRHI->RHICreateTextureView(
				Texture.GetReference(), SampledDesc);
			ASSERT_TRUE(SampledView);
			auto Sampler = Durin::GDynamicRHI->RHICreateSampler(
				Durin::FRHISamplerDesc::PointClamp());
			ASSERT_TRUE(Sampler);
			const auto BufferDesc = Durin::FRHIBufferCreateDesc::Create(
				"Metal sampled result", 4, 4,
				Durin::EBufferUsageFlags::StructuredBuffer
					| Durin::EBufferUsageFlags::UnorderedAccess
					| Durin::EBufferUsageFlags::KeepCPUAccessible);
			auto BufferResult = Durin::GDynamicRHI->RHITryCreateBuffer(
				Commands, BufferDesc);
			ASSERT_TRUE(BufferResult.has_value());
			auto Buffer = std::move(*BufferResult);
			const Durin::FRHIBufferViewDesc BufferViewDesc{.Offset = 0,
				.Size = 4, .Type = Durin::ERHIBufferViewType::StructuredStorage};
			auto BufferView = Durin::GDynamicRHI->RHICreateBufferView(
				Buffer.GetReference(), BufferViewDesc);
			ASSERT_TRUE(BufferView);
			const std::string Source = "#include <metal_stdlib>\nusing namespace metal;\n"
				"kernel void writeMain(texture2d<float, access::write> image [[texture(0)]], "
				"uint2 id [[thread_position_in_grid]]) { image.write(float4(1, 0, 0, 1), id); }\n"
				"kernel void sampleMain(texture2d<float, access::sample> image [[texture(0)]], "
				"sampler pointSampler [[sampler(0)]], device uint* result [[buffer(0)]], "
				"uint id [[thread_position_in_grid]]) { result[id] = uint(image.sample(pointSampler, "
				"float2(0.5, 0.5)).r * 255.0f + 0.5f); }\n";
			Durin::FByteBuffer Code;
			for (char Character : Source) Code.push_back(std::byte(Character));
			auto MakeShader = [&](const char* Entry,
				std::vector<Durin::FMetalShaderBinding> Bindings,
				std::array<uint32_t, 3> Group) {
				auto Desc = Durin::FRHIShaderCreateDesc::Create(Entry,
					Durin::EShaderFrequency::Compute, Code,
					Durin::FXxHash128::HashBuffer(Code));
				Desc.Target = Durin::MetalShaderTarget;
				Desc.CodeFormat = Durin::EShaderCodeFormat::Msl20Source;
				Desc.ComputeThreadGroupSize = Group;
				Desc.SetEntryPoint(Entry);
				Desc.MetalBindings = std::move(Bindings);
				Desc.BindingRemapIdentity = Durin::ComputeMetalBindingRemapIdentity(
					Durin::EShaderFrequency::Compute, Desc.MetalBindings,
					Desc.MetalPushConstantBufferSlot);
				return Durin::GDynamicRHI->RHICreateShader(Desc);
			};
			auto WriteShader = MakeShader("writeMain",
				{{0, 0, Durin::ERHIBindingType::StorageImage, 0, 1}},
				{4, 4, 1});
			auto SampleShader = MakeShader("sampleMain",
				{{0, 0, Durin::ERHIBindingType::Texture, 0, 1},
					{0, 1, Durin::ERHIBindingType::Sampler, 0, 1},
					{0, 2, Durin::ERHIBindingType::StorageBuffer, 0, 1}},
				{1, 1, 1});
			ASSERT_TRUE(WriteShader);
			ASSERT_TRUE(SampleShader);
			auto MakePipeline = [&](Durin::FRHIShader* Shader,
				std::initializer_list<std::pair<uint32_t, Durin::ERHIBindingType>> Bindings) {
				Durin::FComputePipelineStateInitializer Initializer;
				Initializer.ComputeShader = Shader;
				Initializer.PipelineLayout.BindingLayouts.resize(1);
				for (const auto [Slot, Type] : Bindings)
					Initializer.PipelineLayout.BindingLayouts[0].BindingLayouts.emplace_back(
						Durin::EShaderStageFlags::Compute, Slot, Type);
				return Durin::GDynamicRHI->RHICreateComputePipelineState(
					"MetalImageCompute", Initializer);
			};
			auto WritePipeline = MakePipeline(WriteShader.GetReference(),
				{{0, Durin::ERHIBindingType::StorageImage}});
			auto SamplePipeline = MakePipeline(SampleShader.GetReference(),
				{{0, Durin::ERHIBindingType::Texture},
					{1, Durin::ERHIBindingType::Sampler},
					{2, Durin::ERHIBindingType::StorageBuffer}});
			ASSERT_TRUE(WritePipeline);
			ASSERT_TRUE(SamplePipeline);
			const Durin::FRHIShaderParameterResource WriteParameter{
				.Resource = StorageView.GetReference(), .SetIndex = 0,
				.BindingIndex = 0, .Type = Durin::ERHIBindingType::StorageImage};
			const std::array SampleParameters{
				Durin::FRHIShaderParameterResource{.Resource = SampledView.GetReference(),
					.SetIndex = 0, .BindingIndex = 0,
					.Type = Durin::ERHIBindingType::Texture},
				Durin::FRHIShaderParameterResource{.Resource = Sampler.GetReference(),
					.SetIndex = 0, .BindingIndex = 1,
					.Type = Durin::ERHIBindingType::Sampler},
				Durin::FRHIShaderParameterResource{.Resource = BufferView.GetReference(),
					.SetIndex = 0, .BindingIndex = 2,
					.Type = Durin::ERHIBindingType::StorageBuffer}};
			const auto Signal = Commands.BeginGPUSubmission(
				{.Queue = Durin::GDynamicRHI->RHIGetQueueCapabilities().Graphics});
			Commands.SwitchPipeline(Durin::ERHIPipeline::Compute);
			Commands.SetComputePipelineState(*WritePipeline);
			Commands.SetShaderParameters(WriteShader.GetReference(),
				std::span(&WriteParameter, 1));
			Commands.Dispatch(1, 1, 1);
			Commands.SetComputePipelineState(*SamplePipeline);
			Commands.SetShaderParameters(SampleShader.GetReference(), SampleParameters);
			Commands.Dispatch(1, 1, 1);
			Commands.EndGPUSubmission();
			Commands.ImmediateFlush(Durin::EImmediateFlushType::FlushRHIThread,
				Durin::ERHISubmitFlags::SubmitToGPU);
			ASSERT_EQ(Durin::GDynamicRHI->RHIWaitForCompletion(Signal, 1'000'000'000),
				Durin::ERHIGPUWaitResult::Complete);
			const auto* Value = static_cast<const uint32_t*>(
				static_cast<Durin::FMetalBuffer*>(Buffer.GetReference())
					->GetHandle()->contents());
			ASSERT_NE(Value, nullptr);
			EXPECT_EQ(*Value, 255u);
			Durin::FByteBuffer Pixels;
			ASSERT_TRUE(Durin::GDynamicRHI->RHIReadTexture2D(
				Commands, Texture.GetReference(), 0, 0, Pixels));
			ASSERT_EQ(Pixels.size(), 64u);
			for (size_t Index = 0; Index < Pixels.size(); Index += 4)
			{
				EXPECT_EQ(Pixels[Index], std::byte{255});
				EXPECT_EQ(Pixels[Index + 1], std::byte{0});
				EXPECT_EQ(Pixels[Index + 2], std::byte{0});
				EXPECT_EQ(Pixels[Index + 3], std::byte{255});
			}
			const std::string DrawSource = "#include <metal_stdlib>\n"
				"using namespace metal;\n"
				"struct VertexOut { float4 position [[position]]; float2 uv; };\n"
				"vertex VertexOut vertexMain(uint id [[vertex_id]]) { "
				"float2 p[3] = {float2(-1,-1),float2(3,-1),float2(-1,3)}; "
				"VertexOut o; o.position=float4(p[id],0,1); "
				"o.uv=(p[id]+1)*0.5; return o; }\n"
				"fragment float4 fragmentMain(VertexOut input [[stage_in]], "
				"texture2d<float, access::sample> image [[texture(0)]], "
				"sampler pointSampler [[sampler(0)]], "
				"constant float& factor [[buffer(0)]], "
				"constant float& extra [[buffer(1)]]) "
				"{ return float4(image.sample(pointSampler, input.uv).rgb * factor * extra, 1); }\n";
			Durin::FByteBuffer DrawCode;
			for (char Character : DrawSource) DrawCode.push_back(std::byte(Character));
			auto MakeDrawShader = [&](const char* Entry,
				Durin::EShaderFrequency Frequency,
				std::vector<Durin::FMetalShaderBinding> Bindings,
				uint32_t PushSlot) {
				auto Desc = Durin::FRHIShaderCreateDesc::Create(Entry,
					Frequency, DrawCode, Durin::FXxHash128::HashBuffer(DrawCode));
				Desc.Target = Durin::MetalShaderTarget;
				Desc.CodeFormat = Durin::EShaderCodeFormat::Msl20Source;
				Desc.SetEntryPoint(Entry);
				Desc.MetalBindings = std::move(Bindings);
				Desc.MetalPushConstantBufferSlot = PushSlot;
				Desc.BindingRemapIdentity = Durin::ComputeMetalBindingRemapIdentity(
					Frequency, Desc.MetalBindings,
					Desc.MetalPushConstantBufferSlot);
				return Durin::GDynamicRHI->RHICreateShader(Desc);
			};
			auto DrawVertex = MakeDrawShader("vertexMain",
				Durin::EShaderFrequency::Vertex, {}, UINT32_MAX);
			auto DrawFragment = MakeDrawShader("fragmentMain",
				Durin::EShaderFrequency::Fragment,
				{{0, 0, Durin::ERHIBindingType::Texture, 0, 1},
					{0, 1, Durin::ERHIBindingType::Sampler, 0, 1},
					{0, 2, Durin::ERHIBindingType::UniformBuffer, 0, 1}}, 1);
			ASSERT_TRUE(DrawVertex);
			ASSERT_TRUE(DrawFragment);
			auto Declaration = Durin::GDynamicRHI->RHICreateVertexDeclaration({});
			ASSERT_TRUE(Declaration);
			auto ColorDesc = Durin::FRHITextureCreateDesc::Create2D(
				"Metal sampled color", 4, 4, Durin::EPixelFormat::RGBA8_UNORM);
			ColorDesc.SetFlags(Durin::ETextureCreateFlags::RenderTargetable
				| Durin::ETextureCreateFlags::CPUReadback);
			auto ColorResult = Durin::GDynamicRHI->RHITryCreateTexture(
				Commands, ColorDesc);
			ASSERT_TRUE(ColorResult.has_value());
			auto Color = std::move(*ColorResult);
			Durin::FRHIRenderPassInfo Pass;
			Pass.RenderTargetLayout.NumColorRenderTargets = 1;
			Pass.RenderTargetLayout.ColorAttachments[0].RenderTarget.Format =
				Durin::EPixelFormat::RGBA8_UNORM;
			Pass.ColorRenderTargets[0] = Color.GetReference();
			Pass.ColorClearValues[0] = Durin::FClearValueBinding(0, 1, 0, 1);
			Durin::FGraphicsPipelineStateInitializer DrawInitializer;
			DrawInitializer.BoundShaders = {DrawVertex.GetReference(),
				DrawFragment.GetReference()};
			DrawInitializer.VertexDeclaration = Declaration.GetReference();
			DrawInitializer.RenderTargetLayout = Pass.RenderTargetLayout;
			DrawInitializer.RasterizerState.CullMode = Durin::ERHICullMode::None;
			DrawInitializer.PipelineLayout.BindingLayouts.resize(1);
			DrawInitializer.PipelineLayout.BindingLayouts[0].BindingLayouts.emplace_back(
				Durin::EShaderStageFlags::Fragment, 0,
				Durin::ERHIBindingType::Texture);
			DrawInitializer.PipelineLayout.BindingLayouts[0].BindingLayouts.emplace_back(
				Durin::EShaderStageFlags::Fragment, 1,
				Durin::ERHIBindingType::Sampler);
			DrawInitializer.PipelineLayout.BindingLayouts[0].BindingLayouts.emplace_back(
				Durin::EShaderStageFlags::Fragment, 2,
				Durin::ERHIBindingType::UniformBuffer);
			DrawInitializer.PipelineLayout.PushConstantRanges.push_back(
				{Durin::EShaderStageFlags::Fragment, 0, 4});
			auto DrawPipeline = Durin::GDynamicRHI->RHICreateGraphicsPipelineState(
				"MetalSampledDraw", DrawInitializer);
			ASSERT_TRUE(DrawPipeline);
			const float Factor[4] = {0.5f, 0, 0, 0};
			auto UniformDesc = Durin::FRHIBufferCreateDesc::Create(
				"Metal fragment uniform", sizeof(Factor), sizeof(Factor),
				Durin::EBufferUsageFlags::UniformBuffer);
			UniformDesc.InitialData = {.Data = Factor, .Size = sizeof(Factor)};
			auto UniformResult = Durin::GDynamicRHI->RHITryCreateBuffer(
				Commands, UniformDesc);
			ASSERT_TRUE(UniformResult.has_value());
			auto Uniform = std::move(*UniformResult);
			const Durin::FRHIBufferViewDesc UniformViewDesc{.Offset = 0,
				.Size = sizeof(Factor), .Type = Durin::ERHIBufferViewType::Uniform};
			auto UniformView = Durin::GDynamicRHI->RHICreateBufferView(
				Uniform.GetReference(), UniformViewDesc);
			ASSERT_TRUE(UniformView);
			const std::array DrawParameters{
				Durin::FRHIShaderParameterResource{
					.Resource = SampledView.GetReference(), .SetIndex = 0,
					.BindingIndex = 0, .Type = Durin::ERHIBindingType::Texture},
				Durin::FRHIShaderParameterResource{
					.Resource = Sampler.GetReference(), .SetIndex = 0,
					.BindingIndex = 1, .Type = Durin::ERHIBindingType::Sampler},
				Durin::FRHIShaderParameterResource{
					.Resource = UniformView.GetReference(), .SetIndex = 0,
					.BindingIndex = 2, .Type = Durin::ERHIBindingType::UniformBuffer}};
			const auto DrawSignal = Commands.BeginGPUSubmission(
				{.Queue = Durin::GDynamicRHI->RHIGetQueueCapabilities().Graphics});
			Commands.SwitchPipeline(Durin::ERHIPipeline::Graphics);
			Commands.BeginRenderPass(Pass, "MetalSampledDraw");
			Commands.SetGraphicsPipelineState(*DrawPipeline);
			Commands.SetShaderParameters(DrawFragment.GetReference(), DrawParameters);
			const float Extra = 0.5f;
			Commands.PushConstants(Durin::EShaderStageFlags::Fragment,
				0, sizeof(Extra), &Extra);
			Commands.Draw({.VertexCount = 3});
			Commands.EndRenderPass();
			Commands.EndGPUSubmission();
			Commands.ImmediateFlush(Durin::EImmediateFlushType::FlushRHIThread,
				Durin::ERHISubmitFlags::SubmitToGPU);
			ASSERT_EQ(Durin::GDynamicRHI->RHIWaitForCompletion(
				DrawSignal, 1'000'000'000), Durin::ERHIGPUWaitResult::Complete);
			Pixels.clear();
			ASSERT_TRUE(Durin::GDynamicRHI->RHIReadTexture2D(
				Commands, Color.GetReference(), 0, 0, Pixels));
			ASSERT_EQ(Pixels.size(), 64u);
			for (size_t Index = 0; Index < Pixels.size(); Index += 4)
			{
				EXPECT_EQ(Pixels[Index], std::byte{64});
				EXPECT_EQ(Pixels[Index + 1], std::byte{0});
				EXPECT_EQ(Pixels[Index + 2], std::byte{0});
				EXPECT_EQ(Pixels[Index + 3], std::byte{255});
			}
			Durin::RHIExit();
		}
	}
}

TEST(FMetalRHITextureTests, CppViewAndSourceSurvivePoolDrainWithDelayedNativeWork)
{
	FScopedEnvironmentVariable Backend("DURIN_RHI_BACKEND", "metal");
	for (const char* Mode : {"inline", "threaded"})
	{
		SCOPED_TRACE(Mode);
		FScopedEnvironmentVariable Execution("DURIN_RHI_EXECUTION", Mode);
		for (uint32_t Round = 0; Round < 8; ++Round)
		{
			SCOPED_TRACE(Round);
			__weak id<MTLTexture> SourceNative = nil;
			__weak id<MTLTexture> ViewNative = nil;
			@autoreleasepool
			{
				ASSERT_TRUE(Durin::RHIInit(Durin::FRHIInitializationContext::Headless()));
				FScopedRHIExit Exit;
				auto& Commands = Durin::FRHICommandListImmediate::Get();
				Durin::TRefCountPtr<Durin::FRHITextureView> View;
				NS::SharedPtr<MTL::CommandQueue> Queue;
				NS::SharedPtr<MTL::CommandBuffer> Command;
				NS::SharedPtr<MTL::SharedEvent> Event;
				NS::SharedPtr<MTL::Buffer> Readback;
				std::array<uint8_t, 64> Pattern;
				for (size_t Index = 0; Index < Pattern.size(); ++Index)
					Pattern[Index] = static_cast<uint8_t>(Index * 7 + Round);
				@autoreleasepool
				{
					auto Desc = Durin::FRHITextureCreateDesc::Create2D(
						"Metal retained source", 4, 4, Durin::EPixelFormat::RGBA8_UNORM);
					Desc.SetFlags(Durin::ETextureCreateFlags::ShaderResource
						| Durin::ETextureCreateFlags::CPUReadback);
					auto Result = Durin::GDynamicRHI->RHITryCreateTexture(Commands, Desc);
					ASSERT_TRUE(Result);
					auto Texture = std::move(*Result);
					const Durin::FRHITextureViewDesc ViewDesc{
						.Usage = Durin::ERHITextureViewUsage::Sampled,
						.Dimension = Durin::ERHITextureViewDimension::Texture2D,
						.Format = Durin::EPixelFormat::RGBA8_UNORM,
						.Range = {Durin::ERHITextureAspect::Color, 0, 1, 0, 1}};
					View = Durin::GDynamicRHI->RHICreateTextureView(Texture.GetReference(), ViewDesc);
					ASSERT_TRUE(View);
					SourceNative = (__bridge id<MTLTexture>)static_cast<void*>(
						static_cast<Durin::FMetalTexture*>(Texture.GetReference())->GetHandle());
					auto* NativeView = static_cast<Durin::FMetalTextureView*>(View.GetReference())->GetHandle();
					ViewNative = (__bridge id<MTLTexture>)static_cast<void*>(NativeView);
					Durin::GDynamicRHI->RHIUpdateTexture2D(Commands, Texture.GetReference(),
						0, 0, {0, 0, 0, 0, 4, 4}, 16, std::as_bytes(std::span{Pattern}));
					// Finish the public upload before using a separate native qualification queue.
					Commands.BlockUntilGPUIdle();
					Commands.ImmediateFlush(Durin::EImmediateFlushType::FlushRHIThread);
					auto* Device = NativeView->device();
					Queue = NS::TransferPtr(Device->newCommandQueue());
					Event = NS::TransferPtr(Device->newSharedEvent());
					Readback = NS::TransferPtr(Device->newBuffer(64, MTL::ResourceStorageModeShared));
					ASSERT_TRUE(Queue);
					ASSERT_TRUE(Event);
					ASSERT_TRUE(Readback);
					Command = NS::RetainPtr(Queue->commandBuffer());
					ASSERT_TRUE(Command);
					Command->encodeWait(Event.get(), 1);
					auto Encoder = NS::RetainPtr(Command->blitCommandEncoder());
					ASSERT_TRUE(Encoder);
					Encoder->copyFromTexture(NativeView, 0, 0, MTL::Origin::Make(0, 0, 0),
						MTL::Size::Make(4, 4, 1), Readback.get(), 0, 16, 64);
					Encoder->endEncoding();
					Texture = nullptr;
				}
				ASSERT_TRUE(View->GetTexture());
				EXPECT_NE(SourceNative, nil);
				EXPECT_NE(ViewNative, nil);
				struct FSignalOnExit
				{
					MTL::SharedEvent* Event;
					~FSignalOnExit() { Event->setSignaledValue(1); }
				} SignalOnExit{Event.get()};
				Command->commit();
				@autoreleasepool
				{
					View = nullptr;
					Commands.ImmediateFlush(Durin::EImmediateFlushType::FlushRHIThread,
						Durin::ERHISubmitFlags::DeleteResources);
				}
				// The real GPU wait keeps work outstanding across both caller and pool release.
				EXPECT_NE(Command->status(), MTL::CommandBufferStatusCompleted);
				EXPECT_NE(SourceNative, nil);
				EXPECT_NE(ViewNative, nil);
				Event->setSignaledValue(1);
				Command->waitUntilCompleted();
				ASSERT_EQ(Command->status(), MTL::CommandBufferStatusCompleted);
				ASSERT_NE(Readback->contents(), nullptr);
				EXPECT_EQ(std::memcmp(Readback->contents(), Pattern.data(), Pattern.size()), 0);
				Command.reset();
				Queue.reset();
				Durin::RHIExit();
			}
			EXPECT_EQ(SourceNative, nil);
			EXPECT_EQ(ViewNative, nil);
		}
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
			UnsupportedDesc.AddFlags(Durin::ETextureCreateFlags::ResolveTargetable);
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
				static_cast<Durin::FMetalBuffer*>(Destination.GetReference())->GetHandle()->contents());
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

TEST(FMetalRHITextureTests, RecordedColorClearIsReadableAfterGPUCompletion)
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
			constexpr uint32_t Width = 8;
			constexpr uint32_t Height = 8;
			auto Desc = Durin::FRHITextureCreateDesc::Create2D(
				"Metal color clear", Width, Height, Durin::EPixelFormat::RGBA8_UNORM);
			Desc.SetFlags(Durin::ETextureCreateFlags::RenderTargetable
				| Durin::ETextureCreateFlags::CPUReadback);
			ASSERT_TRUE(Durin::GDynamicRHI->RHIIsTextureSupported(Desc));
			auto Created = Durin::GDynamicRHI->RHITryCreateTexture(Commands, Desc);
			ASSERT_TRUE(Created.has_value());
			auto Texture = std::move(*Created);
			Durin::FRHIRenderPassInfo Pass;
			Pass.RenderTargetLayout.NumColorRenderTargets = 1;
			Pass.RenderTargetLayout.ColorAttachments[0].RenderTarget.Format =
				Durin::EPixelFormat::RGBA8_UNORM;
			Pass.ColorRenderTargets[0] = Texture.GetReference();
			Pass.ColorClearValues[0] = Durin::FClearValueBinding(0.0f, 1.0f, 0.0f, 1.0f);
			ASSERT_TRUE(Pass.RenderTargetLayout.IsValid());
			const auto Signal = Commands.BeginGPUSubmission(
				{.Queue = Durin::GDynamicRHI->RHIGetQueueCapabilities().Graphics});
			Commands.SwitchPipeline(Durin::ERHIPipeline::Graphics);
			Commands.BeginRenderPass(Pass, "MetalColorClear");
			Commands.EndRenderPass();
			Commands.EndGPUSubmission();
			Commands.ImmediateFlush(Durin::EImmediateFlushType::FlushRHIThread,
				Durin::ERHISubmitFlags::SubmitToGPU);
			ASSERT_EQ(Durin::GDynamicRHI->RHIWaitForCompletion(Signal, 1'000'000'000),
				Durin::ERHIGPUWaitResult::Complete);
			Durin::FByteBuffer Pixels;
			ASSERT_TRUE(Durin::GDynamicRHI->RHIReadTexture2D(
				Commands, Texture.GetReference(), 0, 0, Pixels));
			ASSERT_EQ(Pixels.size(), Width * Height * 4);
			for (size_t Index = 0; Index < Pixels.size(); Index += 4)
			{
			EXPECT_EQ(Pixels[Index], std::byte{0});
			EXPECT_EQ(Pixels[Index + 1], std::byte{255});
			EXPECT_EQ(Pixels[Index + 2], std::byte{0});
			EXPECT_EQ(Pixels[Index + 3], std::byte{255});
			}
			Texture = nullptr;
			Durin::RHIExit();
		}
	}
}

TEST(FMetalRHIViewportTests, LayerViewportClearsAndPresentsInBothExecutionModes)
{
	@autoreleasepool
	{
		FScopedEnvironmentVariable Backend("DURIN_RHI_BACKEND", "metal");
		for (const char* Mode : {"inline", "threaded"})
		{
			SCOPED_TRACE(Mode);
			FScopedEnvironmentVariable Execution("DURIN_RHI_EXECUTION", Mode);
			CAMetalLayer* Layer = [CAMetalLayer layer];
			void* LayerHandle = (__bridge void*)Layer;
			const auto Target = Durin::FRHIPresentationTarget{.PlatformTarget =
				std::make_shared<Durin::FMacOSPresentationTarget>(LayerHandle,
					reinterpret_cast<CA::MetalLayer*>(LayerHandle))};
			ASSERT_TRUE(Durin::RHIInit(
				Durin::FRHIInitializationContext::Presentation(Target)));
			FScopedRHIExit Exit;
			Durin::FRHIViewportCreateInfo Info;
			Info.PresentationTarget = Target;
			Info.SizeX = 8;
			Info.SizeY = 8;
			Info.PreferredPixelFormat = Durin::EPixelFormat::SBGRA8_UNORM;
			Info.bAdoptInitializationPresentationCandidate = true;
			auto MissingLayerInfo = Info;
			MissingLayerInfo.PresentationTarget.PlatformTarget =
				std::make_shared<Durin::FMacOSPresentationTarget>(LayerHandle, nullptr);
			EXPECT_FALSE(Durin::GDynamicRHI->RHICreateViewport(MissingLayerInfo));
			MissingLayerInfo.PresentationTarget.PlatformTarget =
				std::make_shared<Durin::FNativePresentationTarget>(LayerHandle);
			EXPECT_FALSE(Durin::GDynamicRHI->RHICreateViewport(MissingLayerInfo));
			auto Viewport = Durin::GDynamicRHI->RHICreateViewport(Info);
			ASSERT_TRUE(Viewport);
			id<CAMetalDrawable> Probe = [Layer nextDrawable];
			ASSERT_NE(Probe, nil);
			Probe = nil;
			EXPECT_FALSE(Durin::GDynamicRHI->RHICreateViewport(Info));
			auto BackBuffer = Durin::GDynamicRHI->RHIGetViewportBackBuffer(
				Viewport.GetReference());
			ASSERT_TRUE(BackBuffer);
			EXPECT_EQ(BackBuffer->GetFormat(), Durin::EPixelFormat::SBGRA8_UNORM);
			auto& Commands = Durin::FRHICommandListImmediate::Get();
			Durin::FRHIRenderPassInfo Pass;
			Pass.RenderTargetLayout.NumColorRenderTargets = 1;
			Pass.RenderTargetLayout.ColorAttachments[0].RenderTarget.Format =
				BackBuffer->GetFormat();
			Pass.ColorRenderTargets[0] = BackBuffer.GetReference();
			Pass.ColorClearValues[0] = Durin::FClearValueBinding(1, 0, 0, 1);
			Commands.SwitchPipeline(Durin::ERHIPipeline::Graphics);
			Commands.BeginDrawingViewport(Viewport.GetReference(), nullptr);
			Commands.BeginRenderPass(Pass, "MetalViewportClear");
			Commands.EndRenderPass();
			Commands.EndDrawingViewport(Viewport.GetReference(), true, false);
			Commands.ImmediateFlush(Durin::EImmediateFlushType::FlushRHIThread,
				Durin::ERHISubmitFlags::SubmitToGPU);
			Durin::FByteBuffer Pixels;
			ASSERT_TRUE(Durin::GDynamicRHI->RHIReadTexture2D(
				Commands, BackBuffer.GetReference(), 0, 0, Pixels));
			ASSERT_EQ(Pixels.size(), 8u * 8u * 4u);
			for (size_t Index = 0; Index < Pixels.size(); Index += 4)
			{
				EXPECT_EQ(Pixels[Index], std::byte{0});
				EXPECT_EQ(Pixels[Index + 1], std::byte{0});
				EXPECT_EQ(Pixels[Index + 2], std::byte{255});
				EXPECT_EQ(Pixels[Index + 3], std::byte{255});
			}
			Durin::GDynamicRHI->RHIResizeViewport(Viewport.GetReference(), 12, 10, false);
			auto Resized = Durin::GDynamicRHI->RHIGetViewportBackBuffer(
				Viewport.GetReference());
			ASSERT_TRUE(Resized);
			EXPECT_EQ(Resized->GetSizeX(), 12u);
			EXPECT_EQ(Resized->GetSizeY(), 10u);
			Pass.ColorRenderTargets[0] = Resized.GetReference();
			Pass.ColorClearValues[0] = Durin::FClearValueBinding(0, 1, 0, 1);
			const auto ResizedSignal = Commands.BeginGPUSubmission(
				{.Queue = Durin::GDynamicRHI->RHIGetQueueCapabilities().Graphics});
			Commands.BeginRenderPass(Pass, "MetalResizedViewportClear");
			Commands.EndRenderPass();
			Commands.EndGPUSubmission();
			Commands.BeginDrawingViewport(Viewport.GetReference(), nullptr);
			Commands.EndDrawingViewport(Viewport.GetReference(), true, false);
			Commands.ImmediateFlush(Durin::EImmediateFlushType::FlushRHIThread,
				Durin::ERHISubmitFlags::SubmitToGPU);
			ASSERT_EQ(Durin::GDynamicRHI->RHIWaitForCompletion(
				ResizedSignal, 1'000'000'000), Durin::ERHIGPUWaitResult::Complete);
			Pixels.clear();
			ASSERT_TRUE(Durin::GDynamicRHI->RHIReadTexture2D(
				Commands, Resized.GetReference(), 0, 0, Pixels));
			ASSERT_EQ(Pixels.size(), 12u * 10u * 4u);
			for (size_t Index = 0; Index < Pixels.size(); Index += 4)
			{
				EXPECT_EQ(Pixels[Index], std::byte{0});
				EXPECT_EQ(Pixels[Index + 1], std::byte{255});
				EXPECT_EQ(Pixels[Index + 2], std::byte{0});
				EXPECT_EQ(Pixels[Index + 3], std::byte{255});
			}
			Durin::GDynamicRHI->RHIResizeViewport(Viewport.GetReference(), 0, 0, false);
			EXPECT_EQ(Durin::GDynamicRHI->RHIGetViewportBackBuffer(
				Viewport.GetReference())->GetSizeX(), 12u);
			Resized = nullptr;
			BackBuffer = nullptr;
			Viewport = nullptr;
			Info.bAdoptInitializationPresentationCandidate = false;
			auto RecreatedViewport = Durin::GDynamicRHI->RHICreateViewport(Info);
			ASSERT_TRUE(RecreatedViewport);
			RecreatedViewport = nullptr;
			Durin::RHIExit();
		}
	}
}

TEST(FMetalRHITextureTests, ProductionFallbackDescriptorsAdmitShaderOnlyUsage)
{
	@autoreleasepool
	{
		FScopedEnvironmentVariable Backend("DURIN_RHI_BACKEND", "metal");
		ASSERT_TRUE(Durin::RHIInit(Durin::FRHIInitializationContext::Headless()));
		FScopedRHIExit Exit;
		auto& Commands = Durin::FRHICommandListImmediate::Get();
		auto CubeDesc = Durin::FRHITextureCreateDesc::CreateCube("Metal fallback cube")
			.SetExtent(1)
			.SetFormat(Durin::EPixelFormat::RGBA8_UNORM)
			.SetFlags(Durin::ETextureCreateFlags::ShaderResource);
		ASSERT_TRUE(Durin::GDynamicRHI->RHIIsTextureSupported(CubeDesc));
		auto CubeResult = Durin::GDynamicRHI->RHITryCreateTexture(Commands, CubeDesc);
		ASSERT_TRUE(CubeResult.has_value());
		auto Cube = std::move(*CubeResult);
		auto CubeView = Durin::GDynamicRHI->RHICreateTextureView(
			Cube.GetReference(), Durin::MakeDefaultTextureViewDesc(
				*Cube, Durin::ERHITextureViewUsage::Sampled));
		ASSERT_TRUE(CubeView);
		EXPECT_EQ(static_cast<Durin::FMetalTextureView*>(CubeView.GetReference())
			->GetHandle()->textureType(), MTL::TextureTypeCube);
		const std::array<uint8_t, 4> Red{255, 0, 0, 255};
		const Durin::FUpdateTextureRegion2D Region(0, 0, 0, 0, 1, 1);
		Durin::GDynamicRHI->RHIUpdateTexture2D(Commands, Cube.GetReference(),
			0, 0, Region, 4, std::as_bytes(std::span(Red)));
		auto ShadowDesc = Durin::FRHITextureCreateDesc::Create2DArray(
			"Metal fallback shadow")
			.SetExtent(1)
			.SetArraySize(3)
			.SetFormat(Durin::EPixelFormat::D32)
			.SetFlags(Durin::ETextureCreateFlags::ShaderResource);
		ASSERT_TRUE(Durin::GDynamicRHI->RHIIsTextureSupported(ShadowDesc));
		auto ShadowResult = Durin::GDynamicRHI->RHITryCreateTexture(
			Commands, ShadowDesc);
		ASSERT_TRUE(ShadowResult.has_value());
		auto Shadow = std::move(*ShadowResult);
		auto ShadowView = Durin::GDynamicRHI->RHICreateTextureView(
			Shadow.GetReference(), Durin::MakeDefaultTextureViewDesc(
				*Shadow, Durin::ERHITextureViewUsage::Sampled));
		ASSERT_TRUE(ShadowView);
		EXPECT_EQ(static_cast<Durin::FMetalTextureView*>(ShadowView.GetReference())
			->GetHandle()->textureType(), MTL::TextureType2DArray);
		const std::string Source = "#include <metal_stdlib>\nusing namespace metal;\n"
			"kernel void sampleCube(texturecube<float, access::sample> cube [[texture(0)]], "
			"device float4* result [[buffer(0)]]) { "
			"constexpr sampler nearest(coord::normalized, filter::nearest); "
			"result[0] = cube.sample(nearest, float3(1,0,0)); }\n"
			"kernel void writeCubeFace(texture2d<float, access::write> face [[texture(0)]]) { "
			"face.write(float4(1,0,0,1), uint2(0,0)); }\n";
		Durin::FByteBuffer Code;
		for (char Character : Source) Code.push_back(std::byte(Character));
		auto ShaderDesc = Durin::FRHIShaderCreateDesc::Create("MetalCubeSample",
			Durin::EShaderFrequency::Compute, Code,
			Durin::FXxHash128::HashBuffer(Code));
		ShaderDesc.Target = Durin::MetalShaderTarget;
		ShaderDesc.CodeFormat = Durin::EShaderCodeFormat::Msl20Source;
		ShaderDesc.ComputeThreadGroupSize = {1, 1, 1};
		ShaderDesc.SetEntryPoint("sampleCube");
		ShaderDesc.MetalBindings = {
			{0, 0, Durin::ERHIBindingType::Texture, 0, 1},
			{0, 1, Durin::ERHIBindingType::StorageBuffer, 0, 1}};
		ShaderDesc.BindingRemapIdentity = Durin::ComputeMetalBindingRemapIdentity(
			Durin::EShaderFrequency::Compute, ShaderDesc.MetalBindings,
			ShaderDesc.MetalPushConstantBufferSlot);
		auto Shader = Durin::GDynamicRHI->RHICreateShader(ShaderDesc);
		ASSERT_TRUE(Shader);
		Durin::FComputePipelineStateInitializer PipelineDesc;
		PipelineDesc.ComputeShader = Shader.GetReference();
		PipelineDesc.PipelineLayout.BindingLayouts.resize(1);
		PipelineDesc.PipelineLayout.BindingLayouts[0].BindingLayouts.emplace_back(
			Durin::EShaderStageFlags::Compute, 0, Durin::ERHIBindingType::Texture);
		PipelineDesc.PipelineLayout.BindingLayouts[0].BindingLayouts.emplace_back(
			Durin::EShaderStageFlags::Compute, 1,
			Durin::ERHIBindingType::StorageBuffer);
		auto Pipeline = Durin::GDynamicRHI->RHICreateComputePipelineState(
			"MetalCubeSample", PipelineDesc);
		ASSERT_TRUE(Pipeline);
		auto ResultDesc = Durin::FRHIBufferCreateDesc::Create("Metal cube result",
			16, 16, Durin::EBufferUsageFlags::StructuredBuffer
				| Durin::EBufferUsageFlags::UnorderedAccess
				| Durin::EBufferUsageFlags::KeepCPUAccessible);
		auto ResultBuffer = Durin::GDynamicRHI->RHITryCreateBuffer(
			Commands, ResultDesc);
		ASSERT_TRUE(ResultBuffer.has_value());
		const Durin::FRHIBufferViewDesc ResultViewDesc{
			.Offset = 0, .Size = 16,
			.Type = Durin::ERHIBufferViewType::StructuredStorage};
		auto ResultView = Durin::GDynamicRHI->RHICreateBufferView(
			ResultBuffer->GetReference(), ResultViewDesc);
		ASSERT_TRUE(ResultView);
		const std::array<Durin::FRHIShaderParameterResource, 2> Parameters{{
			{.Resource = CubeView.GetReference(), .SetIndex = 0,
				.BindingIndex = 0, .Type = Durin::ERHIBindingType::Texture},
			{.Resource = ResultView.GetReference(), .SetIndex = 0,
				.BindingIndex = 1, .Type = Durin::ERHIBindingType::StorageBuffer}}};
		const auto Signal = Commands.BeginGPUSubmission(
			{.Queue = Durin::GDynamicRHI->RHIGetQueueCapabilities().Compute});
		Commands.SwitchPipeline(Durin::ERHIPipeline::Compute);
		Commands.SetComputePipelineState(*Pipeline);
		Commands.SetShaderParameters(Shader.GetReference(), Parameters);
		Commands.Dispatch(1, 1, 1);
		Commands.EndGPUSubmission();
		Commands.ImmediateFlush(Durin::EImmediateFlushType::FlushRHIThread,
			Durin::ERHISubmitFlags::SubmitToGPU);
		ASSERT_EQ(Durin::GDynamicRHI->RHIWaitForCompletion(
			Signal, 1'000'000'000), Durin::ERHIGPUWaitResult::Complete);
		const auto* Result = static_cast<const float*>(
			static_cast<Durin::FMetalBuffer*>(ResultBuffer->GetReference())
				->GetHandle()->contents());
		ASSERT_NE(Result, nullptr);
		EXPECT_FLOAT_EQ(Result[0], 1.0f);
		EXPECT_FLOAT_EQ(Result[1], 0.0f);
		EXPECT_FLOAT_EQ(Result[2], 0.0f);
		EXPECT_FLOAT_EQ(Result[3], 1.0f);
		auto RadianceDesc = Durin::FRHITextureCreateDesc::CreateCube(
			"Metal writable radiance cube")
			.SetExtent(1)
			.SetFormat(Durin::EPixelFormat::RGBA16_FLOAT)
			.SetFlags(Durin::ETextureCreateFlags::ShaderResource
				| Durin::ETextureCreateFlags::Storage
				| Durin::ETextureCreateFlags::CPUReadback);
		ASSERT_TRUE(Durin::GDynamicRHI->RHIIsTextureSupported(RadianceDesc));
		auto RadianceResult = Durin::GDynamicRHI->RHITryCreateTexture(
			Commands, RadianceDesc);
		ASSERT_TRUE(RadianceResult.has_value());
		auto Radiance = std::move(*RadianceResult);
		auto FaceDesc = Durin::MakeDefaultTextureViewDesc(
			*Radiance, Durin::ERHITextureViewUsage::Storage);
		FaceDesc.Dimension = Durin::ERHITextureViewDimension::Texture2D;
		FaceDesc.Range.NumArrayLayers = 1;
		auto FaceView = Durin::GDynamicRHI->RHICreateTextureView(
			Radiance.GetReference(), FaceDesc);
		ASSERT_TRUE(FaceView);
		EXPECT_EQ(static_cast<Durin::FMetalTextureView*>(FaceView.GetReference())
			->GetHandle()->textureType(), MTL::TextureType2D);
		auto WriteDesc = Durin::FRHIShaderCreateDesc::Create("MetalCubeFaceWrite",
			Durin::EShaderFrequency::Compute, Code,
			Durin::FXxHash128::HashBuffer(Code));
		WriteDesc.Target = Durin::MetalShaderTarget;
		WriteDesc.CodeFormat = Durin::EShaderCodeFormat::Msl20Source;
		WriteDesc.ComputeThreadGroupSize = {1, 1, 1};
		WriteDesc.SetEntryPoint("writeCubeFace");
		WriteDesc.MetalBindings = {
			{0, 0, Durin::ERHIBindingType::StorageImage, 0, 1}};
		WriteDesc.BindingRemapIdentity = Durin::ComputeMetalBindingRemapIdentity(
			Durin::EShaderFrequency::Compute, WriteDesc.MetalBindings,
			WriteDesc.MetalPushConstantBufferSlot);
		auto WriteShader = Durin::GDynamicRHI->RHICreateShader(WriteDesc);
		ASSERT_TRUE(WriteShader);
		Durin::FComputePipelineStateInitializer WritePipelineDesc;
		WritePipelineDesc.ComputeShader = WriteShader.GetReference();
		WritePipelineDesc.PipelineLayout.BindingLayouts.resize(1);
		WritePipelineDesc.PipelineLayout.BindingLayouts[0].BindingLayouts.emplace_back(
			Durin::EShaderStageFlags::Compute, 0,
			Durin::ERHIBindingType::StorageImage);
		auto WritePipeline = Durin::GDynamicRHI->RHICreateComputePipelineState(
			"MetalCubeFaceWrite", WritePipelineDesc);
		ASSERT_TRUE(WritePipeline);
		const Durin::FRHIShaderParameterResource FaceParameter{
			.Resource = FaceView.GetReference(), .SetIndex = 0,
			.BindingIndex = 0, .Type = Durin::ERHIBindingType::StorageImage};
		const auto WriteSignal = Commands.BeginGPUSubmission(
			{.Queue = Durin::GDynamicRHI->RHIGetQueueCapabilities().Compute});
		Commands.SwitchPipeline(Durin::ERHIPipeline::Compute);
		Commands.SetComputePipelineState(*WritePipeline);
		Commands.SetShaderParameters(WriteShader.GetReference(),
			std::span(&FaceParameter, 1));
		Commands.Dispatch(1, 1, 1);
		Commands.EndGPUSubmission();
		Commands.ImmediateFlush(Durin::EImmediateFlushType::FlushRHIThread,
			Durin::ERHISubmitFlags::SubmitToGPU);
		ASSERT_EQ(Durin::GDynamicRHI->RHIWaitForCompletion(
			WriteSignal, 1'000'000'000), Durin::ERHIGPUWaitResult::Complete);
		Durin::FByteBuffer RadiancePixels;
		ASSERT_TRUE(Durin::GDynamicRHI->RHIReadTexture2D(
			Commands, Radiance.GetReference(), 0, 0, RadiancePixels));
		const std::array<std::byte, 8> ExpectedRadiance{
			std::byte{0}, std::byte{0x3c}, std::byte{0}, std::byte{0},
			std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0x3c}};
		EXPECT_EQ(RadiancePixels, Durin::FByteBuffer(
			ExpectedRadiance.begin(), ExpectedRadiance.end()));
		Durin::RHIExit();
	}
}

TEST(FMetalRHITextureTests, BC1SrgbCubeUploadsAndReadsBlockRowsAndMipTail)
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
			const auto Desc = Durin::FRHITextureCreateDesc::CreateCube("Metal BC1 sky cube")
				.SetExtent(8)
				.SetFormat(Durin::EPixelFormat::BC1_UNORM_SRGB)
				.SetNumMips(4)
				.SetFlags(Durin::ETextureCreateFlags::ShaderResource
					| Durin::ETextureCreateFlags::CPUReadback);
			ASSERT_TRUE(Durin::GDynamicRHI->RHIIsTextureSupported(Desc));
			auto UnsupportedCopy = Desc;
			UnsupportedCopy.SetFlags(Durin::ETextureCreateFlags::ShaderResource
				| Durin::ETextureCreateFlags::SourceCopy);
			EXPECT_FALSE(Durin::GDynamicRHI->RHIIsTextureSupported(UnsupportedCopy));
			auto Created = Durin::GDynamicRHI->RHITryCreateTexture(Commands, Desc);
			ASSERT_TRUE(Created.has_value());
			auto Cube = std::move(*Created);
			const std::array<std::byte, 8> GrayBlock{
				std::byte{0x10}, std::byte{0x84}, std::byte{0x10}, std::byte{0x84},
				std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}};
			std::array<std::byte, 40> Pitched{};
			for (uint32_t Block = 0; Block < 2; ++Block)
			{
				std::memcpy(Pitched.data() + Block * 8, GrayBlock.data(), 8);
				std::memcpy(Pitched.data() + 24 + Block * 8, GrayBlock.data(), 8);
			}
			const auto Signal = Commands.BeginGPUSubmission(
				{.Queue = Durin::GDynamicRHI->RHIGetQueueCapabilities().Graphics});
			for (uint32_t Face = 0; Face < 6; ++Face)
			{
				Durin::GDynamicRHI->RHIUpdateTexture2D(Commands, Cube.GetReference(),
					0, Face, {0, 0, 0, 0, 8, 8}, 24, std::span(Pitched));
				for (uint32_t Mip = 1; Mip < 4; ++Mip)
				{
					const uint32_t Extent = 8u >> Mip;
					Durin::GDynamicRHI->RHIUpdateTexture2D(Commands, Cube.GetReference(),
						Mip, Face, {0, 0, 0, 0, Extent, Extent}, 8, std::span(GrayBlock));
				}
			}
			Commands.EndGPUSubmission();
			Commands.ImmediateFlush(Durin::EImmediateFlushType::FlushRHIThread,
				Durin::ERHISubmitFlags::SubmitToGPU);
			ASSERT_EQ(Durin::GDynamicRHI->RHIWaitForCompletion(Signal, 1'000'000'000),
				Durin::ERHIGPUWaitResult::Complete);
			Durin::FByteBuffer Readback;
			ASSERT_TRUE(Durin::GDynamicRHI->RHIReadTexture2D(
				Commands, Cube.GetReference(), 0, 0, Readback));
			ASSERT_EQ(Readback.size(), 32u);
			for (size_t Block = 0; Block < 4; ++Block)
				EXPECT_EQ(std::memcmp(Readback.data() + Block * 8,
					GrayBlock.data(), 8), 0);
			Readback.clear();
			ASSERT_TRUE(Durin::GDynamicRHI->RHIReadTexture2D(
				Commands, Cube.GetReference(), 3, 5, Readback));
			EXPECT_EQ(Readback, (Durin::FByteBuffer(GrayBlock.begin(), GrayBlock.end())));
			auto CubeView = Durin::GDynamicRHI->RHICreateTextureView(
				Cube.GetReference(), Durin::MakeDefaultTextureViewDesc(
					*Cube, Durin::ERHITextureViewUsage::Sampled));
			ASSERT_TRUE(CubeView);
			const std::string Source = "#include <metal_stdlib>\nusing namespace metal;\n"
				"kernel void sampleBC1(texturecube<float, access::sample> cube [[texture(0)]], "
				"device float4* output [[buffer(0)]]) { "
				"constexpr sampler nearest(coord::normalized, filter::nearest); "
				"output[0] = cube.sample(nearest, float3(1,0,0)); }\n";
			Durin::FByteBuffer Code;
			for (char Character : Source) Code.push_back(std::byte(Character));
			auto ShaderDesc = Durin::FRHIShaderCreateDesc::Create("MetalBC1CubeSample",
				Durin::EShaderFrequency::Compute, Code,
				Durin::FXxHash128::HashBuffer(Code));
			ShaderDesc.Target = Durin::MetalShaderTarget;
			ShaderDesc.CodeFormat = Durin::EShaderCodeFormat::Msl20Source;
			ShaderDesc.ComputeThreadGroupSize = {1, 1, 1};
			ShaderDesc.SetEntryPoint("sampleBC1");
			ShaderDesc.MetalBindings = {
				{0, 0, Durin::ERHIBindingType::Texture, 0, 1},
				{0, 1, Durin::ERHIBindingType::StorageBuffer, 0, 1}};
			ShaderDesc.BindingRemapIdentity = Durin::ComputeMetalBindingRemapIdentity(
				Durin::EShaderFrequency::Compute, ShaderDesc.MetalBindings,
				ShaderDesc.MetalPushConstantBufferSlot);
			auto Shader = Durin::GDynamicRHI->RHICreateShader(ShaderDesc);
			ASSERT_TRUE(Shader);
			Durin::FComputePipelineStateInitializer PipelineDesc;
			PipelineDesc.ComputeShader = Shader.GetReference();
			PipelineDesc.PipelineLayout.BindingLayouts.resize(1);
			PipelineDesc.PipelineLayout.BindingLayouts[0].BindingLayouts.emplace_back(
				Durin::EShaderStageFlags::Compute, 0, Durin::ERHIBindingType::Texture);
			PipelineDesc.PipelineLayout.BindingLayouts[0].BindingLayouts.emplace_back(
				Durin::EShaderStageFlags::Compute, 1, Durin::ERHIBindingType::StorageBuffer);
			auto Pipeline = Durin::GDynamicRHI->RHICreateComputePipelineState(
				"MetalBC1CubeSample", PipelineDesc);
			ASSERT_TRUE(Pipeline);
			const auto ResultDesc = Durin::FRHIBufferCreateDesc::Create(
				"Metal BC1 sampled color", 16, 16,
				Durin::EBufferUsageFlags::StructuredBuffer
					| Durin::EBufferUsageFlags::UnorderedAccess
					| Durin::EBufferUsageFlags::KeepCPUAccessible);
			auto Result = Durin::GDynamicRHI->RHITryCreateBuffer(Commands, ResultDesc);
			ASSERT_TRUE(Result.has_value());
			const Durin::FRHIBufferViewDesc ResultViewDesc{
				.Offset = 0, .Size = 16,
				.Type = Durin::ERHIBufferViewType::StructuredStorage};
			auto ResultView = Durin::GDynamicRHI->RHICreateBufferView(
				Result->GetReference(), ResultViewDesc);
			ASSERT_TRUE(ResultView);
			const std::array<Durin::FRHIShaderParameterResource, 2> Parameters{{
				{.Resource = CubeView.GetReference(), .SetIndex = 0,
					.BindingIndex = 0, .Type = Durin::ERHIBindingType::Texture},
				{.Resource = ResultView.GetReference(), .SetIndex = 0,
					.BindingIndex = 1, .Type = Durin::ERHIBindingType::StorageBuffer}}};
			const auto SampleSignal = Commands.BeginGPUSubmission(
				{.Queue = Durin::GDynamicRHI->RHIGetQueueCapabilities().Compute});
			Commands.SwitchPipeline(Durin::ERHIPipeline::Compute);
			Commands.SetComputePipelineState(*Pipeline);
			Commands.SetShaderParameters(Shader.GetReference(), Parameters);
			Commands.Dispatch(1, 1, 1);
			Commands.EndGPUSubmission();
			Commands.ImmediateFlush(Durin::EImmediateFlushType::FlushRHIThread,
				Durin::ERHISubmitFlags::SubmitToGPU);
			ASSERT_EQ(Durin::GDynamicRHI->RHIWaitForCompletion(SampleSignal, 1'000'000'000),
				Durin::ERHIGPUWaitResult::Complete);
			const auto* Color = static_cast<const float*>(
				static_cast<Durin::FMetalBuffer*>(Result->GetReference())->GetHandle()->contents());
			ASSERT_NE(Color, nullptr);
			// BC1 expands 0x8410 to (16/31, 32/63, 16/31) in sRGB space.
			EXPECT_NEAR(Color[0], 0.2307f, 0.01f);
			EXPECT_NEAR(Color[1], 0.2233f, 0.01f);
			EXPECT_NEAR(Color[2], 0.2307f, 0.01f);
			EXPECT_FLOAT_EQ(Color[3], 1.0f);
			Durin::RHIExit();
		}
	}
}

TEST(FMetalRHITextureTests, DepthArrayLayersDrawIndependentlyInBothExecutionModes)
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
			auto Desc = Durin::FRHITextureCreateDesc::Create2DArray(
				"Metal layered depth target")
				.SetExtent(4)
				.SetArraySize(3)
				.SetFormat(Durin::EPixelFormat::D32)
				.SetFlags(Durin::ETextureCreateFlags::DepthStencilTargetable
					| Durin::ETextureCreateFlags::ShaderResource
					| Durin::ETextureCreateFlags::CPUReadback);
			ASSERT_TRUE(Durin::GDynamicRHI->RHIIsTextureSupported(Desc));
			auto TargetResult = Durin::GDynamicRHI->RHITryCreateTexture(Commands, Desc);
			ASSERT_TRUE(TargetResult.has_value());
			auto Target = std::move(*TargetResult);
			const std::string Source = "#include <metal_stdlib>\nusing namespace metal;\n"
				"vertex float4 depthMain(uint id [[vertex_id]]) { "
				"float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)}; "
				"return float4(p[id],0.125,1); }\n"
				"fragment void emptyMain() {}\n";
			Durin::FByteBuffer Code;
			for (char Character : Source) Code.push_back(std::byte(Character));
			auto MakeShader = [&](const char* Entry,
				Durin::EShaderFrequency Frequency) {
				auto ShaderDesc = Durin::FRHIShaderCreateDesc::Create(Entry,
					Frequency, Code, Durin::FXxHash128::HashBuffer(Code));
				ShaderDesc.Target = Durin::MetalShaderTarget;
				ShaderDesc.CodeFormat = Durin::EShaderCodeFormat::Msl20Source;
				ShaderDesc.SetEntryPoint(Entry);
				ShaderDesc.BindingRemapIdentity = Durin::ComputeMetalBindingRemapIdentity(
					Frequency, ShaderDesc.MetalBindings,
					ShaderDesc.MetalPushConstantBufferSlot);
				return Durin::GDynamicRHI->RHICreateShader(ShaderDesc);
			};
			auto Vertex = MakeShader("depthMain", Durin::EShaderFrequency::Vertex);
			auto Fragment = MakeShader("emptyMain", Durin::EShaderFrequency::Fragment);
			auto Declaration = Durin::GDynamicRHI->RHICreateVertexDeclaration({});
			ASSERT_TRUE(Vertex);
			ASSERT_TRUE(Fragment);
			ASSERT_TRUE(Declaration);
			const std::array<float, 3> ClearDepths{0.25f, 0.5f, 0.75f};
			for (uint32_t Layer = 0; Layer < 3; ++Layer)
			{
				auto ViewDesc = Durin::MakeDefaultTextureViewDesc(
					*Target, Durin::ERHITextureViewUsage::DepthStencilAttachment);
				ViewDesc.Dimension = Durin::ERHITextureViewDimension::Texture2D;
				ViewDesc.Range.FirstArrayLayer = Layer;
				ViewDesc.Range.NumArrayLayers = 1;
				auto View = Durin::GDynamicRHI->RHICreateTextureView(
					Target.GetReference(), ViewDesc);
				ASSERT_TRUE(View);
				Durin::FRHIRenderPassInfo Pass;
				Pass.RenderTargetLayout.bHasDepthStencil = true;
				Pass.RenderTargetLayout.DepthStencilAttachment.Format =
					Durin::EPixelFormat::D32;
				Pass.DepthStencilRenderTarget = Target.GetReference();
				Pass.DepthStencilRenderTargetView = View.GetReference();
				Pass.DepthStencilClearValue = Durin::FClearValueBinding(
					ClearDepths[Layer], 0);
				ASSERT_TRUE(Pass.RenderTargetLayout.IsValid());
				Durin::FGraphicsPipelineStateInitializer Initializer;
				Initializer.VertexDeclaration = Declaration.GetReference();
				Initializer.RenderTargetLayout = Pass.RenderTargetLayout;
				Initializer.RasterizerState.CullMode = Durin::ERHICullMode::None;
				Initializer.RasterizerState.bEnableDepthBias = true;
				Initializer.DepthStencilState.bEnableTest = true;
				Initializer.DepthStencilState.bEnableWrite = true;
				Initializer.DepthStencilState.CompareOp = Durin::ERHIDepthCompareOp::Less;
				Initializer.BoundShaders = {Vertex.GetReference(), Fragment.GetReference()};
				auto Pipeline = Durin::GDynamicRHI->RHICreateGraphicsPipelineState(
					"MetalLayeredDepthOnly", Initializer);
				ASSERT_TRUE(Pipeline);
				const auto Signal = Commands.BeginGPUSubmission(
					{.Queue = Durin::GDynamicRHI->RHIGetQueueCapabilities().Graphics});
				Commands.SwitchPipeline(Durin::ERHIPipeline::Graphics);
				Commands.BeginRenderPass(Pass, "MetalLayeredDepthClear");
				Commands.SetGraphicsPipelineState(*Pipeline);
				Commands.SetDepthBias(0.0f, 0.0f, 0.0f);
				Commands.Draw({.VertexCount = 3});
				Commands.EndRenderPass();
				Commands.EndGPUSubmission();
				Commands.ImmediateFlush(Durin::EImmediateFlushType::FlushRHIThread,
					Durin::ERHISubmitFlags::SubmitToGPU);
				ASSERT_EQ(Durin::GDynamicRHI->RHIWaitForCompletion(
					Signal, 1'000'000'000), Durin::ERHIGPUWaitResult::Complete);
			}
			for (uint32_t Layer = 0; Layer < 3; ++Layer)
			{
				Durin::FByteBuffer Pixels;
				ASSERT_TRUE(Durin::GDynamicRHI->RHIReadTexture2D(
					Commands, Target.GetReference(), 0, Layer, Pixels));
				ASSERT_EQ(Pixels.size(), 4u * 4u * sizeof(float));
				for (size_t Offset = 0; Offset < Pixels.size(); Offset += sizeof(float))
				{
					float Value = 0;
					std::memcpy(&Value, Pixels.data() + Offset, sizeof(Value));
					EXPECT_FLOAT_EQ(Value, 0.125f);
				}
			}
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
			static_cast<Durin::FMetalBuffer*>(Destination.GetReference())->GetHandle()->contents());
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
					static_cast<Durin::FMetalBuffer*>(Destination.GetReference())->GetHandle()->contents());
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
				static_cast<Durin::FMetalBuffer*>(Destination.GetReference())->GetHandle()->contents());
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
				static_cast<Durin::FMetalBuffer*>(Destination.GetReference())->GetHandle()->contents());
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
					| Durin::ETextureCreateFlags::DestinationCopy
					| Durin::ETextureCreateFlags::ShaderResource);
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
			auto SampledViewDesc = VolumeViewDesc;
			SampledViewDesc.Usage = Durin::ERHITextureViewUsage::Sampled;
			auto SampledView = Durin::GDynamicRHI->RHICreateTextureView(
				First.GetReference(), SampledViewDesc);
			ASSERT_TRUE(SampledView);
			EXPECT_EQ(static_cast<Durin::FMetalTextureView*>(SampledView.GetReference())
				->GetHandle()->textureType(), MTL::TextureType3D);
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
			const std::string ShaderSource =
				"#include <metal_stdlib>\nusing namespace metal;\n"
				"kernel void sampleVolume(texture3d<float, access::sample> volume [[texture(0)]], "
				"device uint* result [[buffer(0)]]) { "
				"constexpr sampler nearest(coord::normalized, filter::nearest); "
				"result[0] = uint(volume.sample(nearest, float3(0.25)).r * 255.0f + 0.5f); }\n";
			Durin::FByteBuffer ShaderCode;
			for (char Character : ShaderSource) ShaderCode.push_back(std::byte(Character));
			auto ShaderDesc = Durin::FRHIShaderCreateDesc::Create("MetalVolumeSample",
				Durin::EShaderFrequency::Compute, ShaderCode,
				Durin::FXxHash128::HashBuffer(ShaderCode));
			ShaderDesc.Target = Durin::MetalShaderTarget;
			ShaderDesc.CodeFormat = Durin::EShaderCodeFormat::Msl20Source;
			ShaderDesc.ComputeThreadGroupSize = {1, 1, 1};
			ShaderDesc.SetEntryPoint("sampleVolume");
			ShaderDesc.MetalBindings = {
				{0, 0, Durin::ERHIBindingType::Texture, 0, 1},
				{0, 1, Durin::ERHIBindingType::StorageBuffer, 0, 1}};
			ShaderDesc.BindingRemapIdentity = Durin::ComputeMetalBindingRemapIdentity(
				Durin::EShaderFrequency::Compute, ShaderDesc.MetalBindings,
				ShaderDesc.MetalPushConstantBufferSlot);
			auto Shader = Durin::GDynamicRHI->RHICreateShader(ShaderDesc);
			ASSERT_TRUE(Shader);
			Durin::FComputePipelineStateInitializer PipelineDesc;
			PipelineDesc.ComputeShader = Shader.GetReference();
			PipelineDesc.PipelineLayout.BindingLayouts.resize(1);
			PipelineDesc.PipelineLayout.BindingLayouts[0].BindingLayouts.emplace_back(
				Durin::EShaderStageFlags::Compute, 0, Durin::ERHIBindingType::Texture);
			PipelineDesc.PipelineLayout.BindingLayouts[0].BindingLayouts.emplace_back(
				Durin::EShaderStageFlags::Compute, 1,
				Durin::ERHIBindingType::StorageBuffer);
			auto Pipeline = Durin::GDynamicRHI->RHICreateComputePipelineState(
				"MetalVolumeSample", PipelineDesc);
			ASSERT_TRUE(Pipeline);
			auto ResultDesc = Durin::FRHIBufferCreateDesc::Create("Volume sample result",
				4, 4, Durin::EBufferUsageFlags::StructuredBuffer
					| Durin::EBufferUsageFlags::UnorderedAccess
					| Durin::EBufferUsageFlags::KeepCPUAccessible);
			auto ResultBuffer = Durin::GDynamicRHI->RHITryCreateBuffer(Commands, ResultDesc);
			ASSERT_TRUE(ResultBuffer.has_value());
			const Durin::FRHIBufferViewDesc ResultViewDesc{
				.Offset = 0, .Size = 4,
				.Type = Durin::ERHIBufferViewType::StructuredStorage};
			auto ResultView = Durin::GDynamicRHI->RHICreateBufferView(
				ResultBuffer->GetReference(), ResultViewDesc);
			ASSERT_TRUE(ResultView);
			const std::array<Durin::FRHIShaderParameterResource, 2> Parameters{{
				{.Resource = SampledView.GetReference(), .SetIndex = 0,
					.BindingIndex = 0, .Type = Durin::ERHIBindingType::Texture},
				{.Resource = ResultView.GetReference(), .SetIndex = 0,
					.BindingIndex = 1, .Type = Durin::ERHIBindingType::StorageBuffer}}};
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
			Commands.SwitchPipeline(Durin::ERHIPipeline::Compute);
			Commands.SetComputePipelineState(*Pipeline);
			Commands.SetShaderParameters(Shader.GetReference(), Parameters);
			Commands.Dispatch(1, 1, 1);
			Commands.EndGPUSubmission();
			Source = nullptr;
			SampledView = nullptr;
			First = nullptr;
			Second = nullptr;
			Commands.ImmediateFlush(Durin::EImmediateFlushType::FlushRHIThread,
				Durin::ERHISubmitFlags::SubmitToGPU);
			ASSERT_EQ(Durin::GDynamicRHI->RHIWaitForCompletion(Signal, 1'000'000'000),
				Durin::ERHIGPUWaitResult::Complete);
			const auto* Bytes = static_cast<const uint8_t*>(
				static_cast<Durin::FMetalBuffer*>(Destination.GetReference())->GetHandle()->contents());
			ASSERT_NE(Bytes, nullptr);
			EXPECT_EQ(std::memcmp(Bytes, Pattern, ByteCount), 0);
			const auto* SampledValue = static_cast<const uint32_t*>(
				static_cast<Durin::FMetalBuffer*>(ResultBuffer->GetReference())
					->GetHandle()->contents());
			ASSERT_NE(SampledValue, nullptr);
			EXPECT_EQ(*SampledValue, 4u);
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
					static_cast<Durin::FMetalBuffer*>(Buffer.GetReference())->GetHandle()->contents());
				ASSERT_NE(Output, nullptr);
				EXPECT_EQ(std::memcmp(Output, Expected, OutputSize), 0);
				Buffer = nullptr;
			}
			Texture = nullptr;
			Durin::RHIExit();
		}
	}
}

TEST(FMetalRHIPipelineTests, CppPipelineOwnersSurvivePoolsAndAsyncCacheTeardown)
{
	FScopedEnvironmentVariable Backend("DURIN_RHI_BACKEND", "metal");
	Durin::GGameThreadId = Durin::FPlatformLTS::GetCurrentThreadId();
	Durin::GIsGameThreadIdInitialized = true;
	ASSERT_TRUE(Durin::InitializeTaskScheduler(2));
	struct FStopScheduler { ~FStopScheduler() { Durin::ShutdownTaskScheduler(); } } Stop;
	for (const char* Mode : {"inline", "threaded"})
	for (int Round = 0; Round < 8; ++Round)
	{
		SCOPED_TRACE(Mode);
		SCOPED_TRACE(Round);
		FScopedEnvironmentVariable Execution("DURIN_RHI_EXECUTION", Mode);
		ASSERT_TRUE(Durin::RHIInit(Durin::FRHIInitializationContext::Headless()));
		FScopedRHIExit Exit;
		__weak id<MTLFunction> WeakFunction;
		__weak id<MTLComputePipelineState> WeakPipeline;
		Durin::FComputePipelineStateRef First;
		Durin::FComputePipelineStateRef Replacement;
		@autoreleasepool
		{
			const std::string Source = "#include <metal_stdlib>\nusing namespace metal;\nkernel void mainA() {}\nkernel void mainB() {}\nkernel void mainC() {}\n";
			Durin::FByteBuffer Code;
			for (char Character : Source) Code.push_back(std::byte(Character));
			auto Desc = Durin::FRHIShaderCreateDesc::Create("pool pipeline",
				Durin::EShaderFrequency::Compute, Code, Durin::FXxHash128::HashBuffer(Code));
			Desc.Target = Durin::MetalShaderTarget;
			Desc.CodeFormat = Durin::EShaderCodeFormat::Msl20Source;
			Desc.ComputeThreadGroupSize = {1, 1, 1};
			Desc.BindingRemapIdentity = Durin::ComputeMetalBindingRemapIdentity(
				Desc.Frequency, Desc.MetalBindings, Desc.MetalPushConstantBufferSlot);
			Desc.SetEntryPoint("mainA");
			auto Shader = Durin::GDynamicRHI->RHICreateShader(Desc);
			ASSERT_TRUE(Shader);
			WeakFunction = (__bridge id<MTLFunction>)static_cast<void*>(
				static_cast<Durin::FMetalShader*>(Shader.GetReference())->GetFunction());
			Durin::FComputePipelineStateInitializer Initializer;
			Initializer.ComputeShader = Shader.GetReference();
			auto* Cache = Durin::GDynamicRHI->RHIGetPipelineStateCache();
			ASSERT_NE(Cache, nullptr);
			auto Requested = Cache->GetCompute(Initializer, "first");
			ASSERT_TRUE(Requested);
			First = *Requested;
			Desc.SetEntryPoint("mainB");
			auto OtherShader = Durin::GDynamicRHI->RHICreateShader(Desc);
			ASSERT_TRUE(OtherShader);
			Initializer.ComputeShader = OtherShader.GetReference();
			auto Other = Cache->GetCompute(Initializer, "replacement");
			ASSERT_TRUE(Other);
			Replacement = *Other;
			Shader = nullptr;
			OtherShader = nullptr;
			ASSERT_TRUE(First->Wait());
			ASSERT_TRUE(Replacement->Wait());
			Desc.SetEntryPoint("mainC");
			auto LateShader = Durin::GDynamicRHI->RHICreateShader(Desc);
			ASSERT_TRUE(LateShader);
			Initializer.ComputeShader = LateShader.GetReference();
			auto Late = Cache->GetCompute(Initializer, "close outstanding");
			ASSERT_TRUE(Late);
			LateShader = nullptr;
			Cache->StopAndWait();
			EXPECT_TRUE((*Late)->IsComplete());
			EXPECT_TRUE((*Late)->GetState() == Durin::ERHIPipelineRequestState::Ready
				|| (*Late)->GetState() == Durin::ERHIPipelineRequestState::Canceled);
			if ((*Late)->GetState() == Durin::ERHIPipelineRequestState::Canceled)
				EXPECT_FALSE((*Late)->GetRHIPipeline());
			auto Native = First->GetRHIPipeline();
			ASSERT_TRUE(Native);
			EXPECT_NE(Native.GetReference(), Replacement->GetRHIPipeline().GetReference());
			WeakPipeline = (__bridge id<MTLComputePipelineState>)static_cast<void*>(
				static_cast<Durin::FMetalComputePipelineState*>(Native.GetReference())->GetPipeline());
		}
		EXPECT_NE(WeakFunction, nil);
		EXPECT_NE(WeakPipeline, nil);
		@autoreleasepool
		{
			Durin::RHIExit();
			EXPECT_FALSE(First->GetRHIPipeline());
			EXPECT_FALSE(Replacement->GetRHIPipeline());
			First.reset();
			Replacement.reset();
		}
		EXPECT_EQ(WeakFunction, nil);
		EXPECT_EQ(WeakPipeline, nil);
	}
}

TEST(FMetalRHISubmissionTests, DelayedGPUCompletionRetainsPayloadAndShutdownDrainsCallbacks)
{
	FScopedEnvironmentVariable Backend("DURIN_RHI_BACKEND", "metal");
	for (const char* Mode : {"inline", "threaded"})
	for (bool bShutdownWhilePending : {false, true})
	{
		SCOPED_TRACE(Mode);
		SCOPED_TRACE(bShutdownWhilePending);
		FScopedEnvironmentVariable Execution("DURIN_RHI_EXECUTION", Mode);
		ASSERT_TRUE(Durin::RHIInit(Durin::FRHIInitializationContext::Headless()));
		FScopedRHIExit Exit;
		auto& Commands = Durin::FRHICommandListImmediate::Get();
		__weak id<MTLBuffer> WeakOutput;
		__weak id<MTLComputePipelineState> WeakPipeline;
		auto Queue = NS::RetainPtr(Durin::GetMetalSubmissionQueue(*Durin::GDynamicRHI));
		ASSERT_TRUE(Queue);
		auto Event = NS::TransferPtr(Queue->device()->newSharedEvent());
		ASSERT_TRUE(Event);
		auto WaitCommand = NS::RetainPtr(Queue->commandBuffer());
		ASSERT_TRUE(WaitCommand);
		struct FOpenGate
		{
			MTL::SharedEvent* Event;
			auto Open() const -> void { if (Event) Event->setSignaledValue(1); }
			~FOpenGate() { Open(); }
		} OpenGate{Event.get()};
		Durin::FBufferRHIRef Destination;
		Durin::FRHIGPUSyncPointRef Signal;
		std::shared_ptr<Durin::FRHITextureReadback> Readback;
		@autoreleasepool
		{
			const std::string Source = "#include <metal_stdlib>\nusing namespace metal;\n"
				"kernel void delayed("
				"device uint* result [[buffer(0)]]) { "
				"result[0] = 0x12345678u; }\n";
			Durin::FByteBuffer Code;
			for (char Character : Source) Code.push_back(std::byte(Character));
			auto ShaderDesc = Durin::FRHIShaderCreateDesc::Create("delayed payload",
				Durin::EShaderFrequency::Compute, Code, Durin::FXxHash128::HashBuffer(Code));
			ShaderDesc.Target = Durin::MetalShaderTarget;
			ShaderDesc.CodeFormat = Durin::EShaderCodeFormat::Msl20Source;
			ShaderDesc.ComputeThreadGroupSize = {1, 1, 1};
			ShaderDesc.SetEntryPoint("delayed");
			ShaderDesc.MetalBindings = {{0, 0, Durin::ERHIBindingType::StorageBuffer, 0, 1}};
			ShaderDesc.BindingRemapIdentity = Durin::ComputeMetalBindingRemapIdentity(
				ShaderDesc.Frequency, ShaderDesc.MetalBindings, ShaderDesc.MetalPushConstantBufferSlot);
			auto Shader = Durin::GDynamicRHI->RHICreateShader(ShaderDesc);
			ASSERT_TRUE(Shader);
			Durin::FComputePipelineStateInitializer Initializer;
			Initializer.ComputeShader = Shader.GetReference();
			Initializer.PipelineLayout.BindingLayouts.resize(1);
			for (uint32_t Slot = 0; Slot < 1; ++Slot)
				Initializer.PipelineLayout.BindingLayouts[0].BindingLayouts.emplace_back(
					Durin::EShaderStageFlags::Compute, Slot, Durin::ERHIBindingType::StorageBuffer);
			auto Pipeline = Durin::GDynamicRHI->RHICreateComputePipelineState("delayed", Initializer);
			ASSERT_TRUE(Pipeline);
			WeakPipeline = (__bridge id<MTLComputePipelineState>)static_cast<void*>(
				static_cast<Durin::FMetalComputePipelineState*>(Pipeline.GetReference())->GetPipeline());
			const uint32_t Zero = 0;
			auto BufferDesc = Durin::FRHIBufferCreateDesc::Create("gate", 4, 4,
				Durin::EBufferUsageFlags::StructuredBuffer | Durin::EBufferUsageFlags::UnorderedAccess
					| Durin::EBufferUsageFlags::KeepCPUAccessible | Durin::EBufferUsageFlags::SourceCopy);
			BufferDesc.InitialData = {.Data = &Zero, .Size = sizeof(Zero)};
			auto OutputResult = Durin::GDynamicRHI->RHITryCreateBuffer(Commands, BufferDesc);
			ASSERT_TRUE(OutputResult);
			auto Output = std::move(*OutputResult);
			WeakOutput = (__bridge id<MTLBuffer>)static_cast<void*>(
				static_cast<Durin::FMetalBuffer*>(Output.GetReference())->GetHandle());
			const auto DestinationDesc = Durin::FRHIBufferCreateDesc::Create("delayed result", 4, 4,
				Durin::EBufferUsageFlags::DestinationCopy | Durin::EBufferUsageFlags::KeepCPUAccessible);
			auto DestinationResult = Durin::GDynamicRHI->RHITryCreateBuffer(Commands, DestinationDesc);
			ASSERT_TRUE(DestinationResult);
			Destination = std::move(*DestinationResult);
			const Durin::FRHIBufferViewDesc ViewDesc{.Offset = 0, .Size = 4,
				.Type = Durin::ERHIBufferViewType::StructuredStorage};
			auto OutputView = Durin::GDynamicRHI->RHICreateBufferView(Output.GetReference(), ViewDesc);
			ASSERT_TRUE(OutputView);
			const Durin::FRHIShaderParameterResource Parameter{
				.Resource = OutputView.GetReference(), .SetIndex = 0, .BindingIndex = 0,
				.Type = Durin::ERHIBindingType::StorageBuffer};
			auto TextureDesc = Durin::FRHITextureCreateDesc::Create2D("delayed readback", 1, 1,
				Durin::EPixelFormat::RGBA8_UNORM);
			TextureDesc.SetFlags(Durin::ETextureCreateFlags::DestinationCopy
				| Durin::ETextureCreateFlags::CPUReadback);
			auto TextureResult = Durin::GDynamicRHI->RHITryCreateTexture(Commands, TextureDesc);
			ASSERT_TRUE(TextureResult);
			auto Texture = std::move(*TextureResult);
			const Durin::FByteBuffer Pixels{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
			Commands.UpdateTexture2D(Texture.GetReference(), 0, 0, {0, 0, 0, 0, 1, 1}, 4, Pixels);
			Commands.BlockUntilGPUIdle();
			WaitCommand->encodeWait(Event.get(), 1);
			WaitCommand->commit();
			Signal = Commands.BeginGPUSubmission(
				{.Queue = Durin::GDynamicRHI->RHIGetQueueCapabilities().Compute});
			Commands.SwitchPipeline(Durin::ERHIPipeline::Compute);
			Commands.SetComputePipelineState(*Pipeline);
			Commands.SetShaderParameters(Shader.GetReference(), {&Parameter, 1});
			Commands.Dispatch(1, 1, 1);
			const Durin::FRHIBufferCopyRegion Copy{.Size = 4};
			Commands.CopyBuffer(Output.GetReference(), Destination.GetReference(), {&Copy, 1});
			Readback = Commands.EnqueueTextureReadback(Texture.GetReference());
			Commands.EndGPUSubmission();
			Shader = nullptr;
			Pipeline = nullptr;
			OutputView = nullptr;
			Output = nullptr;
			Texture = nullptr;
			Commands.ImmediateFlush(Durin::EImmediateFlushType::FlushRHIThread,
				Durin::ERHISubmitFlags::SubmitToGPU | Durin::ERHISubmitFlags::DeleteResources);
		}
		EXPECT_NE(WeakOutput, nil);
		EXPECT_NE(WeakPipeline, nil);
		EXPECT_EQ(Readback->GetState(), Durin::ERHITextureReadbackState::Pending);
		EXPECT_EQ(Durin::GDynamicRHI->RHIWaitForCompletion(Signal, 1'000'000),
			Durin::ERHIGPUWaitResult::Timeout);
		if (bShutdownWhilePending)
		{
			std::jthread Release([&] {
				std::this_thread::sleep_for(std::chrono::milliseconds(20));
				OpenGate.Open();
			});
			Durin::RHIExit();
		}
		else
		{
			OpenGate.Open();
			ASSERT_EQ(Durin::GDynamicRHI->RHIWaitForCompletion(Signal, 1'000'000'000),
				Durin::ERHIGPUWaitResult::Complete);
			Durin::RHIExit();
		}
		EXPECT_EQ(Readback->GetState(), Durin::ERHITextureReadbackState::Ready);
		Durin::FByteBuffer Actual;
		ASSERT_TRUE(Readback->TakePixels(Actual));
		EXPECT_EQ(Actual, (Durin::FByteBuffer{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}}));
		EXPECT_EQ(*static_cast<const uint32_t*>(
			static_cast<Durin::FMetalBuffer*>(Destination.GetReference())->GetHandle()->contents()), 0x12345678u);
		OpenGate.Event = nullptr;
		@autoreleasepool { WaitCommand.reset(); Queue.reset(); Event.reset(); Destination = nullptr; }
		EXPECT_EQ(WeakOutput, nil);
		EXPECT_EQ(WeakPipeline, nil);
	}
}

TEST(FMetalRHISubmissionTests, EmptySubmissionsCompleteInBothExecutionModes)
{
	FScopedEnvironmentVariable Backend("DURIN_RHI_BACKEND", "metal");
	for (const char* Mode : {"inline", "threaded"})
	{
		SCOPED_TRACE(Mode);
		FScopedEnvironmentVariable Execution("DURIN_RHI_EXECUTION", Mode);
		ASSERT_TRUE(Durin::RHIInit(Durin::FRHIInitializationContext::Headless()));
		FScopedRHIExit Exit;
		auto& Commands = Durin::FRHICommandListImmediate::Get();
		for (int Round = 0; Round < 16; ++Round)
		{
			const auto Signal = Commands.BeginGPUSubmission(
				{.Queue = Durin::GDynamicRHI->RHIGetQueueCapabilities().Graphics});
			Commands.EndGPUSubmission();
			Commands.ImmediateFlush(Durin::EImmediateFlushType::FlushRHIThread,
				Durin::ERHISubmitFlags::SubmitToGPU);
			EXPECT_EQ(Durin::GDynamicRHI->RHIWaitForCompletion(Signal, 1'000'000'000),
				Durin::ERHIGPUWaitResult::Complete);
		}
		Durin::RHIExit();
	}
}

TEST(FMetalRHISubmissionTests, SimulatedNativeErrorFailsReadbackAndReleasesPayload)
{
	const Durin::FMetalAutoreleasePool Pool;
	auto Native = Durin::CreateMetalCppDeviceAndQueue();
	Durin::FMetalSubmissionState State;
	State.Generation = Durin::AllocateRHIDeviceGeneration();
	State.Timeline = std::make_unique<Durin::FRHIGPUQueueTimeline>(State.Generation,
		Durin::FRHIQueueId{0});
	const auto Signal = State.Timeline->Reserve();
	ASSERT_TRUE(State.Timeline->MarkSubmitted(Signal));
	State.PendingCallbacks = 1;
	auto Request = std::make_shared<Durin::FRHITextureReadback>();
	std::weak_ptr<int> WeakStorage;
	__weak id<MTLBuffer> WeakBuffer;
	@autoreleasepool
	{
		auto Owners = std::make_shared<Durin::FMetalSubmissionOwners>();
		auto Buffer = NS::TransferPtr(Native.Device->newBuffer(4, MTL::ResourceStorageModeShared));
		ASSERT_TRUE(Buffer);
		WeakBuffer = (__bridge id<MTLBuffer>)static_cast<void*>(Buffer.get());
		auto Storage = std::make_shared<int>(7);
		WeakStorage = Storage;
		Owners->StorageOwners.push_back(Storage);
		Owners->NativeResources.push_back(Buffer);
		Owners->Readbacks.push_back({Buffer, 4, Request});
		Buffer.reset();
		Storage.reset();
		// Inject status into the production completion routine, without inducing a GPU fault.
		Durin::CompleteMetalSubmission(State, Signal, *Owners, MTL::CommandBufferStatusError);
		EXPECT_EQ(Request->GetState(), Durin::ERHITextureReadbackState::Failed);
		EXPECT_EQ(Signal.GetState(), Durin::ERHIGPUSubmissionState::Failed);
		EXPECT_EQ(State.PendingCallbacks, 0u);
		EXPECT_TRUE(WeakStorage.expired());
		Owners.reset();
	}
	EXPECT_TRUE(WeakStorage.expired());
	EXPECT_EQ(WeakBuffer, nil);
}

TEST(FMetalRHIViewportTests, CppLayerOwnershipAndDrawableFailureRecoverAcrossTeardown)
{
	FScopedEnvironmentVariable Backend("DURIN_RHI_BACKEND", "metal");
	for (const char* Mode : {"inline", "threaded"})
	for (int Round = 0; Round < 4; ++Round)
	{
		SCOPED_TRACE(Mode);
		SCOPED_TRACE(Round);
		FScopedEnvironmentVariable Execution("DURIN_RHI_EXECUTION", Mode);
		__weak FMetalQualificationLayer* WeakLayer;
		@autoreleasepool
		{
			FMetalQualificationLayer* Layer = [FMetalQualificationLayer layer];
			WeakLayer = Layer;
			void* Handle = (__bridge void*)Layer;
			const Durin::FRHIPresentationTarget Target{.PlatformTarget =
				std::make_shared<Durin::FMacOSPresentationTarget>(Handle,
					reinterpret_cast<CA::MetalLayer*>(Handle))};
			ASSERT_TRUE(Durin::RHIInit(Durin::FRHIInitializationContext::Presentation(
				Target)));
			FScopedRHIExit Exit;
			Durin::FRHIViewportCreateInfo Info;
			Info.PresentationTarget = Target;
			Info.SizeX = 8;
			Info.SizeY = 8;
			Info.bAdoptInitializationPresentationCandidate = true;
			auto Viewport = Durin::GDynamicRHI->RHICreateViewport(Info);
			ASSERT_TRUE(Viewport);
			Layer.FailNextDrawable = YES;
			Layer = nil;
			// The platform handle is borrowed; the viewport owns a separate layer reference.
			auto& Commands = Durin::FRHICommandListImmediate::Get();
			Commands.BeginDrawingViewport(Viewport.GetReference(), nullptr);
			Commands.EndDrawingViewport(Viewport.GetReference(), true, false);
			Commands.BlockUntilGPUIdle();
			Commands.ImmediateFlush(Durin::EImmediateFlushType::FlushRHIThread);
			ASSERT_NE(WeakLayer, nil);
			EXPECT_EQ(WeakLayer.AcquisitionCount, 1u);
			Commands.BeginDrawingViewport(Viewport.GetReference(), nullptr);
			Commands.EndDrawingViewport(Viewport.GetReference(), true, true);
			Commands.BlockUntilGPUIdle();
			Commands.ImmediateFlush(Durin::EImmediateFlushType::FlushRHIThread);
			EXPECT_EQ(WeakLayer.AcquisitionCount, 2u);
			EXPECT_EQ(WeakLayer.SuccessfulAcquisitionCount, 1u);
			EXPECT_TRUE(WeakLayer.displaySyncEnabled);
			Viewport = nullptr;
			Durin::RHIExit();
			[CATransaction flush];
		}
		for (int Attempt = 0; WeakLayer && Attempt < 20; ++Attempt)
		{
			@autoreleasepool
			{
				[[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
				[CATransaction flush];
			}
		}
		EXPECT_EQ(WeakLayer, nil);
	}
}


TEST(FMetalRHITransitionTests, ExactRangesAndRejectedBatchesPreserveStateInBothExecutionModes)
{
	using namespace Durin;
	FScopedEnvironmentVariable Backend("DURIN_RHI_BACKEND", "metal");
	for (const char* Mode : {"inline", "threaded"})
	{
		SCOPED_TRACE(Mode);
		FScopedEnvironmentVariable Execution("DURIN_RHI_EXECUTION", Mode);
		ASSERT_TRUE(RHIInit(FRHIInitializationContext::Headless()));
		FScopedRHIExit Exit;
		auto& Commands = FRHICommandListImmediate::Get();
		auto Buffer = GDynamicRHI->RHITryCreateBuffer(Commands, FRHIBufferCreateDesc::Create(
			"transition ranges", 64, 1, EBufferUsageFlags::SourceCopy | EBufferUsageFlags::DestinationCopy));
		ASSERT_TRUE(Buffer);
		auto TextureDesc = FRHITextureCreateDesc::Create2D("transition subresources", 4, 4, EPixelFormat::RGBA8_UNORM);
		TextureDesc.Dimension = ETextureDimension::Texture2DArray;
		TextureDesc.ArraySize = 2;
		TextureDesc.NumMips = 2;
		TextureDesc.SetFlags(ETextureCreateFlags::SourceCopy | ETextureCreateFlags::DestinationCopy);
		auto Texture = GDynamicRHI->RHITryCreateTexture(Commands, TextureDesc);
		ASSERT_TRUE(Texture);
		auto* NativeBuffer = static_cast<FMetalBuffer*>(Buffer->GetReference());
		auto* NativeTexture = static_cast<FMetalTexture*>(Texture->GetReference());
		const FRHITextureSubresourceRange First{ERHITextureAspect::Color, 0, 1, 0, 1};
		const FRHITextureSubresourceRange Last{ERHITextureAspect::Color, 1, 1, 1, 1};
		Commands.BeginGPUSubmission({.Queue = GDynamicRHI->RHIGetQueueCapabilities().Graphics});
		const std::array BufferTransitions{
			FRHIBufferTransition{Buffer->GetReference(), 0, 16, ERHIAccess::None, ERHIAccess::TransferWrite},
			FRHIBufferTransition{Buffer->GetReference(), 32, 16, ERHIAccess::None, ERHIAccess::TransferRead}};
		Commands.TransitionBuffers(BufferTransitions);
		const std::array TextureTransitions{
			FRHITextureTransition{Texture->GetReference(), First, ERHIAccess::None, ERHIAccess::TransferWrite},
			FRHITextureTransition{Texture->GetReference(), Last, ERHIAccess::None, ERHIAccess::TransferRead}};
		Commands.TransitionTextures(TextureTransitions);
		Commands.EndGPUSubmission();
		Commands.ImmediateFlush(EImmediateFlushType::FlushRHIThread);
		GCommandListExecutor.ExecuteSynchronousOperation(false, [&] {
			ERHIAccess Tracked = ERHIAccess::None;
			EXPECT_TRUE(NativeBuffer->GetStateTracker().Validate(16, 16, ERHIAccess::None, Tracked));
			EXPECT_FALSE(NativeBuffer->GetStateTracker().Validate(0, 64, ERHIAccess::TransferWrite, Tracked));
			EXPECT_TRUE(NativeBuffer->GetStateTracker().Validate(0, 64, ERHIAccess::Discard, Tracked));
			const std::array InvalidBuffers{
				FRHIBufferTransition{Buffer->GetReference(), 16, 16, ERHIAccess::None, ERHIAccess::TransferWrite},
				FRHIBufferTransition{Buffer->GetReference(), 32, 16, ERHIAccess::None, ERHIAccess::TransferWrite}};
			const auto BufferError = ApplyMetalBufferTransitions(InvalidBuffers);
			ASSERT_TRUE(BufferError);
			EXPECT_NE(BufferError->find("offset=32"), std::string::npos);
			EXPECT_TRUE(NativeBuffer->GetStateTracker().Validate(16, 16, ERHIAccess::None, Tracked));
			EXPECT_TRUE(NativeBuffer->GetStateTracker().Validate(32, 16, ERHIAccess::TransferRead, Tracked));
			const FRHITextureSubresourceRange Untouched{ERHITextureAspect::Color, 1, 1, 0, 1};
			EXPECT_TRUE(NativeTexture->ValidateAccess(Untouched, ERHIAccess::None, Tracked));
			const std::array InvalidTextures{
				FRHITextureTransition{Texture->GetReference(), Untouched, ERHIAccess::None, ERHIAccess::TransferWrite},
				FRHITextureTransition{Texture->GetReference(), Last, ERHIAccess::None, ERHIAccess::TransferWrite, true}};
			const auto TextureError = ApplyMetalTextureTransitions(InvalidTextures);
			ASSERT_TRUE(TextureError);
			EXPECT_NE(TextureError->find("layer=1+1"), std::string::npos);
			// Content discard does not waive ExpectedBefore; no earlier entry was published.
			EXPECT_TRUE(NativeTexture->ValidateAccess(Untouched, ERHIAccess::None, Tracked));
			EXPECT_TRUE(NativeTexture->ValidateAccess(Last, ERHIAccess::TransferRead, Tracked));
			const std::array Reset{FRHITextureTransition{Texture->GetReference(),
				{ERHITextureAspect::Color, 0, 2, 0, 2}, ERHIAccess::Discard, ERHIAccess::TransferRead}};
			EXPECT_FALSE(ApplyMetalTextureTransitions(Reset));
			EXPECT_TRUE(NativeTexture->ValidateAccess({ERHITextureAspect::Color, 0, 2, 0, 2}, ERHIAccess::TransferRead, Tracked));
		});
		*Buffer = nullptr;
		*Texture = nullptr;
		RHIExit();
	}
}
