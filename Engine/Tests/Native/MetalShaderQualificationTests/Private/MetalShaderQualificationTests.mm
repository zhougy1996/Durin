#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "slang.h"
#include "slang-com-ptr.h"
#include "spirv_cross/spirv_cross_c.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace
{
	struct FFloat3
	{
		float X, Y, Z;
	};

	struct alignas(16) FCandidate
	{
		FFloat3 BoundsMin;
		uint32_t ArgumentIndex;
		FFloat3 BoundsMax;
		uint32_t TransformIndex;
	};

	struct alignas(16) FCullingUniform
	{
		float ViewProjection[16];
		uint32_t CandidateCount;
		uint32_t Padding[3];
	};

	static_assert(sizeof(FCandidate) == 32);
	static_assert(sizeof(FCullingUniform) == 80);

	auto CompileShaderCode(const char* FileName, const char* EntryName,
		SlangCompileTarget TargetFormat = SLANG_METAL,
		bool bExplicitComputeStage = false) -> std::string
	{
		std::ifstream File(std::string(DURIN_TEST_DATA_DIR) + "/" + FileName);
		if (!File.is_open())
		{
			ADD_FAILURE() << "Missing shader fixture " << FileName;
			return {};
		}
		const std::string Source((std::istreambuf_iterator<char>(File)), std::istreambuf_iterator<char>());
		Slang::ComPtr<slang::IGlobalSession> Global;
		if (SLANG_FAILED(slang_createGlobalSession(SLANG_API_VERSION, Global.writeRef())))
		{
			ADD_FAILURE() << "Slang global session creation failed";
			return {};
		}
		slang::TargetDesc Target = {};
		Target.format = TargetFormat;
		if (TargetFormat == SLANG_SPIRV)
			Target.profile = Global->findProfile("spirv_1_5");
		slang::SessionDesc SessionDesc = {};
		SessionDesc.targets = &Target;
		SessionDesc.targetCount = 1;
		Slang::ComPtr<slang::ISession> Session;
		if (SLANG_FAILED(Global->createSession(SessionDesc, Session.writeRef())))
		{
			ADD_FAILURE() << "Slang Metal session creation failed";
			return {};
		}
		Slang::ComPtr<slang::IBlob> Diagnostics;
		slang::IModule* Module = Session->loadModuleFromSourceString(
			FileName, FileName, Source.c_str(), Diagnostics.writeRef());
		if (!Module)
		{
			ADD_FAILURE() << (Diagnostics ? (const char*)Diagnostics->getBufferPointer() : "Slang module load failed");
			return {};
		}
		Slang::ComPtr<slang::IEntryPoint> Entry;
		const auto EntryResult = bExplicitComputeStage
			? Module->findAndCheckEntryPoint(EntryName, SLANG_STAGE_COMPUTE, Entry.writeRef(), Diagnostics.writeRef())
			: Module->findEntryPointByName(EntryName, Entry.writeRef());
		if (SLANG_FAILED(EntryResult))
		{
			ADD_FAILURE() << (Diagnostics ? (const char*)Diagnostics->getBufferPointer() : "Slang entry point lookup failed");
			return {};
		}
		slang::IComponentType* Components[] = {Module, Entry.get()};
		Slang::ComPtr<slang::IComponentType> Composite;
		if (SLANG_FAILED(Session->createCompositeComponentType(
			Components, 2, Composite.writeRef(), Diagnostics.writeRef())))
		{
			ADD_FAILURE() << (Diagnostics ? (const char*)Diagnostics->getBufferPointer() : "Slang composition failed");
			return {};
		}
		Slang::ComPtr<slang::IComponentType> Linked;
		if (SLANG_FAILED(Composite->link(Linked.writeRef(), Diagnostics.writeRef())))
		{
			ADD_FAILURE() << (Diagnostics ? (const char*)Diagnostics->getBufferPointer() : "Slang link failed");
			return {};
		}
		Slang::ComPtr<slang::IBlob> Code;
		if (SLANG_FAILED(Linked->getEntryPointCode(0, 0, Code.writeRef(), Diagnostics.writeRef())) || !Code)
		{
			ADD_FAILURE() << (Diagnostics ? (const char*)Diagnostics->getBufferPointer() : "Slang MSL output failed");
			return {};
		}
		return std::string((const char*)Code->getBufferPointer(), Code->getBufferSize());
	}

	auto TranslateSpirvToMetal(const std::string& Spirv,
		SpvExecutionModel Stage = SpvExecutionModelGLCompute,
		bool bResourceArray = false) -> std::string
	{
		if (Spirv.empty() || Spirv.size() % sizeof(SpvId) != 0)
		{
			ADD_FAILURE() << "SPIR-V output has an invalid size";
			return {};
		}
		std::vector<SpvId> Words(Spirv.size() / sizeof(SpvId));
		std::memcpy(Words.data(), Spirv.data(), Spirv.size());
		spvc_context Context = nullptr;
		if (spvc_context_create(&Context) != SPVC_SUCCESS)
		{
			ADD_FAILURE() << "SPIRV-Cross context creation failed";
			return {};
		}
		spvc_parsed_ir IR = nullptr;
		spvc_compiler Compiler = nullptr;
		const char* Source = nullptr;
		const auto ParseResult = spvc_context_parse_spirv(Context, Words.data(), Words.size(), &IR);
		const auto CompilerResult = ParseResult == SPVC_SUCCESS
			? spvc_context_create_compiler(Context, SPVC_BACKEND_MSL, IR,
				SPVC_CAPTURE_MODE_TAKE_OWNERSHIP, &Compiler)
			: ParseResult;
		spvc_result BindingResult = CompilerResult;
		if (BindingResult == SPVC_SUCCESS)
		{
			spvc_compiler_options Options = nullptr;
			BindingResult = spvc_compiler_create_compiler_options(Compiler, &Options);
			if (BindingResult == SPVC_SUCCESS)
				BindingResult = spvc_compiler_options_set_uint(Options,
					SPVC_COMPILER_OPTION_MSL_VERSION, SPVC_MAKE_MSL_VERSION(2, 0, 0));
			if (BindingResult == SPVC_SUCCESS)
				BindingResult = spvc_compiler_install_compiler_options(Compiler, Options);
		}
		if (BindingResult == SPVC_SUCCESS)
		{
			// Fixture-specific explicit remaps also reserve vertex buffer 0 for
			// the ImGui vertex stream. Production needs a checked allocation map.
			const unsigned BindingCount = bResourceArray ? 3 :
				Stage == SpvExecutionModelGLCompute ? 4 :
				Stage == SpvExecutionModelVertex ? 1 : 2;
			for (unsigned Index = 0; Index < BindingCount; ++Index)
			{
				spvc_msl_resource_binding_2 Binding;
				spvc_msl_resource_binding_init_2(&Binding);
				Binding.stage = Stage;
				Binding.desc_set = 0;
				Binding.binding = Stage == SpvExecutionModelVertex ? 2 : Index;
				Binding.count = bResourceArray && Index < 2 ? 2 : 1;
				if (bResourceArray && Index == 0)
					Binding.msl_texture = 0;
				else if (bResourceArray && Index == 1)
					Binding.msl_sampler = 0;
				else if (bResourceArray)
					Binding.msl_buffer = 0;
				else if (Stage == SpvExecutionModelGLCompute)
					Binding.msl_buffer = Index;
				else if (Stage == SpvExecutionModelVertex)
					Binding.msl_buffer = 1;
				else if (Index == 0)
					Binding.msl_texture = 0;
				else
					Binding.msl_sampler = 0;
				BindingResult = spvc_compiler_msl_add_resource_binding_2(Compiler, &Binding);
				if (BindingResult != SPVC_SUCCESS) break;
			}
			if (BindingResult == SPVC_SUCCESS && Stage == SpvExecutionModelGLCompute)
			{
				spvc_msl_resource_binding_2 PushBinding;
				spvc_msl_resource_binding_init_2(&PushBinding);
				PushBinding.stage = Stage;
				PushBinding.desc_set = SPVC_MSL_PUSH_CONSTANT_DESC_SET;
				PushBinding.binding = SPVC_MSL_PUSH_CONSTANT_BINDING;
				PushBinding.msl_buffer = 4;
				BindingResult = spvc_compiler_msl_add_resource_binding_2(Compiler, &PushBinding);
			}
		}
		const auto CompileResult = BindingResult == SPVC_SUCCESS
			? spvc_compiler_compile(Compiler, &Source) : BindingResult;
		std::string Output;
		if (CompileResult == SPVC_SUCCESS && Source)
			Output = Source;
		else
			ADD_FAILURE() << spvc_context_get_last_error_string(Context);
		spvc_context_destroy(Context);
		return Output;
	}

	TEST(FMetalShaderQualificationTests, RendererTextureFormatUsageAllocates)
	{
		@autoreleasepool
		{
			id<MTLDevice> Device = MTLCreateSystemDefaultDevice();
			ASSERT_NE(Device, nil);
			fprintf(stderr, "Metal qualification GPU: %s; Apple7=%d Apple8=%d Apple9=%d Apple10=%d\n",
				[[Device name] UTF8String], [Device supportsFamily:MTLGPUFamilyApple7],
				[Device supportsFamily:MTLGPUFamilyApple8],
				[Device supportsFamily:MTLGPUFamilyApple9],
				[Device supportsFamily:MTLGPUFamilyApple10]);
			struct FFormatCase
			{
				const char* Name;
				MTLPixelFormat Format;
				MTLTextureUsage Usage;
			};
			const FFormatCase Cases[] = {
				{"R16 sampled texture", MTLPixelFormatR16Float,
					MTLTextureUsageShaderRead},
				{"RG8 sampled texture", MTLPixelFormatRG8Unorm,
					MTLTextureUsageShaderRead},
				{"R8 visibility and cloud storage", MTLPixelFormatR8Unorm,
					MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite},
				{"RGBA8 GBuffer", MTLPixelFormatRGBA8Unorm,
					MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead},
				{"sRGB RGBA editor output", MTLPixelFormatRGBA8Unorm_sRGB,
					MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead},
				{"sRGB BGRA presentation", MTLPixelFormatBGRA8Unorm_sRGB,
					MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead},
				{"R11G11B10 GBuffer emissive", MTLPixelFormatRG11B10Float,
					MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead},
				{"RGBA16 scene color and compute storage", MTLPixelFormatRGBA16Float,
					MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite},
				{"RGBA32 sampled normal", MTLPixelFormatRGBA32Float,
					MTLTextureUsageShaderRead},
				{"RG32 hit proxy", MTLPixelFormatRG32Uint,
					MTLTextureUsageRenderTarget},
				{"D32 scene and shadow depth", MTLPixelFormatDepth32Float,
					MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead},
			};
			for (const FFormatCase& Case : Cases)
			{
				SCOPED_TRACE(Case.Name);
				MTLTextureDescriptor* Desc = [MTLTextureDescriptor
					texture2DDescriptorWithPixelFormat:Case.Format
					width:8 height:8 mipmapped:NO];
				Desc.storageMode = MTLStorageModePrivate;
				Desc.usage = Case.Usage;
				id<MTLTexture> Texture = [Device newTextureWithDescriptor:Desc];
				ASSERT_NE(Texture, nil);
				EXPECT_EQ(Texture.pixelFormat, Case.Format);
				EXPECT_EQ(Texture.usage, Case.Usage);
			}
			MTLTextureDescriptor* CubeDesc = [MTLTextureDescriptor
				textureCubeDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
				size:8 mipmapped:NO];
			CubeDesc.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
			ASSERT_NE([Device newTextureWithDescriptor:CubeDesc], nil);
			MTLTextureDescriptor* ShadowDesc = [MTLTextureDescriptor
				texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
				width:8 height:8 mipmapped:NO];
			ShadowDesc.textureType = MTLTextureType2DArray;
			ShadowDesc.arrayLength = 3;
			ShadowDesc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
			ASSERT_NE([Device newTextureWithDescriptor:ShadowDesc], nil);
		}
	}

	TEST(FMetalShaderQualificationTests, R8StorageTextureWriteProducesCheckedReadback)
	{
		@autoreleasepool
		{
			id<MTLDevice> Device = MTLCreateSystemDefaultDevice();
			ASSERT_NE(Device, nil);
			NSString* Source = @"#include <metal_stdlib>\n"
				"using namespace metal;\n"
				"kernel void main0(texture2d<float, access::write> output [[texture(0)]], "
				"uint2 position [[thread_position_in_grid]]) {\n"
				"  output.write(float4(0.25, 0, 0, 1), position);\n"
				"}\n";
			NSError* Error = nil;
			id<MTLLibrary> Library = [Device newLibraryWithSource:Source options:nil error:&Error];
			ASSERT_NE(Library, nil) << [[Error description] UTF8String];
			id<MTLFunction> Function = [Library newFunctionWithName:@"main0"];
			ASSERT_NE(Function, nil);
			id<MTLComputePipelineState> Pipeline = [Device
				newComputePipelineStateWithFunction:Function error:&Error];
			ASSERT_NE(Pipeline, nil) << [[Error description] UTF8String];
			MTLTextureDescriptor* Desc = [MTLTextureDescriptor
				texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
				width:1 height:1 mipmapped:NO];
			Desc.storageMode = MTLStorageModeShared;
			Desc.usage = MTLTextureUsageShaderWrite;
			id<MTLTexture> Output = [Device newTextureWithDescriptor:Desc];
			ASSERT_NE(Output, nil);
			id<MTLCommandQueue> Queue = [Device newCommandQueue];
			ASSERT_NE(Queue, nil);
			id<MTLCommandBuffer> Command = [Queue commandBuffer];
			id<MTLComputeCommandEncoder> Encoder = [Command computeCommandEncoder];
			ASSERT_NE(Encoder, nil);
			[Encoder setComputePipelineState:Pipeline];
			[Encoder setTexture:Output atIndex:0];
			[Encoder dispatchThreads:MTLSizeMake(1, 1, 1)
				threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
			[Encoder endEncoding];
			[Command commit];
			[Command waitUntilCompleted];
			ASSERT_EQ(Command.status, MTLCommandBufferStatusCompleted)
				<< [[Command.error description] UTF8String];
			uint8_t Pixel = 0;
			[Output getBytes:&Pixel bytesPerRow:sizeof(Pixel)
				fromRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0];
			EXPECT_NEAR(Pixel, 64, 1);
		}
	}

	TEST(FMetalShaderQualificationTests, FloatAndIntegerRenderTargetsProduceCheckedReadback)
	{
		@autoreleasepool
		{
			id<MTLDevice> Device = MTLCreateSystemDefaultDevice();
			ASSERT_NE(Device, nil);
			NSString* Source = @"#include <metal_stdlib>\n"
				"using namespace metal;\n"
				"vertex float4 vertexMain(uint index [[vertex_id]]) {\n"
				"  float2 positions[3] = {float2(-1,-1), float2(3,-1), float2(-1,3)};\n"
				"  return float4(positions[index], 0, 1);\n"
				"}\n"
				"fragment float4 floatMain() { return float4(0.5, 0.25, 1, 0); }\n"
				"fragment uint2 integerMain() { return uint2(0x12345678u, 0x9abcdef0u); }\n";
			NSError* Error = nil;
			id<MTLLibrary> Library = [Device newLibraryWithSource:Source options:nil error:&Error];
			ASSERT_NE(Library, nil) << [[Error description] UTF8String];
			id<MTLFunction> Vertex = [Library newFunctionWithName:@"vertexMain"];
			ASSERT_NE(Vertex, nil);
			id<MTLCommandQueue> Queue = [Device newCommandQueue];
			ASSERT_NE(Queue, nil);
			const struct {
				MTLPixelFormat Format;
				NSString* FragmentName;
			} Cases[] = {
				{MTLPixelFormatRGBA16Float, @"floatMain"},
				{MTLPixelFormatRG11B10Float, @"floatMain"},
				{MTLPixelFormatRG32Uint, @"integerMain"},
			};
			for (const auto& Case : Cases)
			{
				SCOPED_TRACE([Case.FragmentName UTF8String]);
				MTLRenderPipelineDescriptor* PipelineDesc = [MTLRenderPipelineDescriptor new];
				PipelineDesc.vertexFunction = Vertex;
				PipelineDesc.fragmentFunction = [Library newFunctionWithName:Case.FragmentName];
				ASSERT_NE(PipelineDesc.fragmentFunction, nil);
				PipelineDesc.colorAttachments[0].pixelFormat = Case.Format;
				id<MTLRenderPipelineState> Pipeline = [Device
					newRenderPipelineStateWithDescriptor:PipelineDesc error:&Error];
				ASSERT_NE(Pipeline, nil) << [[Error description] UTF8String];
				MTLTextureDescriptor* Desc = [MTLTextureDescriptor
					texture2DDescriptorWithPixelFormat:Case.Format
					width:1 height:1 mipmapped:NO];
				Desc.storageMode = MTLStorageModeShared;
				Desc.usage = MTLTextureUsageRenderTarget;
				id<MTLTexture> Target = [Device newTextureWithDescriptor:Desc];
				ASSERT_NE(Target, nil);
				MTLRenderPassDescriptor* Pass = [MTLRenderPassDescriptor renderPassDescriptor];
				Pass.colorAttachments[0].texture = Target;
				Pass.colorAttachments[0].loadAction = MTLLoadActionClear;
				Pass.colorAttachments[0].storeAction = MTLStoreActionStore;
				id<MTLCommandBuffer> Command = [Queue commandBuffer];
				id<MTLRenderCommandEncoder> Encoder = [Command renderCommandEncoderWithDescriptor:Pass];
				ASSERT_NE(Encoder, nil);
				[Encoder setRenderPipelineState:Pipeline];
				[Encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
				[Encoder endEncoding];
				[Command commit];
				[Command waitUntilCompleted];
				ASSERT_EQ(Command.status, MTLCommandBufferStatusCompleted)
					<< [[Command.error description] UTF8String];
				if (Case.Format == MTLPixelFormatRGBA16Float)
				{
				uint16_t Pixel[4] = {};
				[Target getBytes:Pixel bytesPerRow:sizeof(Pixel)
					fromRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0];
				EXPECT_EQ(Pixel[0], 0x3800u);
				EXPECT_EQ(Pixel[1], 0x3400u);
				EXPECT_EQ(Pixel[2], 0x3c00u);
				EXPECT_EQ(Pixel[3], 0u);
			}
				else if (Case.Format == MTLPixelFormatRG11B10Float)
				{
				uint32_t Pixel = 0;
				[Target getBytes:&Pixel bytesPerRow:sizeof(Pixel)
					fromRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0];
				constexpr uint32_t Expected = (14u << 6u)
					| ((13u << 6u) << 11u) | ((15u << 5u) << 22u);
				EXPECT_EQ(Pixel, Expected);
				}
				else
				{
				uint32_t Pixel[2] = {};
				[Target getBytes:Pixel bytesPerRow:sizeof(Pixel)
					fromRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0];
				EXPECT_EQ(Pixel[0], 0x12345678u);
				EXPECT_EQ(Pixel[1], 0x9abcdef0u);
				}
			}
		}
	}

	TEST(FMetalShaderQualificationTests, DepthAndScreenCoordinatesProduceCheckedReadback)
	{
		@autoreleasepool
		{
			id<MTLDevice> Device = MTLCreateSystemDefaultDevice();
			ASSERT_NE(Device, nil);
			NSString* Source = @"#include <metal_stdlib>\n"
				"using namespace metal;\n"
				"vertex float4 vertexMain(uint index [[vertex_id]]) {\n"
				"  float2 positions[3] = {float2(-1,-1), float2(3,-1), float2(-1,3)};\n"
				"  return float4(positions[index], 0.375, 1);\n"
				"}\n"
				"fragment void depthMain() {}\n"
				"fragment float4 coordinateMain(float4 position [[position]]) {\n"
				"  return float4(position.x < 1 ? 1 : 0, position.y < 1 ? 1 : 0, 0, 1);\n"
				"}\n";
			NSError* Error = nil;
			id<MTLLibrary> Library = [Device newLibraryWithSource:Source options:nil error:&Error];
			ASSERT_NE(Library, nil) << [[Error description] UTF8String];
			id<MTLFunction> Vertex = [Library newFunctionWithName:@"vertexMain"];
			ASSERT_NE(Vertex, nil);
			id<MTLCommandQueue> Queue = [Device newCommandQueue];
			ASSERT_NE(Queue, nil);
			MTLTextureDescriptor* DepthDesc = [MTLTextureDescriptor
				texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
				width:1 height:1 mipmapped:NO];
			DepthDesc.storageMode = MTLStorageModeShared;
			DepthDesc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
			id<MTLTexture> Depth = [Device newTextureWithDescriptor:DepthDesc];
			ASSERT_NE(Depth, nil);
			MTLRenderPipelineDescriptor* DepthPipelineDesc = [MTLRenderPipelineDescriptor new];
			DepthPipelineDesc.vertexFunction = Vertex;
			DepthPipelineDesc.fragmentFunction = [Library newFunctionWithName:@"depthMain"];
			DepthPipelineDesc.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
			id<MTLRenderPipelineState> DepthPipeline = [Device
				newRenderPipelineStateWithDescriptor:DepthPipelineDesc error:&Error];
			ASSERT_NE(DepthPipeline, nil) << [[Error description] UTF8String];
			MTLDepthStencilDescriptor* DepthStateDesc = [MTLDepthStencilDescriptor new];
			DepthStateDesc.depthCompareFunction = MTLCompareFunctionAlways;
			DepthStateDesc.depthWriteEnabled = YES;
			id<MTLDepthStencilState> DepthState = [Device
				newDepthStencilStateWithDescriptor:DepthStateDesc];
			ASSERT_NE(DepthState, nil);
			MTLRenderPassDescriptor* DepthPass = [MTLRenderPassDescriptor renderPassDescriptor];
			DepthPass.depthAttachment.texture = Depth;
			DepthPass.depthAttachment.loadAction = MTLLoadActionClear;
			DepthPass.depthAttachment.storeAction = MTLStoreActionStore;
			DepthPass.depthAttachment.clearDepth = 1;
			id<MTLCommandBuffer> DepthCommand = [Queue commandBuffer];
			id<MTLRenderCommandEncoder> DepthEncoder = [DepthCommand
				renderCommandEncoderWithDescriptor:DepthPass];
			ASSERT_NE(DepthEncoder, nil);
			[DepthEncoder setRenderPipelineState:DepthPipeline];
			[DepthEncoder setDepthStencilState:DepthState];
			[DepthEncoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
			[DepthEncoder endEncoding];
			[DepthCommand commit];
			[DepthCommand waitUntilCompleted];
			ASSERT_EQ(DepthCommand.status, MTLCommandBufferStatusCompleted)
				<< [[DepthCommand.error description] UTF8String];
			float DepthValue = 0;
			[Depth getBytes:&DepthValue bytesPerRow:sizeof(DepthValue)
				fromRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0];
			EXPECT_NEAR(DepthValue, 0.375f, 0.00001f);

			MTLTextureDescriptor* ColorDesc = [MTLTextureDescriptor
				texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
				width:2 height:2 mipmapped:NO];
			ColorDesc.storageMode = MTLStorageModeShared;
			ColorDesc.usage = MTLTextureUsageRenderTarget;
			id<MTLTexture> Color = [Device newTextureWithDescriptor:ColorDesc];
			ASSERT_NE(Color, nil);
			MTLRenderPipelineDescriptor* ColorPipelineDesc = [MTLRenderPipelineDescriptor new];
			ColorPipelineDesc.vertexFunction = Vertex;
			ColorPipelineDesc.fragmentFunction = [Library newFunctionWithName:@"coordinateMain"];
			ColorPipelineDesc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
			id<MTLRenderPipelineState> ColorPipeline = [Device
				newRenderPipelineStateWithDescriptor:ColorPipelineDesc error:&Error];
			ASSERT_NE(ColorPipeline, nil) << [[Error description] UTF8String];
			MTLRenderPassDescriptor* ColorPass = [MTLRenderPassDescriptor renderPassDescriptor];
			ColorPass.colorAttachments[0].texture = Color;
			ColorPass.colorAttachments[0].loadAction = MTLLoadActionClear;
			ColorPass.colorAttachments[0].storeAction = MTLStoreActionStore;
			id<MTLCommandBuffer> ColorCommand = [Queue commandBuffer];
			id<MTLRenderCommandEncoder> ColorEncoder = [ColorCommand
				renderCommandEncoderWithDescriptor:ColorPass];
			ASSERT_NE(ColorEncoder, nil);
			[ColorEncoder setRenderPipelineState:ColorPipeline];
			[ColorEncoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
			[ColorEncoder endEncoding];
			[ColorCommand commit];
			[ColorCommand waitUntilCompleted];
			ASSERT_EQ(ColorCommand.status, MTLCommandBufferStatusCompleted)
				<< [[ColorCommand.error description] UTF8String];
			uint8_t Pixels[16] = {};
			[Color getBytes:Pixels bytesPerRow:8
				fromRegion:MTLRegionMake2D(0, 0, 2, 2) mipmapLevel:0];
			const uint8_t Expected[16] = {
				255, 255, 0, 255, 0, 255, 0, 255,
				255, 0, 0, 255, 0, 0, 0, 255,
			};
			for (size_t Index = 0; Index < sizeof(Pixels); ++Index)
				EXPECT_EQ(Pixels[Index], Expected[Index]) << "byte " << Index;
		}
	}

	TEST(FMetalShaderQualificationTests, AuthoredGPUCullingProducesCheckedReadback)
	{
		@autoreleasepool
		{
			const std::string Spirv = CompileShaderCode(
				"GBufferGPUCulling.slang", "CullMain", SLANG_SPIRV);
			ASSERT_FALSE(Spirv.empty());
			const std::string MetalCode = TranslateSpirvToMetal(Spirv);
			ASSERT_FALSE(MetalCode.empty());
			NSString* MetalSource = [[NSString alloc] initWithBytes:MetalCode.data()
				length:MetalCode.size() encoding:NSUTF8StringEncoding];
			ASSERT_NE(MetalSource, nil);

			id<MTLDevice> Device = MTLCreateSystemDefaultDevice();
			ASSERT_NE(Device, nil) << "Metal device unavailable on qualification host";
			NSError* Error = nil;
			id<MTLLibrary> Library = [Device newLibraryWithSource:MetalSource options:nil error:&Error];
			ASSERT_NE(Library, nil) << [[Error description] UTF8String];
			id<MTLFunction> Function = [Library newFunctionWithName:@"main0"];
			ASSERT_NE(Function, nil);
			id<MTLComputePipelineState> Pipeline = [Device newComputePipelineStateWithFunction:Function error:&Error];
			ASSERT_NE(Pipeline, nil) << [[Error description] UTF8String];

			FCandidate Candidates[2] = {};
			Candidates[0].BoundsMin = {-0.2f, -0.2f, 0.2f};
			Candidates[0].BoundsMax = {0.2f, 0.2f, 0.4f};
			Candidates[0].TransformIndex = 7;
			Candidates[1].BoundsMin = {1.6f, -0.2f, 0.2f};
			Candidates[1].BoundsMax = {1.8f, 0.2f, 0.4f};
			Candidates[1].TransformIndex = 9;
			FCullingUniform Uniform = {};
			// Match the row-major float array filled by GBufferRenderer.
			Uniform.ViewProjection[0] = 0.5f;
			Uniform.ViewProjection[5] = 1;
			Uniform.ViewProjection[10] = 1;
			Uniform.ViewProjection[15] = 1;
			Uniform.ViewProjection[3] = 0.25f;
			Uniform.CandidateCount = 2;
			uint32_t Visible[4] = {0, 0, 0, 0};
			uint32_t Indirect[5] = {3, 0, 0, 0, 2};
			id<MTLBuffer> CandidateBuffer = [Device newBufferWithBytes:Candidates length:sizeof(Candidates) options:MTLResourceStorageModeShared];
			id<MTLBuffer> VisibleBuffer = [Device newBufferWithBytes:Visible length:sizeof(Visible) options:MTLResourceStorageModeShared];
			id<MTLBuffer> IndirectBuffer = [Device newBufferWithBytes:Indirect length:sizeof(Indirect) options:MTLResourceStorageModeShared];
			id<MTLBuffer> UniformBuffer = [Device newBufferWithBytes:&Uniform length:sizeof(Uniform) options:MTLResourceStorageModeShared];
			ASSERT_NE(CandidateBuffer, nil);
			ASSERT_NE(VisibleBuffer, nil);
			ASSERT_NE(IndirectBuffer, nil);
			ASSERT_NE(UniformBuffer, nil);

			id<MTLCommandQueue> Queue = [Device newCommandQueue];
			ASSERT_NE(Queue, nil);
			id<MTLCommandBuffer> Command = [Queue commandBuffer];
			ASSERT_NE(Command, nil);
			id<MTLComputeCommandEncoder> Encoder = [Command computeCommandEncoder];
			ASSERT_NE(Encoder, nil);
			[Encoder setComputePipelineState:Pipeline];
			[Encoder setBuffer:CandidateBuffer offset:0 atIndex:0];
			[Encoder setBuffer:VisibleBuffer offset:0 atIndex:1];
			[Encoder setBuffer:IndirectBuffer offset:0 atIndex:2];
			[Encoder setBuffer:UniformBuffer offset:0 atIndex:3];
			[Encoder dispatchThreads:MTLSizeMake(2, 1, 1) threadsPerThreadgroup:MTLSizeMake(2, 1, 1)];
			[Encoder endEncoding];
			[Command commit];
			[Command waitUntilCompleted];
			ASSERT_EQ(Command.status, MTLCommandBufferStatusCompleted) << [[Command.error description] UTF8String];
			const auto* ActualVisible = static_cast<const uint32_t*>(VisibleBuffer.contents);
			const auto* ActualIndirect = static_cast<const uint32_t*>(IndirectBuffer.contents);
			EXPECT_EQ(ActualIndirect[1], 1u);
			EXPECT_EQ(ActualVisible[2], 7u);
			EXPECT_EQ(ActualVisible[3], 0u);
		}
	}

	TEST(FMetalShaderQualificationTests, PushConstantAndStorageTextureProduceCheckedReadback)
	{
		@autoreleasepool
		{
			const std::string MetalCode = CompileShaderCode(
				"PublicComputePipeline.slang", "ComputeMain", SLANG_METAL, true);
			ASSERT_FALSE(MetalCode.empty());
			NSString* MetalSource = [[NSString alloc] initWithBytes:MetalCode.data()
				length:MetalCode.size() encoding:NSUTF8StringEncoding];
			ASSERT_NE(MetalSource, nil);
			id<MTLDevice> Device = MTLCreateSystemDefaultDevice();
			ASSERT_NE(Device, nil) << "Metal device unavailable on qualification host";
			NSError* Error = nil;
			id<MTLLibrary> Library = [Device newLibraryWithSource:MetalSource options:nil error:&Error];
			ASSERT_NE(Library, nil) << [[Error description] UTF8String];
			id<MTLFunction> Function = [Library newFunctionWithName:@"ComputeMain"];
			ASSERT_NE(Function, nil);
			id<MTLComputePipelineState> Pipeline = [Device newComputePipelineStateWithFunction:Function error:&Error];
			ASSERT_NE(Pipeline, nil) << [[Error description] UTF8String];

			const uint32_t InitialOutput = 0;
			const uint32_t Increment = 5;
			id<MTLBuffer> OutputBuffer = [Device newBufferWithBytes:&InitialOutput
				length:sizeof(InitialOutput) options:MTLResourceStorageModeShared];
			id<MTLBuffer> PushConstantBuffer = [Device newBufferWithBytes:&Increment
				length:sizeof(Increment) options:MTLResourceStorageModeShared];
			ASSERT_NE(OutputBuffer, nil);
			ASSERT_NE(PushConstantBuffer, nil);
			MTLTextureDescriptor* TextureDesc = [MTLTextureDescriptor
				texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
				width:1 height:1 mipmapped:NO];
			TextureDesc.storageMode = MTLStorageModeShared;
			TextureDesc.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
			id<MTLTexture> OutputTexture = [Device newTextureWithDescriptor:TextureDesc];
			ASSERT_NE(OutputTexture, nil);
			id<MTLCommandQueue> Queue = [Device newCommandQueue];
			ASSERT_NE(Queue, nil);
			id<MTLCommandBuffer> Command = [Queue commandBuffer];
			ASSERT_NE(Command, nil);
			id<MTLComputeCommandEncoder> Encoder = [Command computeCommandEncoder];
			ASSERT_NE(Encoder, nil);
			[Encoder setComputePipelineState:Pipeline];
			[Encoder setBuffer:OutputBuffer offset:0 atIndex:0];
			[Encoder setBuffer:PushConstantBuffer offset:0 atIndex:1];
			[Encoder setTexture:OutputTexture atIndex:0];
			[Encoder dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
			[Encoder endEncoding];
			[Command commit];
			[Command waitUntilCompleted];
			ASSERT_EQ(Command.status, MTLCommandBufferStatusCompleted) << [[Command.error description] UTF8String];
			EXPECT_EQ(*static_cast<const uint32_t*>(OutputBuffer.contents), 0xff352515u);
			uint8_t Pixels[4] = {};
			[OutputTexture getBytes:Pixels bytesPerRow:sizeof(Pixels)
				fromRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0];
			EXPECT_EQ(Pixels[0], 21);
			EXPECT_EQ(Pixels[1], 37);
			EXPECT_EQ(Pixels[2], 53);
			EXPECT_EQ(Pixels[3], 255);
		}
	}

	TEST(FMetalShaderQualificationTests, SelectedRouteStorageTextureProducesCheckedReadback)
	{
		@autoreleasepool
		{
			const std::string Spirv = CompileShaderCode(
				"PublicComputePipeline.slang", "ComputeMain", SLANG_SPIRV);
			ASSERT_FALSE(Spirv.empty());
			const std::string MetalCode = TranslateSpirvToMetal(Spirv);
			ASSERT_FALSE(MetalCode.empty());
			NSString* MetalSource = [[NSString alloc] initWithBytes:MetalCode.data()
				length:MetalCode.size() encoding:NSUTF8StringEncoding];
			ASSERT_NE(MetalSource, nil);
			id<MTLDevice> Device = MTLCreateSystemDefaultDevice();
			ASSERT_NE(Device, nil);
			NSError* Error = nil;
			id<MTLLibrary> Library = [Device newLibraryWithSource:MetalSource options:nil error:&Error];
			ASSERT_NE(Library, nil) << [[Error description] UTF8String];
			id<MTLFunction> Function = [Library newFunctionWithName:@"main0"];
			ASSERT_NE(Function, nil);
			id<MTLComputePipelineState> Pipeline = [Device
				newComputePipelineStateWithFunction:Function error:&Error];
			ASSERT_NE(Pipeline, nil) << [[Error description] UTF8String];
			id<MTLCommandQueue> Queue = [Device newCommandQueue];
			ASSERT_NE(Queue, nil);
			for (const MTLPixelFormat Format : {
				MTLPixelFormatR8Unorm, MTLPixelFormatRGBA8Unorm,
				MTLPixelFormatRGBA16Float})
			{
				SCOPED_TRACE(static_cast<unsigned>(Format));
				const uint32_t InitialOutput = 0;
				const uint32_t Increment = 5;
				id<MTLBuffer> OutputBuffer = [Device newBufferWithBytes:&InitialOutput
					length:sizeof(InitialOutput) options:MTLResourceStorageModeShared];
				id<MTLBuffer> PushConstantBuffer = [Device newBufferWithBytes:&Increment
					length:sizeof(Increment) options:MTLResourceStorageModeShared];
				ASSERT_NE(OutputBuffer, nil);
				ASSERT_NE(PushConstantBuffer, nil);
				MTLTextureDescriptor* Desc = [MTLTextureDescriptor
					texture2DDescriptorWithPixelFormat:Format
					width:1 height:1 mipmapped:NO];
				Desc.storageMode = MTLStorageModeShared;
				Desc.usage = MTLTextureUsageShaderWrite;
				id<MTLTexture> OutputTexture = [Device newTextureWithDescriptor:Desc];
				ASSERT_NE(OutputTexture, nil);
				id<MTLCommandBuffer> Command = [Queue commandBuffer];
				id<MTLComputeCommandEncoder> Encoder = [Command computeCommandEncoder];
				ASSERT_NE(Encoder, nil);
				[Encoder setComputePipelineState:Pipeline];
				[Encoder setBuffer:OutputBuffer offset:0 atIndex:0];
				[Encoder setBuffer:PushConstantBuffer offset:0 atIndex:4];
				[Encoder setTexture:OutputTexture atIndex:0];
				[Encoder dispatchThreads:MTLSizeMake(1, 1, 1)
					threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
				[Encoder endEncoding];
				[Command commit];
				[Command waitUntilCompleted];
				ASSERT_EQ(Command.status, MTLCommandBufferStatusCompleted)
					<< [[Command.error description] UTF8String];
				EXPECT_EQ(*static_cast<const uint32_t*>(OutputBuffer.contents), 0xff352515u);
				uint8_t Pixels[8] = {};
				const size_t PixelSize = Format == MTLPixelFormatR8Unorm ? 1 :
					Format == MTLPixelFormatRGBA8Unorm ? 4 : 8;
				[OutputTexture getBytes:Pixels bytesPerRow:PixelSize
					fromRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0];
				if (Format == MTLPixelFormatRGBA16Float)
				{
				uint16_t HalfPixels[4] = {};
				std::memcpy(HalfPixels, Pixels, sizeof(HalfPixels));
				const uint16_t Expected[4] = {0x2d45, 0x30a5, 0x32a7, 0x3c00};
				for (size_t Index = 0; Index < 4; ++Index)
					EXPECT_NEAR(HalfPixels[Index], Expected[Index], 1) << "channel " << Index;
				}
				else
				{
				EXPECT_EQ(Pixels[0], 21);
				if (Format == MTLPixelFormatRGBA8Unorm)
				{
					EXPECT_EQ(Pixels[1], 37);
					EXPECT_EQ(Pixels[2], 53);
					EXPECT_EQ(Pixels[3], 255);
				}
				}
			}
		}
	}

	TEST(FMetalShaderQualificationTests, AuthoredImGuiDrawSamplesTextureIntoCheckedPixel)
	{
		@autoreleasepool
		{
			const std::string VertexSpirv = CompileShaderCode(
				"ImGui.slang", "VertexMain", SLANG_SPIRV);
			const std::string FragmentSpirv = CompileShaderCode(
				"ImGui.slang", "FragmentMain", SLANG_SPIRV);
			ASSERT_FALSE(VertexSpirv.empty());
			ASSERT_FALSE(FragmentSpirv.empty());
			const std::string VertexMetal = TranslateSpirvToMetal(VertexSpirv, SpvExecutionModelVertex);
			const std::string FragmentMetal = TranslateSpirvToMetal(FragmentSpirv, SpvExecutionModelFragment);
			ASSERT_FALSE(VertexMetal.empty());
			ASSERT_FALSE(FragmentMetal.empty());
			NSString* VertexSource = [[NSString alloc] initWithBytes:VertexMetal.data()
				length:VertexMetal.size() encoding:NSUTF8StringEncoding];
			NSString* FragmentSource = [[NSString alloc] initWithBytes:FragmentMetal.data()
				length:FragmentMetal.size() encoding:NSUTF8StringEncoding];
			ASSERT_NE(VertexSource, nil);
			ASSERT_NE(FragmentSource, nil);
			id<MTLDevice> Device = MTLCreateSystemDefaultDevice();
			ASSERT_NE(Device, nil);
			NSError* Error = nil;
			id<MTLLibrary> VertexLibrary = [Device newLibraryWithSource:VertexSource options:nil error:&Error];
			ASSERT_NE(VertexLibrary, nil) << [[Error description] UTF8String];
			id<MTLLibrary> FragmentLibrary = [Device newLibraryWithSource:FragmentSource options:nil error:&Error];
			ASSERT_NE(FragmentLibrary, nil) << [[Error description] UTF8String];
			id<MTLFunction> VertexFunction = [VertexLibrary newFunctionWithName:@"main0"];
			id<MTLFunction> FragmentFunction = [FragmentLibrary newFunctionWithName:@"main0"];
			ASSERT_NE(VertexFunction, nil);
			ASSERT_NE(FragmentFunction, nil);
			MTLVertexDescriptor* VertexDesc = [MTLVertexDescriptor vertexDescriptor];
			VertexDesc.attributes[0].format = MTLVertexFormatFloat2;
			VertexDesc.attributes[0].offset = 0;
			VertexDesc.attributes[0].bufferIndex = 0;
			VertexDesc.attributes[1].format = MTLVertexFormatFloat2;
			VertexDesc.attributes[1].offset = 8;
			VertexDesc.attributes[1].bufferIndex = 0;
			VertexDesc.attributes[2].format = MTLVertexFormatFloat4;
			VertexDesc.attributes[2].offset = 16;
			VertexDesc.attributes[2].bufferIndex = 0;
			VertexDesc.layouts[0].stride = 32;
			VertexDesc.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;
			MTLRenderPipelineDescriptor* PipelineDesc = [MTLRenderPipelineDescriptor new];
			PipelineDesc.vertexFunction = VertexFunction;
			PipelineDesc.fragmentFunction = FragmentFunction;
			PipelineDesc.vertexDescriptor = VertexDesc;
			PipelineDesc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
			id<MTLRenderPipelineState> Pipeline = [Device newRenderPipelineStateWithDescriptor:PipelineDesc error:&Error];
			ASSERT_NE(Pipeline, nil) << [[Error description] UTF8String];

			struct FVertex { float Position[2], UV[2], Color[4]; };
			static_assert(sizeof(FVertex) == 32);
			const FVertex Vertices[3] = {
				{{-1, -1}, {0.5f, 0.5f}, {0.5f, 1, 0.25f, 1}},
				{{3, -1}, {0.5f, 0.5f}, {0.5f, 1, 0.25f, 1}},
				{{-1, 3}, {0.5f, 0.5f}, {0.5f, 1, 0.25f, 1}},
			};
			const float Projection[4] = {1, 1, 0, 0};
			id<MTLBuffer> VertexBuffer = [Device newBufferWithBytes:Vertices length:sizeof(Vertices) options:MTLResourceStorageModeShared];
			id<MTLBuffer> ProjectionBuffer = [Device newBufferWithBytes:Projection length:sizeof(Projection) options:MTLResourceStorageModeShared];
			ASSERT_NE(VertexBuffer, nil);
			ASSERT_NE(ProjectionBuffer, nil);
			MTLTextureDescriptor* FontDesc = [MTLTextureDescriptor
				texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
				width:1 height:1 mipmapped:NO];
			FontDesc.storageMode = MTLStorageModeShared;
			FontDesc.usage = MTLTextureUsageShaderRead;
			id<MTLTexture> Font = [Device newTextureWithDescriptor:FontDesc];
			ASSERT_NE(Font, nil);
			const uint8_t FontPixel[4] = {128, 64, 255, 255};
			[Font replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0
				withBytes:FontPixel bytesPerRow:sizeof(FontPixel)];
			MTLSamplerDescriptor* SamplerDesc = [MTLSamplerDescriptor new];
			SamplerDesc.minFilter = MTLSamplerMinMagFilterNearest;
			SamplerDesc.magFilter = MTLSamplerMinMagFilterNearest;
			id<MTLSamplerState> Sampler = [Device newSamplerStateWithDescriptor:SamplerDesc];
			ASSERT_NE(Sampler, nil);
			MTLTextureDescriptor* OutputDesc = [MTLTextureDescriptor
				texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
				width:1 height:1 mipmapped:NO];
			OutputDesc.storageMode = MTLStorageModeShared;
			OutputDesc.usage = MTLTextureUsageRenderTarget;
			id<MTLTexture> Output = [Device newTextureWithDescriptor:OutputDesc];
			ASSERT_NE(Output, nil);
			MTLRenderPassDescriptor* Pass = [MTLRenderPassDescriptor renderPassDescriptor];
			Pass.colorAttachments[0].texture = Output;
			Pass.colorAttachments[0].loadAction = MTLLoadActionClear;
			Pass.colorAttachments[0].storeAction = MTLStoreActionStore;
			Pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
			id<MTLCommandQueue> Queue = [Device newCommandQueue];
			ASSERT_NE(Queue, nil);
			id<MTLCommandBuffer> Command = [Queue commandBuffer];
			id<MTLRenderCommandEncoder> Encoder = [Command renderCommandEncoderWithDescriptor:Pass];
			ASSERT_NE(Encoder, nil);
			[Encoder setRenderPipelineState:Pipeline];
			[Encoder setVertexBuffer:VertexBuffer offset:0 atIndex:0];
			[Encoder setVertexBuffer:ProjectionBuffer offset:0 atIndex:1];
			[Encoder setFragmentTexture:Font atIndex:0];
			[Encoder setFragmentSamplerState:Sampler atIndex:0];
			[Encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
			[Encoder endEncoding];
			[Command commit];
			[Command waitUntilCompleted];
			ASSERT_EQ(Command.status, MTLCommandBufferStatusCompleted) << [[Command.error description] UTF8String];
			uint8_t Pixels[4] = {};
			[Output getBytes:Pixels bytesPerRow:sizeof(Pixels)
				fromRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0];
			EXPECT_NEAR(Pixels[0], 64, 1);
			EXPECT_NEAR(Pixels[1], 64, 1);
			EXPECT_NEAR(Pixels[2], 64, 1);
			EXPECT_EQ(Pixels[3], 255);
		}
	}

	TEST(FMetalShaderQualificationTests, ResourceArraysSelectDistinctTextures)
	{
		@autoreleasepool
		{
			const std::string Spirv = CompileShaderCode(
				"ResourceArray.slang", "ComputeMain", SLANG_SPIRV);
			ASSERT_FALSE(Spirv.empty());
			const std::string MetalCode = TranslateSpirvToMetal(
				Spirv, SpvExecutionModelGLCompute, true);
			ASSERT_FALSE(MetalCode.empty());
			NSString* MetalSource = [[NSString alloc] initWithBytes:MetalCode.data()
				length:MetalCode.size() encoding:NSUTF8StringEncoding];
			ASSERT_NE(MetalSource, nil);
			id<MTLDevice> Device = MTLCreateSystemDefaultDevice();
			ASSERT_NE(Device, nil);
			NSError* Error = nil;
			id<MTLLibrary> Library = [Device newLibraryWithSource:MetalSource options:nil error:&Error];
			ASSERT_NE(Library, nil) << [[Error description] UTF8String];
			id<MTLFunction> Function = [Library newFunctionWithName:@"main0"];
			ASSERT_NE(Function, nil);
			id<MTLComputePipelineState> Pipeline = [Device newComputePipelineStateWithFunction:Function error:&Error];
			ASSERT_NE(Pipeline, nil) << [[Error description] UTF8String];
			MTLTextureDescriptor* TextureDesc = [MTLTextureDescriptor
				texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
				width:1 height:1 mipmapped:NO];
			TextureDesc.storageMode = MTLStorageModeShared;
			TextureDesc.usage = MTLTextureUsageShaderRead;
			id<MTLTexture> Textures[2] = {
				[Device newTextureWithDescriptor:TextureDesc],
				[Device newTextureWithDescriptor:TextureDesc]};
			ASSERT_NE(Textures[0], nil);
			ASSERT_NE(Textures[1], nil);
			const uint8_t FirstPixel[4] = {10, 20, 0, 255};
			const uint8_t SecondPixel[4] = {30, 40, 0, 255};
			[Textures[0] replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0
				withBytes:FirstPixel bytesPerRow:sizeof(FirstPixel)];
			[Textures[1] replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0
				withBytes:SecondPixel bytesPerRow:sizeof(SecondPixel)];
			MTLSamplerDescriptor* SamplerDesc = [MTLSamplerDescriptor new];
			SamplerDesc.minFilter = MTLSamplerMinMagFilterNearest;
			SamplerDesc.magFilter = MTLSamplerMinMagFilterNearest;
			id<MTLSamplerState> Samplers[2] = {
				[Device newSamplerStateWithDescriptor:SamplerDesc],
				[Device newSamplerStateWithDescriptor:SamplerDesc]};
			ASSERT_NE(Samplers[0], nil);
			ASSERT_NE(Samplers[1], nil);
			const uint32_t InitialOutput[2] = {};
			id<MTLBuffer> Output = [Device newBufferWithBytes:InitialOutput
				length:sizeof(InitialOutput) options:MTLResourceStorageModeShared];
			ASSERT_NE(Output, nil);
			id<MTLCommandQueue> Queue = [Device newCommandQueue];
			ASSERT_NE(Queue, nil);
			id<MTLCommandBuffer> Command = [Queue commandBuffer];
			id<MTLComputeCommandEncoder> Encoder = [Command computeCommandEncoder];
			ASSERT_NE(Encoder, nil);
			[Encoder setComputePipelineState:Pipeline];
			[Encoder setBuffer:Output offset:0 atIndex:0];
			[Encoder setTextures:Textures withRange:NSMakeRange(0, 2)];
			[Encoder setSamplerStates:Samplers withRange:NSMakeRange(0, 2)];
			[Encoder dispatchThreads:MTLSizeMake(2, 1, 1) threadsPerThreadgroup:MTLSizeMake(2, 1, 1)];
			[Encoder endEncoding];
			[Command commit];
			[Command waitUntilCompleted];
			ASSERT_EQ(Command.status, MTLCommandBufferStatusCompleted) << [[Command.error description] UTF8String];
			const auto* Values = static_cast<const uint32_t*>(Output.contents);
			EXPECT_EQ(Values[0], 10u | (20u << 8));
			EXPECT_EQ(Values[1], 30u | (40u << 8));
		}
	}
}
