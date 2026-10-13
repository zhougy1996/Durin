#pragma once

#include "CoreMinimal.h"
#include "DynamicRHI.h"
#include "Modules/ModuleManager.h"
#include "RHIGlobals.h"
#include <gtest/gtest.h>

namespace Durin::VulkanRHI
{
	inline auto ExpectVulkanModuleUnloaded() -> void
	{
		if (FModuleManager::Get().IsModuleLoaded("VulkanRHI"))
			EXPECT_TRUE(FModuleManager::Get().UnloadModule("VulkanRHI"));
	}

	class FVulkanIntegrationTest : public testing::Test
	{
	protected:
		auto SetUp() -> void override
		{
			if (const char* ExistingMode = std::getenv("DURIN_RHI_EXECUTION"))
			{
				PreviousExecutionMode = ExistingMode;
			}
			if (const char* ExistingValidation =
				std::getenv("DURIN_VULKAN_VALIDATION"))
			{
				PreviousValidationMode = ExistingValidation;
			}
			if (const char* Existing = std::getenv("DURIN_VULKAN_FULL_DESCRIPTOR_VALIDATION"))
				PreviousDescriptorValidationMode = Existing;
			_putenv_s("DURIN_VULKAN_FULL_DESCRIPTOR_VALIDATION", "off");
			_putenv_s("DURIN_RHI_EXECUTION", "threaded");
		}

		auto TearDown() -> void override
		{
			if (GDynamicRHI)
			{
				RHIExit();
			}
			ExpectVulkanModuleUnloaded();
			_putenv_s("DURIN_RHI_EXECUTION",
				PreviousExecutionMode ? PreviousExecutionMode->c_str() : "");
			_putenv_s("DURIN_VULKAN_VALIDATION",
				PreviousValidationMode ? PreviousValidationMode->c_str() : "");
			_putenv_s("DURIN_VULKAN_FULL_DESCRIPTOR_VALIDATION",
				PreviousDescriptorValidationMode ? PreviousDescriptorValidationMode->c_str() : "");
		}

		std::optional<std::string> PreviousExecutionMode;
		std::optional<std::string> PreviousValidationMode;
		std::optional<std::string> PreviousDescriptorValidationMode;
	};
}
