#pragma once

#include "Renderers/GBufferRenderer.h"

namespace Durin
{
	enum class EScenePassStatus : uint8
	{
		NotRequested,
		Complete,
		Failed
	};

	struct FGBufferPassResult
	{
		FGBufferRenderer::FTargets* Targets = nullptr;
		EScenePassStatus Status = EScenePassStatus::NotRequested;
		bool bRenderedGeometry = false;

		[[nodiscard]] auto IsComplete() const -> bool
		{
			return Status == EScenePassStatus::Complete && Targets != nullptr;
		}
	};
} // namespace Durin
