#include "RHIGlobals.h"

#include "DynamicRHI.h"
#include "RHICommandList.h"

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
		// The command list exists before the backend; bind its default pipeline only after the context is valid.
		FRHICommandListImmediate::Get().SwitchPipeline(ERHIPipeline::Graphics);
		DURIN_DEBUG("RHI initialized successfully");
	}

	auto RHIExit() -> void
	{
		check(GDynamicRHI);
		FRHICommandListImmediate::Get().SwitchPipeline(ERHIPipeline::None);
		GDynamicRHI->Shutdown();
		delete GDynamicRHI;
		GDynamicRHI = nullptr;
	}
}
