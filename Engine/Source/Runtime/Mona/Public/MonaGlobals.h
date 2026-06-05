#pragma once

#include "MonaAPI.h"
#include "MonaCoreFwd.h"
namespace Durin::Mona
{
	MONA_API auto MonaInit() -> void;

	MONA_API auto MonaShutdown() -> void;

	MONA_API auto NewFrame() -> void;

	MONA_API auto Render() -> void;
}
