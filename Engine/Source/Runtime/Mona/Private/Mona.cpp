#include "Mona.h"

#include "ThirdParty/ImGui/ImGuiCommon.h"

namespace Doge::Mona
{
	auto Text(const FString& InText) -> void
	{
		ImGui::Text("Test text {%s}", InText.c_str());
	}
}