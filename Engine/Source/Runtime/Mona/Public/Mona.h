#pragma once

#include "MonaCore.h"
#include "MonaGlobals.h"
#include "Application/MonaApplication.h"
#include "Rendering/MonaRenderer.h"
#include "Rendering/MonaRHIRenderer.h"
#include "Widgets/MFunctionWidget.h"
#include "Widgets/MViewport.h"
#include "Widgets/MWindow.h"

namespace Durin::Mona
{
	MONA_API auto Text(const std::string& InText) -> void;

} // namespace Durin::Mona
