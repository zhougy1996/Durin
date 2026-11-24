#pragma once

#include "DObject/Package.h"

namespace Durin::Testing
{
	struct FScopedPackageNotificationCount
	{
		using FEvent = TMulticastDelegate<void(DPackage&)>;
		FScopedPackageNotificationCount(FEvent& InEvent, DPackage* Expected)
			: Event(InEvent), Handle(Event.AddLambda([this, Expected](DPackage& Package) {
				if (&Package == Expected) ++Count;
			})) {}
		~FScopedPackageNotificationCount() { Event.Remove(Handle); }
		FScopedPackageNotificationCount(const FScopedPackageNotificationCount&) = delete;
		auto operator=(const FScopedPackageNotificationCount&) -> FScopedPackageNotificationCount& = delete;

		int Count = 0;
	private:
		FEvent& Event;
		FDelegateHandle Handle;
	};
}
