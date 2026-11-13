/* Static loader bridging the original Xash3D library API to the original
 * built-in MainUI and GS renderer. Dynamic ELF SOs are not supported
 * on EE; the existing engine still calls COM_LoadLibrary/GetProcAddress. */
#include "common.h"
#include "library.h"
#include "render_api.h"
#include <string.h>

extern int GetMenuAPI(void *, void *, void *);
extern int GetExtAPI(int, void *, void *);
extern int GetRefAPI(int, void *, void *, void *);
extern void GetRefHumanReadableName(char *, size_t);

/* The client and server are the actual cs16-client-main/ReGameDLL exports.
 * Weak references permit the core/menu ELF to link before the two archives
 * are ready; missing game code is explicitly rejected at runtime. */
extern void GetClientAPI(void *) __attribute__((weak));
/* Xash's original client extension must be exported even in static PS2
 * builds. Without it, the original HUD has a null GL_LoadTexture callback. */
extern int HUD_GetRenderInterface(int, render_api_t *, render_interface_t *) __attribute__((weak));
extern void GetEntityAPI(void *, int) __attribute__((weak));
extern void GetEntityAPI2(void *, int *) __attribute__((weak));
extern void GetNewDLLFunctions(void *, int *) __attribute__((weak));
extern void GiveFnptrsToDll(void *, void *) __attribute__((weak));
extern void Server_GetBlendingInterface(void) __attribute__((weak));
extern void Server_SplitAimAssist(int, float *, float, float) __attribute__((weak));
/* The linker generates a table of the real ReGameDLL LINK_ENTITY_TO_CLASS
 * exports when the server archive is included. Keep partial ELFs linkable. */
extern void *PS2_ServerEntityByName(const char *name) __attribute__((weak));

static int menu_marker, gs_marker, client_marker, server_marker;

void *COM_LoadLibrary(const char *filename, int ordinals, qboolean directpath)
{
    (void)ordinals; (void)directpath;
    if (!filename) return NULL;
    if (strstr(filename, "menu")) return &menu_marker;
    if (strstr(filename, "ref_gs")) return &gs_marker;
    if (strstr(filename, "client") && GetClientAPI) return &client_marker;
    if (strstr(filename, "server") && GiveFnptrsToDll &&
        (GetEntityAPI || GetEntityAPI2)) return &server_marker;
    COM_PushLibraryError("PS2: original Counter-Strike game library is not linked yet");
    return NULL;
}

void COM_FreeLibrary(void *handle) { (void)handle; }

void *COM_GetProcAddress(void *handle, const char *name)
{
    if (!name) return NULL;
    if (handle == &menu_marker) {
        if (strcmp(name, "GetMenuAPI") == 0) return (void *)GetMenuAPI;
        if (strcmp(name, "GetExtAPI") == 0) return (void *)GetExtAPI;
    }
    if (handle == &gs_marker) {
        if (strcmp(name, "GetRefAPI") == 0) return (void *)GetRefAPI;
        if (strcmp(name, "GetRefHumanReadableName") == 0)
            return (void *)GetRefHumanReadableName;
    }
    if (handle == &client_marker) {
        if (strcmp(name, "GetClientAPI") == 0) return (void *)GetClientAPI;
        if (strcmp(name, "HUD_GetRenderInterface") == 0)
            return (void *)HUD_GetRenderInterface;
    }
    if (handle == &server_marker) {
        if (strcmp(name, "GetEntityAPI") == 0) return (void *)GetEntityAPI;
        if (strcmp(name, "GetEntityAPI2") == 0) return (void *)GetEntityAPI2;
        if (strcmp(name, "GetNewDLLFunctions") == 0) return (void *)GetNewDLLFunctions;
        if (strcmp(name, "GiveFnptrsToDll") == 0) return (void *)GiveFnptrsToDll;
        if (strcmp(name, "Server_GetBlendingInterface") == 0)
            return (void *)Server_GetBlendingInterface;
        if (strcmp(name, "Server_SplitAimAssist") == 0)
            return (void *)Server_SplitAimAssist;
        /* GoldSrc resolves every map classname as a named game DLL export.
         * Without this, worldspawn never creates CWorld and g_pGameRules stays
         * NULL; ReGameDLL crashes in ServerActivate / ReadMultiplayCvars. */
        if (PS2_ServerEntityByName)
            return PS2_ServerEntityByName(name);
    }
    return NULL;
}

void *COM_FunctionFromName(void *instance, const char *name)
{
    return COM_GetProcAddress(instance, name);
}

const char *COM_NameForFunction(void *instance, void *function)
{
    (void)instance;
    if (function == (void *)GetMenuAPI) return "GetMenuAPI";
    if (function == (void *)GetExtAPI) return "GetExtAPI";
    if (function == (void *)GetRefAPI) return "GetRefAPI";
    if (function == (void *)GetClientAPI) return "GetClientAPI";
    if (function == (void *)GetEntityAPI2) return "GetEntityAPI2";
    if (function == (void *)GiveFnptrsToDll) return "GiveFnptrsToDll";
    return NULL;
}
