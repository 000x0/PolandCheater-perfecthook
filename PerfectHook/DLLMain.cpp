// General shit
#include "DLLMain.h"
#include "Utilities.h"

#include <iostream>
#include <chrono>
#include <ratio>
#include <thread>

// Stuff to initialise
#include "Offsets.h"
#include "Interfaces.h"
#include "Hooks.h"
#include "RenderManager.h"
#include "Hacks.h"

#include "recvproxy.h"
#include "MiscHacks.h"



bool DoUnload;

// Our thread we use to setup everything we need
// Everything appart from code in hooks get's called from inside 
// here.

int wait_for_modules(std::int32_t timeout, std::initializer_list<std::wstring> modules)
{
    bool signaled[32] = { 0 };
    bool success = false;

    std::uint32_t totalSlept = 0;

    if (timeout == 0) {
        for (auto& mod : modules) {
            if (GetModuleHandleW(std::data(mod)) == NULL)
                return WAIT_TIMEOUT;
        }
        return WAIT_OBJECT_0;
    }

    if (timeout < 0)
        timeout = INT32_MAX;

    while (true) {
        for (auto i = 0u; i < modules.size(); ++i) {
            auto& module = *(modules.begin() + i);
            if (!signaled[i] && GetModuleHandleW(std::data(module)) != NULL) {
                signaled[i] = true;

                //
                // Checks if all modules are signaled
                //
                bool done = true;
                for (auto j = 0u; j < modules.size(); ++j) {
                    if (!signaled[j]) {
                        done = false;
                        break;
                    }
                }
                if (done) {
                    success = true;
                    goto exit;
                }
            }
        }
        if (totalSlept > std::uint32_t(timeout)) {
            break;
        }
        Sleep(10);
        totalSlept += 10;
    }

exit:
    return success ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
}

bool unhook = true;

bool on_dll_attach(void* base)
{



    if (wait_for_modules(10000, { L"client.dll", L"engine.dll", L"shaderapidx9.dll" }) == WAIT_TIMEOUT) {
        // One or more modules were not loaded in time
        return FALSE;
    }




    Offsets::Initialise();
    I::Initialise();
    NetVarManager->Initialize();


    Offsetz::GetOffsets();
    Render::Initialise();
    Hacks::SetupHacks();
    Hooks::Hook();
    NetvarHook();
    AnimationFixHook();






    while (DoUnload == false)
    {
        Sleep(1000);
    }


    AnimationFixUnhook();
    UnloadProxy();
    Hooks::Unhook();

    Sleep(2000);
    FreeLibraryAndExitThread((HMODULE)base, 0);
}






bool on_dll_detach()
{
    if (unhook)
    {
        AnimationFixUnhook();
        UnloadProxy();
        Hooks::Unhook();
    }
    return 1;
}

BOOL WINAPI DllMain(
    _In_      HINSTANCE hinstDll,
    _In_      DWORD     fdwReason,
    _In_opt_	LPVOID    lpvReserved
)
{
    switch (fdwReason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hinstDll);
        CreateThread(nullptr, 0, (LPTHREAD_START_ROUTINE)on_dll_attach, hinstDll, 0, nullptr);
        break;
    case DLL_PROCESS_DETACH:
        if (lpvReserved == nullptr)
            return on_dll_detach();
    }
    return TRUE;
}