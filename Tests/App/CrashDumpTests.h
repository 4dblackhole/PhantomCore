#pragma once
int TestCrashDump();
// The child half of TestCrashDump: installs the service in the parent's test
// folder and crashes in the named way.
int RunCrashDumpChild(const char *kind, const char *parentProcessId);
