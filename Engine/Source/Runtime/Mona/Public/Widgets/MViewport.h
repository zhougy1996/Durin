#pragma once

#include "MonaAPI.h"
#include "Widgets/MWidget.h"

namespace Durin::Mona
{
	class MONA_API MViewport : public MWidget
	{
	public:
		MViewport() = default;
		virtual ~MViewport() = default;
	};
}