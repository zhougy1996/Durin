#include "Widgets/MViewport.h"

namespace Durin
{
	auto MViewport::SetDesiredSize(const FVector2f& InDesiredSize) -> void
	{
		DesiredSize = InDesiredSize;
	}

	auto MViewport::GetDesiredSize() const -> FVector2f
	{
		return DesiredSize;
	}

	auto MViewport::SetViewport(const std::shared_ptr<Mona::IMonaViewport>& InViewport) -> void
	{
		Viewport = InViewport;
	}

	auto MViewport::GetViewport() const -> std::shared_ptr<Mona::IMonaViewport>
	{
		return Viewport.lock();
	}

	auto MViewport::Draw() -> void
	{
	}
}
