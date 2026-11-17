#pragma once

#include "EngineSharedPCH.h"

// DurinEd owns this inventory. Its own sources still use SharedPCH_Engine so
// editor exports are never precompiled with a consumer's import definitions.
#include "DurinEd.h"
