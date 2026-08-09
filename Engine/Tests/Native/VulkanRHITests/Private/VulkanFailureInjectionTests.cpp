#include <gtest/gtest.h>

#include "Modules/ModuleManager.h"
#include "PCH.VulkanRHI.h"
#include "CoreGlobals.h"
#include "Application/GenericApplication.h"
#include "ApplicationCoreGlobals.h"
#include "HAL/PlatformLTS.h"
#include "RHIGlobals.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "Shader/SlangShaderCompiler.h"
#include "VulkanRHIPrivate.h"
#include "VulkanExtensions.h"
#include "VulkanDevice.h"
#include "VulkanContext.h"
#include "VulkanSwapchain.h"
#include "VulkanViewport.h"
#include "Window/GenericWindow.h"
#include "Window/GenericWindowDefinition.h"

namespace Durin::VulkanRHI
{
	namespace
	{
		auto MakeInstanceNegotiationInput() -> FVulkanInstanceNegotiationInput
		{
			FVulkanInstanceNegotiationInput Input;
			Input.LoaderApiVersion = VK_API_VERSION_1_3;
			Input.PlatformRequiredExtensions = {
				VK_KHR_SURFACE_EXTENSION_NAME,
				VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
			Input.AvailableExtensions = Input.PlatformRequiredExtensions;
			return Input;
		}

		auto MakePhysicalDeviceCandidateInput() -> FVulkanPhysicalDeviceCandidateInput
		{
			FVulkanPhysicalDeviceCandidateInput Input;
			Input.DeviceName = "Suitable GPU";
			Input.DeviceType = vk::PhysicalDeviceType::eDiscreteGpu;
			Input.ApiVersion = VK_API_VERSION_1_3;
			Input.VendorId = 1;
			Input.DeviceId = 2;
			Input.MaxImageDimension2D = 8192;
			Input.MaxImageDimensionCube = 8192;
			Input.MaxImageArrayLayers = 256;
			Input.bFillModeNonSolid = true;
			Input.bShaderDrawParameters = true;
			Input.AvailableExtensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
			Input.QueueFamilies = {
				{vk::QueueFlagBits::eGraphics | vk::QueueFlagBits::eCompute
					| vk::QueueFlagBits::eTransfer, 1, true}};
			return Input;
		}

		class FVulkanCreateFailureInjectionTests : public testing::Test
		{
		protected:
			auto SetUp() -> void override
			{
				if (const char* ExistingMode = std::getenv("DURIN_RHI_EXECUTION"))
				{
					PreviousExecutionMode = ExistingMode;
				}
				_putenv_s("DURIN_RHI_EXECUTION", "threaded");
				ResetVulkanCreateFailures();
			}

			auto TearDown() -> void override
			{
				if (GDynamicRHI)
				{
					RHIExit();
				}
				FModuleManager::Get().UnloadModule("VulkanRHI");
				ResetVulkanCreateFailures();
				_putenv_s("DURIN_RHI_EXECUTION",
					PreviousExecutionMode ? PreviousExecutionMode->c_str() : "");
			}

			std::optional<std::string> PreviousExecutionMode;
		};
	}

	TEST(FVulkanInstanceNegotiationTests, ValidationPolicyIsConfigurationAware)
	{
		EXPECT_TRUE(ResolveVulkanValidationPolicy(nullptr, true, false).bRequestDiagnostics);
		EXPECT_FALSE(ResolveVulkanValidationPolicy("auto", false, false).bRequestDiagnostics);
		EXPECT_TRUE(ResolveVulkanValidationPolicy("on", false, false).bRequestDiagnostics);
		EXPECT_FALSE(ResolveVulkanValidationPolicy("off", true, false).bRequestDiagnostics);
		EXPECT_FALSE(ResolveVulkanValidationPolicy("on", true, true).bRequestDiagnostics);
		const FVulkanValidationPolicy Invalid = ResolveVulkanValidationPolicy("invalid", true, false);
		EXPECT_TRUE(Invalid.bInvalidSetting);
		EXPECT_TRUE(Invalid.bRequestDiagnostics);
	}

	TEST(FVulkanInstanceNegotiationTests, RejectsLoaderFloorAndMissingPlatformRequirement)
	{
		FVulkanInstanceNegotiationInput Input = MakeInstanceNegotiationInput();
		Input.LoaderApiVersion = VK_API_VERSION_1_0;
		FVulkanInstanceNegotiationResult Result = NegotiateVulkanInstance(Input);
		EXPECT_FALSE(Result.IsSuccess());
		EXPECT_NE(Result.Diagnostic.find("Vulkan 1.1"), std::string::npos);

		Input = MakeInstanceNegotiationInput();
		Input.AvailableExtensions.pop_back();
		Result = NegotiateVulkanInstance(Input);
		EXPECT_FALSE(Result.IsSuccess());
		EXPECT_NE(Result.Diagnostic.find(VK_KHR_WIN32_SURFACE_EXTENSION_NAME), std::string::npos);
		EXPECT_NE(Result.Diagnostic.find("platform required"), std::string::npos);
	}

	TEST(FVulkanInstanceNegotiationTests, PromotedAndOptionalRequirementsAreDeduplicated)
	{
		FVulkanInstanceNegotiationInput Input = MakeInstanceNegotiationInput();
		Input.PlatformRequiredExtensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
		Input.AvailableExtensions.insert(Input.AvailableExtensions.end(), {
			VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
			VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME,
			VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME});
		const FVulkanInstanceNegotiationResult Result = NegotiateVulkanInstance(Input);
		ASSERT_TRUE(Result.IsSuccess()) << Result.Diagnostic;
		EXPECT_EQ(std::ranges::count(Result.EnabledExtensions,
			std::string(VK_KHR_SURFACE_EXTENSION_NAME)), 1);
		EXPECT_EQ(std::ranges::count(Result.EnabledExtensions,
			std::string(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME)), 0);
		EXPECT_EQ(std::ranges::count(Result.EnabledExtensions,
			std::string(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME)), 1);
		EXPECT_EQ(std::ranges::count(Result.EnabledExtensions,
			std::string(VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME)), 1);
	}

	TEST(FVulkanInstanceNegotiationTests, OptionalDiagnosticsActivateIndependently)
	{
		FVulkanInstanceNegotiationInput Input = MakeInstanceNegotiationInput();
		Input.bRequestDiagnostics = true;
		FVulkanInstanceNegotiationResult Result = NegotiateVulkanInstance(Input);
		ASSERT_TRUE(Result.IsSuccess()) << Result.Diagnostic;
		EXPECT_TRUE(Result.EnabledExtensions.empty()
			|| std::ranges::find(Result.EnabledExtensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)
				== Result.EnabledExtensions.end());
		EXPECT_TRUE(Result.EnabledLayers.empty());

		Input.AvailableLayers.emplace_back("VK_LAYER_KHRONOS_validation");
		Result = NegotiateVulkanInstance(Input);
		EXPECT_EQ(Result.EnabledLayers,
			std::vector<std::string>{"VK_LAYER_KHRONOS_validation"});
		EXPECT_EQ(std::ranges::count(Result.EnabledExtensions,
			std::string(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)), 0);

		Input.AvailableExtensions.emplace_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
		Result = NegotiateVulkanInstance(Input);
		EXPECT_EQ(std::ranges::count(Result.EnabledExtensions,
			std::string(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)), 1);
	}

	TEST(FVulkanInstanceNegotiationTests, FailedCandidateDoesNotAffectFollowingSuccess)
	{
		FVulkanInstanceNegotiationInput Input = MakeInstanceNegotiationInput();
		Input.AvailableExtensions.clear();
		EXPECT_FALSE(NegotiateVulkanInstance(Input).IsSuccess());
		Input = MakeInstanceNegotiationInput();
		const FVulkanInstanceNegotiationResult Result = NegotiateVulkanInstance(Input);
		EXPECT_TRUE(Result.IsSuccess()) << Result.Diagnostic;
		EXPECT_EQ(Result.EnabledExtensions.size(), 2u);
	}

	TEST(FVulkanDeviceCandidateTests, RejectsEveryHardRequirementBeforeRanking)
	{
		struct FCase
		{
			std::string_view ExpectedReason;
			std::function<void(FVulkanPhysicalDeviceCandidateInput&)> BreakRequirement;
		};
		const std::array Cases{
			FCase{"Vulkan 1.1", [](auto& Input) { Input.ApiVersion = VK_API_VERSION_1_0; }},
			FCase{"VK_KHR_swapchain", [](auto& Input) { Input.AvailableExtensions.clear(); }},
			FCase{"fillModeNonSolid", [](auto& Input) { Input.bFillModeNonSolid = false; }},
			FCase{"shaderDrawParameters", [](auto& Input) { Input.bShaderDrawParameters = false; }},
			FCase{"maxImageDimension2D", [](auto& Input) { Input.MaxImageDimension2D = 0; }},
			FCase{"maxImageDimensionCube", [](auto& Input) { Input.MaxImageDimensionCube = 0; }},
			FCase{"below six", [](auto& Input) { Input.MaxImageArrayLayers = 5; }},
			FCase{"graphics, compute, and Win32 presentation", [](auto& Input) {
				Input.QueueFamilies[0].bSupportsWin32Presentation = false;
			}},
		};
		for (const FCase& Case : Cases)
		{
			FVulkanPhysicalDeviceCandidateInput Input = MakePhysicalDeviceCandidateInput();
			Case.BreakRequirement(Input);
			const FVulkanPhysicalDeviceCandidateEvaluation Result =
				EvaluateVulkanPhysicalDeviceCandidate(Input);
			ASSERT_FALSE(Result.IsSuitable());
			EXPECT_TRUE(std::ranges::any_of(Result.RejectionReasons,
				[&Case](const std::string& Reason) { return Reason.find(Case.ExpectedReason) != std::string::npos; }));
		}
	}

	TEST(FVulkanDeviceCandidateTests, SelectsLowestCompleteQueueAndOptionalFeatures)
	{
		FVulkanPhysicalDeviceCandidateInput Input = MakePhysicalDeviceCandidateInput();
		Input.QueueFamilies.insert(Input.QueueFamilies.begin(),
			{vk::QueueFlagBits::eGraphics | vk::QueueFlagBits::eCompute, 1, false});
		Input.bSynchronization2Feature = true;
		Input.bSwapchainMaintenanceFeature = true;
		Input.bHasSwapchainMaintenanceInstanceDependencies = true;
		Input.AvailableExtensions.push_back(VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
		const FVulkanPhysicalDeviceCandidateEvaluation Result =
			EvaluateVulkanPhysicalDeviceCandidate(Input);
		ASSERT_TRUE(Result.IsSuitable());
		EXPECT_EQ(Result.GraphicsPresentQueueFamilyIndex, 1);
		EXPECT_TRUE(Result.bEnableSynchronization2);
		EXPECT_TRUE(Result.bEnableSwapchainMaintenance1);
		EXPECT_EQ(std::ranges::count(Result.EnabledExtensions,
			std::string(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME)), 0);
	}

	TEST(FVulkanDeviceCandidateTests, RankingIsDeterministic)
	{
		FVulkanPhysicalDeviceCandidateInput Discrete = MakePhysicalDeviceCandidateInput();
		FVulkanPhysicalDeviceCandidateInput Integrated = Discrete;
		Integrated.DeviceType = vk::PhysicalDeviceType::eIntegratedGpu;
		EXPECT_TRUE(IsVulkanPhysicalDeviceCandidatePreferred(Discrete, Integrated));
		Integrated = Discrete;
		Integrated.MaxImageDimension2D = Discrete.MaxImageDimension2D - 1;
		EXPECT_TRUE(IsVulkanPhysicalDeviceCandidatePreferred(Discrete, Integrated));
		Integrated = Discrete;
		Integrated.VendorId = Discrete.VendorId + 1;
		EXPECT_TRUE(IsVulkanPhysicalDeviceCandidatePreferred(Discrete, Integrated));
		Integrated = Discrete;
		Integrated.DeviceName = "Z GPU";
		EXPECT_TRUE(IsVulkanPhysicalDeviceCandidatePreferred(Discrete, Integrated));
	}

	TEST(FVulkanDeviceCandidateTests, AllDeviceDiagnosticIsBoundedAndQualified)
	{
		std::vector<FVulkanPhysicalDeviceCandidateInput> Inputs(20);
		std::vector<FVulkanPhysicalDeviceCandidateEvaluation> Evaluations(20);
		for (size_t DeviceIndex = 0; DeviceIndex < Inputs.size(); ++DeviceIndex)
		{
			Inputs[DeviceIndex].DeviceName = std::format("GPU {}", DeviceIndex);
			for (size_t ReasonIndex = 0; ReasonIndex < 10; ++ReasonIndex)
				Evaluations[DeviceIndex].RejectionReasons.push_back(
					std::format("reason {} {}", ReasonIndex, std::string(300, 'x')));
		}
		const std::string Diagnostic =
			FormatVulkanPhysicalDeviceRejectionDiagnostic(Inputs, Evaluations);
		EXPECT_NE(Diagnostic.find("[0] GPU 0"), std::string::npos);
		EXPECT_NE(Diagnostic.find("[15] GPU 15"), std::string::npos);
		EXPECT_EQ(Diagnostic.find("GPU 16"), std::string::npos);
		EXPECT_NE(Diagnostic.find("(+4 devices)"), std::string::npos);
		EXPECT_NE(Diagnostic.find("(+2 reasons)"), std::string::npos);
		EXPECT_EQ(Diagnostic.find(std::string(257, 'x')), std::string::npos);
	}

	TEST_F(FVulkanCreateFailureInjectionTests, ArmedBoundaryFailsExactlyOnce)
	{
		ArmVulkanCreateFailure(EVulkanCreateFailurePoint::Image);

		EXPECT_TRUE(ConsumeVulkanCreateFailure(EVulkanCreateFailurePoint::Image));
		EXPECT_FALSE(ConsumeVulkanCreateFailure(EVulkanCreateFailurePoint::Image));
	}

	TEST_F(FVulkanCreateFailureInjectionTests, ArmedBoundariesRemainIndependent)
	{
		ArmVulkanCreateFailure(EVulkanCreateFailurePoint::Device);
		ArmVulkanCreateFailure(EVulkanCreateFailurePoint::Buffer);

		EXPECT_FALSE(ConsumeVulkanCreateFailure(EVulkanCreateFailurePoint::Swapchain));
		EXPECT_TRUE(ConsumeVulkanCreateFailure(EVulkanCreateFailurePoint::Buffer));
		EXPECT_TRUE(ConsumeVulkanCreateFailure(EVulkanCreateFailurePoint::Device));
	}

	TEST_F(FVulkanCreateFailureInjectionTests, NativeBoundaryReportsInjectedOutOfMemory)
	{
		ArmVulkanCreateFailure(EVulkanCreateFailurePoint::Instance);

		try
		{
			ThrowIfVulkanNativeCreateFailureIsArmed(EVulkanCreateFailurePoint::Instance);
			FAIL() << "Expected the armed native creation boundary to throw.";
		}
		catch (const vk::SystemError& Error)
		{
			EXPECT_EQ(Error.code().value(), static_cast<int>(vk::Result::eErrorOutOfDeviceMemory));
		}
		EXPECT_NO_THROW(ThrowIfVulkanNativeCreateFailureIsArmed(EVulkanCreateFailurePoint::Instance));
	}

	TEST_F(FVulkanCreateFailureInjectionTests,
		InitializationFailuresRollbackAndReleaseTheBackendModule)
	{
		const std::array FailureCases = {
			std::pair{EVulkanCreateFailurePoint::Instance,
				std::string_view("Vulkan instance creation failed")},
			std::pair{EVulkanCreateFailurePoint::Device,
				std::string_view("Vulkan logical-device creation failed")},
			std::pair{EVulkanCreateFailurePoint::Allocator,
				std::string_view("Vulkan allocator creation failed")},
		};

		for (const auto& [FailurePoint, ExpectedDiagnostic] : FailureCases)
		{
			SCOPED_TRACE(ExpectedDiagnostic);
			ArmVulkanCreateFailure(FailurePoint);

			EXPECT_FALSE(RHIInit());
			EXPECT_EQ(GDynamicRHI, nullptr);
			EXPECT_FALSE(FModuleManager::Get().IsModuleLoaded("VulkanRHI"));
			EXPECT_NE(GetLastRHIInitializationDiagnostic().find(
				ExpectedDiagnostic), std::string_view::npos);
		}

		ASSERT_TRUE(RHIInit());
		EXPECT_TRUE(GetLastRHIInitializationDiagnostic().empty());
		EXPECT_TRUE(FModuleManager::Get().IsModuleLoaded("VulkanRHI"));
		RHIExit();
		FModuleManager::Get().UnloadModule("VulkanRHI");
	}

	TEST_F(FVulkanCreateFailureInjectionTests,
		InlineRuntimeFactoryFailureReturnsNullThenRecovers)
	{
		_putenv_s("DURIN_RHI_EXECUTION", "inline");
		ASSERT_TRUE(RHIInit());
		EXPECT_EQ(GRHIThread, nullptr);

		FRHICommandListImmediate& RHICmdList =
			FRHICommandListImmediate::Get();
		const FRHIBufferCreateDesc BufferDesc = FRHIBufferCreateDesc::Create(
			"RecoverableInlineBuffer", 256, 16,
			EBufferUsageFlags::VertexBuffer | EBufferUsageFlags::Static);
		ArmVulkanCreateFailure(EVulkanCreateFailurePoint::Buffer);
		EXPECT_FALSE(GDynamicRHI->RHICreateBuffer(RHICmdList, BufferDesc));
		FBufferRHIRef Buffer =
			GDynamicRHI->RHICreateBuffer(RHICmdList, BufferDesc);
		ASSERT_TRUE(Buffer);

		Buffer = nullptr;
		RHICmdList.ImmediateFlush(
			EImmediateFlushType::FlushRHIThreadFlushResources);
	}

	TEST_F(FVulkanCreateFailureInjectionTests,
		CapabilitiesAndExactTextureSupportRejectBeforeNativeCreation)
	{
		_putenv_s("DURIN_RHI_EXECUTION", "inline");
		ASSERT_TRUE(RHIInit());
		const FRHICapabilities* Capabilities = GDynamicRHI->RHIGetCapabilities();
		ASSERT_NE(Capabilities, nullptr);
		EXPECT_EQ(Capabilities->FeatureLevel, ERHIFeatureLevel::ES3_1);
		EXPECT_EQ(Capabilities->SupportedTextureDimensions,
			ERHITextureDimensionFlags::Texture2D | ERHITextureDimensionFlags::TextureCube);
		EXPECT_GE(Capabilities->MaxTextureDimension2D, 1u);
		EXPECT_GE(Capabilities->MaxTextureDimensionCube, 1u);
		EXPECT_GE(Capabilities->MaxTextureArrayLayers,
			static_cast<uint32>(TextureCubeFaceCount));
		EXPECT_TRUE(EnumHasAnyFlags(Capabilities->ColorSampleCounts,
			ERHISampleCountFlags::Samples1));
		EXPECT_TRUE(EnumHasAnyFlags(Capabilities->DepthSampleCounts,
			ERHISampleCountFlags::Samples1));

		FRHITextureCreateDesc Texture2D = FRHITextureCreateDesc::Create2D(
			"Supported2D", 4, 4, EPixelFormat::RGBA8_UNORM);
		Texture2D.Flags = ETextureCreateFlags::ShaderResource;
		FRHITextureCreateDesc TextureCube = FRHITextureCreateDesc::CreateCube(
			"SupportedCube").SetExtent(4).SetFormat(EPixelFormat::RGBA8_UNORM)
			.SetFlags(ETextureCreateFlags::ShaderResource);
		EXPECT_TRUE(GDynamicRHI->RHIIsTextureSupported(Texture2D));
		EXPECT_TRUE(GDynamicRHI->RHIIsTextureSupported(TextureCube));

		const std::array DeferredDescriptions{
			FRHITextureCreateDesc::Create2DArray("Deferred2DArray")
				.SetArraySize(2).SetFormat(EPixelFormat::RGBA8_UNORM),
			FRHITextureCreateDesc::Create3D("Deferred3D")
				.SetDepth(4).SetFormat(EPixelFormat::RGBA8_UNORM),
			FRHITextureCreateDesc::CreateCubeArray("DeferredCubeArray")
				.SetExtent(4).SetFormat(EPixelFormat::RGBA8_UNORM),
		};
		FRHICommandListImmediate& RHICmdList = FRHICommandListImmediate::Get();
		for (const FRHITextureCreateDesc& Desc : DeferredDescriptions)
		{
			SCOPED_TRACE(static_cast<uint32>(Desc.Dimension));
			EXPECT_FALSE(GDynamicRHI->RHIIsTextureSupported(Desc));
			ArmVulkanCreateFailure(EVulkanCreateFailurePoint::Image);
			EXPECT_FALSE(GDynamicRHI->RHICreateTexture(RHICmdList, Desc));
			EXPECT_TRUE(ConsumeVulkanCreateFailure(EVulkanCreateFailurePoint::Image));
		}

		FRHITextureCreateDesc Oversized = Texture2D;
		Oversized.SetExtent(static_cast<int32>(Capabilities->MaxTextureDimension2D + 1), 1);
		std::string ValidationError;
		ASSERT_TRUE(ValidateTextureCreateDesc(Oversized, ValidationError)) << ValidationError;
		EXPECT_FALSE(GDynamicRHI->RHIIsTextureSupported(Oversized));

		FTextureRHIRef Created2D = GDynamicRHI->RHICreateTexture(RHICmdList, Texture2D);
		FTextureRHIRef CreatedCube = GDynamicRHI->RHICreateTexture(RHICmdList, TextureCube);
		EXPECT_TRUE(Created2D);
		EXPECT_TRUE(CreatedCube);
		Created2D = nullptr;
		CreatedCube = nullptr;
		RHICmdList.ImmediateFlush(EImmediateFlushType::FlushRHIThreadFlushResources);
	}

	TEST_F(FVulkanCreateFailureInjectionTests,
		RuntimeFactoriesReturnNullThenRecoverOnTheSameRHIThread)
	{
		ASSERT_TRUE(RHIInit());
		ASSERT_NE(GRHIThread, nullptr);
		if (!GIsGameThreadIdInitialized)
		{
			GGameThreadId = FPlatformLTS::GetCurrentThreadId();
			GIsGameThreadIdInitialized = true;
		}
		struct FRenderingThreadScope
		{
			FRenderingThreadScope() { InitRenderingThread(); }
			~FRenderingThreadScope() { ShutdownRenderingThread(); }
		} RenderingThreadScope;

		FRHICommandListImmediate& RHICmdList =
			FRHICommandListImmediate::Get();

		const FRHIBufferCreateDesc BufferDesc = FRHIBufferCreateDesc::Create(
			"RecoverableBuffer", 256, 16,
			EBufferUsageFlags::VertexBuffer | EBufferUsageFlags::Static);
		ArmVulkanCreateFailure(EVulkanCreateFailurePoint::Buffer);
		EXPECT_FALSE(GDynamicRHI->RHICreateBuffer(RHICmdList, BufferDesc));
		FBufferRHIRef Buffer =
			GDynamicRHI->RHICreateBuffer(RHICmdList, BufferDesc);
		ASSERT_TRUE(Buffer);

		FRHITextureCreateDesc TextureDesc = FRHITextureCreateDesc::Create2D(
			"RecoverableTexture", 4, 4, EPixelFormat::RGBA8_UNORM);
		TextureDesc.Flags = ETextureCreateFlags::ShaderResource;
		for (const EVulkanCreateFailurePoint FailurePoint : {
			EVulkanCreateFailurePoint::Image,
			EVulkanCreateFailurePoint::ImageView})
		{
			ArmVulkanCreateFailure(FailurePoint);
			EXPECT_FALSE(GDynamicRHI->RHICreateTexture(RHICmdList, TextureDesc));
		}
		FTextureRHIRef Texture =
			GDynamicRHI->RHICreateTexture(RHICmdList, TextureDesc);
		ASSERT_TRUE(Texture);

		FRHISamplerDesc SamplerDesc;
		ArmVulkanCreateFailure(EVulkanCreateFailurePoint::Sampler);
		EXPECT_FALSE(GDynamicRHI->RHICreateSampler(SamplerDesc));
		TRefCountPtr<FRHISampler> Sampler =
			GDynamicRHI->RHICreateSampler(SamplerDesc);
		ASSERT_TRUE(Sampler);

		FVertexDeclarationElementList Elements;
		ArmVulkanCreateFailure(EVulkanCreateFailurePoint::VertexDeclaration);
		EXPECT_FALSE(GDynamicRHI->RHICreateVertexDeclaration(Elements));
		FVertexDeclarationRHIRef VertexDeclaration =
			GDynamicRHI->RHICreateVertexDeclaration(Elements);
		ASSERT_TRUE(VertexDeclaration);
		bool bRHIThreadLocalFailureReturnedNull = false;
		FVertexDeclarationRHIRef RHIThreadLocalVertexDeclaration;
		ArmVulkanCreateFailure(EVulkanCreateFailurePoint::VertexDeclaration);
		GCommandListExecutor.ExecuteSynchronousOperation(false, [&]() {
			bRHIThreadLocalFailureReturnedNull =
				!GDynamicRHI->RHICreateVertexDeclaration(Elements);
			RHIThreadLocalVertexDeclaration =
				GDynamicRHI->RHICreateVertexDeclaration(Elements);
		});
		EXPECT_TRUE(bRHIThreadLocalFailureReturnedNull);
		ASSERT_TRUE(RHIThreadLocalVertexDeclaration);

		const std::filesystem::path ShaderPath =
			std::filesystem::path(DURIN_TEST_DATA_DIR)
			/ "RecoverableResourceFactories.slang";
		FShaderCompileOptions CompileOptions;
		CompileOptions.EntryPoints = {"VertexMain", "FragmentMain"};
		CompileOptions.Frequencies = {
			EShaderFrequency::Vertex, EShaderFrequency::Fragment};
		FSlangShaderCompiler Compiler;
		const FShaderCompilerOutput CompileOutput =
			Compiler.Compile(ShaderPath.string(), CompileOptions);
		ASSERT_TRUE(CompileOutput) << CompileOutput.ErrorMessage;
		ASSERT_EQ(CompileOutput.CompiledShaders.size(), 2u);

		auto MakeCreateDesc = [](const FCompiledShader& CompiledShader) {
			FRHIShaderCreateDesc Desc = FRHIShaderCreateDesc::Create(
				CompiledShader.DebugName.c_str(), CompiledShader.Frequency,
				*CompiledShader.Code, CompiledShader.Hash);
			Desc.SetEntryPoint(CompiledShader.BinaryEntryPoint.c_str());
			return Desc;
		};
		const FRHIShaderCreateDesc VertexShaderDesc =
			MakeCreateDesc(CompileOutput.CompiledShaders[0]);
		const FRHIShaderCreateDesc FragmentShaderDesc =
			MakeCreateDesc(CompileOutput.CompiledShaders[1]);
		ArmVulkanCreateFailure(EVulkanCreateFailurePoint::ShaderModule);
		EXPECT_FALSE(GDynamicRHI->RHICreateShader(VertexShaderDesc));
		FShaderRHIRef VertexShader =
			GDynamicRHI->RHICreateShader(VertexShaderDesc);
		FShaderRHIRef FragmentShader =
			GDynamicRHI->RHICreateShader(FragmentShaderDesc);
		ASSERT_TRUE(VertexShader);
		ASSERT_TRUE(FragmentShader);

		FRHIRenderTargetLayout RenderTargetLayout;
		RenderTargetLayout.NumColorRenderTargets = 1;
		auto& ColorAttachment =
			RenderTargetLayout.ColorAttachments[0].RenderTarget;
		ColorAttachment.Format = EPixelFormat::RGBA8_UNORM;
		ColorAttachment.LoadAction = ERHIRenderTargetLoadAction::Clear;
		ColorAttachment.StoreAction = ERHIRenderTargetStoreAction::Store;
		ColorAttachment.InitialLayout = ERHITextureLayout::Undefined;
		ColorAttachment.InitialAccess = ERHIAccess::None;
		ColorAttachment.FinalLayout = ERHITextureLayout::ShaderReadOnly;
		ColorAttachment.FinalAccess = ERHIAccess::GraphicsShaderRead;
		FGraphicsPipelineStateInitializer Initializer;
		Initializer.RenderTargetLayout = RenderTargetLayout;
		Initializer.BoundShaders.VertexShader = VertexShader;
		Initializer.BoundShaders.FragmentShader = FragmentShader;
		Initializer.VertexDeclaration = VertexDeclaration;
		Initializer.PipelineLayout.BindingLayouts.emplace_back()
			.BindingLayouts.emplace_back(
				EShaderStageFlags::Vertex, 0, ERHIBindingType::UniformBuffer);

		const FName PipelineName("RecoverableGraphicsPipeline");
		const FVulkanGraphicsPipelineTestStats PipelineStatsBefore =
			GetVulkanGraphicsPipelineTestStats();
		const FVulkanStructuralCacheTestStats StructuralStatsBefore =
			GetVulkanStructuralCacheTestStats();
		ArmVulkanCreateFailure(EVulkanCreateFailurePoint::RenderPass);
		EXPECT_FALSE(GDynamicRHI->RHICreateGraphicsPipelineState(
			PipelineName, Initializer));
		EXPECT_EQ(GetVulkanStructuralCacheTestStats().RenderPassEntryCount,
			StructuralStatsBefore.RenderPassEntryCount);
		ArmVulkanCreateFailure(EVulkanCreateFailurePoint::DescriptorSetLayout);
		EXPECT_FALSE(GDynamicRHI->RHICreateGraphicsPipelineState(
			PipelineName, Initializer));
		const FVulkanStructuralCacheTestStats StatsAfterDescriptorFailure =
			GetVulkanStructuralCacheTestStats();
		EXPECT_EQ(StatsAfterDescriptorFailure.RenderPassEntryCount,
			StructuralStatsBefore.RenderPassEntryCount + 1);
		EXPECT_EQ(StatsAfterDescriptorFailure.DescriptorSetLayoutEntryCount,
			StructuralStatsBefore.DescriptorSetLayoutEntryCount);
		EXPECT_EQ(StatsAfterDescriptorFailure.PipelineLayoutEntryCount,
			StructuralStatsBefore.PipelineLayoutEntryCount);
		ArmVulkanCreateFailure(EVulkanCreateFailurePoint::PipelineLayout);
		EXPECT_FALSE(GDynamicRHI->RHICreateGraphicsPipelineState(
			PipelineName, Initializer));
		ArmVulkanCreateFailure(EVulkanCreateFailurePoint::GraphicsPipeline);
		EXPECT_FALSE(GDynamicRHI->RHICreateGraphicsPipelineState(
			PipelineName, Initializer));
		FGraphicsPipelineStateRHIRef Pipeline =
			GDynamicRHI->RHICreateGraphicsPipelineState(
				PipelineName, Initializer);
		ASSERT_TRUE(Pipeline);
		FGraphicsPipelineStateRHIRef SameNamePipeline =
			GDynamicRHI->RHICreateGraphicsPipelineState(
				PipelineName, Initializer);
		ASSERT_TRUE(SameNamePipeline);
		EXPECT_NE(Pipeline.GetReference(), SameNamePipeline.GetReference());
		EXPECT_EQ(Pipeline->GetRefCount(), 1u);
		EXPECT_EQ(SameNamePipeline->GetRefCount(), 1u);

		FGraphicsPipelineStateInitializer ChangedInitializer = Initializer;
		ChangedInitializer.RasterizerState.CullMode = ERHICullMode::None;
		FGraphicsPipelineStateRHIRef ChangedSameNamePipeline =
			GDynamicRHI->RHICreateGraphicsPipelineState(
				PipelineName, ChangedInitializer);
		ASSERT_TRUE(ChangedSameNamePipeline);
		EXPECT_NE(Pipeline.GetReference(), ChangedSameNamePipeline.GetReference());
		EXPECT_EQ(ChangedSameNamePipeline->GetRefCount(), 1u);

		std::vector<FGraphicsPipelineStateRHIRef> StatePipelines;
		auto CreateStatePipeline = [&](FGraphicsPipelineStateInitializer State,
			std::string_view Suffix) {
			FGraphicsPipelineStateRHIRef StatePipeline =
				GDynamicRHI->RHICreateGraphicsPipelineState(
					FName(std::format("RecoverableGraphicsPipeline_{}", Suffix)),
					State);
			EXPECT_TRUE(StatePipeline);
			if (StatePipeline)
				StatePipelines.push_back(std::move(StatePipeline));
		};
		FGraphicsPipelineStateInitializer WireframeInitializer = Initializer;
		WireframeInitializer.RasterizerState.PolygonMode = ERHIPolygonMode::Line;
		CreateStatePipeline(WireframeInitializer, "Wireframe");
		FGraphicsPipelineStateInitializer CounterClockwiseInitializer = Initializer;
		CounterClockwiseInitializer.RasterizerState.FrontFace =
			ERHIFrontFace::CounterClockwise;
		CreateStatePipeline(CounterClockwiseInitializer, "CounterClockwise");
		FGraphicsPipelineStateInitializer DepthInitializer = Initializer;
		DepthInitializer.DepthState.bEnableTest = true;
		DepthInitializer.DepthState.bEnableWrite = true;
		CreateStatePipeline(DepthInitializer, "Depth");
		FGraphicsPipelineStateInitializer BlendInitializer = Initializer;
		BlendInitializer.ColorBlendState = FRHIColorBlendState::StraightAlpha();
		CreateStatePipeline(BlendInitializer, "StraightAlpha");

		FGraphicsPipelineStateInitializer TwoSetInitializer = Initializer;
		TwoSetInitializer.PipelineLayout.BindingLayouts.emplace_back()
			.BindingLayouts.emplace_back(
				EShaderStageFlags::Fragment, 1, ERHIBindingType::Sampler);
		const FVulkanStructuralCacheTestStats StatsBeforeDependentLayoutFailure =
			GetVulkanStructuralCacheTestStats();
		ArmVulkanCreateFailure(EVulkanCreateFailurePoint::DescriptorSetLayout);
		EXPECT_FALSE(GDynamicRHI->RHICreateGraphicsPipelineState(
			"DependentDescriptorFailure", TwoSetInitializer));
		const FVulkanStructuralCacheTestStats StatsAfterDependentLayoutFailure =
			GetVulkanStructuralCacheTestStats();
		EXPECT_EQ(StatsAfterDependentLayoutFailure.DescriptorSetLayoutEntryCount,
			StatsBeforeDependentLayoutFailure.DescriptorSetLayoutEntryCount);
		EXPECT_EQ(StatsAfterDependentLayoutFailure.PipelineLayoutEntryCount,
			StatsBeforeDependentLayoutFailure.PipelineLayoutEntryCount);
		FGraphicsPipelineStateRHIRef TwoSetPipeline =
			GDynamicRHI->RHICreateGraphicsPipelineState(
				"RecoveredDependentDescriptor", TwoSetInitializer);
		ASSERT_TRUE(TwoSetPipeline);

		FGraphicsPipelineStateInitializer InvalidInitializer = Initializer;
		InvalidInitializer.RasterizerState.FrontFace = ERHIFrontFace::Count;
		EXPECT_FALSE(GDynamicRHI->RHICreateGraphicsPipelineState(
			"InvalidGraphicsPipeline", InvalidInitializer));
		const FVulkanGraphicsPipelineTestStats PipelineStatsAfterCreation =
			GetVulkanGraphicsPipelineTestStats();
		EXPECT_EQ(
			PipelineStatsAfterCreation.CommittedPipelineCount,
			PipelineStatsBefore.CommittedPipelineCount + 8);
		EXPECT_EQ(
			PipelineStatsAfterCreation.CreatedPipelineLayoutCount,
			PipelineStatsBefore.CreatedPipelineLayoutCount + 9);
		EXPECT_EQ(
			PipelineStatsAfterCreation.RolledBackPipelineLayoutCount,
			PipelineStatsBefore.RolledBackPipelineLayoutCount + 1);
		const FVulkanStructuralCacheTestStats StatsAfterPipelineCreation =
			GetVulkanStructuralCacheTestStats();
		EXPECT_EQ(StatsAfterPipelineCreation.DescriptorSetLayoutEntryCount,
			StructuralStatsBefore.DescriptorSetLayoutEntryCount + 2);
		EXPECT_EQ(StatsAfterPipelineCreation.PipelineLayoutEntryCount,
			StructuralStatsBefore.PipelineLayoutEntryCount + 2);

		FRHITextureCreateDesc RenderTargetDesc = FRHITextureCreateDesc::Create2D(
			"RecoverableFramebufferTexture", 8, 8, EPixelFormat::RGBA8_UNORM);
		RenderTargetDesc.Flags = ETextureCreateFlags::RenderTargetable
			| ETextureCreateFlags::ShaderResource;
		FTextureRHIRef RenderTarget =
			GDynamicRHI->RHICreateTexture(RHICmdList, RenderTargetDesc);
		ASSERT_TRUE(RenderTarget);
		FRHIRenderPassInfo PassInfo;
		PassInfo.RenderTargetLayout = RenderTargetLayout;
		PassInfo.ColorRenderTargets[0] = RenderTarget;
		PassInfo.ColorClearValues[0] = FClearValueBinding(0.0f, 0.0f, 0.0f, 1.0f);
		const FVulkanStructuralCacheTestStats StatsBeforeFramebuffer =
			GetVulkanStructuralCacheTestStats();
		auto ExpectFramebufferFailure = [&](EVulkanCreateFailurePoint FailurePoint) {
			ArmVulkanCreateFailure(FailurePoint);
			bool bFailed = false;
			GCommandListExecutor.ExecuteSynchronousOperation(false, [&]() {
				try
				{
					auto* Context = static_cast<FVulkanCommandListContext*>(
						GDynamicRHI->RHIGetDefaultContext());
					Context->RHIBeginRenderPass(PassInfo, "InjectedFramebufferFailure");
					Context->RHIEndRenderPass();
				}
				catch (...)
				{
					bFailed = true;
				}
			});
			EXPECT_TRUE(bFailed);
			EXPECT_EQ(GetVulkanStructuralCacheTestStats().FramebufferEntryCount,
				StatsBeforeFramebuffer.FramebufferEntryCount);
		};
		ExpectFramebufferFailure(EVulkanCreateFailurePoint::FramebufferImageView);
		ExpectFramebufferFailure(EVulkanCreateFailurePoint::Framebuffer);
		const FVulkanStructuralCacheTestStats StatsAfterFramebufferFailure =
			GetVulkanStructuralCacheTestStats();
		EXPECT_EQ(
			StatsAfterFramebufferFailure.CreatedFramebufferViewCount
				- StatsBeforeFramebuffer.CreatedFramebufferViewCount,
			StatsAfterFramebufferFailure.ReleasedFramebufferViewCount
				- StatsBeforeFramebuffer.ReleasedFramebufferViewCount);
		GCommandListExecutor.ExecuteSynchronousOperation(false, [&]() {
			auto* Context = static_cast<FVulkanCommandListContext*>(
				GDynamicRHI->RHIGetDefaultContext());
			Context->RHIBeginRenderPass(PassInfo, "RecoveredFramebuffer");
			Context->RHIEndRenderPass();
		});
		EXPECT_EQ(GetVulkanStructuralCacheTestStats().FramebufferEntryCount,
			StatsBeforeFramebuffer.FramebufferEntryCount + 1);

		StatePipelines.clear();
		TwoSetPipeline = nullptr;
		ChangedSameNamePipeline = nullptr;
		SameNamePipeline = nullptr;
		Pipeline = nullptr;
		FragmentShader = nullptr;
		VertexShader = nullptr;
		RHIThreadLocalVertexDeclaration = nullptr;
		VertexDeclaration = nullptr;
		Sampler = nullptr;
		Texture = nullptr;
		RenderTarget = nullptr;
		Buffer = nullptr;
		RHICmdList.ImmediateFlush(EImmediateFlushType::FlushRHIThreadFlushResources);
		const FVulkanGraphicsPipelineTestStats PipelineStatsAfterRelease =
			GetVulkanGraphicsPipelineTestStats();
		EXPECT_EQ(
			PipelineStatsAfterRelease.DestroyedPipelineCount,
			PipelineStatsBefore.DestroyedPipelineCount + 8);
	}

	TEST_F(FVulkanCreateFailureInjectionTests,
		ViewportOutputCandidatesFailAtomicallyAndRecover)
	{
		class FTestApplication final : public FGenericApplication
		{
		public:
			explicit FTestApplication(std::vector<std::shared_ptr<FGenericWindow>> InWindows)
				: Windows(std::move(InWindows))
			{
			}

			auto FindWindowByNativeWindowHandle(void* InNativeWindowHandle)
				-> std::shared_ptr<FGenericWindow> override
			{
				const auto It = std::ranges::find_if(Windows,
					[InNativeWindowHandle](const std::shared_ptr<FGenericWindow>& Window) {
						return Window && Window->GetOSNativeWindowHandle() == InNativeWindowHandle;
					});
				return It != Windows.end() ? *It : nullptr;
			}

			std::vector<std::shared_ptr<FGenericWindow>> Windows;
		};

		struct FApplicationCoreScope
		{
			FApplicationCoreScope() { InitializeApplicationCore(); }
			~FApplicationCoreScope()
			{
				GApp = nullptr;
				ShutdownApplicationCore();
			}
		} ApplicationCoreScope;
		std::shared_ptr<FGenericWindow> Window = MakePlatformWindow();
		auto WindowDefinition = std::make_shared<FGenericWindowDefinition>();
		WindowDefinition->XDesiredPositionOnScreen = 0.0f;
		WindowDefinition->YDesiredPositionOnScreen = 0.0f;
		WindowDefinition->WidthDesiredOnScreen = 64.0f;
		WindowDefinition->HeightDesiredOnScreen = 64.0f;
		WindowDefinition->Title = "Vulkan viewport transaction test";
		Window->Initialize(WindowDefinition);
		ASSERT_NE(Window->GetOSNativeWindowHandle(), nullptr);
		std::shared_ptr<FGenericWindow> DetachedWindow = MakePlatformWindow();
		auto DetachedDefinition = std::make_shared<FGenericWindowDefinition>(*WindowDefinition);
		DetachedDefinition->XDesiredPositionOnScreen = 96.0f;
		DetachedDefinition->Title = "Vulkan detached viewport transaction test";
		DetachedWindow->Initialize(DetachedDefinition);
		ASSERT_NE(DetachedWindow->GetOSNativeWindowHandle(), nullptr);
		GApp = std::make_shared<FTestApplication>(
			std::vector<std::shared_ptr<FGenericWindow>>{Window, DetachedWindow});
		ASSERT_EQ(GApp->FindWindowByNativeWindowHandle(
			Window->GetOSNativeWindowHandle()), Window);

		ASSERT_TRUE(RHIInit());
		ASSERT_NE(GRHIThread, nullptr);
		if (!GIsGameThreadIdInitialized)
		{
			GGameThreadId = FPlatformLTS::GetCurrentThreadId();
			GIsGameThreadIdInitialized = true;
		}
		struct FRenderingThreadScope
		{
			FRenderingThreadScope() { InitRenderingThread(); }
			~FRenderingThreadScope() { ShutdownRenderingThread(); }
		} RenderingThreadScope;

		ArmVulkanCreateFailure(EVulkanCreateFailurePoint::Swapchain);
		TRefCountPtr<FRHIViewport> Viewport = GDynamicRHI->RHICreateViewport(
			Window->GetOSNativeWindowHandle(), 64, 64, false,
			EPixelFormat::SBGRA8_UNORM,
			EViewportPresentModePolicy::MainWindow);
		ASSERT_TRUE(Viewport);

		auto* VulkanViewport = static_cast<FVulkanViewport*>(Viewport.GetReference());
		GCommandListExecutor.ExecuteSynchronousOperation(false, [VulkanViewport]() {
			EXPECT_FALSE(VulkanViewport->HasAvailableOutput());
		});
		bool bInitialBackBufferWasNull = false;
		ENQUEUE_RENDER_COMMAND(CheckUnavailableVulkanViewport)(
			[Viewport, &bInitialBackBufferWasNull](FRHICommandListImmediate&) {
				bInitialBackBufferWasNull =
					!GDynamicRHI->RHIGetViewportBackBuffer(Viewport);
			});
		FlushRenderingCommands();
		EXPECT_TRUE(bInitialBackBufferWasNull);
		ArmVulkanCreateFailure(EVulkanCreateFailurePoint::Swapchain);
		GCommandListExecutor.ExecuteSynchronousOperation(false, [VulkanViewport]() {
			VulkanViewport->BeginDrawing();
		});
		EXPECT_TRUE(ConsumeVulkanCreateFailure(EVulkanCreateFailurePoint::Swapchain));
		GCommandListExecutor.ExecuteSynchronousOperation(false, [VulkanViewport]() {
			VulkanViewport->RecreateSwapchain();
		});
		vk::SwapchainKHR FirstSwapchain = VK_NULL_HANDLE;
		GCommandListExecutor.ExecuteSynchronousOperation(false, [&]() {
			ASSERT_NE(VulkanViewport->GetSwapchain(), nullptr);
			FirstSwapchain = VulkanViewport->GetSwapchain()->GetHandle();
		});
		ASSERT_TRUE(FirstSwapchain);

		ArmVulkanCreateFailure(EVulkanCreateFailurePoint::Swapchain);
		GDynamicRHI->RHIResizeViewport(Viewport, 80, 80, false);
		FlushRenderingCommands();
		GCommandListExecutor.ExecuteSynchronousOperation(false, [&]() {
			ASSERT_NE(VulkanViewport->GetSwapchain(), nullptr);
			EXPECT_EQ(VulkanViewport->GetSwapchain()->GetHandle(), FirstSwapchain);
		});

		ArmVulkanCreateFailure(EVulkanCreateFailurePoint::SwapchainImageView);
		GCommandListExecutor.ExecuteSynchronousOperation(false, [VulkanViewport]() {
			VulkanViewport->RecreateSwapchain();
		});
		GCommandListExecutor.ExecuteSynchronousOperation(false, [VulkanViewport]() {
			EXPECT_EQ(VulkanViewport->GetSwapchain(), nullptr);
			EXPECT_FALSE(VulkanViewport->HasAvailableOutput());
		});

		GCommandListExecutor.ExecuteSynchronousOperation(false, [VulkanViewport]() {
			VulkanViewport->RecreateSwapchain();
		});
		GCommandListExecutor.ExecuteSynchronousOperation(false, [VulkanViewport]() {
			EXPECT_NE(VulkanViewport->GetSwapchain(), nullptr);
			EXPECT_TRUE(VulkanViewport->HasAvailableOutput());
		});

		ArmVulkanCreateFailure(EVulkanCreateFailurePoint::SwapchainSemaphore);
		GCommandListExecutor.ExecuteSynchronousOperation(false, [VulkanViewport]() {
			VulkanViewport->RecreateSwapchain();
			EXPECT_FALSE(VulkanViewport->HasAvailableOutput());
			VulkanViewport->RecreateSwapchain();
			EXPECT_TRUE(VulkanViewport->HasAvailableOutput());
		});

		TRefCountPtr<FRHIViewport> DetachedViewport = GDynamicRHI->RHICreateViewport(
			DetachedWindow->GetOSNativeWindowHandle(), 64, 64, false,
			EPixelFormat::SBGRA8_UNORM,
			EViewportPresentModePolicy::ImGuiDetachedViewport);
		ASSERT_TRUE(DetachedViewport);
		auto* DetachedVulkanViewport = static_cast<FVulkanViewport*>(DetachedViewport.GetReference());
		GCommandListExecutor.ExecuteSynchronousOperation(false, [DetachedVulkanViewport]() {
			DetachedVulkanViewport->RecreateSwapchain();
			EXPECT_TRUE(DetachedVulkanViewport->HasAvailableOutput());
		});
		GDynamicRHI->RHIResizeViewport(DetachedViewport, 72, 72, false);
		FlushRenderingCommands();
		GCommandListExecutor.ExecuteSynchronousOperation(false, [DetachedVulkanViewport]() {
			EXPECT_TRUE(DetachedVulkanViewport->HasAvailableOutput());
		});
		DetachedViewport = nullptr;

		Viewport = nullptr;
		FRHICommandListImmediate::Get().ImmediateFlush(
			EImmediateFlushType::FlushRHIThreadFlushResources);
	}
} // namespace Durin::VulkanRHI
