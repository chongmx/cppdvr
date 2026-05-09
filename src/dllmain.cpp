/**
 * dllmain.cpp — Windows DLL entry point
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

BOOL WINAPI DllMain(HINSTANCE /*hInst*/, DWORD reason, LPVOID /*reserved*/) {
    // Winsock is initialised by the WsaGuard global in dvrip.cpp.
    (void)reason;
    return TRUE;
}
