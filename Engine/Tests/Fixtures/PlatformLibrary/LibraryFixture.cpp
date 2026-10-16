#if defined(_WIN32)
#define FIXTURE_EXPORT __declspec(dllexport)
#define FIXTURE_IMPORT __declspec(dllimport)
#else
#define FIXTURE_EXPORT __attribute__((visibility("default")))
#define FIXTURE_IMPORT
#endif

extern "C" FIXTURE_IMPORT int DurinLibraryDependencyValue();
extern "C"
{
	FIXTURE_EXPORT int DurinLibraryFixtureData = 73;
	FIXTURE_EXPORT int DurinLibraryFixtureValue()
	{
		return DurinLibraryDependencyValue() + 1;
	}
}
