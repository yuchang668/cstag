#include <ctype.h>
#include <iconv.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <limits.h>
#include <getopt.h>
#include <dirent.h>
#include <sys/stat.h>
#include "path.h"
#include "task.h"
#include "dbop.h"

#if defined(_WIN32) && !defined(__CYGWIN__)
#define NULLFILE                        "NUL"
#else
#define NULLFILE                        "/dev/null"
#endif

#define BUFSIZE                         (PATH_MAX + 16)

#define DBNAME                          "tag.db"

#define GROUPSEP                        "\x1D"
#define FIELDSEP                        "\x1E"
#define FIELDEND                        "\x1F"
#define FIELDCTX(t, k, v)               FIELDSEP #t "$" k "=%" v FIELDEND
#define FIELDINT(k, v)                  FIELDCTX(I, k, v)
#define FIELDTXT(k, v)                  FIELDCTX(T, k, v)
#define GROUPEND                        GROUPSEP "\n"

#define ch2code(chr)                    (chr - '0' + (chr < '5'))
#define boolean(str)                    (!str || strcasecmp(str, "yes") == 0 || strcasecmp(str, "true") == 0 || strcasecmp(str, "1") == 0 ? 1 : 0)
#define echomsg(args...)                fprintf(stdout, PROGRAM_NAME ": " args)
#define echoerr(args...)                fprintf(stderr, PROGRAM_NAME ": " args)

#define PROGRAM_NAME                    "cstag"
#define PROGRAM_VERSION                 "0.0.1"
#define PROGRAM_USAGE                   "\
Usage:\n\
  " PROGRAM_NAME " [OPTION] [FILES...]\n\
\n\
Option:\n\
  -0PATTERN                    search symbols.\n\
  -1PATTERN                    search definitions.\n\
  -2PATTERN                    search caller function.\n\
  -3PATTERN                    search references.\n\
  -4PATTERN                    search strings.\n\
  -5                           change search mode.\n\
  -6PATTERN                    search context with egrep mode.\n\
  -7PATTERN                    search file.\n\
  -8PATTERN                    search file including this file.\n\
  -9PATTERN                    search assignment.\n\
  -rPATTERN                    search matched path.\n\
  -ePATTERN                    search pattern, using basic regexp.\n\
  -EPATTERN                    search pattern, using advanced regexp.\n\
  -f FILE                      the path of database file.\n\
  -L FILE                      read file list from the file, if FILE is '-',\n\
                               read stdin instead.\n\
  -o FILE                      output tags to the file.\n\
  -P DIR                       the prefix path when generating database.\n\
  -p FORMAT, --print=FORMAT    print with the format.\n\
  -u                           update incrementally.\n\
  -d                           update incrementally, not check the database.\n\
  -C                           ignore case when search.\n\
  -l                           Line-oriented interface.\n\
  -s                           output format of cscope.\n\
  -c                           output format of ctags.\n\
  -x                           output format of xref.\n\
  -g                           output format of grep.\n\
  -X                           output format of xml.\n\
  -R, --recurse[=yes|no]       search subdirectories recursively.\n\
  -V, --verbose                verbose mode.\n\
  -v, --version                print version.\n\
  -h, --help                   print help message.\n\
  --fs-sensitive[=true|false]  treat path as the sensitive setting of fs.\n\
  --output-encoding[=ENCODING] output encoding of tags,\n\
                               which need the support of ctags.\n\
\n\
  * any other parameter will pass through ctags.\n\
    cstag use UTF-8 charset internally, so output-encoding MUST be put in\n\
    command line if you want to change output encoding,\n\
    event if you have been set it through --options parameter,\n\
    because cstag only catch it through command line.\n\
\n\
Environment:\n\
  CTAGSPATH         custom ctags executable program path.\n\
\n\
  * any other environmental variables of ctags could be set also.\n\
"

typedef void (*write_t)(char *, int, int64_t, int64_t, void *);

enum {
    TAGPATH = 1,
    TAGXML,
    TAGXREF,
    TAGGREP,
    TAGCTAGS,
    TAGCSCOPE,
    TAGCUSTOM,
    TAGCOUNT
};

static int debugmode = 0;
static int recursive = 0;

static const char *tagformats[TAGCOUNT] = {
        [TAGPATH] = "%" FIELD_CHR_PATH "\n",
        [TAGXREF] = "%" FIELD_CHR_NAME "\t%" FIELD_CHR_KIND "\t%" FIELD_CHR_LINE "\t%" FIELD_CHR_PATH "\t%" FIELD_CHR_COMPACT "\n",
        [TAGGREP] = "%" FIELD_CHR_PATH ":%" FIELD_CHR_LINE ":%" FIELD_CHR_COMPACT "\n",
        [TAGCSCOPE] = "%" FIELD_CHR_PATH " %" FIELD_CHR_NAME " %" FIELD_CHR_LINE " %" FIELD_CHR_COMPACT "\n"
};

__attribute__((weak)) ssize_t getline(char **lineptr, size_t *n, FILE *fp)
{
    int ch;
    char *line;
    size_t tmp;
    ssize_t rc = 0;

    if (!lineptr || !n || !fp)
        return -1;

    if (!*lineptr || !*n) {
        *n = 256;
        *lineptr = (char *) malloc(*n);
        if (!*lineptr)
            return -1;
    }

    while ((ch = fgetc(fp)) != EOF) {
        if (rc == *n) {
            tmp = *n + 256;
            line = (char *) realloc(*lineptr, tmp);
            if (!line)
                return -1;
            *n = tmp;
            *lineptr = line;
        }
        (*lineptr)[rc++] = ch;
        if (ch == '\n')
            break;
    }
    (*lineptr)[rc] = '\0';

    return rc;
}

/**
 * findfile的回调函数，将文件路径写入数据库
 * @param path 文件路径
 * @param len  路径字符串长度
 * @param size 文件字节数
 * @param time 文件修改时间
 * @param ctx  上下文
 */
static void writepath(char *path, int len, int64_t size, int64_t time, void *ctx)
{
    int idx;
    size_t linecap = 0;
    uint64_t fid, cnt = 0;
    void **data = (void **) ctx;
    FILE *si = (FILE *) data[0];
    FILE *so = (FILE *) data[1];
    db_t db = (db_t) data[2];
    char *temp, *token, *fields[FIELD_MAX], *line = NULL;

    dbbegin(db);

    if (!(fid = dbsetfile(db, path, size, time))) {
        dbrollback(db);
        return;
    }

    path[len] = '\n';
    fwrite(path, len + 1, 1, so);
    fflush(so);
    path[len] = '\0';

    while (getline(&line, &linecap, si) > 0 && strcmp(line, GROUPEND) != 0) {
        memset(fields, 0, sizeof(fields));
        for (idx = 0, temp = line; (token = strsep(&temp, FIELDEND)) && *token++ == *FIELDSEP; idx++)
            fields[idx] = token;
        if (dbaddatag(db, fid, fields) == 0)
            cnt++;
    }

    free(line);

    if (cnt == 0)
        dbrollback(db);
    else
        dbcommit(db);

    if (debugmode && cnt)
        echomsg("parsed %s, size=%llu, tags=%llu\n", path, size, cnt);
}

/**
 * findfile回调函数，仅写入数据中不存在或变更的文件
 * @param path 文件路径
 * @param len  文件字符串长度
 * @param size 文件字节数
 * @param time 文件修改时间
 * @param ctx  上下文
 */
static void checkpath(char *path, int len, int64_t size, int64_t time, void *ctx)
{
    int64_t fsize, ftime;
    void **data = (void **) ctx;
    db_t db = (db_t) data[2];

    if (!dbgetfile(db, path, &fsize, &ftime) || fsize != size || ftime != time)
        writepath(path, len, size, time, ctx);
}

/**
 * 检查数据文件变更情况
 * @param fid  文件id
 * @param path 文件路径
 * @param size 文件字节数
 * @param time 文件修改时间
 * @param ctx  上下文
 */
static void checkfile(int64_t fid, const char *path, int64_t size, int64_t time, void *ctx)
{
    void **data = (void **) ctx;
    db_t db = (db_t) data[2];
    struct stat info = {0};

    if (stat(path, &info) != 0 || !S_ISREG(info.st_mode)) {
        dbdelfile(db, path);
        if (debugmode)
            echomsg("delete %s\n", path);
    }
}

static inline void _findfile(char *path, int len, write_t func, void *ctx)
{
    int tmp;
    DIR *dir;
    struct stat info;
    struct dirent *entry;

    if ((dir = opendir(path))) {
        while ((entry = readdir(dir))) {
            if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0 &&
                (tmp = snprintf(path + len, BUFSIZE - len, PATHSEP "%s", entry->d_name)) > 0 &&
                (tmp += len, stat(path, &info)) == 0) {
                if (S_ISREG(info.st_mode))
                    func(path, tmp, info.st_size, info.st_mtime, ctx);
                else if (S_ISDIR(info.st_mode) && recursive)
                    _findfile(path, tmp, func, ctx);
            }
        }
        path[len] = '\0';
        closedir(dir);
    }
}

/**
 * 查找文件
 * @param path 文件路径
 * @param len  文件字符串长度
 * @param func 查找到文件时的处理函数
 * @param ctx  传给回调函数的上下文
 */
static void findfile(char *path, int len, write_t func, void *ctx)
{
    struct stat info = {0};

    if (stat(path, &info) == 0) {
        if (S_ISREG(info.st_mode))
            func(path, len, info.st_size, info.st_mtime, ctx);
        else if (S_ISDIR(info.st_mode))
            _findfile(path, len, func, ctx);
    }
}

/**
 * 根据编码，按指定格式将字符串写入文件
 * @param fp  文件句柄
 * @param cd  编码句柄
 * @param fmt 格式字符串
 * @param ...
 */
static void print(FILE *fp, iconv_t cd, const char *fmt, ...)
{
    int len;
    va_list ap;
    size_t ret, insize, outsize;
    char tmpbuf[BUFSIZE], *inbuf, *outbuf, *buf = NULL;

    va_start(ap, fmt);

    if (!cd || cd == (iconv_t) (-1))
        vfprintf(fp, fmt, ap);
    else if ((len = vasprintf(&buf, fmt, ap)) > 0) {
        inbuf = buf;
        insize = len;
        do {
            outbuf = tmpbuf;
            outsize = sizeof(tmpbuf);
            ret = iconv(cd, &inbuf, &insize, &outbuf, &outsize);
            len = sizeof(tmpbuf) - outsize;
            if (len > 0) {
                fwrite(tmpbuf, len, 1, fp);
                fflush(fp);
            }
        } while (ret == (size_t) (-1) && !outsize);
        free(buf);
    }

    va_end(ap);
}

/**
 * 按指定格式输出tag
 * @param fp     文件句柄
 * @param cd     编码句柄
 * @param fmt    格式字符串
 * @param fields tag内容
 */
static void echofmt(FILE *fp, iconv_t cd, const char *cwd, const char *fmt, char **fields)
{
    int idx, len;
    char ch, buf[32];
    const char *p, *q, *k;
    char *field, tmpbuf[PATH_MAX * 3 / 2 + 1] = {0}, pathbuf[PATH_MAX * 2 + 1] = {0};

    if (!fields[FIELD_IDX_MARK] ||
        !fields[FIELD_IDX_PATH] ||
        !fields[FIELD_IDX_NAME] ||
        !fields[FIELD_IDX_KIND] ||
        !fields[FIELD_IDX_LINE] ||
        !fields[FIELD_IDX_PATTERN] ||
        !fields[FIELD_IDX_COMPACT])
        return;

    for (idx = -1, q = NULL, p = fmt; *p; idx = -1, p++) {
        switch (*p) {
            case *FIELD_CHR_PATH:
                idx = FIELD_IDX_PATH;
                break;
            case *FIELD_CHR_MARK:
                idx = FIELD_IDX_MARK;
                break;
            case *FIELD_CHR_NAME:
                idx = FIELD_IDX_NAME;
                break;
            case *FIELD_CHR_PATTERN:
                idx = FIELD_IDX_PATTERN;
                break;
            case *FIELD_CHR_COMPACT:
                idx = FIELD_IDX_COMPACT;
                break;
            case *FIELD_CHR_LINE:
                idx = FIELD_IDX_LINE;
                break;
            case *FIELD_CHR_ENDL:
                idx = FIELD_IDX_ENDL;
                break;
            case *FIELD_CHR_LANG:
                idx = FIELD_IDX_LANG;
                break;
            case *FIELD_CHR_ROLE:
                idx = FIELD_IDX_ROLE;
                break;
            case *FIELD_CHR_KIND:
                idx = FIELD_IDX_KIND;
                break;
            case *FIELD_CHR_TYPE:
                idx = FIELD_IDX_TYPE;
                break;
            case *FIELD_CHR_SIGN:
                idx = FIELD_IDX_SIGN;
                break;
            case *FIELD_CHR_ACCESS:
                idx = FIELD_IDX_ACCESS;
                break;
            case *FIELD_CHR_INHERIT:
                idx = FIELD_IDX_INHERIT;
                break;
            case *FIELD_CHR_IMPL:
                idx = FIELD_IDX_IMPL;
                break;
            case *FIELD_CHR_KSCOPE:
                idx = FIELD_IDX_KSCOPE;
                break;
            case *FIELD_CHR_NSCOPE:
                idx = FIELD_IDX_NSCOPE;
                break;
            case *FIELD_CHR_EXTRAS:
                idx = FIELD_IDX_EXTRAS;
                break;
            case '%':
                q = q ? NULL : p;
                break;
            default:
                break;
        }
        if (!q) {
            ch = *p;
            if (ch == '\\')
                switch (*++p) {
                    case 'a':
                        ch = '\a';
                        break;
                    case 'b':
                        ch = '\b';
                        break;
                    case 'f':
                        ch = '\f';
                        break;
                    case 'n':
                        ch = '\n';
                        break;
                    case 'r':
                        ch = '\r';
                        break;
                    case 't':
                        ch = '\t';
                        break;
                    case 'v':
                        ch = '\v';
                        break;
                    case '0':
                        ch = (char) strtoul(++p, (char **) &k, 8);
                        p = k - 1;
                        break;
                    case 'x':
                    case 'X':
                        ch = (char) strtoul(++p, (char **) &k, 16);
                        p = k - 1;
                        break;
                    case '\\':
                        ch = '\\';
                        break;
                    default:
                        ch = '?';
                        break;
                }
            print(fp, cd, "%c", ch);
        } else if (idx >= 0) {
            for (k = q + 1 + (q[1] == '-'); k < p && isdigit(*k); k++);
            if (k == p && (len = p - q) < sizeof(buf) - 2) {
                strncpy(buf, q, len);
                strcpy(buf + len, "s");
                field = fields[idx];
                if (idx == FIELD_IDX_PATH)
                    field = pathescape(relpath(cwd, field, pathbuf), tmpbuf);
                print(fp, cd, buf, field);
            }
            q = NULL;
        }
    }
}

/**
 * 输出xml格式tag
 * @param fp      文件句柄
 * @param cd      编码句柄
 * @param path    tag所属文件路径
 * @param mark    tag标记
 * @param name    tag名称
 * @param pattern tag地址模式
 * @param compact tag上下文信息
 * @param line    tag开始行
 * @param endl    tag结束行
 * @param lang    tag所属语言
 * @param role    tag角色
 * @param kind    tag类型
 * @param type    tag类型名称
 * @param sign    tag签名
 * @param access  tag访问属性
 * @param inherit tag继承属性
 * @param impl    tag实现
 * @param kscope  tag作用域类型
 * @param nscope  tag作用域名称
 * @param extras  tag额外信息
 */
static void echoxml(FILE *fp, iconv_t cd, const char *cwd,
                    const char *path,
                    const char *mark,
                    const char *name,
                    const char *pattern,
                    const char *compact,
                    const char *line,
                    const char *endl,
                    const char *lang,
                    const char *role,
                    const char *kind,
                    const char *type,
                    const char *sign,
                    const char *access,
                    const char *inherit,
                    const char *impl,
                    const char *kscope,
                    const char *nscope,
                    const char *extras)
{
    char buf[PATH_MAX * 3 / 2 + 1] = {0}, pathbuf[PATH_MAX * 2 + 1] = {0};

    if (!mark || !path || !name || !kind || !line || !pattern || !compact)
        return;

    path = pathescape(relpath(cwd, path, pathbuf), buf);

    print(fp, cd, "<tag mark=\"%s\">", mark);
    print(fp, cd,
          "<path>%s</path>"
          "<name>%s</name>"
          "<pattern>%s</pattern>"
          "<compact>%s</compact>"
          "<kind>%s</kind>"
          "<line>%s</line>",
          path,
          name,
          pattern,
          compact,
          kind,
          line);
    if (endl && strtoull(endl, NULL, 10))
        print(fp, cd, "<endl>%s</endl>", endl);
    if (lang && *lang)
        print(fp, cd, "<language>%s</language>", lang);
    if (kscope && *kscope && nscope && *nscope) {
        print(fp, cd, "<scope>");
        print(fp, cd, "<kind>%s</kind>", kscope);
        print(fp, cd, "<name>%s</name>", nscope);
        print(fp, cd, "</scope>");
    }
    if (type && *type)
        print(fp, cd, "<type>%s</type>", type);
    if (inherit && *inherit)
        print(fp, cd, "<inherits>%s</inherits>", inherit);
    if (access && *access)
        print(fp, cd, "<access>%s</access>", access);
    if (impl && *impl)
        print(fp, cd, "<implementation>%s</implementation>", impl);
    if (sign && *sign)
        print(fp, cd, "<signature>%s</signature>", sign);
    if (role && *role)
        print(fp, cd, "<roles>%s</roles>", role);
    if (extras && *extras)
        print(fp, cd, "<extras>%s</extras>", extras);
    print(fp, cd, "</tag>\n");
}

/**
 * 输出ctags格式tag
 * @param fp      文件句柄
 * @param cd      编码句柄
 * @param path    tag所属文件路径
 * @param mark    tag标记
 * @param name    tag名称
 * @param pattern tag地址模式
 * @param compact tag上下文信息
 * @param line    tag开始行
 * @param endl    tag结束行
 * @param lang    tag所属语言
 * @param role    tag角色
 * @param kind    tag类型
 * @param type    tag类型名称
 * @param sign    tag签名
 * @param access  tag访问属性
 * @param inherit tag继承属性
 * @param impl    tag实现
 * @param kscope  tag作用域类型
 * @param nscope  tag作用域名称
 * @param extras  tag额外信息
 */
static void echotag(FILE *fp, iconv_t cd, const char *cwd,
                    const char *path,
                    const char *name,
                    const char *pattern,
                    const char *line,
                    const char *endl,
                    const char *lang,
                    const char *role,
                    const char *kind,
                    const char *type,
                    const char *sign,
                    const char *access,
                    const char *inherit,
                    const char *impl,
                    const char *kscope,
                    const char *nscope,
                    const char *extras)
{
    char buf[PATH_MAX * 3 / 2 + 1] = {0}, pathbuf[PATH_MAX * 2 + 1] = {0};

    if (!path || !name || !kind || !line || !pattern)
        return;

    path = pathescape(relpath(cwd, path, pathbuf), buf);

    print(fp, cd, "%s\t%s\t%s;\"\tkind:%s\tline:%s", name, path, pattern, kind, line);
    if (lang && *lang)
        print(fp, cd, "\tlanguage:%s", lang);
    if (kscope && *kscope && nscope && *nscope)
        print(fp, cd, "\tscope:%s:%s", kscope, nscope);
    if (type && *type)
        print(fp, cd, "\t%s", type);
    if (extras && strstr(extras, "fileScope"))
        print(fp, cd, "\tfile:");
    if (inherit && *inherit)
        print(fp, cd, "\tinherits:%s", inherit);
    if (access && *access)
        print(fp, cd, "\taccess:%s", access);
    if (impl && *impl)
        print(fp, cd, "\timplementation:%s", impl);
    if (sign && *sign)
        print(fp, cd, "\tsignature:%s", sign);
    if (role && *role)
        print(fp, cd, "\troles:%s", role);
    if (extras && *extras)
        print(fp, cd, "\textras:%s", extras);
    if (endl && strtoull(endl, NULL, 10))
        print(fp, cd, "\tend:%s", endl);
    print(fp, cd, "\n");
}

/**
 * 将数据库指定内容转储到文件
 * @param fp     文件句柄
 * @param cd     编码句柄
 * @param db     数据库句柄
 * @param mode   数据库模式
 * @param total  是否显示tags条数
 * @param tagfmt tag输出格式
 * @param opcode 查询操作码
 * @param search 查询内容
 */
static void dumptag(FILE *fp, iconv_t cd, db_t db,
                    unsigned char mode,
                    unsigned char total,
                    unsigned char tagfmt,
                    unsigned char opcode,
                    const char *search,
                    const char *cwd)
{
    int row, rows = 0, cols = 0;
    char **table = NULL;

    if (!opcode)
        table = dbfindtags(db, mode, search, &rows, &cols);
    else if (opcode > 9)
        table = dbfindpath(db, mode, search, &rows, &cols);
    else
        table = dbreadtags(db, mode, opcode, search, &rows, &cols);

    if (!table)
        return;

    if (total) {
        switch (tagfmt) {
            case TAGXML:
                print(fp, cd, "xml: ");
                break;
            case TAGXREF:
                print(fp, cd, "xref: ");
                break;
            case TAGCTAGS:
                print(fp, cd, "ctags: ");
                break;
            case TAGCSCOPE:
                print(fp, cd, "cscope: ");
                break;
            default:
                print(fp, cd, "total: ");
                break;
        }
        print(fp, cd, "%d lines\n", (rows > 0 ? rows - 1 : rows));
    }

    if (opcode > 9)
        tagfmt = TAGPATH;

    switch (tagfmt) {
        case TAGXML:
            for (row = 1; row < rows; row++)
                echoxml(fp, cd, cwd,
                        table[row * cols + FIELD_IDX_PATH],
                        table[row * cols + FIELD_IDX_MARK],
                        table[row * cols + FIELD_IDX_NAME],
                        table[row * cols + FIELD_IDX_PATTERN],
                        table[row * cols + FIELD_IDX_COMPACT],
                        table[row * cols + FIELD_IDX_LINE],
                        table[row * cols + FIELD_IDX_ENDL],
                        table[row * cols + FIELD_IDX_LANG],
                        table[row * cols + FIELD_IDX_ROLE],
                        table[row * cols + FIELD_IDX_KIND],
                        table[row * cols + FIELD_IDX_TYPE],
                        table[row * cols + FIELD_IDX_SIGN],
                        table[row * cols + FIELD_IDX_ACCESS],
                        table[row * cols + FIELD_IDX_INHERIT],
                        table[row * cols + FIELD_IDX_IMPL],
                        table[row * cols + FIELD_IDX_KSCOPE],
                        table[row * cols + FIELD_IDX_NSCOPE],
                        table[row * cols + FIELD_IDX_EXTRAS]);
            break;
        case TAGCTAGS:
            for (row = 1; row < rows; row++)
                echotag(fp, cd, cwd,
                        table[row * cols + FIELD_IDX_PATH],
                        table[row * cols + FIELD_IDX_NAME],
                        table[row * cols + FIELD_IDX_PATTERN],
                        table[row * cols + FIELD_IDX_LINE],
                        table[row * cols + FIELD_IDX_ENDL],
                        table[row * cols + FIELD_IDX_LANG],
                        table[row * cols + FIELD_IDX_ROLE],
                        table[row * cols + FIELD_IDX_KIND],
                        table[row * cols + FIELD_IDX_TYPE],
                        table[row * cols + FIELD_IDX_SIGN],
                        table[row * cols + FIELD_IDX_ACCESS],
                        table[row * cols + FIELD_IDX_INHERIT],
                        table[row * cols + FIELD_IDX_IMPL],
                        table[row * cols + FIELD_IDX_KSCOPE],
                        table[row * cols + FIELD_IDX_NSCOPE],
                        table[row * cols + FIELD_IDX_EXTRAS]);
            break;
        default:
            for (row = 1; row < rows; row++)
                echofmt(fp, cd, cwd, tagformats[tagfmt], &table[row * cols]);
            break;
    }

    dbfree(table);
}

int main(int argc, char *const argv[])
{
    db_t db = NULL;
    FILE *fp = NULL;
    FILE *si = NULL;
    FILE *so = NULL;
    char regexp = 0;
    char opcode = 0;
    char tagfmt = 0;
    char update = 0;
    char caseless = 0;
    char linemode = 0;
    char buf[BUFSIZE];
    char cwd[BUFSIZE];
    char pwd[BUFSIZE];
    int tmp, idx, pid;
    size_t linesz = 0;
    write_t writeline;
    iconv_t cd = NULL;
    char *temp = NULL;
    char *line = NULL;
    char *encode = NULL;
    char *search = NULL;
    char *dbpath = NULL;
    char *inpath = NULL;
    char *output = NULL;
    char *prefix = NULL;
    void *data[5] = {0};
    char *args[argc + 10];
    struct stat info = {0};
    const struct option opts[] = {
            {"output-encoding", optional_argument, NULL, 't'},
            {"fs-sensitive",    optional_argument, NULL, 'z'},
            {"recurse",         optional_argument, NULL, 'R'},
            {"print",           required_argument, NULL, 'p'},
            {"verbose",         no_argument,       NULL, 'V'},
            {"version",         no_argument,       NULL, 'v'},
            {"help",            no_argument,       NULL, 'h'},
            {0}
    };

    args[idx = 0] = "ctags";

    while ((tmp = getopt_long(argc, argv, ":50:1:2:3:4:6:7:8:9:r:e:E:f:L:o:P:p:scgxXudClVRvh", opts, NULL)) != -1) {
        switch (tmp) {
            case '?':
                args[++idx] = argv[optind - 1];
                break;
            case '5':
                regexp = 1;
                break;
            case '0':
            case '1':
            case '2':
            case '3':
            case '4':
            case '6':
            case '8':
            case '9':
                opcode = ch2code(tmp);
                search = optarg;
                break;
            case '7':
                opcode = 10;
                search = optarg;
                break;
            case 'r':
                opcode = 7;
                search = optarg;
                break;
            case 'e':
                regexp = 0;
                opcode = 0;
                search = optarg;
                break;
            case 'E':
                regexp = 1;
                opcode = 0;
                search = optarg;
                break;
            case 't':
                encode = optarg;
                break;
            case 'f':
                dbpath = optarg;
                break;
            case 'L':
                inpath = optarg;
                break;
            case 'o':
                output = optarg;
                break;
            case 'P':
                prefix = optarg;
                break;
            case 'p':
                tagformats[TAGCUSTOM] = optarg;
                tagfmt = TAGCUSTOM;
                break;
            case 's':
                tagfmt = TAGCSCOPE;
                break;
            case 'c':
                tagfmt = TAGCTAGS;
                break;
            case 'g':
                tagfmt = TAGGREP;
                break;
            case 'x':
                tagfmt = TAGXREF;
                break;
            case 'X':
                tagfmt = TAGXML;
                break;
            case 'u':
                update = 1;
                break;
            case 'd':
                update = 2;
                break;
            case 'C':
                caseless = 1;
                break;
            case 'l':
                linemode = 1;
                break;
            case 'V':
                debugmode = 1;
                break;
            case 'R':
                recursive = boolean(optarg);
                break;
            case 'z':
                sensitivefs = boolean(optarg);
                break;
            case 'v':
                fprintf(stdout, "v%s\n", PROGRAM_VERSION);
                return 0;
            case 'h':
                fprintf(stdout, PROGRAM_USAGE);
                return 0;
            case ':':
                echoerr("option '-%c' requires value.\n", optopt);
                return 1;
            default:
                break;
        }
    }

    if (!getcwd(cwd, BUFSIZE)) {
        echoerr("current directory doesn't exist.\n");
        return 1;
    }

    if (!dbpath)
        for (sprintf(buf, "%s" PATHSEP, cwd), dbpath = buf; (temp = strrchr(dbpath, PATHSEP[0])); *temp = '\0') {
            strcpy(temp + 1, DBNAME);
            if (stat(dbpath, &info) == 0 && S_ISREG(info.st_mode))
                break;
        }

    if (!prefix && (temp = strrchr(dbpath, PATHSEP[0])) && (tmp = temp - dbpath) > 0)
        strncpy(pwd, dbpath, tmp)[tmp] = '\0';
    else if (!abspath(NULL, prefix, pwd) || stat(pwd, &info) != 0 || !S_ISDIR(info.st_mode))
        strcpy(pwd, cwd);

    if (debugmode) {
        echomsg("dbpath %s\n", dbpath);
        echomsg("prefix %s\n", pwd);
    }

    if (!(db = dbopen(pwd, dbpath, caseless ? DB_ICASE : 0))) {
        echoerr("open database failed.\n");
        return 1;
    }

    if (encode)
        args[++idx] = "--output-encoding=UTF-8";

    args[++idx] = "-uxL";
    args[++idx] = NULLFILE;
    args[++idx] = "--filter";
    args[++idx] = "--fields=*";
    args[++idx] = "--extras=*";
    args[++idx] = "--pseudo-tags=";
    args[++idx] = "--filter-terminator=" GROUPEND;
    args[++idx] = "--_xformat=" \
        FIELDTXT(FIELD_STR_MARK, FIELD_CHR_MARK) \
        FIELDTXT(FIELD_STR_NAME, FIELD_CHR_NAME) \
        FIELDTXT(FIELD_STR_PATTERN, FIELD_CHR_PATTERN) \
        FIELDTXT(FIELD_STR_COMPACT, FIELD_CHR_COMPACT) \
        FIELDINT(FIELD_STR_LINE, FIELD_CHR_LINE) \
        FIELDINT(FIELD_STR_ENDL, FIELD_CHR_ENDL) \
        FIELDTXT(FIELD_STR_LANG, FIELD_CHR_LANG) \
        FIELDTXT(FIELD_STR_ROLE, FIELD_CHR_ROLE) \
        FIELDTXT(FIELD_STR_KIND, FIELD_CHR_KIND) \
        FIELDTXT(FIELD_STR_TYPE, FIELD_CHR_TYPE) \
        FIELDTXT(FIELD_STR_SIGN, FIELD_CHR_SIGN) \
        FIELDTXT(FIELD_STR_ACCESS, FIELD_CHR_ACCESS) \
        FIELDTXT(FIELD_STR_INHERIT, FIELD_CHR_INHERIT) \
        FIELDTXT(FIELD_STR_IMPL, FIELD_CHR_IMPL) \
        FIELDTXT(FIELD_STR_KSCOPE, FIELD_CHR_KSCOPE) \
        FIELDTXT(FIELD_STR_NSCOPE, FIELD_CHR_NSCOPE) \
        FIELDTXT(FIELD_STR_EXTRAS, FIELD_CHR_EXTRAS) \
        "";
    args[++idx] = NULL;

    pid = taskexec(abspath(NULL, getenv("CTAGSPATH"), buf), args, pwd, &si, &so);
    if (pid <= 0 || !si || !so) {
        dbclose(db);
        echoerr("execute '%s' failed.\n", args[0]);
        return 1;
    }

    data[0] = si;
    data[1] = so;
    data[2] = db;
    data[3] = pwd;
    data[4] = cwd;

    writeline = update ? checkpath : writepath;

    for (idx = optind; idx < argc; idx++) {
        tmp = snprintf(buf, BUFSIZE, "%s", argv[idx]);
        if (tmp > 0)
            findfile(buf, tmp, writeline, (void *) data);
    }

    if (inpath && ((strcmp(inpath, "-") == 0 && (linemode = 0, fp = stdin)) ||
                   (stat(inpath, &info) == 0 && S_ISREG(info.st_mode) && (fp = fopen(inpath, "r"))))) {
        while ((tmp = getline(&line, &linesz, fp)) > 0) {
            for (temp = line + tmp; temp > line && isspace(temp[-1]); temp--);
            for (tmp = temp - line, *temp = '\0', temp = line; *temp && isspace(*temp); temp++);
            tmp -= temp - line;
            if (*temp != '#')
                findfile(temp, tmp, writeline, (void *) data);
        }
        fclose(fp);
    }

    fclose(si);
    fclose(so);
    taskwait(pid);

    if (update != 2)
        dballfile(db, checkfile, (void *) data);

    if (!tagfmt)
        tagfmt = linemode ? TAGCSCOPE : TAGCTAGS;

    if (opcode || search) {
        fp = output ? fopen(output, "w") : NULL;
        cd = fp && encode ? iconv_open(encode, "UTF-8") : NULL;
        if (fp) {
            if (tagfmt == TAGXML) {
                print(fp, cd, "<?xml version=\"1.0\" encoding=\"%s\">\n", encode ? encode : "UTF-8");
                print(fp, cd, "<tags>\n");
            } else if (tagfmt == TAGCTAGS) {
                if (encode)
                    print(fp, cd, "!_TAG_FILE_ENCODING\t%s\t//\n", encode);
                print(fp, cd, "!_TAG_FILE_FORMAT\t2\t/extended format; --format=1 will not append ;\" to lines/\n");
                print(fp, cd, "!_TAG_FILE_SORTED\t1\t/0=unsorted, 1=sorted, 2=foldcase/\n");
            }
        }

        dumptag(fp ? fp : stdout, cd, db, (regexp ? DB_REGEX : 0) | (caseless ? DB_ICASE : 0) | DB_MATCH,
                debugmode, tagfmt, opcode, search, cwd);

        if (fp) {
            if (tagfmt == TAGXML)
                print(fp, cd, "</tags>");
            fclose(fp);
        }

        iconv_close(cd);
    }

    while (linemode && (fprintf(stdout, ">> "), fflush(stdout), tmp = getline(&line, &linesz, stdin)) > 0) {
        for (temp = line + tmp; temp > line && isspace(temp[-1]); temp--);
        for (temp[0] = '\0', temp = line; *temp && isspace(*temp); temp++);
        for (opcode = 0, search = NULL; *temp && *temp == '5'; temp++)
            regexp = 1;
        tmp = *temp++;
        switch (tmp) {
            case '0':
            case '1':
            case '2':
            case '3':
            case '4':
            case '6':
            case '8':
            case '9':
                opcode = ch2code(tmp);
                search = temp;
                break;
            case '7':
                opcode = 10;
                search = temp;
                break;
            case 'r':
                opcode = 7;
                search = temp;
                break;
            case 'e':
                regexp = 0;
                opcode = 0;
                search = temp;
                break;
            case 'E':
                regexp = 1;
                opcode = 0;
                search = temp;
                break;
            case 'c':
                caseless = !caseless;
                break;
            case 'C':
                caseless = 1;
                break;
            case 'R':
                regexp = 0;
                caseless = 0;
                break;
            case 'F':
                break;
            case 'q':
                linemode = 0;
                break;
            default:
                if (tmp)
                    echoerr("unknown command '%c%s'.\n", tmp, search ? search : "");
                break;
        }

        if (opcode || search)
            dumptag(stdout, NULL, db, (regexp ? DB_REGEX : 0) | (caseless ? DB_ICASE : 0) | DB_MATCH,
                    1, TAGCSCOPE, opcode, search, cwd);
    }

    free(line);
    dbclose(db);

    return 0;
}