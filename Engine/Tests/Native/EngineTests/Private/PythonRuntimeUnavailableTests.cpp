#include "PythonRuntime.h"
#include "NativeDObjectTestSupport.h"
#include <gtest/gtest.h>

TEST(FPythonRuntimeUnavailableTests, KeepsNativeEditorHostsUsableWithoutAnInterpreter)
{
    Durin::Testing::InitializeDObjectSystemForTests();
    Durin::FPythonRuntime Runtime;
    auto Started = Runtime.Start();
    ASSERT_FALSE(Started);
    EXPECT_NE(std::string::npos, Started.error().find("unavailable"));
    EXPECT_FALSE(Runtime.GetJobResult(1));
    EXPECT_FALSE(Runtime.CancelJob(1));
    Runtime.Stop(); Runtime.Stop();
}
