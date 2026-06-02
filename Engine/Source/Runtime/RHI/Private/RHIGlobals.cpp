#include "RHIGlobals.h"

#include "DynamicRHI.h"

namespace Durin
{
	static auto CreateDynamicRHI() -> FDynamicRHI*
	{
		IDynamicRHIModule* DynamicRHIModule = FModuleManager::LoadModule<IDynamicRHIModule>("VulkanRHI");
		if (!DynamicRHIModule)
		{
			DURIN_ERROR("Failed to load VulkanRHI module");
			return nullptr;
		}
		return DynamicRHIModule->CreateRHI();
	}

	auto RHIInit() -> void
	{
		GDynamicRHI = CreateDynamicRHI();
		if (GDynamicRHI == nullptr)
		{
			DURIN_ERROR("Failed to create dynamic RHI");
			return;
		}
		GDynamicRHI->Init();
		DURIN_DEBUG("RHI initialized successfully");
	}

	auto RHIExit() -> void
	{
		check(GDynamicRHI);
		GDynamicRHI->Shutdown();
		delete GDynamicRHI;
		GDynamicRHI = nullptr;
	}
}
