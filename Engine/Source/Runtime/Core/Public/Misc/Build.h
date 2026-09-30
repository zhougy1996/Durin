#pragma once

#ifndef DURIN_WITH_EDITOR
	#error DURIN_WITH_EDITOR must be supplied by the runtime variant.
#endif
#ifndef DURIN_WITH_EDITORONLY_DATA
	#error DURIN_WITH_EDITORONLY_DATA must be supplied by the runtime variant.
#endif
#if (DURIN_WITH_EDITOR != 0 && DURIN_WITH_EDITOR != 1) || (DURIN_WITH_EDITORONLY_DATA != 0 && DURIN_WITH_EDITORONLY_DATA != 1)
	#error Durin editor feature macros must be either 0 or 1.
#endif
#if DURIN_WITH_EDITOR && !DURIN_WITH_EDITORONLY_DATA
	#error Editor behavior requires editor-only data.
#endif

#ifndef DURIN_BUILD_DEBUG
	#define DURIN_BUILD_DEBUG 0
#endif

#ifndef DURIN_BUILD_RELEASE
	#define DURIN_BUILD_RELEASE 0
#endif

#ifndef DURIN_BUILD_SHIPPING
	#define DURIN_BUILD_SHIPPING 0
#endif

#if (DURIN_BUILD_DEBUG + DURIN_BUILD_RELEASE + DURIN_BUILD_SHIPPING) != 1
	#error Exactly one Durin build configuration must be active.
#endif

#ifdef DO_CHECK
	#undef DO_CHECK
#endif

#if DURIN_BUILD_DEBUG || DURIN_BUILD_RELEASE
	#define DO_CHECK 1
#else
	#define DO_CHECK 0
#endif

#if DURIN_BUILD_DEBUG
	#define DURIN_BUILD_TYPE_STRING "Debug"
#elif DURIN_BUILD_RELEASE
	#define DURIN_BUILD_TYPE_STRING "Release"
#else
	#define DURIN_BUILD_TYPE_STRING "Shipping"
#endif

#if DURIN_BUILD_DEBUG
	#define DURIN_VISUALIZERS_HELPERS
#endif
