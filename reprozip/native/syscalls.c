#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sched.h>
#include <sys/ptrace.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include "config.h"
#include "database.h"
#include "log.h"
#include "ptrace_utils.h"
#include "syscalls.h"
#include "tracer.h"
#include "utils.h"


#ifndef __X32_SYSCALL_BIT
#define __X32_SYSCALL_BIT 0x40000000
#endif
#ifndef SYS_CONNECT
#define SYS_CONNECT 3
#endif
#ifndef SYS_ACCEPT
#define SYS_ACCEPT 5
#endif


#define SYSCALL_I386        0
#define SYSCALL_X86_64      1
#define SYSCALL_X86_64_x32  2


#define verbosity trace_verbosity

struct ExecveInfo {
    char *binary;
    char **argv;
    char **envp;
};


struct syscall_table_entry {
    const char *name;
    int (*proc_entry)(const char*, struct Process *, unsigned int);
    int (*proc_exit)(const char*, struct Process *, unsigned int);
    unsigned int udata;
};

struct syscall_table {
    size_t length;
    struct syscall_table_entry *entries;
};

struct syscall_table *syscall_tables = NULL;


static char *abs_path_arg(const struct Process *process, size_t arg)
{
    char *pathname = tracee_strdup(process->tid, process->params[arg].p);
    if(pathname[0] != '/')
    {
        char *oldpath = pathname;
        pathname = abspath(process->wd, oldpath);
        free(oldpath);
    }
    return pathname;
}


static void print_sockaddr(FILE *stream, void *address, socklen_t addrlen)
{
    const short family = ((struct sockaddr*)address)->sa_family;
    if(family == AF_INET && addrlen >= sizeof(struct sockaddr_in))
    {
        struct sockaddr_in *address_ = address;
        fprintf(stream, "%s:%d",
                inet_ntoa(address_->sin_addr),
                ntohs(address_->sin_port));
    }
    else if(family == AF_INET6
          && addrlen >= sizeof(struct sockaddr_in6))
    {
        struct sockaddr_in6 *address_ = address;
        char buf[50];
        inet_ntop(AF_INET6, &address_->sin6_addr, buf, sizeof(buf));
        fprintf(stream, "[%s]:%d", buf, ntohs(address_->sin6_port));
    }
    else
        fprintf(stream, "<unknown destination, sa_family=%d>", family);
}


/* ********************
 * Other syscalls that might be of interest but that we don't handle yet
 */

static int syscall_unhandled_path1(const char *name, struct Process *process,
                                   unsigned int udata)
{
    if(verbosity >= 1 && process->in_syscall && process->retvalue.i >= 0
     && name != NULL)
    {
        char *pathname = tracee_strdup(process->tid,
                                       process->params[0].p);
        if(pathname[0] != '/')
        {
            char *oldpath = pathname;
            pathname = abspath(process->wd, oldpath);
            free(oldpath);
        }
        log_warn(process->tid, "process used unhandled system call %s(\"%s\")",
                 name, pathname);
    }
    return 0;
}

static int syscall_unhandled_other(const char *name, struct Process *process,
                                   unsigned int udata)
{
    if(verbosity >= 1 && process->in_syscall && process->retvalue.i >= 0
     && name != NULL)
        log_warn(process->tid, "process used unhandled system call %s", name);
    return 0;
}


/* ********************
 * open(), creat(), access()
 */

#define SYSCALL_OPENING_OPEN    1
#define SYSCALL_OPENING_ACCESS  2
#define SYSCALL_OPENING_CREAT   3

static int syscall_fileopening(const char *name, struct Process *process,
                               unsigned int syscall)
{
    unsigned int mode;
    char *pathname = abs_path_arg(process, 0);

    if(syscall == SYSCALL_OPENING_ACCESS)
        mode = FILE_STAT;
    else if(syscall == SYSCALL_OPENING_CREAT)
        mode = flags2mode(process->params[1].u |
                          O_CREAT | O_WRONLY | O_TRUNC);
    else /* syscall == SYSCALL_OPENING_OPEN */
        mode = flags2mode(process->params[1].u);

    if(verbosity >= 3)
    {
        /* Converts mode to string s_mode */
        char mode_buf[42] = "";
        const char *s_mode;
        if(mode & FILE_READ)
            strcat(mode_buf, "|FILE_READ");
        if(mode & FILE_WRITE)
            strcat(mode_buf, "|FILE_WRITE");
        if(mode & FILE_WDIR)
            strcat(mode_buf, "|FILE_WDIR");
        if(mode & FILE_STAT)
            strcat(mode_buf, "|FILE_STAT");
        s_mode = mode_buf[0]?mode_buf + 1:"0";

        if(syscall == SYSCALL_OPENING_OPEN)
            log_debug(process->tid,
                      "open(\"%s\", mode=%s) = %d (%s)",
                      pathname,
                      s_mode,
                      (int)process->retvalue.i,
                      (process->retvalue.i >= 0)?"success":"failure");
        else /* creat or access */
            log_debug(process->tid,
                      "%s(\"%s\") (mode=%s) = %d (%s)",
                      (syscall == SYSCALL_OPENING_OPEN)?"open":
                          (syscall == SYSCALL_OPENING_CREAT)?"creat":"access",
                      pathname,
                      s_mode,
                      (int)process->retvalue.i,
                      (process->retvalue.i >= 0)?"success":"failure");
    }

    if(process->retvalue.i >= 0)
    {
        if(db_add_file_open(process->identifier,
                            pathname,
                            mode,
                            path_is_dir(pathname)) != 0)
            return -1;
    }

    free(pathname);
    return 0;
}


/* ********************
 * stat(), lstat()
 */

static int syscall_filestat(const char *name, struct Process *process,
                        unsigned int udata)
{
    char *pathname = abs_path_arg(process, 0);
    if(process->retvalue.i >= 0)
    {
        if(db_add_file_open(process->identifier,
                            pathname,
                            FILE_STAT,
                            path_is_dir(pathname)) != 0)
            return -1;
    }
    free(pathname);
    return 0;
}


/* ********************
 * readlink()
 */

static int syscall_readlink(const char *name, struct Process *process,
                            unsigned int udata)
{
    char *pathname = abs_path_arg(process, 0);
    if(process->retvalue.i >= 0)
    {
        if(db_add_file_open(process->identifier,
                            pathname,
                            FILE_STAT,
                            0) != 0)
            return -1;
    }
    free(pathname);
    return 0;
}


/* ********************
 * mkdir()
 */

static int syscall_mkdir(const char *name, struct Process *process,
                         unsigned int udata)
{
    char *pathname = abs_path_arg(process, 0);
    if(process->retvalue.i >= 0)
    {
        if(db_add_file_open(process->identifier,
                            pathname,
                            FILE_WRITE,
                            1) != 0)
            return -1;
    }
    return 0;
}


/* ********************
 * symlink()
 */

static int syscall_symlink(const char *name, struct Process *process,
                           unsigned int is_symlinkat)
{
    char *pathname;
    if(is_symlinkat && process->params[1].i != AT_FDCWD)
        return syscall_unhandled_other(name, process, 0);
    else if(is_symlinkat)
        pathname = abs_path_arg(process, 2);
    else /* symlink */
        pathname = abs_path_arg(process, 1);
    if(process->retvalue.i >= 0)
    {
        if(db_add_file_open(process->identifier,
                            pathname,
                            FILE_WRITE,
                            1) != 0)
            return -1;
    }
    return 0;
}


/* ********************
 * chdir()
 */

static int syscall_chdir(const char *name, struct Process *process,
                         unsigned int udata)
{
    char *pathname = abs_path_arg(process, 0);
    if(process->retvalue.i >= 0)
    {
        free(process->wd);
        process->wd = pathname;
        if(db_add_file_open(process->identifier,
                            pathname,
                            FILE_WDIR,
                            1) != 0)
            return -1;
    }
    else
        free(pathname);
    return 0;
}


/* ********************
 * execve()
 */

static int syscall_execve_in(const char *name, struct Process *process,
                             unsigned int udata)
{
    /* int execve(const char *filename,
     *            char *const argv[],
     *            char *const envp[]); */
    struct ExecveInfo *execi = malloc(sizeof(struct ExecveInfo));
    execi->binary = abs_path_arg(process, 0);
    execi->argv = tracee_strarraydup(process->mode, process->tid,
                                     process->params[1].p);
    execi->envp = tracee_strarraydup(process->mode, process->tid,
                                     process->params[2].p);
    if(verbosity >= 3)
    {
        log_debug(process->tid, "execve called:\n  binary=%s\n  argv:",
                  execi->binary);
        {
            /* Note: this conversion is correct and shouldn't need a
             * cast */
            const char *const *v = (const char* const*)execi->argv;
            while(*v)
            {
                log_debug(process->tid, "    %s", *v);
                ++v;
            }
        }
        {
            size_t nb = 0;
            while(execi->envp[nb] != NULL)
                ++nb;
            log_debug(process->tid, "  envp: (%u entries)", (unsigned int)nb);
        }
    }
    process->syscall_info = execi;
    return 0;
}

static int syscall_execve_out(const char *name, struct Process *process,
                              unsigned int execve_syscall)
{
    struct Process *exec_process = process;
    struct ExecveInfo *execi = exec_process->syscall_info;
    if(execi == NULL)
    {
        /* On Linux, execve changes tid to the thread leader's tid, no
         * matter which thread made the call. This means that the process
         * that just returned from execve might not be the one which
         * called.
         * So we start by finding the one which called execve.
         * Possible confusion here if two threads call execve at the same
         * time, but that would be very bad code. */
        size_t i;
        for(i = 0; i < processes_size; ++i)
        {
            if(processes[i]->status == PROCESS_ATTACHED
             && processes[i]->tgid == process->tgid
             && processes[i]->in_syscall
             && processes[i]->current_syscall == (int)execve_syscall
             && processes[i]->syscall_info != NULL)
            {
                exec_process = processes[i];
                break;
            }
        }
        if(exec_process == NULL)
        {
            /* LCOV_EXCL_START : internal error */
            log_critical(process->tid,
                         "execve() completed but call wasn't recorded");
            return -1;
            /* LCOV_EXCL_END */
        }
        execi = exec_process->syscall_info;

        /* The process that called execve() disappears without any trace */
        if(db_add_exit(exec_process->identifier, 0) != 0)
            return -1;
        free(exec_process->wd);
        exec_process->status = PROCESS_FREE;
    }
    if(process->retvalue.i >= 0)
    {
        /* Note: execi->argv needs a cast to suppress a bogus warning
         * While conversion from char** to const char** is invalid,
         * conversion from char** to const char*const* is, in fact, safe.
         * G++ accepts it, GCC issues a warning. */
        if(db_add_exec(process->identifier, execi->binary,
                       (const char *const*)execi->argv,
                       (const char *const*)execi->envp,
                       process->wd) != 0)
            return -1;
        /* Note that here, the database records that the thread leader
         * called execve, instead of thread exec_process->tid. */
        if(verbosity >= 2)
            log_info(exec_process->tid, "successfully exec'd %s",
                     execi->binary);
        /* Process will get SIGTRAP with PTRACE_EVENT_EXEC */
        if(trace_add_files_from_proc(process->identifier, process->tid,
                                     execi->binary) != 0)
            return -1;
    }

    free_strarray(execi->argv);
    free_strarray(execi->envp);
    free(execi->binary);
    free(execi);
    exec_process->syscall_info = NULL;
    return 0;
}


/* ********************
 * fork(), clone(), ...
 */

#define SYSCALL_FORK_FORK   1
#define SYSCALL_FORK_VFORK  2
#define SYSCALL_FORK_CLONE  3

static int syscall_forking(const char *name, struct Process *process,
                           unsigned int syscall)
{
#ifndef CLONE_THREAD
#define CLONE_THREAD 0x00010000
#endif
    if(process->retvalue.i > 0)
    {
        int is_thread = 0;
        pid_t new_tid = process->retvalue.i;
        struct Process *new_process;
        if(syscall == SYSCALL_FORK_CLONE)
            is_thread = process->params[0].u & CLONE_THREAD;
        if(verbosity >= 2)
            log_info(new_tid, "process created by %d via %s\n"
                     "    (thread: %s) (working directory: %s)",
                     process->tid,
                     (syscall == SYSCALL_FORK_FORK)?"fork()":
                     (syscall == SYSCALL_FORK_VFORK)?"vfork()":
                     "clone()",
                     is_thread?"yes":"no",
                     process->wd);

        /* At this point, the process might have been seen by waitpid in
         * trace() or not. */
        new_process = trace_find_process(new_tid);
        if(new_process != NULL)
        {
            /* Process has been seen before and options were set */
            if(new_process->status != PROCESS_UNKNOWN)
            {
                /* LCOV_EXCL_START : internal error */
                log_critical(new_tid,
                             "just created process that is already running "
                             "(status=%d)", new_process->status);
                return -1;
                /* LCOV_EXCL_END */
            }
            new_process->status = PROCESS_ATTACHED;
            ptrace(PTRACE_SYSCALL, new_process->tid, NULL, NULL);
            if(verbosity >= 2)
            {
                unsigned int nproc, unknown;
                trace_count_processes(&nproc, &unknown);
                log_info(0, "%d processes (inc. %d unattached)",
                         nproc, unknown);
            }
        }
        else
        {
            /* Process hasn't been seen before (syscall returned first) */
            new_process = trace_get_empty_process();
            new_process->status = PROCESS_ALLOCATED;
            /* New process gets a SIGSTOP, but we resume on attach */
            new_process->tid = new_tid;
            new_process->in_syscall = 0;
        }
        if(is_thread)
            new_process->tgid = process->tgid;
        else
            new_process->tgid = new_process->tid;
        new_process->wd = strdup(process->wd);

        /* Parent will also get a SIGTRAP with PTRACE_EVENT_FORK */

        if(db_add_process(&new_process->identifier,
                          process->identifier,
                          process->wd) != 0)
            return -1;
    }
    return 0;
}


/* ********************
 * Network connections
 */

static int handle_accept(struct Process *process,
                         void *arg1, void *arg2)
{
    socklen_t addrlen;
    tracee_read(process->tid, (void*)&addrlen, arg2, sizeof(addrlen));
    if(addrlen >= sizeof(short))
    {
        void *address = malloc(addrlen);
        tracee_read(process->tid, address, arg1, addrlen);
        log_warn_(process->tid, "process accepted a connection from ");
        print_sockaddr(stderr, address, addrlen);
        fprintf(stderr, "\n");
        free(address);
    }
    return 0;
}

static int handle_connect(struct Process *process,
                          void *arg1, socklen_t addrlen)
{
    if(addrlen >= sizeof(short))
    {
        void *address = malloc(addrlen);
        tracee_read(process->tid, address, arg1, addrlen);
        log_warn_(process->tid, "process connected to ");
        print_sockaddr(stderr, address, addrlen);
        fprintf(stderr, "\n");
        free(address);
    }
    return 0;
}

static int syscall_socketcall(const char *name, struct Process *process,
                              unsigned int udata)
{
    /* Argument 1 is an array of longs, which are either numbers of pointers */
    uint64_t args = process->params[1].u;
    /* Size of each element in the array */
    const size_t wordsize = tracee_getwordsize(process->mode);
    /* Note that void* pointer arithmetic is illegal, hence the uint */
    if(process->params[0].u == SYS_ACCEPT)
        return handle_accept(process,
                             tracee_getptr(process->mode, process->tid,
                                           (void*)(args + 1*wordsize)),
                             tracee_getptr(process->mode, process->tid,
                                           (void*)(args + 2*wordsize)));
    else if(process->params[0].u == SYS_CONNECT)
        return handle_connect(process,
                              tracee_getptr(process->mode, process->tid,
                                            (void*)(args + 1*wordsize)),
                              tracee_getlong(process->mode, process->tid,
                                             (void*)(args + 2*wordsize)));
    else
        return 0;
}

static int syscall_accept(const char *name, struct Process *process,
                          unsigned int udata)
{
    return handle_accept(process, process->params[1].p, process->params[2].p);
}

static int syscall_connect(const char *name, struct Process *process,
                           unsigned int udata)
{
    return handle_connect(process, process->params[1].p, process->params[2].u);
}


/* ********************
 * *at variants, handled if dirfd is AT_FDCWD
 */
static int syscall_xxx_at(const char *name, struct Process *process,
                          unsigned int real_syscall)
{
    if(process->params[0].i == AT_FDCWD)
    {
        struct syscall_table_entry *entry = NULL;
        struct syscall_table *tbl;
        size_t syscall_type;
        if(process->mode == MODE_I386)
            syscall_type = SYSCALL_I386;
        else if(process->current_syscall & __X32_SYSCALL_BIT)
            syscall_type = SYSCALL_X86_64_x32;
        else
            syscall_type = SYSCALL_X86_64;
        tbl = &syscall_tables[syscall_type];
        if(real_syscall < tbl->length)
            entry = &tbl->entries[real_syscall];
        if(entry == NULL || entry->name == NULL || entry->proc_exit == NULL)
        {
            log_critical(process->tid, "INVALID SYSCALL in *at dispatch: %d",
                         real_syscall);
            return 0;
        }
        else
        {
            int ret;
            /* Shifts arguments */
            size_t i;
            register_type arg0 = process->params[0];
            for(i = 0; i < PROCESS_ARGS - 1; ++i)
                process->params[i] = process->params[i + 1];
            ret = entry->proc_exit(name, process, entry->udata);
            for(i = PROCESS_ARGS; i > 1; --i)
                process->params[i - 1] = process->params[i - 2];
            process->params[0] = arg0;
            return ret;
        }
    }
    else
        return syscall_unhandled_other(name, process, 0);
}


/* ********************
 * Building the syscall table
 */

struct unprocessed_table_entry {
    unsigned int n;
    const char *name;
    int (*proc_entry)(const char*, struct Process *, unsigned int);
    int (*proc_exit)(const char*, struct Process *, unsigned int);
    unsigned int udata;

};

struct syscall_table *process_table(struct syscall_table *table,
                                    const struct unprocessed_table_entry *orig)
{
    size_t i, length = 0;
    const struct unprocessed_table_entry *pos;

    /* Measure required table */
    pos = orig;
    while(pos->proc_entry || pos->proc_exit)
    {
        if(pos->n + 1 > length)
            length = pos->n + 1;
        ++pos;
    }

    /* Allocate table */
    table->length = length;
    table->entries = malloc(sizeof(struct syscall_table_entry) * length);

    /* Initialize to NULL */
    for(i = 0; i < length; ++i)
    {
        table->entries[i].name = NULL;
        table->entries[i].proc_entry = NULL;
        table->entries[i].proc_exit = NULL;
    }

    /* Copy from unordered list */
    {
        pos = orig;
        while(pos->proc_entry || pos->proc_exit)
        {
            table->entries[pos->n].name = pos->name;
            table->entries[pos->n].proc_entry = pos->proc_entry;
            table->entries[pos->n].proc_exit = pos->proc_exit;
            table->entries[pos->n].udata = pos->udata;
            ++pos;
        }
    }

    return table;
}

void syscall_build_table(void)
{
    if(syscall_tables != NULL)
        return ;

#if defined(I386)
    syscall_tables = malloc(1 * sizeof(struct syscall_table));
#elif defined(X86_64)
    syscall_tables = malloc(3 * sizeof(struct syscall_table));
#else
#   error Unrecognized architecture!
#endif

    /* i386 */
    {
        struct unprocessed_table_entry list[] = {
            {  5, "open", NULL, syscall_fileopening, SYSCALL_OPENING_OPEN},
            {  8, "creat", NULL, syscall_fileopening, SYSCALL_OPENING_CREAT},
            { 33, "access", NULL, syscall_fileopening, SYSCALL_OPENING_ACCESS},

            {106, "stat", NULL, syscall_filestat, 0},
            {107, "lstat", NULL, syscall_filestat, 0},
            {195, "stat64", NULL, syscall_filestat, 0},
            { 18, "oldstat", NULL, syscall_filestat, 0},
            {196, "lstat64", NULL, syscall_filestat, 0},
            { 84, "oldlstat", NULL, syscall_filestat, 0},

            { 85, "readlink", NULL, syscall_readlink, 0},

            { 39, "mkdir", NULL, syscall_mkdir, 0},

            { 83, "symlink", NULL, syscall_symlink, 0},

            { 12, "chdir", NULL, syscall_chdir, 0},

            { 11, "execve", syscall_execve_in, syscall_execve_out, 11},

            {  2, "fork", NULL, syscall_forking, SYSCALL_FORK_FORK},
            {190, "vfork", NULL, syscall_forking, SYSCALL_FORK_VFORK},
            {120, "clone", NULL, syscall_forking, SYSCALL_FORK_CLONE},

            {102, "socketcall", NULL, syscall_socketcall, 0},

            /* Half-implemented: *at() variants, when dirfd is AT_FDCWD */
            {296, "mkdirat", NULL, syscall_xxx_at, 39},
            {295, "openat", NULL, syscall_xxx_at, 5},
            {307, "faccessat", NULL, syscall_xxx_at, 33},
            {305, "readlinkat", NULL, syscall_xxx_at, 85},
            {300, "fstatat64", NULL, syscall_xxx_at, 195},

            {304, "symlinkat", NULL, syscall_symlink, 1},

            /* Unhandled with path as first argument */
            { 38, "rename", NULL, syscall_unhandled_path1, 0},
            { 40, "rmdir", NULL, syscall_unhandled_path1, 0},
            {  9, "link", NULL, syscall_unhandled_path1, 0},
            { 92, "truncate", NULL, syscall_unhandled_path1, 0},
            {193, "truncate64", NULL, syscall_unhandled_path1, 0},
            { 10, "unlink", NULL, syscall_unhandled_path1, 0},
            { 15, "chmod", NULL, syscall_unhandled_path1, 0},
            {182, "chown", NULL, syscall_unhandled_path1, 0},
            {212, "chown32", NULL, syscall_unhandled_path1, 0},
            { 16, "lchown", NULL, syscall_unhandled_path1, 0},
            {198, "lchown32", NULL, syscall_unhandled_path1, 0},
            { 30, "utime", NULL, syscall_unhandled_path1, 0},
            {271, "utimes", NULL, syscall_unhandled_path1, 0},
            {277, "mq_open", NULL, syscall_unhandled_path1, 0},
            {278, "mq_unlink", NULL, syscall_unhandled_path1, 0},

            /* Unhandled which use open descriptors */
            {303, "linkat", NULL, syscall_unhandled_other, 0},
            {302, "renameat", NULL, syscall_unhandled_other, 0},
            {301, "unlinkat", NULL, syscall_unhandled_other, 0},
            {306, "fchmodat", NULL, syscall_unhandled_other, 0},
            {298, "fchownat", NULL, syscall_unhandled_other, 0},

            /* Other unhandled */
            { 26, "ptrace", NULL, syscall_unhandled_other, 0},
            {341, "name_to_handle_at", NULL, syscall_unhandled_other, 0},

            /* Sentinel */
            {0, NULL, NULL, NULL, 0}
        };
        process_table(&syscall_tables[SYSCALL_I386], list);
    }

#ifdef X86_64
    /* x64 */
    {
        struct unprocessed_table_entry list[] = {
            {  2, "open", NULL, syscall_fileopening, SYSCALL_OPENING_OPEN},
            { 85, "creat", NULL, syscall_fileopening, SYSCALL_OPENING_CREAT},
            { 21, "access", NULL, syscall_fileopening, SYSCALL_OPENING_ACCESS},

            {  4, "stat", NULL, syscall_filestat, 0},
            {  6, "lstat", NULL, syscall_filestat, 0},

            { 89, "readlink", NULL, syscall_readlink, 0},

            { 83, "mkdir", NULL, syscall_mkdir, 0},

            { 88, "symlink", NULL, syscall_symlink, 0},

            { 80, "chdir", NULL, syscall_chdir, 0},

            { 59, "execve", syscall_execve_in, syscall_execve_out, 59},

            { 57, "fork", NULL, syscall_forking, SYSCALL_FORK_FORK},
            { 58, "vfork", NULL, syscall_forking, SYSCALL_FORK_VFORK},
            { 56, "clone", NULL, syscall_forking, SYSCALL_FORK_CLONE},

            { 43, "accept", NULL, syscall_accept, 0},
            {288, "accept4", NULL, syscall_accept, 0},
            { 42, "connect", NULL, syscall_connect, 0},

            /* Half-implemented: *at() variants, when dirfd is AT_FDCWD */
            {258, "mkdirat", NULL, syscall_xxx_at, 83},
            {257, "openat", NULL, syscall_xxx_at, 2},
            {269, "faccessat", NULL, syscall_xxx_at, 21},
            {267, "readlinkat", NULL, syscall_xxx_at, 89},
            {262, "newfstatat", NULL, syscall_xxx_at, 4},

            {266, "symlinkat", NULL, syscall_symlink, 1},

            /* Unhandled with path as first argument */
            { 82, "rename", NULL, syscall_unhandled_path1, 0},
            { 84, "rmdir", NULL, syscall_unhandled_path1, 0},
            { 86, "link", NULL, syscall_unhandled_path1, 0},
            { 76, "truncate", NULL, syscall_unhandled_path1, 0},
            { 87, "unlink", NULL, syscall_unhandled_path1, 0},
            { 90, "chmod", NULL, syscall_unhandled_path1, 0},
            { 92, "chown", NULL, syscall_unhandled_path1, 0},
            { 94, "lchown", NULL, syscall_unhandled_path1, 0},
            {132, "utime", NULL, syscall_unhandled_path1, 0},
            {235, "utimes", NULL, syscall_unhandled_path1, 0},
            {240, "mq_open", NULL, syscall_unhandled_path1, 0},
            {241, "mq_unlink", NULL, syscall_unhandled_path1, 0},

            /* Unhandled which use open descriptors */
            {265, "linkat", NULL, syscall_unhandled_other, 0},
            {264, "renameat", NULL, syscall_unhandled_other, 0},
            {263, "unlinkat", NULL, syscall_unhandled_other, 0},
            {268, "fchmodat", NULL, syscall_unhandled_other, 0},
            {260, "fchownat", NULL, syscall_unhandled_other, 0},

            /* Other unhandled */
            {101, "ptrace", NULL, syscall_unhandled_other, 0},
            {303, "name_to_handle_at", NULL, syscall_unhandled_other, 0},

            /* Sentinel */
            {0, NULL, NULL, NULL, 0}
        };
        process_table(&syscall_tables[SYSCALL_X86_64], list);
    }

    /* x32 */
    {
        struct unprocessed_table_entry list[] = {
            {  2, "open", NULL, syscall_fileopening, SYSCALL_OPENING_OPEN},
            { 85, "creat", NULL, syscall_fileopening, SYSCALL_OPENING_CREAT},
            { 21, "access", NULL, syscall_fileopening, SYSCALL_OPENING_ACCESS},

            {  4, "stat", NULL, syscall_filestat, 0},
            {  6, "lstat", NULL, syscall_filestat, 0},

            { 89, "readlink", NULL, syscall_readlink, 0},

            { 83, "mkdir", NULL, syscall_mkdir, 0},

            { 88, "symlink", NULL, syscall_symlink, 0},

            { 80, "chdir", NULL, syscall_chdir, 0},

            {520, "execve", syscall_execve_in, syscall_execve_out,
                     __X32_SYSCALL_BIT + 520},

            { 57, "fork", NULL, syscall_forking, SYSCALL_FORK_FORK},
            { 58, "vfork", NULL, syscall_forking, SYSCALL_FORK_VFORK},
            { 56, "clone", NULL, syscall_forking, SYSCALL_FORK_CLONE},

            { 43, "accept", NULL, syscall_accept, 0},
            {288, "accept4", NULL, syscall_accept, 0},
            { 42, "connect", NULL, syscall_connect, 0},

            /* Half-implemented: *at() variants, when dirfd is AT_FDCWD */
            {258, "mkdirat", NULL, syscall_xxx_at, 83},
            {257, "openat", NULL, syscall_xxx_at, 2},
            {269, "faccessat", NULL, syscall_xxx_at, 21},
            {267, "readlinkat", NULL, syscall_xxx_at, 89},
            {262, "newfstatat", NULL, syscall_xxx_at, 4},

            {266, "symlinkat", NULL, syscall_symlink, 1},

            /* Unhandled with path as first argument */
            { 82, "rename", NULL, syscall_unhandled_path1, 0},
            { 84, "rmdir", NULL, syscall_unhandled_path1, 0},
            { 86, "link", NULL, syscall_unhandled_path1, 0},
            { 76, "truncate", NULL, syscall_unhandled_path1, 0},
            { 87, "unlink", NULL, syscall_unhandled_path1, 0},
            { 90, "chmod", NULL, syscall_unhandled_path1, 0},
            { 92, "chown", NULL, syscall_unhandled_path1, 0},
            { 94, "lchown", NULL, syscall_unhandled_path1, 0},
            {132, "utime", NULL, syscall_unhandled_path1, 0},
            {235, "utimes", NULL, syscall_unhandled_path1, 0},
            {240, "mq_open", NULL, syscall_unhandled_path1, 0},
            {241, "mq_unlink", NULL, syscall_unhandled_path1, 0},

            /* Unhandled which use open descriptors */
            {265, "linkat", NULL, syscall_unhandled_other, 0},
            {264, "renameat", NULL, syscall_unhandled_other, 0},
            {263, "unlinkat", NULL, syscall_unhandled_other, 0},
            {268, "fchmodat", NULL, syscall_unhandled_other, 0},
            {260, "fchownat", NULL, syscall_unhandled_other, 0},

            /* Other unhandled */
            {521, "ptrace", NULL, syscall_unhandled_other, 0},
            {303, "name_to_handle_at", NULL, syscall_unhandled_other, 0},

            /* Sentinel */
            {0, NULL, NULL, NULL, 0}
        };
        process_table(&syscall_tables[SYSCALL_X86_64_x32], list);
    }
#endif
}


/* ********************
 * Handle a syscall via the table
 */

int syscall_handle(struct Process *process)
{
    pid_t tid = process->tid;
    const int syscall = process->current_syscall & ~__X32_SYSCALL_BIT;
    size_t syscall_type;
    if(process->mode == MODE_I386)
    {
        syscall_type = SYSCALL_I386;
        if(verbosity >= 4)
            log_debug(process->tid, "syscall %d (i386)", syscall);
    }
    else if(process->current_syscall & __X32_SYSCALL_BIT)
    {
        /* LCOV_EXCL_START : x32 is not supported right now */
        syscall_type = SYSCALL_X86_64_x32;
        if(verbosity >= 4)
            log_debug(process->tid, "syscall %d (x32)", syscall);
        /* LCOV_EXCL_END */
    }
    else
    {
        syscall_type = SYSCALL_X86_64;
        if(verbosity >= 4)
            log_debug(process->tid, "syscall %d (x64)", syscall);
    }

    {
        struct syscall_table_entry *entry = NULL;
        struct syscall_table *tbl = &syscall_tables[syscall_type];
        if(syscall < 0 || syscall >= 2000)
            log_error(process->tid, "INVALID SYSCALL %d", syscall);
#ifdef X86_64
        /* Workaround for execve() transition x64 -> i386 */
        if(syscall == 59 && process->in_syscall)
        {
            size_t i;
            for(i = 0; i < processes_size; ++i)
            {
                if(processes[i]->status == PROCESS_ATTACHED
                 && processes[i]->tgid == process->tgid
                 && processes[i]->in_syscall
                 && processes[i]->current_syscall == 59
                 && processes[i]->syscall_info != NULL)
                {
                    if(verbosity >= 3)
                        log_debug(process->tid,
                                  "transition x64 -> i386, syscall 59 is still "
                                  "execve");
                    entry = &syscall_tables[SYSCALL_X86_64].entries[59];
                }
            }
        }
        /* Workaround for execve() transition i386 -> x64 */
        else if(syscall == 11 && process->in_syscall)
        {
            size_t i;
            for(i = 0; i < processes_size; ++i)
            {
                if(processes[i]->status == PROCESS_ATTACHED
                 && processes[i]->tgid == process->tgid
                 && processes[i]->in_syscall
                 && processes[i]->current_syscall == 11
                 && processes[i]->syscall_info != NULL)
                {
                    if(verbosity >= 3)
                        log_debug(process->tid,
                                  "transition i386 -> x64, syscall 11 is still "
                                  "execve");
                    entry = &syscall_tables[SYSCALL_I386].entries[11];
                }
            }
        }
        else
#endif
        if(entry == NULL && (syscall >= 0 || (size_t)syscall < tbl->length) )
            entry = &tbl->entries[syscall];
        if(entry != NULL)
        {
            int ret = 0;
            if(entry->name && verbosity >= 3)
                log_debug(process->tid, "%s()", entry->name);
            if(!process->in_syscall && entry->proc_entry)
                ret = entry->proc_entry(entry->name, process, entry->udata);
            else if(process->in_syscall && entry->proc_exit)
                ret = entry->proc_exit(entry->name, process, entry->udata);
            if(ret != 0)
                return -1;
        }
    }

    /* Run to next syscall */
    if(process->in_syscall)
    {
        process->in_syscall = 0;
        process->current_syscall = -1;
        process->syscall_info = NULL;
    }
    else
        process->in_syscall = 1;
    ptrace(PTRACE_SYSCALL, tid, NULL, NULL);

    return 0;
}
