bool TryPerformIgnorableWork();

void PresubmitAcceptsClassifiedVerify()
{
	verify(TryPerformIgnorableWork());
}
