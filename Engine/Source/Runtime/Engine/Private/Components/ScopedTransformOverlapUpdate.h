#pragma once

#include "DObject/ObjectKey.h"

namespace Durin
{
	class DPrimitiveComponent;

	// Publish every pose in a moving attachment tree before querying its endpoint pairs.
	class FScopedTransformOverlapUpdate
	{
	public:
		FScopedTransformOverlapUpdate();
		~FScopedTransformOverlapUpdate();
		FScopedTransformOverlapUpdate(const FScopedTransformOverlapUpdate&) = delete;
		auto operator=(const FScopedTransformOverlapUpdate&) -> FScopedTransformOverlapUpdate& = delete;
		static auto Defer(DPrimitiveComponent& Component) -> bool;

	private:
		static thread_local FScopedTransformOverlapUpdate* Active;
		std::vector<TObjectKey<DPrimitiveComponent>> Pending;
	};
}
