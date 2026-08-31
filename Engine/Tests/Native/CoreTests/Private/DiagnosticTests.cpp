#include "Diagnostics/Diagnostic.h"

#include <gtest/gtest.h>

TEST(DiagnosticTests, OwnsDomainCodeMessageAndContext)
{
	Durin::FDiagnostic Diagnostic{
		.Domain = "TestDomain",
		.Code = "Rejected",
		.Severity = Durin::EDiagnosticSeverity::Warning,
		.Message = "The request was rejected.",
		.Context = "/Test/Asset"};

	EXPECT_EQ(Diagnostic.Domain, "TestDomain");
	EXPECT_EQ(Diagnostic.Code, "Rejected");
	EXPECT_FALSE(Diagnostic.IsError());
	EXPECT_EQ(Diagnostic.Message, "The request was rejected.");
	EXPECT_EQ(Diagnostic.Context, "/Test/Asset");
}

TEST(DiagnosticTests, DefaultsToErrorSeverity)
{
	const Durin::FDiagnostic Diagnostic;
	EXPECT_TRUE(Diagnostic.IsError());
}
