// Function stubs to make the librarian happy.
// https://qualapps.blogspot.md/2007/08/how-to-create-32-bit-import-libraries.html
// https://support.microsoft.com/en-us/help/131313/how-to-create-32-bit-import-libraries-without-.objs-or-source

__declspec(dllexport) void _stdcall RtlImageNtHeader(int a) {}
__declspec(dllexport) void _stdcall NtQuerySystemInformation(int a, int b, int c, int d) {}
__declspec(dllexport) void _stdcall NtQueryInformationThread(int a, int b, int c, int d, int e) {}
__declspec(dllexport) void _stdcall RtlImageRvaToVa(int a, int b, int c, int d) {}
__declspec(dllexport) void _stdcall RtlImageDirectoryEntryToData(int a, int b, int c, int d) {}
