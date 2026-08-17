#include <iostream>
#include <string_view>

int main(int ArgumentCount, char** Arguments)
{
	for (int Index = 1; Index < ArgumentCount; ++Index)
	{
		if (std::string_view(Arguments[Index]) == "--gtest_list_tests")
		{
			std::cout << "ProbeSuite.\n  Pass\n";
			return 0;
		}
	}
	return 0;
}
