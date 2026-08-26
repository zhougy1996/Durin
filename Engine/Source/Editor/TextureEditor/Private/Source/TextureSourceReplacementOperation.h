#pragma once

#include "Editor/CompensatingAsyncOperation.h"

namespace Durin { class DTexture2D; }

namespace Durin::Editor::Texture
{
	struct FTextureSourceReplacementRequest
	{
		DTexture2D* Texture = nullptr;
		std::string ReplacementPhysicalPath;
		std::string SourceVirtualPath;
		std::function<bool(DTexture2D&, std::string&)> Save;
		std::function<void(bool, std::string_view)> Finished;
	};

	// Adapts mounted-source replacement and Texture2D compilation to DurinEd's generic
	// compensating asynchronous operation.
	auto MakeTextureSourceReplacementOperation(
		FTextureSourceReplacementRequest Request)
		-> FCompensatingAsyncOperation::FOperations;
}
