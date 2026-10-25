#pragma once

#include "CoreMinimal.h"
#include "Materials/MaterialInterface.h"
#include "Modules/ModuleManager.h"
#include "RenderingThread.h"

namespace Durin::Testing
{
	// Inspect the actual published proxy, never a second GameThread representation
	// builder. CPU-only tests own a temporary rendering thread when necessary.
	inline auto CaptureMaterialRenderData(const DMaterialInterface& Material) -> FMaterialRenderData
	{
		const bool OwnsThread = GetRenderCommandAdmissionState() == ERenderCommandAdmissionState::Stopped;
		if (OwnsThread)
		{
			FModuleManager::Get().LoadModule("RenderCore");
			InitRenderingThread();
		}
		struct FThreadScope
		{
			bool Owns;
			~FThreadScope() { if (Owns) ShutdownRenderingThread(); }
		} ThreadScope{OwnsThread};
		const auto Proxy = Material.GetRenderProxy();
		FMaterialRenderData Result;
		EnqueueRenderCommand("CapturePublishedMaterialResource", [Proxy, &Result](FRHICommandListImmediate&) {
			Result = Proxy ? Proxy->GetMaterialWithFallback() : GetErrorMaterialRenderData();
		});
		FlushRenderingCommands();
		return Result;
	}
}
