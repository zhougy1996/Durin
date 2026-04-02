#pragma once

#include "RHIResources.h"
#include "Rendering/MonaRenderer.h"

namespace Doge::Mona
{
	class MWindow;

	struct FMonaViewportInfo
	{
		TRefCountPtr<FRHIViewport> ViewportRHI;
		bool bFullScreen;
	};


	class MONACORE_API FMonaRHIRenderer : public FMonaRenderer
	{
	public:
		~FMonaRHIRenderer() override;

		auto CreateViewport(const std::shared_ptr<MWindow>& Window) -> void override;
		auto RequestResize(const std::shared_ptr<MWindow>& Window, uint32 Width, uint32 Height) -> void override;
		auto DrawWindows() -> void override;
		auto OnWindowDestroyed(const std::shared_ptr<MWindow>& Window) -> void override;

		auto GetRHIViewport(const MWindow& Window) -> TRefCountPtr<FRHIViewport>;

		std::unordered_map<const MWindow*, FMonaViewportInfo*> WindowToViewportInfoMap;
	};
}