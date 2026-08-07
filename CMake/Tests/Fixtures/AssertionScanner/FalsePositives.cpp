// check(CommentOnly())
const char* Text = "verify(StringOnly())";
const char* RawText = R"fixture(check(RawStringOnly()))fixture";

#define check(expr) ((void)sizeof(expr))

void FalsePositives(bool Ready)
{
	check(Ready);
}
