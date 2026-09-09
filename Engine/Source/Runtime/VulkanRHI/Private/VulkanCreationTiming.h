#pragma once

#include "VulkanRHIAPI.h"
#include "RHICommandList.h"

#if DURIN_VULKAN_TEST_FAILURE_INJECTION
namespace Durin::VulkanRHI
{
	// Stable diagnostic categories shared by the baseline and migrated factories.
	enum class EVulkanCreationKind : uint8
	{
		GraphicsPipeline, ComputePipeline, Shader, Sampler, Buffer, Texture,
		BufferView, TextureView, VertexDeclaration
	};

	// Diagnostic monotonic nanoseconds; zero means the request never reached a boundary.
	// Capture is opt-in and bounded. Configure/drain only after all producers have joined.
	struct FVulkanCreationTiming
	{
		EVulkanCreationKind Kind = EVulkanCreationKind::GraphicsPipeline;
		uint64 RequestId = 0;
		uint64 KeyHash = 0;
		bool bCompute = false;
		bool bSucceeded = false;
		bool bBackground = false;
		uint64 Entry = 0;
		uint64 Scheduled = 0;
		uint64 BodyStart = 0;
		uint64 NativeStart = 0;
		uint64 NativeEnd = 0;
		uint64 BodyEnd = 0;
		uint64 Returned = 0;
		uint64 NormalizationNanoseconds = 0;
		uint64 PreparationNanoseconds = 0;
		uint64 NativeNanoseconds = 0;
		uint64 DependencyNanoseconds = 0;
		FRHISynchronousOperationTiming Scheduling;
	};

	// Change only with no live device; empty restores the normal application path.
	VULKANRHI_API auto SetVulkanPipelineCachePathForTest(std::filesystem::path Path) -> void;

	VULKANRHI_API auto BeginVulkanCreationTimingCapture(size_t Capacity) -> void;
	VULKANRHI_API auto EndVulkanCreationTimingCapture(uint64& Dropped)
		-> std::vector<FVulkanCreationTiming>;
	VULKANRHI_API auto VulkanCreationTimestamp() -> uint64;
	VULKANRHI_API auto GetVulkanDeviceDescriptionForTiming() -> std::string;
	VULKANRHI_API auto GetVulkanDeviceLuidForTiming() -> std::optional<std::array<std::byte, 8>>;
	VULKANRHI_API auto GetActiveVulkanCreationTiming() -> FVulkanCreationTiming*;

	// Each facade or background creator owns its own record until return.
	// Never transfer a caller's borrowed TLS record into an asynchronous task.
	class FVulkanCreationTimingScope
	{
	public:
		VULKANRHI_API explicit FVulkanCreationTimingScope(EVulkanCreationKind Kind);
		explicit FVulkanCreationTimingScope(bool bCompute)
			: FVulkanCreationTimingScope(bCompute ? EVulkanCreationKind::ComputePipeline
				: EVulkanCreationKind::GraphicsPipeline) {}
		VULKANRHI_API ~FVulkanCreationTimingScope();
		FVulkanCreationTimingScope(const FVulkanCreationTimingScope&) = delete;
		auto operator=(const FVulkanCreationTimingScope&) -> FVulkanCreationTimingScope& = delete;
		auto Get() -> FVulkanCreationTiming* { return bEnabled ? &Timing : nullptr; }
	private:
		FVulkanCreationTiming Timing;
		FVulkanCreationTiming* Previous;
		bool bEnabled = false;
	};

	// Propagates the facade record into the replay body without a global active request.
	class FVulkanCreationBodyTimingScope
	{
	public:
		VULKANRHI_API explicit FVulkanCreationBodyTimingScope(FVulkanCreationTiming* Timing);
		VULKANRHI_API ~FVulkanCreationBodyTimingScope();
		FVulkanCreationBodyTimingScope(const FVulkanCreationBodyTimingScope&) = delete;
		auto operator=(const FVulkanCreationBodyTimingScope&) -> FVulkanCreationBodyTimingScope& = delete;
	private:
		FVulkanCreationTiming* Previous;
	};

	// Brackets only the driver call, including exceptional exits.
	class FVulkanNativeCreationTimingScope
	{
	public:
		VULKANRHI_API FVulkanNativeCreationTimingScope();
		VULKANRHI_API ~FVulkanNativeCreationTimingScope();
		FVulkanNativeCreationTimingScope(const FVulkanNativeCreationTimingScope&) = delete;
		auto operator=(const FVulkanNativeCreationTimingScope&) -> FVulkanNativeCreationTimingScope& = delete;
	private:
		uint64 Start = 0;
	};
	// Dependency table lookup and native layout/render-pass work are excluded from PSO compile time.
	class FVulkanCreationDependencyTimingScope
	{
	public:
		VULKANRHI_API FVulkanCreationDependencyTimingScope();
		VULKANRHI_API ~FVulkanCreationDependencyTimingScope();
		FVulkanCreationDependencyTimingScope(const FVulkanCreationDependencyTimingScope&) = delete;
		auto operator=(const FVulkanCreationDependencyTimingScope&)
			-> FVulkanCreationDependencyTimingScope& = delete;
	private:
		uint64 Start = 0;
	};
	// Measures CPU normalization or native-info preparation, excluding nested driver/dependency spans.
	class FVulkanCreationCpuTimingScope
	{
	public:
		VULKANRHI_API explicit FVulkanCreationCpuTimingScope(bool bNormalization);
		VULKANRHI_API ~FVulkanCreationCpuTimingScope();
		VULKANRHI_API auto Finish() -> void;
		FVulkanCreationCpuTimingScope(const FVulkanCreationCpuTimingScope&) = delete;
		auto operator=(const FVulkanCreationCpuTimingScope&) -> FVulkanCreationCpuTimingScope& = delete;
	private:
		uint64 Start = 0;
		uint64 ExcludedBefore = 0;
		bool bNormalization;
	};

}
#endif
