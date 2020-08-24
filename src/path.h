#ifndef CSTAG_PATH_H
#define CSTAG_PATH_H

#if defined(_WIN32) && !defined(__CYGWIN__)
#define PATHSEP                         "\\"
#else
#define PATHSEP                         "/"
#endif

extern int sensitivefs;

int isabspath(const char *path);

char *pathescape(const char *path, char buf[]);

char *abspath(const char *base, const char *path, char buf[]);

char *relpath(const char *base, const char *path, char buf[]);

#endif //CSTAG_PATH_H