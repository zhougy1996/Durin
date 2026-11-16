#include "Dialogs/FileDialog.h"
#include "Dialogs/MessageDialog.h"

#include <gtest/gtest.h>

TEST(FFileDialogContractTests, NonInteractiveRequestsFailExplicitlyWithoutFabricatingAChoice)
{
	Durin::FFileDialogRequest Request;
	Request.bAllowUserInteraction = false;
	Request.bAllowMultiple = true;
	for (const Durin::FFileDialogResult Result : {
		Durin::OpenFileDialog(Request),
		Durin::OpenFolderDialog(Request),
		Durin::SaveFileDialog(Request)})
	{
		EXPECT_EQ(Result.Status, Durin::EFileDialogStatus::Error);
		EXPECT_TRUE(Result.FilePath.empty());
		EXPECT_TRUE(Result.FilePaths.empty());
		EXPECT_NE(Result.ErrorMessage.find("disabled"), std::string::npos);
	}
}

#if defined(__APPLE__)
TEST(FFileDialogContractTests, InteractiveMacOSRequestsRejectWorkerThreads)
{
	Durin::FFileDialogResult Result;
	std::thread Worker([&] { Result = Durin::OpenFileDialog({}); });
	Worker.join();
	EXPECT_EQ(Result.Status, Durin::EFileDialogStatus::Error);
	EXPECT_TRUE(Result.FilePath.empty());
	EXPECT_NE(Result.ErrorMessage.find("main thread"), std::string::npos);
}
#endif

TEST(FMessageDialogContractTests, DisabledInteractionNeverReturnsConsent)
{
	EXPECT_EQ(Durin::ShowMessageDialog("Save", "Save selected packages?", true, false), Durin::EMessageDialogResult::Error);
	EXPECT_EQ(Durin::ShowMessageDialog("Reload", "Discard unsaved changes?", false, false), Durin::EMessageDialogResult::Error);
}
