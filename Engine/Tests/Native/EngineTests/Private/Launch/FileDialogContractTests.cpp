#include "Dialogs/FileDialog.h"

#include <gtest/gtest.h>

TEST(FFileDialogContractTests, NonInteractiveRequestsFailExplicitlyWithoutFabricatingAChoice)
{
	Durin::FFileDialogRequest Request;
	Request.bAllowUserInteraction = false;
	for (const Durin::FFileDialogResult Result : {
		Durin::OpenFileDialog(Request),
		Durin::OpenFolderDialog(Request),
		Durin::SaveFileDialog(Request)})
	{
		EXPECT_EQ(Result.Status, Durin::EFileDialogStatus::Error);
		EXPECT_TRUE(Result.FilePath.empty());
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
