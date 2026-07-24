#pragma once

#include "MonaAPI.h"
#include "Rendering/RenderingCommon.h"
#include "Widgets/MWidget.h"

namespace Durin
{
	// Presents an engine viewport texture inside the Mona widget tree.
	class MViewport : public MWidget
	{
	public:
		MViewport() = default;
		~MViewport() override = default;

		MONA_API auto Draw() -> void override;

		MONA_API auto SetDesiredSize(const FVector2f& InDesiredSize) -> void;

		MONA_API auto GetDesiredSize() const -> FVector2f;

		MONA_API auto SetViewportInterface(const std::shared_ptr<Mona::IMonaViewport>& InViewport) -> void;

		MONA_API auto GetViewportInterface() const -> std::shared_ptr<Mona::IMonaViewport>;

		MONA_API auto WasTextureDrawn() const -> bool;

	private:
		// Logical widget size requested from the layout system.
		FVector2f DesiredSize = {640.0f, 360.0f};

		// The widget observes the viewport without extending its runtime lifetime.
		std::weak_ptr<Mona::IMonaViewport> ViewportInterface;

		// Records whether the latest draw submitted a valid viewport texture.
		bool bLastDrawSucceeded = false;
	};
}
