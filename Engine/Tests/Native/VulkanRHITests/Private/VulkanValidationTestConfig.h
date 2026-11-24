#pragma once

#include "CoreMinimal.h"
#include "Misc/AppConfig.h"
#include "NativeTestSupport.h"
#include <gtest/gtest.h>

namespace Durin::VulkanRHI
{
	// These standalone test hosts start without application configuration.
	class FScopedVulkanValidationTestConfig
	{
	public:
		FScopedVulkanValidationTestConfig() = default;
		FScopedVulkanValidationTestConfig(const FScopedVulkanValidationTestConfig&) = delete;
		auto operator=(const FScopedVulkanValidationTestConfig&) -> FScopedVulkanValidationTestConfig& = delete;

		~FScopedVulkanValidationTestConfig()
		{
			if (!bPublished) return;
			std::string Error;
			EXPECT_TRUE(LoadAppConfigLayers({}, Error)) << Error;
		}

		auto Set(std::string_view Mode) -> testing::AssertionResult
		{
			const auto Path = Testing::GetTestWorkDirectory() / "VulkanValidation.yaml";
			std::ofstream File(Path);
			if (Mode.empty()) File << "{}\n";
			else File << "VulkanRHI:\n  Validation: " << Mode << '\n';
			File.close();
			if (!File) return testing::AssertionFailure() << "Could not write " << Path;

			std::string Error;
			if (!LoadAppConfigLayers(std::array{Path}, Error))
				return testing::AssertionFailure() << Error;
			bPublished = true;
			return testing::AssertionSuccess();
		}

	private:
		bool bPublished = false;
	};
}
