#ifndef CSTAG_DBOP_H
#define CSTAG_DBOP_H

#include <stdint.h>

#define DB_MATCH                1
#define DB_REGEX                2
#define DB_ICASE                4

#define FIELD_STR_PATH          "path"
#define FIELD_STR_MARK          "mark"
#define FIELD_STR_NAME          "name"
#define FIELD_STR_PATTERN       "pattern"
#define FIELD_STR_COMPACT       "compact"
#define FIELD_STR_LINE          "line"
#define FIELD_STR_ENDL          "endl"
#define FIELD_STR_LANG          "language"
#define FIELD_STR_ROLE          "roles"
#define FIELD_STR_KIND          "kind"
#define FIELD_STR_TYPE          "typeref"
#define FIELD_STR_SIGN          "signature"
#define FIELD_STR_ACCESS        "access"
#define FIELD_STR_INHERIT       "inherits"
#define FIELD_STR_IMPL          "implementation"
#define FIELD_STR_KSCOPE        "scopeKind"
#define FIELD_STR_NSCOPE        "scopeName"
#define FIELD_STR_EXTRAS        "extras"

#define FIELD_CHR_PATH          "F"
#define FIELD_CHR_MARK          "R"
#define FIELD_CHR_NAME          "N"
#define FIELD_CHR_PATTERN       "P"
#define FIELD_CHR_COMPACT       "C"
#define FIELD_CHR_LINE          "n"
#define FIELD_CHR_ENDL          "e"
#define FIELD_CHR_LANG          "l"
#define FIELD_CHR_ROLE          "r"
#define FIELD_CHR_KIND          "K"
#define FIELD_CHR_TYPE          "t"
#define FIELD_CHR_SIGN          "S"
#define FIELD_CHR_ACCESS        "a"
#define FIELD_CHR_INHERIT       "i"
#define FIELD_CHR_IMPL          "m"
#define FIELD_CHR_KSCOPE        "p"
#define FIELD_CHR_NSCOPE        "s"
#define FIELD_CHR_EXTRAS        "E"

enum {
    FIELD_IDX_PATH,
    FIELD_IDX_MARK,
    FIELD_IDX_NAME,
    FIELD_IDX_PATTERN,
    FIELD_IDX_COMPACT,
    FIELD_IDX_LINE,
    FIELD_IDX_ENDL,
    FIELD_IDX_LANG,
    FIELD_IDX_ROLE,
    FIELD_IDX_KIND,
    FIELD_IDX_TYPE,
    FIELD_IDX_SIGN,
    FIELD_IDX_ACCESS,
    FIELD_IDX_INHERIT,
    FIELD_IDX_IMPL,
    FIELD_IDX_KSCOPE,
    FIELD_IDX_NSCOPE,
    FIELD_IDX_EXTRAS,
    FIELD_MAX
};

typedef struct tagDB *db_t;

int dbbegin(db_t db);

int dbcommit(db_t db);

int dbrollback(db_t db);

int dballfile(db_t db, void (*func)(int64_t fid, const char *path, int64_t size, int64_t time, void *ctx), void *ctx);

int64_t dbgetfile(db_t db, const char *path, int64_t *size, int64_t *time);

int64_t dbsetfile(db_t db, const char *path, int64_t size, int64_t time);

int dbdelfile(db_t db, const char *path);

int dbaddatag(db_t db, int64_t fid, char *const *fields);

char **dbreadtags(db_t db, unsigned char mode, unsigned char opcode, const char *pattern, int *rows, int *cols);

char **dbfindpath(db_t db, unsigned char mode, const char *pattern, int *rows, int *cols);

char **dbfindtags(db_t db, unsigned char mode, const char *where, int *rows, int *cols);

void dbfree(char **table);

db_t dbopen(const char *base, const char *path, unsigned char mode);

int dbclose(db_t db);

#endif //CSTAG_DBOP_H