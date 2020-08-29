#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "path.h"

#if defined(_WIN32) && !defined(__CYGWIN__)
#define realpath(path, buf)             _fullpath(buf, path, PATH_MAX)
#define snprintf                        _snprintf
#endif

#ifndef SENSITIVEFS
#if defined(_WIN32) && !defined(__CYGWIN__)
#define SENSITIVEFS                     0
#else
#define SENSITIVEFS                     1
#endif
#endif

#define chrcmp(a, b)                    (sensitivefs ? (a) == (b) : tolower(a) == (b))

/**
 * 指示当前文件系统是否大小写敏感
 */
int sensitivefs = SENSITIVEFS;

/**
 * 检查是否是绝对路径
 * @param path 路径
 * @return     绝对路径返回1；否则返回0；
 */
int isabspath(const char *path)
{
    return
#if defined(_WIN32) && !defined(__CYGWIN__)
        (isalpha(path[0]) && path[1] == ':' && (path[2] == PATHSEP[0] || path[2] == '/')) || path[0] == '/' ||
#endif
            path[0] == PATHSEP[0];
}

/**
 * 转义path里的'\'字符
 * @param path 待转换路径
 * @param buf  转换后的缓冲区，大小至少为(PATH_MAX + 1) * 3
 * @return     转换成功返回buf指针，否则返回NULL
 */
char *pathescape(const char *path, char buf[])
{
    int len;

    if (!path || !buf)
        return NULL;

    for (len = 0; *path; path++) {
        if (*path == '\\' || isspace(*path))
            buf[len++] = '\\';
        buf[len++] = *path;
    }

    return len > 0 ? buf : NULL;
}

/**
 * 将path转成绝对路径
 * 若path是绝对路径，则将path转成绝对路径；
 * 否则，尝试将base + path转成绝对路径；
 * @param base 基本路径
 * @param path 需转换路径
 * @param buf  转换后的缓冲区，大小至少为PATH_MAX + 1
 * @return     转换成功返回buf指针，否则返回NULL
 */
char *abspath(const char *base, const char *path, char buf[])
{
    // 相对路径缓冲区最大为2 * PATH_MAX + '/' + '\0'
    char tmp[PATH_MAX * 2 + 2] = {0};

    if (!path || !buf)
        return NULL;

    if (base && !isabspath(path) && snprintf(tmp, sizeof(tmp), "%s" PATHSEP "%s", base, path) > 0)
        path = tmp;

    return realpath(path, buf);
}

/**
 * 将path转成相对base的相对路径
 * 若base与path存在重叠部分，则转成相对于base的相对路径；
 * 否则，否则直接返回path；
 * @param base 基本绝对路径
 * @param path 待转换的绝对路径
 * @param buf  转换后的缓冲区，大小至少为2 * PATH_MAX + 1
 * @return     转换成功返回buf指针，否则返回NULL
 */
char *relpath(const char *base, const char *path, char buf[])
{
    int len = 0;
    const char *bp, *pp;

    if (!base || !path || !buf)
        return NULL;

    for (bp = base, pp = path; *bp && *pp && chrcmp(*bp, *pp); bp++, pp++);
    if ((*bp && *bp != PATHSEP[0]) || (*pp && *pp != PATHSEP[0]))
        for (; bp > base && *bp != PATHSEP[0]; bp--, pp--);
    if (bp > base)
        for (len = sprintf(buf + len, "."); (bp = strchr(bp, PATHSEP[0])) && bp[1]; bp++)
            len += sprintf(buf + len, PATHSEP "..");
    len += sprintf(buf + len, "%s", pp);

    return len > 0 ? buf : NULL;
}