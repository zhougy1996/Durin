#include "RHIGlobals.h"

#include "DynamicRHI.h"

namespace Doge
{
	static auto CreateDynamicRHI() -> FDynamicRHI*
	{
		IDynamicRHIModule* DynamicRHIModule = FModuleManager::LoadModule<IDynamicRHIModule>("VulkanRHI");
		if (!DynamicRHIModule)
		{
			DOGE_ERROR("Failed to load VulkanRHI module");
			return nullptr;
		}
		return DynamicRHIModule->CreateRHI();
	}

	auto RHIInit() -> void
	{
		GDynamicRHI = CreateDynamicRHI();
		if (GDynamicRHI == nullptr)
		{
			DOGE_ERROR("Failed to create dynamic RHI");
			return;
		}
		GDynamicRHI->Init();
		DOGE_DEBUG("RHI initialized successfully");
	}

	auto RHIExit() -> void
	{
		GDynamicRHI->Shutdown();
		delete GDynamicRHI;
		GDynamicRHI = nullptr;
	}
}