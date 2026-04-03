#include "Mona.h"

#include "ThirdParty/ImGui/ImGuiCommon.h"

namespace Doge::Mona
{
	auto Text(const std::string& InText) -> void
	{
		ImGui::Text("Test text {%s}", InText.c_str());
	}
}