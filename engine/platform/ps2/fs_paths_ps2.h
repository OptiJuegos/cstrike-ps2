/* Read-only installation mapping shared by the PS2 prototype and Xash FS. */
#ifndef XASH_FS_PATHS_PS2_H
#define XASH_FS_PATHS_PS2_H

#include <stddef.h>

#define CS_PS2_ROOT_BYTES 512
typedef int (*PS2RootProbe)(const char *root, void *context);
int PS2_FS_SetRoot(const char *root);
void PS2_FS_ResetRoot(void);
const char *PS2_FS_Root(void);
int PS2_FS_RootResolved(void);
int PS2_FS_MakePath(const char *source, char *destination, size_t capacity);
int PS2_FS_JoinRoot(const char *root, const char *source,
                    char *destination, size_t capacity);
int PS2_FS_SelectRoot(const char *launch, const char *cwd, const char *preferred,
                     PS2RootProbe probe, void *context);
void PS2_FS_SetLaunchPath(const char *path);
void PS2_EarlyInit(void); /* sys_ps2.c: debug screen + exception handler */
const char *PS2_FS_LaunchPath(void);
int PS2_FS_Init(const char *preferred);

/* Legacy symbol used by the standalone diagnostics; now maps the cached root. */
int PS2_FS_MakeHostPath(const char *source, char *destination, size_t capacity);

#endif
