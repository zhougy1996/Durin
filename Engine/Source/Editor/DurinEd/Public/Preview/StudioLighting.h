#pragma once
#include "DurinEdAPI.h"

namespace Durin
{
    class DLevel;
    namespace Editor
    {
        // Authors a normal component reference. Used by new levels and preview scenes.
        DURINED_API auto AddStudioSkyLight(DLevel& Level) -> bool;
    }
}
