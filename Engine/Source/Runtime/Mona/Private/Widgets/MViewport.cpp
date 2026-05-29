#include "Widgets/MViewport.h"

#include "ThirdParty/ImGui/imgui.h"

namespace Durin::Mona
{
	auto MViewport::SetDesiredSize(const FVector2f& InDesiredSize) -> void
	{
		DesiredSize = InDesiredSize;
	}

	auto MViewport::GetDesiredSize() const -> FVector2f
	{
		return DesiredSize;
	}

	auto MViewport::SetViewport(const std::shared_ptr<IMonaViewport>& InViewport) -> void
	{
		Viewport = InViewport;
	}

	auto MViewport::GetViewport() const -> std::shared_ptr<IMonaViewport>
	{
		return Viewport.lock();
	}

	auto MViewport::Draw() -> void
	{
		const FVector2f Size = GetDesiredSize();
		ImGui::BeginChild(
			"DurinSceneViewportPlaceholder",
			ImVec2(Size.x, Size.y),
			ImGuiChildFlags_Borders,
			ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
		);
		ImGui::TextUnformatted("Scene-to-texture viewport is not implemented yet.");
		ImGui::Text("Target size: %.0f x %.0f", Size.x, Size.y);
		ImGui::EndChild();
	}
}
