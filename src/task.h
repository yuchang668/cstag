#ifndef CSTAG_TASK_H
#define CSTAG_TASK_H

int taskexec(const char *file, char *const argv[], const char *cwd, FILE **in, FILE **out);

int taskwait(int pid);

#endif //CSTAG_TASK_H