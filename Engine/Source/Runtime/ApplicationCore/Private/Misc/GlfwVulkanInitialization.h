#pragma once

namespace Durin
{
	// Owns the result of one GLFW Vulkan-extension discovery attempt.
	struct FGlfwVulkanExtensionQueryResult
	{
		std::vector<const char*> Extensions;
		std::string Diagnostic;

		auto Succeeded() const -> bool { return Diagnostic.empty(); }
	};

	// Validates GLFW's borrowed extension array before any pointer range is formed.
	template <typename FExtensionQuery, typename FErrorQuery>
	auto QueryRequiredGlfwVulkanInstanceExtensions(
		FExtensionQuery&& ExtensionQuery,
		FErrorQuery&& ErrorQuery) -> FGlfwVulkanExtensionQueryResult
	{
		uint32_t ExtensionCount = 0;
		const char** Extensions = ExtensionQuery(&ExtensionCount);
		if (!Extensions || ExtensionCount == 0)
		{
			const char* Description = nullptr;
			const int Error = ErrorQuery(&Description);
			return {
				.Extensions = {},
				.Diagnostic = std::format(
					"GLFW Vulkan instance-extension discovery failed ({}): {}.",
					Error,
					Description ? Description : "No native diagnostic was provided")};
		}

		FGlfwVulkanExtensionQueryResult Result;
		Result.Extensions.reserve(ExtensionCount);
		for (uint32_t Index = 0; Index < ExtensionCount; ++Index)
		{
			if (!Extensions[Index] || Extensions[Index][0] == '\0')
			{
				Result.Extensions.clear();
				Result.Diagnostic = std::format(
					"GLFW Vulkan instance-extension discovery returned an invalid name at index {}.",
					Index);
				return Result;
			}
			Result.Extensions.push_back(Extensions[Index]);
		}
		return Result;
	}
}
