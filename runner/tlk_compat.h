/* POSIX calls the runner uses that MinGW's CRT lacks or spells differently.
 * Empty on Linux/macOS. */
#ifndef TLK_COMPAT_H
#define TLK_COMPAT_H

#ifdef _WIN32
#include <stdlib.h>
#include <direct.h>

/* setenv: _putenv_s writes the same CRT environment getenv reads. */
static inline int tlk_setenv(const char *name, const char *value, int overwrite) {
    if (!overwrite && getenv(name)) return 0;
    return _putenv_s(name, value) ? -1 : 0;
}
#define setenv tlk_setenv
/* mkdir: no mode argument on Windows. */
#define tlk_mkdir(path, mode) _mkdir(path)
#else
#include <sys/stat.h>
#define tlk_mkdir(path, mode) mkdir(path, mode)
#endif

#endif
