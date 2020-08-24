#include <stdio.h>

#if defined(_WIN32) && !defined(__CYGWIN__)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fcntl.h>

/**
 * 执行子程序
 * @param file 可执行程序文件路径
 * @param argv 执行参数，用户必须保证最后一个元素为NULL
 * @param cwd  设置程序运行目录
 * @param in   子程序输入
 * @param out  子程序输出
 * @return     子进程PID(>0)
 */
int taskexec(const char *file, char *const argv[], const char *cwd, FILE ** in, FILE ** out)
{
    BOOL bFlag;
    DWORD dwPID;
    INT idx, len;
    CHAR *p, *cmd;
    HANDLE ipipe[2], opipe[2];
    STARTUPINFO si = {0};
    PROCESS_INFORMATION pi = {0};
    SECURITY_ATTRIBUTES sa = {0};

    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    for (len = 1, idx = 0; argv[idx]; idx++) {
        len += strlen(argv[idx]) + 3;
    }

    cmd = (char *)malloc(len);
    if (!cmd)
        return -1;

    for (len = idx = 0; argv[idx]; idx++) {
        len += sprintf(cmd + len, "\"%s\" ", argv[idx]);
    }

    if (!CreatePipe(&ipipe[0], &ipipe[1], &sa, 0)) {
        free(cmd);
        return -1;
    }
    if (!CreatePipe(&opipe[0], &opipe[1], &sa, 0)) {
        free(cmd);
        CloseHandle(ipipe[0]);
        CloseHandle(ipipe[0]);
        return -1;
    }
    SetHandleInformation(ipipe[0], HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(opipe[1], HANDLE_FLAG_INHERIT, 0);

    si.cb = sizeof(si);
    si.hStdInput = opipe[0];
    si.hStdOutput = ipipe[1];
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    si.dwFlags = STARTF_USESTDHANDLES;

    bFlag = CreateProcess(file, cmd, NULL, NULL, TRUE, 0, NULL, cwd, &si, &pi);
    dwPID = pi.dwProcessId;

    free(cmd);
    CloseHandle(opipe[0]);
    CloseHandle(ipipe[1]);
    CloseHandle(pi.hThread);

    if (!bFlag) {
        CloseHandle(ipipe[0]);
        CloseHandle(opipe[1]);
        return -1;
    }
    if (!in || !(*in = fdopen(_open_osfhandle((intptr_t)ipipe[0], _O_RDONLY), "r")))
        CloseHandle(ipipe[0]);
    if (!out || !(*out = fdopen(_open_osfhandle((intptr_t)opipe[1], _O_WRONLY), "w")))
        CloseHandle(opipe[1]);

    // 注意windows的pid是32位无符号整型，最低两位都是0
    // 交换最低两位到最高位是为防止转为有符号类型导致为负数的异常情况
    return ((dwPID & 0x03) << 30) | (dwPID >> 2);
}

/**
 * 等待子进程结束
 * @param pid 子进程PID
 * @return    调用结果，0表示成功，非0表示失败
 */
int taskwait(int pid)
{
    DWORD dwPID = (pid << 2) | (((unsigned int)pid >> 30) & 0x03);
    HANDLE hProcess = OpenProcess(0, FALSE, dwPID);
    BOOL bFlag = WaitForSingleObject(hProcess, INFINITE);
    CloseHandle(hProcess);
    return bFlag;
}

#else

#include <unistd.h>
#include <sys/wait.h>

/**
 * 执行子程序
 * @param file 可执行程序文件路径
 * @param argv 执行参数，用户必须保证最后一个元素为NULL
 * @param cwd  设置程序运行目录
 * @param in   子程序输入
 * @param out  子程序输出
 * @return     子进程PID(>0)
 */
int taskexec(const char *file, char *const argv[], const char *cwd, FILE **in, FILE **out)
{
    int pid, ipipe[2], opipe[2];

    if (pipe(ipipe) < 0)
        return -1;

    if (pipe(opipe) < 0) {
        close(ipipe[0]);
        close(ipipe[1]);
        return -1;
    }
    pid = fork();
    if (pid == 0) {
        close(ipipe[0]);
        close(opipe[1]);
        pid = dup2(ipipe[1], STDOUT_FILENO) | dup2(opipe[0], STDIN_FILENO);
        close(ipipe[1]);
        close(opipe[0]);
        if (pid > 0) {
            chdir(cwd);
            execvp(file ? file : argv[0], argv);
        }
    } else {
        close(ipipe[1]);
        close(opipe[0]);
        if (pid < 0) {
            close(ipipe[0]);
            close(opipe[1]);
        } else {
            if (!in || !(*in = fdopen(ipipe[0], "r")))
                close(ipipe[0]);
            if (!out || !(*out = fdopen(opipe[1], "w")))
                close(opipe[1]);
        }
    }

    return pid;
}

/**
 * 等待子进程结束
 * @param pid 子进程PID
 * @return    调用结果，0表示成功，非0表示失败
 */
int taskwait(int pid)
{
    return waitpid(pid, NULL, 0);
}

#endif