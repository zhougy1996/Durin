#pragma once

#include "MonaAPI.h"
#include "Rendering/RenderingCommon.h"
#include "Widgets/MWidget.h"

namespace Durin
{
	class MViewport : public MWidget
	{
	public:
		MViewport() = default;
		~MViewport() override = default;

		MONA_API auto Draw() -> void override;

		MONA_API auto SetDesiredSize(const FVector2f& InDesiredSize) -> void;

		MONA_API auto GetDesiredSize() const -> FVector2f;

		MONA_API auto SetViewport(const std::shared_ptr<Mona::IMonaViewport>& InViewport) -> void;

		MONA_API auto GetViewport() const -> std::shared_ptr<Mona::IMonaViewport>;

	private:
		FVector2f DesiredSize = {640.0f, 360.0f};

		std::weak_ptr<Mona::IMonaViewport> Viewport;
	};
}
