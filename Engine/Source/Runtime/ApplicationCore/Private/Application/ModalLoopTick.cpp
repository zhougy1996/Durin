#include "Application/ModalLoopTick.h"

namespace Durin
{
	namespace
	{
		FModalLoopTickCallback GModalLoopTickCallback = nullptr;
	}

	auto SetModalLoopTickCallback(FModalLoopTickCallback Callback) -> void
	{
		GModalLoopTickCallback = Callback;
	}

	auto RequestModalLoopTick(EModalLoopTickMode Mode) -> void
	{
		if (GModalLoopTickCallback != nullptr)
		{
			GModalLoopTickCallback(Mode);
		}
	}
}
