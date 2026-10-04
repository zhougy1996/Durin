#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "Materials/MaterialExpressionBuild.h"
#include "Materials/MaterialProgramCompiler.h"
#include "ShaderBuild/ShaderPaths.h"
#include "slang.h"
#include "slang-com-ptr.h"
#include "spirv_cross_c.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace
{
	constexpr Durin::FGuid TintId{0, 0, 1, 1};
	constexpr Durin::FGuid UVId{0, 0, 1, 2};
	constexpr Durin::FGuid TextureId{0, 0, 1, 3};
	constexpr Durin::FGuid MaskId{0, 0, 1, 4};

	auto ResolveNodeSemantics(Durin::MIR::FModule& IR, Durin::MIR::FNode& Node) -> void
	{
		std::vector<Durin::FMaterialValueSemantics> Inputs;
		for (const auto Index : Node.Inputs) Inputs.push_back(IR.Nodes.at(Index).GetSemantics());
		const auto Result = Durin::ResolveMaterialProgramNodeSemantics(
			Node.Opcode, Node.ResultType, Inputs);
		if (!Result) return;
		Node.LegalStages = Result->Stages;
		Node.SpatialKind = Result->Kind;
		Node.CoordinateSpace = Result->Space;
	}

	auto MakeMaterialInput(bool bTextured) -> Durin::MIR::FCompilerInput
	{
		using namespace Durin;
		MIR::FGraphBuilder Graph(std::span<DMaterialExpression* const>{});
		MIR::FCompilerInput Input;
		Input.IR = Graph.FinishSurface({}).IR;
		if (!bTextured) Input.StaticProperties.ShadingModel = EMaterialShadingModel::Unlit;
		else
		{
			Input.StaticProperties.BlendMode = EMaterialBlendMode::Masked;
			Input.Parameters = {{TintId, EMaterialParameterType::Vector4},
				{UVId, EMaterialParameterType::Vector4},
				{TextureId, EMaterialParameterType::Texture},
				{MaskId, EMaterialParameterType::Scalar}};
			auto Add = [&](MIR::FNode Node) {
				ResolveNodeSemantics(Input.IR, Node);
				Input.IR.Nodes.push_back(std::move(Node));
				return static_cast<uint32>(Input.IR.Nodes.size() - 1);
			};
			const auto TintValue = Add({.Opcode = EMaterialProgramOpcode::Parameter,
				.ResultType = EMaterialProgramValueType::Float4, .Payload = TintId});
			const auto UVValue = Add({.Opcode = EMaterialProgramOpcode::Parameter,
				.ResultType = EMaterialProgramValueType::Float4, .Payload = UVId});
			const auto UV2 = Add({.Opcode = EMaterialProgramOpcode::Swizzle,
				.ResultType = EMaterialProgramValueType::Float2, .Inputs = {UVValue},
				.Payload = MIR::FSwizzle{2, {0, 1}}});
			const auto TextureValue = Add({.Opcode = EMaterialProgramOpcode::TextureParameter,
				.ResultType = EMaterialProgramValueType::Texture2D, .Payload = TextureId});
			const auto Sample = Add({.Opcode = EMaterialProgramOpcode::TextureSample2D,
				.ResultType = EMaterialProgramValueType::Float4, .Inputs = {TextureValue, UV2}});
			const auto Color4 = Add({.Opcode = EMaterialProgramOpcode::Multiply,
				.ResultType = EMaterialProgramValueType::Float4, .Inputs = {TintValue, Sample}});
			const auto Color3 = Add({.Opcode = EMaterialProgramOpcode::Swizzle,
				.ResultType = EMaterialProgramValueType::Float3, .Inputs = {Color4},
				.Payload = MIR::FSwizzle{3, {0, 1, 2}}});
			Input.IR.SurfaceRoot.Inputs[0].bExpression = true;
			Input.IR.SurfaceRoot.Inputs[0].ExpressionIndex = Color3;
			Input.IR.SurfaceRoot.Inputs[7].bExpression = true;
			Input.IR.SurfaceRoot.Inputs[7].ExpressionIndex = Add({
				.Opcode = EMaterialProgramOpcode::Parameter,
				.ResultType = EMaterialProgramValueType::Float, .Payload = MaskId});
		}
		return Input;
	}

	auto TranslateMaterialStage(const Durin::FCompiledShader& Shader, std::string& Error)
		-> std::string
	{
		if (!Shader.Code || Shader.Code->size() % sizeof(SpvId) != 0)
		{
			Error = "Invalid material SPIR-V byte count";
			return {};
		}
		std::vector<SpvId> Words(Shader.Code->size() / sizeof(SpvId));
		std::memcpy(Words.data(), Shader.Code->data(), Shader.Code->size());
		spvc_context Context = nullptr;
		if (spvc_context_create(&Context) != SPVC_SUCCESS)
		{
			Error = "SPIRV-Cross context creation failed";
			return {};
		}
		spvc_parsed_ir IR = nullptr;
		spvc_compiler Compiler = nullptr;
		spvc_compiler_options Options = nullptr;
		const char* Source = nullptr;
		auto Result = spvc_context_parse_spirv(Context, Words.data(), Words.size(), &IR);
		if (Result == SPVC_SUCCESS)
			Result = spvc_context_create_compiler(Context, SPVC_BACKEND_MSL, IR,
				SPVC_CAPTURE_MODE_TAKE_OWNERSHIP, &Compiler);
		if (Result == SPVC_SUCCESS)
			Result = spvc_compiler_create_compiler_options(Compiler, &Options);
		if (Result == SPVC_SUCCESS)
			Result = spvc_compiler_options_set_uint(Options,
				SPVC_COMPILER_OPTION_MSL_VERSION, SPVC_MAKE_MSL_VERSION(2, 0, 0));
		if (Result == SPVC_SUCCESS)
			Result = spvc_compiler_install_compiler_options(Compiler, Options);
		if (Result == SPVC_SUCCESS) Result = spvc_compiler_compile(Compiler, &Source);
		const std::string Metal = Result == SPVC_SUCCESS && Source ? Source : "";
		if (Metal.empty()) Error = spvc_context_get_last_error_string(Context);
		spvc_context_destroy(Context);
		return Metal;
	}

	auto CompileMaterialDirectMSL(std::string_view Source, std::string_view Entry,
		std::string& Error) -> std::string
	{
		Slang::ComPtr<slang::IGlobalSession> Global;
		if (SLANG_FAILED(slang_createGlobalSession(SLANG_API_VERSION, Global.writeRef())))
		{
			Error = "Slang global session creation failed";
			return {};
		}
		const auto& Mounts = Durin::FShaderPaths::GetRegisteredMountPoints();
		const auto Mount = std::ranges::find(Mounts, "/Engine/",
			&Durin::FShaderPaths::FShaderMountPoint::VirtualRoot);
		if (Mount == Mounts.end())
		{
			Error = "Engine shader mount unavailable";
			return {};
		}
		const char* SearchPath = Mount->SourceDir.c_str();
		slang::TargetDesc Target = {};
		Target.format = SLANG_METAL;
		slang::SessionDesc Description = {};
		Description.targets = &Target;
		Description.targetCount = 1;
		Description.searchPaths = &SearchPath;
		Description.searchPathCount = 1;
		Slang::ComPtr<slang::ISession> Session;
		if (SLANG_FAILED(Global->createSession(Description, Session.writeRef())))
		{
			Error = "Slang direct-MSL session creation failed";
			return {};
		}
		const std::string SourceText(Source), EntryName(Entry);
		Slang::ComPtr<slang::IBlob> Diagnostics;
		slang::IModule* Module = Session->loadModuleFromSourceString(
			"GeneratedMaterial", "GeneratedMaterial.slang", SourceText.c_str(),
			Diagnostics.writeRef());
		if (!Module)
		{
			Error = Diagnostics ? static_cast<const char*>(Diagnostics->getBufferPointer())
				: "Generated material module load failed";
			return {};
		}
		Slang::ComPtr<slang::IEntryPoint> EntryPoint;
		if (SLANG_FAILED(Module->findEntryPointByName(EntryName.c_str(), EntryPoint.writeRef())))
		{
			Error = "Generated material entry point lookup failed";
			return {};
		}
		slang::IComponentType* Components[] = {Module, EntryPoint.get()};
		Slang::ComPtr<slang::IComponentType> Composite;
		if (SLANG_FAILED(Session->createCompositeComponentType(Components, 2,
			Composite.writeRef(), Diagnostics.writeRef())))
		{
			Error = Diagnostics ? static_cast<const char*>(Diagnostics->getBufferPointer())
				: "Generated material composition failed";
			return {};
		}
		Slang::ComPtr<slang::IComponentType> Linked;
		if (SLANG_FAILED(Composite->link(Linked.writeRef(), Diagnostics.writeRef())))
		{
			Error = Diagnostics ? static_cast<const char*>(Diagnostics->getBufferPointer())
				: "Generated material link failed";
			return {};
		}
		Slang::ComPtr<slang::IBlob> Code;
		if (SLANG_FAILED(Linked->getEntryPointCode(0, 0, Code.writeRef(),
			Diagnostics.writeRef())) || !Code)
		{
			Error = Diagnostics ? static_cast<const char*>(Diagnostics->getBufferPointer())
				: "Generated material direct-MSL output failed";
			return {};
		}
		return {static_cast<const char*>(Code->getBufferPointer()), Code->getBufferSize()};
	}
}

TEST(FMetalMaterialQualificationTests, ProductionMaterialDirectMslCompiles)
{
	@autoreleasepool
	{
		id<MTLDevice> Device = MTLCreateSystemDefaultDevice();
		ASSERT_NE(Device, nil);
		for (const bool bTextured : {false, true})
		{
			auto Input = MakeMaterialInput(bTextured);
			const auto Environment = Durin::BuildDefaultMaterialCompilerEnvironment(Input.Environment);
			ASSERT_TRUE(Environment) << Durin::FormatMaterialError(Environment.Error);
			const auto Compiled = Durin::MIR::Compile(Input);
			ASSERT_TRUE(Compiled);
			for (size_t Index = 0; Index < Durin::MaterialCompiledEntryPoints.size(); ++Index)
			{
				const auto Entry = Durin::MaterialCompiledEntryPoints[Index];
				SCOPED_TRACE(std::string(bTextured ? "lit masked " : "unlit ") + std::string(Entry));
				std::string Error;
				const auto MSL = CompileMaterialDirectMSL(Compiled.GeneratedSource, Entry, Error);
				ASSERT_FALSE(MSL.empty()) << Error;
				NSString* Source = [[NSString alloc] initWithBytes:MSL.data()
					length:MSL.size() encoding:NSUTF8StringEncoding];
				ASSERT_NE(Source, nil);
				NSError* MetalError = nil;
				id<MTLLibrary> Library = [Device newLibraryWithSource:Source
					options:nil error:&MetalError];
				ASSERT_NE(Library, nil) << [[MetalError description] UTF8String];
				id<MTLFunction> Function = [Library
					newFunctionWithName:[NSString stringWithUTF8String:Entry.data()]];
				ASSERT_NE(Function, nil);
				EXPECT_EQ(Function.functionType,
					Index < 4 ? MTLFunctionTypeFragment : MTLFunctionTypeVertex);
			}
		}
	}
}

TEST(FMetalMaterialQualificationTests, ProductionMaterialVariantsCompileAsMetalLibraries)
{
	@autoreleasepool
	{
		id<MTLDevice> Device = MTLCreateSystemDefaultDevice();
		ASSERT_NE(Device, nil);
		for (const bool bTextured : {false, true})
		{
			SCOPED_TRACE(bTextured ? "masked textured material" : "resource-free unlit material");
			auto Input = MakeMaterialInput(bTextured);
			const auto Environment = Durin::BuildDefaultMaterialCompilerEnvironment(Input.Environment);
			ASSERT_TRUE(Environment) << Durin::FormatMaterialError(Environment.Error);
			const auto Compiled = Durin::MIR::Compile(Input);
			ASSERT_TRUE(Compiled) << (Compiled.Diagnostics.empty() ? "missing diagnostic"
				: Durin::FormatMaterialError(Compiled.Diagnostics.front().Error));
			ASSERT_EQ(Compiled.CompiledShaders.size(), Durin::MaterialCompiledEntryPoints.size());
			EXPECT_EQ(Compiled.Layout.ResourceFieldCount, bTextured ? 1u : 0u);
			EXPECT_EQ(Compiled.Layout.Fields.size(), bTextured ? 4u : 0u);
			ASSERT_TRUE(Durin::ValidateMaterialCompiledStages(
				Compiled.CompiledShaders, Compiled.Layout));
			for (const auto& Shader : Compiled.CompiledShaders)
			{
				SCOPED_TRACE(Shader.SourceEntryPoint);
				std::string CrossError;
				const std::string MetalCode = TranslateMaterialStage(Shader, CrossError);
				ASSERT_FALSE(MetalCode.empty()) << CrossError;
				NSString* Source = [[NSString alloc] initWithBytes:MetalCode.data()
					length:MetalCode.size() encoding:NSUTF8StringEncoding];
				ASSERT_NE(Source, nil);
				NSError* MetalError = nil;
				id<MTLLibrary> Library = [Device newLibraryWithSource:Source
					options:nil error:&MetalError];
				ASSERT_NE(Library, nil) << [[MetalError description] UTF8String];
				id<MTLFunction> Function = [Library newFunctionWithName:@"main0"];
				ASSERT_NE(Function, nil);
				EXPECT_EQ(Function.functionType,
					Shader.Frequency == Durin::EShaderFrequency::Vertex
						? MTLFunctionTypeVertex : MTLFunctionTypeFragment);
			}
		}
	}
}

TEST(FMetalMaterialQualificationTests, ProductionLitAndUnlitMaterialDrawProduceCheckedPixels)
{
	@autoreleasepool
	{
		for (const bool bLit : {false, true})
		{
			SCOPED_TRACE(bLit ? "lit GBuffer" : "unlit color");
			auto Input = MakeMaterialInput(false);
			if (bLit) Input.StaticProperties.ShadingModel = Durin::EMaterialShadingModel::Lit;
			const auto Environment = Durin::BuildDefaultMaterialCompilerEnvironment(Input.Environment);
			ASSERT_TRUE(Environment) << Durin::FormatMaterialError(Environment.Error);
			const auto Compiled = Durin::MIR::Compile(Input);
			ASSERT_TRUE(Compiled) << (Compiled.Diagnostics.empty() ? "missing diagnostic"
				: Durin::FormatMaterialError(Compiled.Diagnostics.front().Error));
			id<MTLDevice> Device = MTLCreateSystemDefaultDevice();
			ASSERT_NE(Device, nil);
			id<MTLFunction> VertexFunction = nil;
			id<MTLFunction> FragmentFunction = nil;
			for (const int Index : {bLit ? 1 : 0, 4})
			{
				const auto& Shader = Compiled.CompiledShaders[Index];
				std::string CrossError;
				const std::string MetalCode = TranslateMaterialStage(Shader, CrossError);
				ASSERT_FALSE(MetalCode.empty()) << CrossError;
				NSString* Source = [[NSString alloc] initWithBytes:MetalCode.data()
					length:MetalCode.size() encoding:NSUTF8StringEncoding];
				ASSERT_NE(Source, nil);
				NSError* Error = nil;
				id<MTLLibrary> Library = [Device newLibraryWithSource:Source
					options:nil error:&Error];
				ASSERT_NE(Library, nil) << [[Error description] UTF8String];
				if (Index == 4) VertexFunction = [Library newFunctionWithName:@"main0"];
				else FragmentFunction = [Library newFunctionWithName:@"main0"];
			}
			ASSERT_NE(VertexFunction, nil);
			ASSERT_NE(FragmentFunction, nil);

			MTLVertexDescriptor* VertexDesc = [MTLVertexDescriptor vertexDescriptor];
			const NSUInteger Offsets[8] = {0, 16, 32, 48, 56, 64, 72, 80};
			const MTLVertexFormat Formats[8] = {MTLVertexFormatFloat3,
				MTLVertexFormatFloat4, MTLVertexFormatFloat4,
				MTLVertexFormatFloat2, MTLVertexFormatFloat2,
				MTLVertexFormatFloat2, MTLVertexFormatFloat2,
				MTLVertexFormatFloat4};
			for (NSUInteger Index = 0; Index < 8; ++Index)
			{
				VertexDesc.attributes[Index].format = Formats[Index];
				VertexDesc.attributes[Index].offset = Offsets[Index];
				VertexDesc.attributes[Index].bufferIndex = 1;
			}
			VertexDesc.layouts[1].stride = 96;
			VertexDesc.layouts[1].stepFunction = MTLVertexStepFunctionPerVertex;
			MTLRenderPipelineDescriptor* PipelineDesc = [MTLRenderPipelineDescriptor new];
			PipelineDesc.vertexFunction = VertexFunction;
			PipelineDesc.fragmentFunction = FragmentFunction;
			PipelineDesc.vertexDescriptor = VertexDesc;
			for (NSUInteger Index = 0; Index < (bLit ? 4u : 1u); ++Index)
				PipelineDesc.colorAttachments[Index].pixelFormat = MTLPixelFormatRGBA8Unorm;
			NSError* Error = nil;
			id<MTLRenderPipelineState> Pipeline = [Device
				newRenderPipelineStateWithDescriptor:PipelineDesc error:&Error];
			ASSERT_NE(Pipeline, nil) << [[Error description] UTF8String];

			struct FVertex
			{
				float Position[3], Padding;
				float Normal[4], Tangent[4], UV[4][2], Color[4];
			};
			static_assert(sizeof(FVertex) == 96);
			FVertex Vertices[3] = {};
			Vertices[0].Position[0] = -1; Vertices[0].Position[1] = -1;
			Vertices[1].Position[0] = 3; Vertices[1].Position[1] = -1;
			Vertices[2].Position[0] = -1; Vertices[2].Position[1] = 3;
			for (auto& Vertex : Vertices)
			{
				Vertex.Normal[2] = 1;
				Vertex.Tangent[0] = 1;
				Vertex.Tangent[3] = 1;
				Vertex.Color[0] = Vertex.Color[1] = Vertex.Color[2] = Vertex.Color[3] = 1;
			}
			struct FPrimitive
			{
				float Matrices[4][16];
				float Bounds[4], Transform[4];
			};
			static_assert(sizeof(FPrimitive) == 288);
			FPrimitive Primitive = {};
			for (auto& Matrix : Primitive.Matrices)
				for (int Index = 0; Index < 4; ++Index) Matrix[Index * 5] = 1;
			Primitive.Transform[0] = 1;
			id<MTLBuffer> VertexBuffer = [Device newBufferWithBytes:Vertices
				length:sizeof(Vertices) options:MTLResourceStorageModeShared];
			id<MTLBuffer> PrimitiveBuffer = [Device newBufferWithBytes:&Primitive
				length:sizeof(Primitive) options:MTLResourceStorageModeShared];
			ASSERT_NE(VertexBuffer, nil);
			ASSERT_NE(PrimitiveBuffer, nil);
			MTLTextureDescriptor* TargetDesc = [MTLTextureDescriptor
				texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
				width:1 height:1 mipmapped:NO];
			TargetDesc.storageMode = MTLStorageModeShared;
			TargetDesc.usage = MTLTextureUsageRenderTarget;
			std::array<id<MTLTexture>, 4> Targets{};
			for (NSUInteger Index = 0; Index < (bLit ? 4u : 1u); ++Index)
			{
				Targets[Index] = [Device newTextureWithDescriptor:TargetDesc];
				ASSERT_NE(Targets[Index], nil);
			}
			MTLRenderPassDescriptor* Pass = [MTLRenderPassDescriptor renderPassDescriptor];
			for (NSUInteger Index = 0; Index < (bLit ? 4u : 1u); ++Index)
			{
				Pass.colorAttachments[Index].texture = Targets[Index];
				Pass.colorAttachments[Index].loadAction = MTLLoadActionClear;
				Pass.colorAttachments[Index].storeAction = MTLStoreActionStore;
				Pass.colorAttachments[Index].clearColor = MTLClearColorMake(0, 0, 0, 0);
			}
			std::array<float, 48> ViewUniform{};
			id<MTLBuffer> ViewBuffer = bLit ? [Device newBufferWithBytes:ViewUniform.data()
				length:sizeof(ViewUniform) options:MTLResourceStorageModeShared] : nil;
			if (bLit) ASSERT_NE(ViewBuffer, nil);
			id<MTLCommandQueue> Queue = [Device newCommandQueue];
			ASSERT_NE(Queue, nil);
			id<MTLCommandBuffer> Command = [Queue commandBuffer];
			id<MTLRenderCommandEncoder> Encoder = [Command renderCommandEncoderWithDescriptor:Pass];
			ASSERT_NE(Encoder, nil);
			[Encoder setRenderPipelineState:Pipeline];
			[Encoder setFrontFacingWinding:MTLWindingCounterClockwise];
			[Encoder setVertexBuffer:PrimitiveBuffer offset:0 atIndex:0];
			[Encoder setVertexBuffer:VertexBuffer offset:0 atIndex:1];
			if (bLit) [Encoder setFragmentBuffer:ViewBuffer offset:0 atIndex:0];
			[Encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
			[Encoder endEncoding];
			[Command commit];
			[Command waitUntilCompleted];
			ASSERT_EQ(Command.status, MTLCommandBufferStatusCompleted)
				<< [[Command.error description] UTF8String];
			uint8_t Pixel[4] = {};
			[Targets[0] getBytes:Pixel bytesPerRow:sizeof(Pixel)
				fromRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0];
			EXPECT_NEAR(Pixel[0], 128, 1);
			EXPECT_NEAR(Pixel[1], 128, 1);
			EXPECT_NEAR(Pixel[2], 128, 1);
			EXPECT_EQ(Pixel[3], bLit ? 0 : 255);
			if (bLit)
			{
				const std::array<std::array<int, 4>, 3> Expected{{
					{{128, 128, 128, 128}}, {{128, 255, 255, 1}}, {{0, 0, 0, 0}}}};
				for (NSUInteger TargetIndex = 1; TargetIndex < 4; ++TargetIndex)
				{
					[Targets[TargetIndex] getBytes:Pixel bytesPerRow:sizeof(Pixel)
						fromRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0];
					for (int Channel = 0; Channel < 4; ++Channel)
						EXPECT_NEAR(Pixel[Channel], Expected[TargetIndex - 1][Channel], 1)
							<< "GBuffer target " << TargetIndex << " channel " << Channel;
				}
			}
		}
	}
}

TEST(FMetalMaterialQualificationTests, MaskedMaterialSamplesTextureAndDiscardsBelowThreshold)
{
	@autoreleasepool
	{
		auto Input = MakeMaterialInput(true);
		Input.StaticProperties.ShadingModel = Durin::EMaterialShadingModel::Unlit;
		const auto Environment = Durin::BuildDefaultMaterialCompilerEnvironment(Input.Environment);
		ASSERT_TRUE(Environment) << Durin::FormatMaterialError(Environment.Error);
		const auto Compiled = Durin::MIR::Compile(Input);
		ASSERT_TRUE(Compiled) << (Compiled.Diagnostics.empty() ? "missing diagnostic"
			: Durin::FormatMaterialError(Compiled.Diagnostics.front().Error));
		ASSERT_EQ(Compiled.Layout.UniformPayloadSize, 64u);
		ASSERT_EQ(Compiled.Layout.ResourceFieldCount, 1u);
		const auto& Shader = Compiled.CompiledShaders[0];
		ASSERT_EQ(Shader.SourceEntryPoint, "FragmentMain");
		std::string CrossError;
		const std::string MetalCode = TranslateMaterialStage(Shader, CrossError);
		ASSERT_FALSE(MetalCode.empty()) << CrossError;
		NSString* FragmentSource = [[NSString alloc] initWithBytes:MetalCode.data()
			length:MetalCode.size() encoding:NSUTF8StringEncoding];
		ASSERT_NE(FragmentSource, nil);
		NSString* VertexSource = @"#include <metal_stdlib>\n"
			"using namespace metal;\n"
			"vertex float4 main0(uint index [[vertex_id]]) {\n"
			"  float2 positions[3] = {float2(-1,-1), float2(3,-1), float2(-1,3)};\n"
			"  return float4(positions[index], 0, 1);\n"
			"}\n";
		id<MTLDevice> Device = MTLCreateSystemDefaultDevice();
		ASSERT_NE(Device, nil);
		NSError* Error = nil;
		id<MTLLibrary> VertexLibrary = [Device newLibraryWithSource:VertexSource
			options:nil error:&Error];
		ASSERT_NE(VertexLibrary, nil) << [[Error description] UTF8String];
		id<MTLLibrary> FragmentLibrary = [Device newLibraryWithSource:FragmentSource
			options:nil error:&Error];
		ASSERT_NE(FragmentLibrary, nil) << [[Error description] UTF8String];
		MTLRenderPipelineDescriptor* PipelineDesc = [MTLRenderPipelineDescriptor new];
		PipelineDesc.vertexFunction = [VertexLibrary newFunctionWithName:@"main0"];
		PipelineDesc.fragmentFunction = [FragmentLibrary newFunctionWithName:@"main0"];
		ASSERT_NE(PipelineDesc.vertexFunction, nil);
		ASSERT_NE(PipelineDesc.fragmentFunction, nil);
		PipelineDesc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
		id<MTLRenderPipelineState> Pipeline = [Device
			newRenderPipelineStateWithDescriptor:PipelineDesc error:&Error];
		ASSERT_NE(Pipeline, nil) << [[Error description] UTF8String];
		MTLTextureDescriptor* SampleDesc = [MTLTextureDescriptor
			texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
			width:1 height:1 mipmapped:NO];
		SampleDesc.storageMode = MTLStorageModeShared;
		SampleDesc.usage = MTLTextureUsageShaderRead;
		id<MTLTexture> Sample = [Device newTextureWithDescriptor:SampleDesc];
		ASSERT_NE(Sample, nil);
		const uint8_t Texel[4] = {128, 64, 255, 255};
		[Sample replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0
			withBytes:Texel bytesPerRow:sizeof(Texel)];
		MTLSamplerDescriptor* SamplerDesc = [MTLSamplerDescriptor new];
		SamplerDesc.minFilter = MTLSamplerMinMagFilterNearest;
		SamplerDesc.magFilter = MTLSamplerMinMagFilterNearest;
		id<MTLSamplerState> Sampler = [Device newSamplerStateWithDescriptor:SamplerDesc];
		ASSERT_NE(Sampler, nil);
		MTLTextureDescriptor* TargetDesc = [MTLTextureDescriptor
			texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
			width:1 height:1 mipmapped:NO];
		TargetDesc.storageMode = MTLStorageModeShared;
		TargetDesc.usage = MTLTextureUsageRenderTarget;
		id<MTLTexture> Target = [Device newTextureWithDescriptor:TargetDesc];
		ASSERT_NE(Target, nil);
		id<MTLCommandQueue> Queue = [Device newCommandQueue];
		ASSERT_NE(Queue, nil);
		for (const bool bDiscard : {false, true})
		{
			SCOPED_TRACE(bDiscard ? "masked fragment discarded" : "textured fragment retained");
			std::array<float, 16> Uniform{};
			unsigned MatchedFields = 0;
			for (const auto& Field : Compiled.Layout.Fields)
			{
				if (Field.Storage != Durin::EMaterialRenderFieldStorage::Uniform) continue;
				ASSERT_EQ(Field.Offset % sizeof(float), 0u);
				ASSERT_LT(Field.Offset, sizeof(Uniform));
				float* Destination = Uniform.data() + Field.Offset / sizeof(float);
				if (Field.ParameterId == TintId)
				{
					ASSERT_EQ(Field.Size, 16u);
					Destination[0] = 1; Destination[1] = 0.5f;
					Destination[2] = 0.25f; Destination[3] = 1;
				}
				else if (Field.ParameterId == UVId)
				{
					ASSERT_EQ(Field.Size, 16u);
					Destination[0] = 0.5f; Destination[1] = 0.5f;
				}
				else if (Field.ParameterId == MaskId)
				{
					ASSERT_EQ(Field.Size, 4u);
					Destination[0] = bDiscard ? 0 : 1;
				}
				else FAIL() << "Unexpected material uniform field";
				++MatchedFields;
			}
			ASSERT_EQ(MatchedFields, 3u);
			id<MTLBuffer> UniformBuffer = [Device newBufferWithBytes:Uniform.data()
				length:sizeof(Uniform) options:MTLResourceStorageModeShared];
			ASSERT_NE(UniformBuffer, nil);
			MTLRenderPassDescriptor* Pass = [MTLRenderPassDescriptor renderPassDescriptor];
			Pass.colorAttachments[0].texture = Target;
			Pass.colorAttachments[0].loadAction = MTLLoadActionClear;
			Pass.colorAttachments[0].storeAction = MTLStoreActionStore;
			Pass.colorAttachments[0].clearColor = MTLClearColorMake(0.1, 0.2, 0.3, 1);
			id<MTLCommandBuffer> Command = [Queue commandBuffer];
			id<MTLRenderCommandEncoder> Encoder = [Command renderCommandEncoderWithDescriptor:Pass];
			ASSERT_NE(Encoder, nil);
			[Encoder setRenderPipelineState:Pipeline];
			[Encoder setFragmentBuffer:UniformBuffer offset:0 atIndex:0];
			[Encoder setFragmentTexture:Sample atIndex:0];
			[Encoder setFragmentSamplerState:Sampler atIndex:0];
			[Encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
			[Encoder endEncoding];
			[Command commit];
			[Command waitUntilCompleted];
			ASSERT_EQ(Command.status, MTLCommandBufferStatusCompleted)
				<< [[Command.error description] UTF8String];
			uint8_t Pixel[4] = {};
			[Target getBytes:Pixel bytesPerRow:sizeof(Pixel)
				fromRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0];
			const int Expected[4] = {bDiscard ? 26 : 128, bDiscard ? 51 : 32,
				bDiscard ? 77 : 64, 255};
			for (int Index = 0; Index < 4; ++Index)
				EXPECT_NEAR(Pixel[Index], Expected[Index], 1);
		}
	}
}
