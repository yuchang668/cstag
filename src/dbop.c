#include <regex.h>
#include <ctype.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <assert.h>
#include <fnmatch.h>
#include <sqlite3.h>
#include "path.h"
#include "dbop.h"

#define SQL_INIT                "\
PRAGMA foreign_keys = ON;\n\
PRAGMA synchronous = OFF;\n\
CREATE TABLE IF NOT EXISTS file (\n\
    id INTEGER PRIMARY KEY,\n\
    " FIELD_STR_PATH " TEXT UNIQUE NOT NULL,\n\
    size INTEGER DEFAULT 0,\n\
    time INTEGER DEFAULT 0\n\
);\n\
CREATE TABLE IF NOT EXISTS tag (\n\
    fid INTEGER NOT NULL,\n\
    " FIELD_STR_MARK " TEXT NOT NULL,\n\
    " FIELD_STR_NAME " TEXT NOT NULL,\n\
    " FIELD_STR_PATTERN " TEXT NOT NULL,\n\
    " FIELD_STR_COMPACT " TEXT NOT NULL,\n\
    " FIELD_STR_LINE " INTEGER NOT NULL,\n\
    " FIELD_STR_ENDL " INTEGER DEFAULT 0,\n\
    " FIELD_STR_LANG " TEXT,\n\
    " FIELD_STR_ROLE " TEXT,\n\
    " FIELD_STR_KIND " TEXT,\n\
    " FIELD_STR_TYPE " TEXT,\n\
    " FIELD_STR_SIGN " TEXT,\n\
    " FIELD_STR_ACCESS " TEXT,\n\
    " FIELD_STR_INHERIT " TEXT,\n\
    " FIELD_STR_IMPL " TEXT,\n\
    " FIELD_STR_KSCOPE " TEXT,\n\
    " FIELD_STR_NSCOPE " TEXT,\n\
    " FIELD_STR_EXTRAS " TEXT,\n\
    FOREIGN KEY(fid) REFERENCES file(id) ON UPDATE CASCADE ON DELETE CASCADE\n\
);\
"

#define SQL_ALLFILE             "SELECT id, ABSPATH(" FIELD_STR_PATH "), size, time FROM file;"
#define SQL_GETFILE             "SELECT id, size, time FROM file WHERE " FIELD_STR_PATH " MATCH RELPATH(?) LIMIT 1;"
#define SQL_SETFILE             "INSERT OR REPLACE INTO file (" FIELD_STR_PATH ", size, time) VALUES (RELPATH(?), ?, ?);"
#define SQL_DELFILE             "DELETE FROM file WHERE " FIELD_STR_PATH " MATCH RELPATH(?);"
#define SQL_ADDTAGS             "INSERT INTO tag VALUES (\
    $fid,\
    $" FIELD_STR_MARK ",\
    $" FIELD_STR_NAME ",\
    $" FIELD_STR_PATTERN ",\
    $" FIELD_STR_COMPACT ",\
    $" FIELD_STR_LINE ",\
    $" FIELD_STR_ENDL ",\
    $" FIELD_STR_LANG ",\
    $" FIELD_STR_ROLE ",\
    $" FIELD_STR_KIND ",\
    $" FIELD_STR_TYPE ",\
    $" FIELD_STR_SIGN ",\
    $" FIELD_STR_ACCESS ",\
    $" FIELD_STR_INHERIT ",\
    $" FIELD_STR_IMPL ",\
    $" FIELD_STR_KSCOPE ",\
    $" FIELD_STR_NSCOPE ",\
    $" FIELD_STR_EXTRAS "\
);"

#define SQL_QUERYTAG            "SELECT \
ABSPATH(" FIELD_STR_PATH "), \
" FIELD_STR_MARK ", \
" FIELD_STR_NAME ", \
" FIELD_STR_PATTERN ", \
" FIELD_STR_COMPACT ", \
" FIELD_STR_LINE ", \
" FIELD_STR_ENDL ", \
" FIELD_STR_LANG ", \
" FIELD_STR_ROLE ", \
" FIELD_STR_KIND ", \
" FIELD_STR_TYPE ", \
" FIELD_STR_SIGN ", \
" FIELD_STR_ACCESS ", \
" FIELD_STR_INHERIT ", \
" FIELD_STR_IMPL ", \
" FIELD_STR_KSCOPE ", \
" FIELD_STR_NSCOPE ", \
" FIELD_STR_EXTRAS " \
FROM tag INNER JOIN file ON tag.fid = file.id "

#define SQL_TAGSORT             "ORDER BY " FIELD_STR_NAME "," FIELD_STR_LINE "," FIELD_STR_KIND " ASC;"
#define SQL_SYMBOL              SQL_QUERYTAG "WHERE " FIELD_STR_NAME " REGEXP ? " SQL_TAGSORT
#define SQL_DEFINE              SQL_QUERYTAG "WHERE " FIELD_STR_NAME " REGEXP ? AND " FIELD_STR_MARK " = 'D' " SQL_TAGSORT
#define SQL_CALLER              SQL_QUERYTAG "INNER JOIN (SELECT fid," FIELD_STR_LINE " AS line1," FIELD_STR_ENDL " AS line2 FROM tag WHERE " FIELD_STR_NAME " REGEXP ? AND " FIELD_STR_KIND " = 'function') AS scope ON tag.fid = scope.fid WHERE " FIELD_STR_LINE " BETWEEN line1 AND line2 " SQL_TAGSORT
#define SQL_REFER               SQL_QUERYTAG "WHERE " FIELD_STR_NAME " REGEXP ? AND " FIELD_STR_MARK " = 'R' " SQL_TAGSORT
#define SQL_STRING              SQL_QUERYTAG "WHERE " FIELD_STR_NAME " REGEXP ? AND " FIELD_STR_KIND " = 'string' " SQL_TAGSORT
#define SQL_PATTERN             SQL_QUERYTAG "WHERE " FIELD_STR_COMPACT " REGEXP ? " SQL_TAGSORT
#define SQL_INFILE              SQL_QUERYTAG "WHERE " FIELD_STR_PATH " MATCH ? " SQL_TAGSORT
#define SQL_INCLUDE             SQL_QUERYTAG "WHERE " FIELD_STR_NAME " REGEXP ? AND " FIELD_STR_KIND " = 'header' " SQL_TAGSORT
#define SQL_ASSIGN              SQL_QUERYTAG "WHERE " FIELD_STR_NAME " REGEXP ? AND " FIELD_STR_KIND " = 'variable' " SQL_TAGSORT
#define SQL_FPATH               "SELECT ABSPATH(" FIELD_STR_PATH ") FROM file WHERE " FIELD_STR_PATH " MATCH ? ORDER BY " FIELD_STR_PATH " ASC;"

enum {
    DBOP_ADDTAGS,
    QUERY_SYMBOL,
    QUERY_DEFINE,
    QUERY_CALLER,
    QUERY_REFER,
    QUERY_STRING,
    QUERY_PATTERN,
    QUERY_INFILE,
    QUERY_INCLUDE,
    QUERY_ASSIGN,
    QUERY_FPATH,
    DBOP_ALLFILE,
    DBOP_GETFILE,
    DBOP_SETFILE,
    DBOP_DELFILE,
    DBOP_COUNT
};

struct tagDB {
    sqlite3 *db3;
    sqlite3_stmt *stmt[DBOP_COUNT];
    unsigned char mode;
    char path[PATH_MAX + 1];
};

static void strmatch(sqlite3_context *ctx, int argc, sqlite3_value *argv[])
{
    int ret;
    char *search, *string;
    unsigned char mode = *(unsigned char *) sqlite3_user_data(ctx);

    if (argc != 2 || sqlite3_value_type(argv[0]) != SQLITE_TEXT || sqlite3_value_type(argv[1]) != SQLITE_TEXT)
        return;

    search = (char *) sqlite3_value_text(argv[0]);
    string = (char *) sqlite3_value_text(argv[1]);

    if (!search || !string)
        return;

    if (mode & DB_MATCH)
        ret = fnmatch(search, string, FNM_NOESCAPE | (mode & DB_ICASE ? FNM_CASEFOLD : 0));
    else
        ret = (mode & DB_ICASE ? sqlite3_stricmp : strcmp)(search, string);

    sqlite3_result_int(ctx, ret == 0);
}

static void strregexp(sqlite3_context *ctx, int argc, sqlite3_value *argv[])
{
    int ret;
    char *search, *string;
    unsigned char mode = *(unsigned char *) sqlite3_user_data(ctx);
    regex_t regex = {0};

    if (argc != 2 || sqlite3_value_type(argv[0]) != SQLITE_TEXT || sqlite3_value_type(argv[1]) != SQLITE_TEXT)
        return;

    search = (char *) sqlite3_value_text(argv[0]);
    string = (char *) sqlite3_value_text(argv[1]);

    if (!search || !string)
        return;

    if (mode & DB_REGEX) {
        ret = regcomp(&regex, search,
                      REG_NOSUB | (mode & DB_ICASE ? REG_ICASE : 0) | (mode & DB_EXREG ? REG_EXTENDED : 0));
        if (ret == 0) {
            ret = regexec(&regex, string, 0, NULL, 0);
            regfree(&regex);
        }
    } else
        ret = (mode & DB_ICASE ? sqlite3_stricmp : strcmp)(search, string);

    sqlite3_result_int(ctx, ret == 0);
}

static void toabspath(sqlite3_context *ctx, int argc, sqlite3_value *argv[])
{
    char *path, buf[PATH_MAX + 1] = {0};
    const char *base = (const char *) sqlite3_user_data(ctx);

    if (argc != 1 || sqlite3_value_type(argv[0]) != SQLITE_TEXT)
        return;

    path = (char *) sqlite3_value_text(argv[0]);
    if (abspath(base, path, buf))
        sqlite3_result_text(ctx, buf, -1, SQLITE_TRANSIENT);
    else
        sqlite3_result_null(ctx);
}

static void torelpath(sqlite3_context *ctx, int argc, sqlite3_value *argv[])
{
    char *path, buf[PATH_MAX + 1] = {0};
    const char *base = (const char *) sqlite3_user_data(ctx);

    if (argc != 1 || sqlite3_value_type(argv[0]) != SQLITE_TEXT)
        return;

    path = (char *) sqlite3_value_text(argv[0]);
    if (relpath(base, path, buf))
        sqlite3_result_text(ctx, buf, -1, SQLITE_TRANSIENT);
    else
        sqlite3_result_null(ctx);
}

/**
 * 查询数据库
 * @param db   数据库句柄
 * @param mode 数据库模式
 * @param sql  查询语句
 * @param rows 结果集行数
 * @param cols 结果集列数
 * @return     查询成功返回结果集，否则返回NULL
 */
static char **dbquery(db_t db, unsigned char mode, const char *sql, int *rows, int *cols)
{
    int rc;
    char **table = NULL;

    assert(db && db->db3 && sql && rows && cols);

    db->mode = mode;

    rc = sqlite3_get_table(db->db3, sql, &table, rows, cols, NULL);

    return (rc == SQLITE_OK ? table : NULL);
}

/**
 * 开始新的事务
 * @param db 数据库句柄
 * @return   事务开始成功返回0，否则返回非0
 */
int dbbegin(db_t db)
{
    assert(db && db->db3);
    return sqlite3_exec(db->db3, "BEGIN;", NULL, NULL, NULL) == SQLITE_OK ? 0 : -1;
}

/**
 * 提交由dbbegin开始的事务
 * @param db 数据库句柄
 * @return   提交成功返回0，否则返回非0
 */
int dbcommit(db_t db)
{
    assert(db && db->db3);
    return sqlite3_exec(db->db3, "COMMIT;", NULL, NULL, NULL) == SQLITE_OK ? 0 : -1;
}

/**
 * 回滚由dbbegin开始的事务
 * @param db 数据库句柄
 * @return   回滚成功返回0，否则返回非0
 */
int dbrollback(db_t db)
{
    assert(db && db->db3);
    return sqlite3_exec(db->db3, "ROLLBACK;", NULL, NULL, NULL) == SQLITE_OK ? 0 : -1;
}

/**
 * 遍历所有文件
 * @param db   数据库句柄
 * @param func 查找到文件时的回调函数，参数为此文件相关属性
 * @param ctx  回调函数上下文
 * @return     遍历成功返回0，否则返回非0
 */
int dballfile(db_t db, void (*func)(int64_t fid, const char *path, int64_t size, int64_t time, void *ctx), void *ctx)
{
    int rc;
    int64_t fid, size, time;
    const unsigned char *path;

    assert(db && db->stmt[DBOP_ALLFILE]);

    sqlite3_reset(db->stmt[DBOP_ALLFILE]);

    while ((rc = sqlite3_step(db->stmt[DBOP_ALLFILE])) == SQLITE_ROW) {
        fid = sqlite3_column_int64(db->stmt[DBOP_ALLFILE], 0);
        path = sqlite3_column_text(db->stmt[DBOP_ALLFILE], 1);
        size = sqlite3_column_int64(db->stmt[DBOP_ALLFILE], 2);
        time = sqlite3_column_int64(db->stmt[DBOP_ALLFILE], 3);
        func(fid, (char *) path, size, time, ctx);
    }

    return rc == SQLITE_DONE ? 0 : -1;
}

/**
 * 获取文件属性
 * @param db   数据库句柄
 * @param path 文件绝对路径
 * @param size 返回的文件字节数
 * @param time 返回的文件修改时间
 * @return     获取成功返回0，否则返回非0
 */
int64_t dbgetfile(db_t db, const char *path, int64_t *size, int64_t *time)
{
    int64_t id = 0;
    char buf[PATH_MAX + 1] = {0};

    assert(db && db->db3 && db->stmt[DBOP_GETFILE] && path);

    if (!abspath(NULL, path, buf))
        return -1;

    sqlite3_reset(db->stmt[DBOP_GETFILE]);

    sqlite3_bind_text(db->stmt[DBOP_GETFILE], 1, buf, -1, NULL);

    if (sqlite3_step(db->stmt[DBOP_GETFILE]) == SQLITE_ROW) {
        id = sqlite3_column_int64(db->stmt[DBOP_GETFILE], 0);
        if (size)
            *size = sqlite3_column_int64(db->stmt[DBOP_GETFILE], 1);
        if (time)
            *time = sqlite3_column_int64(db->stmt[DBOP_GETFILE], 2);
    }

    return id;
}

/**
 * 添加或修改文件属性信息
 * @param db   数据库句柄
 * @param path 文件绝对路径
 * @param size 文件字节数
 * @param time 文件修改时间
 * @return     设置成功返回0，否则返回非0
 */
int64_t dbsetfile(db_t db, const char *path, int64_t size, int64_t time)
{
    char buf[PATH_MAX + 1] = {0};

    assert(db && db->db3 && db->stmt[DBOP_SETFILE] && path);

    if (!abspath(NULL, path, buf))
        return -1;

    sqlite3_reset(db->stmt[DBOP_SETFILE]);

    sqlite3_bind_text(db->stmt[DBOP_SETFILE], 1, buf, -1, NULL);
    sqlite3_bind_int64(db->stmt[DBOP_SETFILE], 2, size);
    sqlite3_bind_int64(db->stmt[DBOP_SETFILE], 3, time);

    return sqlite3_step(db->stmt[DBOP_SETFILE]) == SQLITE_DONE ? sqlite3_last_insert_rowid(db->db3) : 0;
}

/**
 * 从数据库中删除文件（文件里的tags也会清除）
 * @param db   数据库句柄
 * @param path 文件绝对路径
 * @return     删除成功返回0，否则返回非0
 */
int dbdelfile(db_t db, const char *path)
{
    char buf[PATH_MAX + 1] = {0};

    assert(db && db->stmt[DBOP_DELFILE] && path);

    if (!abspath(NULL, path, buf))
        return -1;

    sqlite3_reset(db->stmt[DBOP_DELFILE]);

    sqlite3_bind_text(db->stmt[DBOP_DELFILE], 1, buf, -1, NULL);

    return sqlite3_step(db->stmt[DBOP_DELFILE]) == SQLITE_DONE ? 0 : -1;
}

/**
 * 向数据库添加一条tag
 * @param db     数据库句柄
 * @param fid    待添加的tag所属的文件id
 * @param fields tag内容，用户必须保证以NULL结尾
 * @return       添加成功返回0，否则返回非0
 */
int dbaddatag(db_t db, int64_t fid, char *const *fields)
{
    int idx, type;
    char *item, *const *field;

    assert(db && db->stmt[DBOP_ADDTAGS]);

    sqlite3_reset(db->stmt[DBOP_ADDTAGS]);

    idx = sqlite3_bind_parameter_index(db->stmt[DBOP_ADDTAGS], "$fid");
    sqlite3_bind_int64(db->stmt[DBOP_ADDTAGS], idx, fid);

    for (field = fields; *field; field++) {
        item = *field;
        type = *item++;
        idx = sqlite3_bind_parameter_index(db->stmt[DBOP_ADDTAGS], strsep(&item, "="));
        if (idx > 0) {
            if (strcmp(item, "-") == 0)
                *item = '\0';
            if (*item == '\0' && type == 'T')
                type = 0;
            switch (type) {
                case 'I':
                    sqlite3_bind_int64(db->stmt[DBOP_ADDTAGS], idx, strtoull(item, NULL, 10));
                    break;
                case 'T':
                    sqlite3_bind_text(db->stmt[DBOP_ADDTAGS], idx, item, -1, NULL);
                    break;
                default:
                    sqlite3_bind_null(db->stmt[DBOP_ADDTAGS], idx);
                    break;
            }
        }
    }

    return sqlite3_step(db->stmt[DBOP_ADDTAGS]) == SQLITE_DONE ? 0 : -1;
}

/**
 * 读取不同操作码对应的tags
 * @param db      数据库句柄
 * @param mode    数据库模式
 * @param opcode  查找操作码
 * @param pattern 对应操作码的模式
 * @param rows    结果集行数
 * @param cols    结果集列数
 * @return        查找成功返回结果集，否则返回NULL
 */
char **dbreadtags(db_t db, unsigned char mode, unsigned char opcode, const char *pattern, int *rows, int *cols)
{
    char *sql, **table = NULL;

    assert(QUERY_SYMBOL <= opcode && opcode <= QUERY_ASSIGN && db && db->stmt[opcode] && pattern);

    sqlite3_reset(db->stmt[opcode]);
    sqlite3_bind_text(db->stmt[opcode], 1, pattern, -1, NULL);

    if ((sql = sqlite3_expanded_sql(db->stmt[opcode]))) {
        table = dbquery(db, mode, sql, rows, cols);
        sqlite3_free(sql);
    }

    return table;
}

/**
 * 根据路径模式查找文件路径
 * @param db      数据库句柄
 * @param mode    数据库模式
 * @param pattern 查找模式
 * @param rows    结果集行数
 * @param cols    结果集列数
 * @return        查找成功返回结果集，否则返回NULL
 */
char **dbfindpath(db_t db, unsigned char mode, const char *pattern, int *rows, int *cols)
{
    char *sql, **table = NULL;

    assert(db && db->stmt[QUERY_FPATH] && pattern);

    sqlite3_reset(db->stmt[QUERY_FPATH]);
    sqlite3_bind_text(db->stmt[QUERY_FPATH], 1, pattern, -1, NULL);

    if ((sql = sqlite3_expanded_sql(db->stmt[QUERY_FPATH]))) {
        table = dbquery(db, mode, sql, rows, cols);
        sqlite3_free(sql);
    }

    return table;
}

/**
 * 根据where参数查找tags
 * @param db    数据库句柄
 * @param mode  数据库模式
 * @param where 查询条件
 * @param rows  结果集行数
 * @param cols  结果集列数
 * @return      查找成功返回结果集，否则返回NULL
 */
char **dbfindtags(db_t db, unsigned char mode, const char *where, int *rows, int *cols)
{
    char *sql, **table = NULL;

    assert(db && where);

    if ((sql = sqlite3_mprintf(SQL_QUERYTAG "WHERE %s " SQL_TAGSORT, where))) {
        table = dbquery(db, mode, sql, rows, cols);
        sqlite3_free(sql);
    }

    return table;
}

/**
 * 释放由dbreadtags/dbfindpath/dbfindtags返回的table
 * @param table 结果集
 */
void dbfree(char **table)
{
    assert(table);
    sqlite3_free_table(table);
}

/**
 * 打开数据库
 * @param base 打开数据库所处目录
 * @param path 数据库文件路径
 * @param mode 数据库模式
 * @return     打开成功返回数据库句柄，否则返回NULL
 */
db_t dbopen(const char *base, const char *path, unsigned char mode)
{
    int rc;
    db_t db;

    // NOTE: the support for SQL foreign key from 3.6.19, but new version numbering conventions start with 3.9.0
    if (sqlite3_libversion_number() < 3009000 || !path || !(db = (db_t) sqlite3_malloc(sizeof(*db))))
        return NULL;

    memset(db, 0, sizeof(*db));
    db->mode = mode;

    if (!abspath(NULL, base, db->path) && !getcwd(db->path, sizeof(db->path))) {
        sqlite3_free(db);
        return NULL;
    }

    if (sqlite3_open(path, &db->db3) != SQLITE_OK ||
        sqlite3_create_function(db->db3, "MATCH", 2, SQLITE_UTF8, &db->mode, strmatch, NULL, NULL) != SQLITE_OK ||
        sqlite3_create_function(db->db3, "REGEXP", 2, SQLITE_UTF8, &db->mode, strregexp, NULL, NULL) != SQLITE_OK ||
        sqlite3_create_function(db->db3, "ABSPATH", 1, SQLITE_UTF8, db->path, toabspath, NULL, NULL) != SQLITE_OK ||
        sqlite3_create_function(db->db3, "RELPATH", 1, SQLITE_UTF8, db->path, torelpath, NULL, NULL) != SQLITE_OK ||
        sqlite3_exec(db->db3, SQL_INIT, NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_close(db->db3);
        sqlite3_free(db);
        return NULL;
    }

    rc = sqlite3_prepare_v2(db->db3, SQL_ADDTAGS, -1, &db->stmt[DBOP_ADDTAGS], NULL) |
         sqlite3_prepare_v2(db->db3, SQL_SYMBOL, -1, &db->stmt[QUERY_SYMBOL], NULL) |
         sqlite3_prepare_v2(db->db3, SQL_DEFINE, -1, &db->stmt[QUERY_DEFINE], NULL) |
         sqlite3_prepare_v2(db->db3, SQL_CALLER, -1, &db->stmt[QUERY_CALLER], NULL) |
         sqlite3_prepare_v2(db->db3, SQL_REFER, -1, &db->stmt[QUERY_REFER], NULL) |
         sqlite3_prepare_v2(db->db3, SQL_STRING, -1, &db->stmt[QUERY_STRING], NULL) |
         sqlite3_prepare_v2(db->db3, SQL_PATTERN, -1, &db->stmt[QUERY_PATTERN], NULL) |
         sqlite3_prepare_v2(db->db3, SQL_INFILE, -1, &db->stmt[QUERY_INFILE], NULL) |
         sqlite3_prepare_v2(db->db3, SQL_INCLUDE, -1, &db->stmt[QUERY_INCLUDE], NULL) |
         sqlite3_prepare_v2(db->db3, SQL_ASSIGN, -1, &db->stmt[QUERY_ASSIGN], NULL) |
         sqlite3_prepare_v2(db->db3, SQL_FPATH, -1, &db->stmt[QUERY_FPATH], NULL) |
         sqlite3_prepare_v2(db->db3, SQL_ALLFILE, -1, &db->stmt[DBOP_ALLFILE], NULL) |
         sqlite3_prepare_v2(db->db3, SQL_GETFILE, -1, &db->stmt[DBOP_GETFILE], NULL) |
         sqlite3_prepare_v2(db->db3, SQL_SETFILE, -1, &db->stmt[DBOP_SETFILE], NULL) |
         sqlite3_prepare_v2(db->db3, SQL_DELFILE, -1, &db->stmt[DBOP_DELFILE], NULL);

    return rc == SQLITE_OK ? db : (dbclose(db), NULL);
}

/**
 * 关闭数据库
 * @param db 数据库句柄
 * @return   关闭成功返回0，否则返回非0
 */
int dbclose(db_t db)
{
    assert(db && db->db3);

    for (int idx = 0; idx < DBOP_COUNT; idx++) {
        if (db->stmt[idx])
            sqlite3_finalize(db->stmt[idx]);
    }

    return sqlite3_close(db->db3) == SQLITE_OK ? (sqlite3_free(db), 0) : -1;
}