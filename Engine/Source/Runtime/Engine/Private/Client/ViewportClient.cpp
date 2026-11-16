#include "Client/ViewportClient.h"
#include "Materials/MaterialInterface.h"

namespace Durin
{
	FViewportClient::~FViewportClient() = default;

	auto FViewportClient::SetPostProcessMaterial(DMaterialInterface* Material, FMaterialRenderProxyRef Proxy) -> void
	{
		PostProcessMaterial = Material;
		if (!Proxy && Material)
		{
			Material->CacheResourceShadersForRendering();
			Proxy = Material->GetRenderProxy();
		}
		auto* Pointer = Proxy.GetReference();
		ViewSettings.PostProcess.Material = Proxy ? std::shared_ptr<FMaterialRenderProxy>(Pointer,
			[Retained = std::move(Proxy)](FMaterialRenderProxy*) mutable { Retained = {}; }) : nullptr;
	}

	auto FViewportClient::CalcSceneView(uint32 Width, uint32 Height, FSceneView& OutView) const -> bool
	{
		return false;
	}
}
