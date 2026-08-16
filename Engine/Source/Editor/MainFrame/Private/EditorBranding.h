#pragma once

class ImDrawList;
struct ImVec2;

namespace Durin
{
	class FRHITexture;
}

namespace Durin::Editor::MainFrame
{
	// Owns the shared GPU texture built from the editor branding source image.
	class FEditorBrandTexture
	{
	public:
		FEditorBrandTexture();
		~FEditorBrandTexture();

		auto Load(std::string& OutError) -> bool;
		auto UpdateAndGetTexture() -> const FRHITexture*;

	private:
		struct FImpl;
		std::unique_ptr<FImpl> Impl;
	};

	auto DrawEditorBrandMark(
		ImDrawList* DrawList,
		const FRHITexture* Texture,
		const ImVec2& Min,
		float Size) -> void;
}
