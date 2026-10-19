#include "PythonRuntime.h"
#include "NativeDObjectTestSupport.h"
#include "Editor/EditorEngineTestSupport.h"
#include "Tools/EditorToolSubsystem.h"
#include "Modules/ModuleManager.h"
#include "DObject/StrongObjectPtr.h"
#include <gtest/gtest.h>

TEST(FPythonRuntimeUnavailableTests, KeepsNativeEditorHostsUsableWithoutAnInterpreter)
{
    Durin::Testing::InitializeDObjectSystemForTests();
    ASSERT_TRUE(Durin::FModuleManager::Get().LoadModule("Engine"));
    ASSERT_TRUE(Durin::FModuleManager::Get().LoadModule("DurinEd"));
    ASSERT_TRUE(Durin::FModuleManager::Get().LoadModule("DurinPython"));
    Durin::TStrongObjectPtr<Durin::DEditorEngine> Host(Durin::NewObject<Durin::DEditorEngine>(nullptr, {}));
    ASSERT_TRUE(Durin::FEditorEngineTestAccess::StartEditor(*Host));
    EXPECT_NE(Host->GetEditorSubsystem<Durin::DEditorToolSubsystem>(), nullptr);
    Durin::FPythonRuntime Runtime;
    auto Started = Runtime.Start();
    ASSERT_FALSE(Started);
    EXPECT_NE(std::string::npos, Started.error().find("unavailable"));
    EXPECT_FALSE(Runtime.GetJobResult(1));
    EXPECT_FALSE(Runtime.CancelJob(1));
    Runtime.Stop(); Runtime.Stop();
    Host->PrepareForShutdown();
    Durin::MarkObjectHierarchyAsGarbage(Host.Get()); Host.Reset(); Durin::CollectGarbage();
}
