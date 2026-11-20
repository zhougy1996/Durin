#include "EditorSupportDelegates.h"

#if DURIN_WITH_EDITOR
namespace Durin
{
	TMulticastDelegate<void(DLevel&)> FEditorSupportDelegates::InvalidateHitProxies;
	TMulticastDelegate<void(DLevel&)> FEditorSupportDelegates::ActorHierarchyChanged;
}
#endif
