#pragma once

#include "Misc/Assertion.h"
#include "Misc/Build.h"

#define DURIN_PRIVATE_REQUIRE(expr) \
	do \
	{ \
		if (!(expr)) \
		{ \
			::Durin::Private::ReportAssertionFailure( \
				#expr, std::source_location::current()); \
		} \
	} while (false)

#define DURIN_PRIVATE_REQUIREF(expr, format, ...) \
	do \
	{ \
		if (!(expr)) \
		{ \
			::Durin::Private::ReportFormattedAssertionFailure( \
				#expr, std::source_location::current(), format \
				__VA_OPT__(,) __VA_ARGS__); \
		} \
	} while (false)

#if DO_CHECK
	#define DURIN_PRIVATE_CHECK(expr) \
		do \
		{ \
			if (!(expr)) \
			{ \
				::Durin::Private::ReportAssertionFailure( \
					#expr, std::source_location::current()); \
			} \
		} while (false)

	#define DURIN_PRIVATE_CHECKF(expr, format, ...) \
		do \
		{ \
			if (!(expr)) \
			{ \
				::Durin::Private::ReportFormattedAssertionFailure( \
					#expr, std::source_location::current(), format \
					__VA_OPT__(,) __VA_ARGS__); \
			} \
		} while (false)
#else
	#define DURIN_PRIVATE_CHECK(expr) ((void)0)
	#define DURIN_PRIVATE_CHECKF(expr, format, ...) ((void)0)
#endif

#ifndef require
	#define require(expr) DURIN_PRIVATE_REQUIRE(expr)
#endif

#ifndef requiref
	#define requiref(expr, format, ...) \
		DURIN_PRIVATE_REQUIREF(expr, format __VA_OPT__(,) __VA_ARGS__)
#endif

#ifndef check
	#define check(expr) DURIN_PRIVATE_CHECK(expr)
#endif

#ifndef checkf
	#define checkf(expr, format, ...) \
		DURIN_PRIVATE_CHECKF(expr, format __VA_OPT__(,) __VA_ARGS__)
#endif

#if DO_CHECK
	#define verify(expr) DURIN_PRIVATE_CHECK(expr)
	#define verifyf(expr, format, ...) \
		DURIN_PRIVATE_CHECKF(expr, format __VA_OPT__(,) __VA_ARGS__)
#else
	#define verify(expr) \
		do \
		{ \
			static_cast<void>(expr); \
		} while (false)
	#define verifyf(expr, format, ...) verify(expr)
#endif

#if DURIN_BUILD_DEBUG
	#define checkSlow(expr) DURIN_PRIVATE_CHECK(expr)
	#define checkfSlow(expr, format, ...) \
		DURIN_PRIVATE_CHECKF(expr, format __VA_OPT__(,) __VA_ARGS__)
#else
	#define checkSlow(expr) ((void)0)
	#define checkfSlow(expr, format, ...) ((void)0)
#endif
