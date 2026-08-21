#include "SceneViewState.h"

namespace Durin
{
	FSceneViewStateOwner::FSceneViewStateOwner(
		FSceneViewStateOwner&& Other) noexcept
		: Id(Other.Id), ReleaseViewState(Other.ReleaseViewState)
	{
		Other.Id = {};
		Other.ReleaseViewState = nullptr;
	}

	auto FSceneViewStateOwner::operator=(
		FSceneViewStateOwner&& Other) noexcept -> FSceneViewStateOwner&
	{
		if (this == &Other)
			return *this;
		Reset();
		Id = Other.Id;
		ReleaseViewState = Other.ReleaseViewState;
		Other.Id = {};
		Other.ReleaseViewState = nullptr;
		return *this;
	}

	FSceneViewStateOwner::~FSceneViewStateOwner()
	{
		Reset();
	}

	auto FSceneViewStateOwner::Reset() -> void
	{
		if (Id.IsValid())
		{
			checkf(ReleaseViewState != nullptr,
				"A valid view-state owner must carry its renderer release policy.");
			ReleaseViewState(Id);
		}
		Id = {};
		ReleaseViewState = nullptr;
	}
}
