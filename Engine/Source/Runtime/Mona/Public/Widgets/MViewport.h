#pragma once

#include "Mona/API.h"
#include "Widgets/MWidget.h"

namespace Doge::Mona
{
	class MONA_API MViewport : public MWidget
	{
	public:
		MViewport() = default;
		virtual ~MViewport() = default;
	};
}