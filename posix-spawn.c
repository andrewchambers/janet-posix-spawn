#ifdef __linux__
#define _GNU_SOURCE
#else
#define _POSIX_C_SOURCE 200809L
#endif

#include <fcntl.h>
#include <unistd.h>
#include <spawn.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <janet.h>

typedef struct {
    pid_t pid;
    int close_signal;
    int exited;
    int wstatus;
} Process;

/*
   Get a process exit code, the process must have had process_wait called.
   Returns -1 and sets errno on error, otherwise returns the exit code.
*/
static int process_exit_code(Process *p) {
    if (!p->exited || p->pid == -1) {
        errno = EINVAL;
        return -1;
    }

    int exit_code = 0;

    if (WIFEXITED(p->wstatus)) {
        exit_code = WEXITSTATUS(p->wstatus);
    } else if (WIFSIGNALED(p->wstatus)) {
        // Should this be a function of the signal?
        exit_code = 129;
    } else {
        /* This should be unreachable afaik */
        errno = EINVAL;
        return -1;
    }

    return exit_code;
}

/*
   Returns -1 and sets errno on error, otherwise returns the process exit code.
*/

static int process_wait(Process *p, int *exit, int flags) {
    int _exit = 0;
    if (!exit)
        exit = &_exit;

    if (p->pid == -1) {
        errno = EINVAL;
        return -1;
    }

    if (p->exited) {
        *exit = process_exit_code(p);
        return 0;
    }

    int err;

    do {
        err = waitpid(p->pid, &p->wstatus, flags);
    } while (err < 0 && errno == EINTR);

    if (err < 0)
        return -1;

    if ((flags & WNOHANG && err == 0)) {
        *exit = -1;
        return 0;
    }

    p->exited = 1;
    *exit = process_exit_code(p);
    return 0;
}

static int process_signal(Process *p, int sig) {
    int err;

    if (p->exited || p->pid == -1)
        return 0;

    do {
        err = kill(p->pid, sig);
    } while (err < 0 && errno == EINTR);

    if (err < 0)
        return -1;

    return 0;
}

static int process_gc(void *ptr, size_t s) {
    (void)s;
    int err;

    Process *p = (Process *)ptr;
    if (!p->exited && p->pid != -1) {
        do {
            err = kill(p->pid, p->close_signal);
        } while (err < 0 && errno == EINTR);
        if (process_wait(p, NULL, 0) < 0) {
            /* Not much we can do here. */
            p->exited = 1;
        }
    }
    return 0;
}


static Janet pspawn_close(int32_t argc, Janet *argv);
static Janet pspawn_wait(int32_t argc, Janet *argv);
static Janet pspawn_signal(int32_t argc, Janet *argv);

static JanetMethod process_methods[] = {
    {"close", pspawn_close}, /* So processes can be used with 'with' */
    {"wait",  pspawn_wait},
    {"signal", pspawn_signal},
    {NULL, NULL}
};

static int process_get(void *ptr, Janet key, Janet *out) {
    Process *p = (Process *)ptr;

    if (!janet_checktype(key, JANET_KEYWORD))
        return 0;

    if (janet_keyeq(key, "pid")) {
        *out = (p->pid == -1) ? janet_wrap_nil() : janet_wrap_integer(p->pid);
        return 1;
    }

    if (janet_keyeq(key, "exit-code")) {
        int exit_code;

        if (process_wait(p, &exit_code, WNOHANG) != 0)
            janet_panicf("error checking exit status: %s", strerror(errno));

        *out = (exit_code == -1) ? janet_wrap_nil() : janet_wrap_integer(exit_code);
        return 1;
    }

    return janet_getmethod(janet_unwrap_keyword(key), process_methods, out);
}

static const JanetAbstractType process_type = {
    "posix-spawn/process", process_gc, NULL, process_get, JANET_ATEND_GET
};

static const char *arg_string(Janet v) {
    switch (janet_type(v)) {
    case JANET_STRING:
        return (const char *)janet_unwrap_string(v);
    case JANET_SYMBOL:
        return (const char *)janet_unwrap_symbol(v);
    default:
        return NULL;
    }
}

static Janet primitive_pspawn(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 8);

    int want_errorf;
    char *error_msg;
    Janet error_ctx;
    const char *pcmd;
    char **pargv;
    char **penviron;
    posix_spawnattr_t attr;
    posix_spawnattr_t *pattr;
    posix_spawn_file_actions_t file_actions;
    posix_spawn_file_actions_t *pfile_actions;

    Process *p = (Process *)janet_abstract(&process_type, sizeof(Process));

    p->close_signal = SIGTERM;
    p->pid = -1;
    p->exited = 1;
    p->wstatus = 0;

    /* This function does not panic, it only has one exit point to simplify cleanup. */
#define PSPAWN_ERROR(M) do { error_msg = M; goto done; } while (0);
#define PSPAWN_ERRORF(M, V) do { want_errorf = 1; error_msg = M; error_ctx = V; goto done; } while (0);
    want_errorf = 0;
    error_msg = NULL;
    error_ctx = janet_wrap_nil();

    pcmd = NULL;
    pargv = NULL;
    penviron = NULL;

    pattr = NULL;
    pfile_actions = NULL;

    sigset_t sig_dflt_set;
    sigset_t sig_mask_set;

    if (posix_spawnattr_init(&attr) != 0) {
        PSPAWN_ERROR("unable to init attr set");
    }
    pattr = &attr;

    if (posix_spawn_file_actions_init(&file_actions) != 0) {
        PSPAWN_ERROR("unable to init file actions set");
    }
    pfile_actions = &file_actions;


    pcmd = arg_string(argv[0]);
    if (!pcmd)
        PSPAWN_ERRORF("%v is not a valid command", argv[0]);

    JanetView args = janet_getindexed(argv, 1);

    pargv = calloc(args.len + 1, sizeof(char *));
    if (!pargv)
        PSPAWN_ERROR("no memory");

    for (size_t i = 0; i < (size_t)args.len; i++) {
        pargv[i] = (char *)arg_string(args.items[i]);
        if (!pcmd)
            PSPAWN_ERRORF("%v is not a valid argument", args.items[i]);
    }

    if (!janet_checktype(argv[2], JANET_NUMBER))
        PSPAWN_ERROR("close signal must be a number");

    int close_signal_int = (int)janet_unwrap_number(argv[2]);
    if (close_signal_int == -1)
        PSPAWN_ERROR("invalid value for :close-signal");

    p->close_signal = close_signal_int;


    JanetView jfile_actions;

    if (!janet_checktype(argv[3], JANET_NIL)) {
        if (!janet_indexed_view(argv[3], &jfile_actions.items, &jfile_actions.len))
            PSPAWN_ERROR("file action elements must be an indexed type");

        for (int i = 0; i < jfile_actions.len; i++) {
            Janet t = jfile_actions.items[i];

            JanetView r;

            if (!janet_indexed_view(t, &r.items, &r.len))
                PSPAWN_ERROR("file action elements must be an indexed type");

            if (r.len < 1)
                PSPAWN_ERROR("file action elements must be at least one element");

            if (janet_keyeq(r.items[0], "dup2")) {

                if (r.len != 3)
                    PSPAWN_ERROR("dup2 file actions have 2 files elements");

                for (int j = 1; j <= 2; j++)
                    if (!janet_checkfile(r.items[j]))
                        PSPAWN_ERRORF(":dup2 value must be a file, got %v", r.items[j]);

                if (posix_spawn_file_actions_adddup2(pfile_actions, fileno(janet_unwrapfile(r.items[1], NULL)), fileno(janet_unwrapfile(r.items[2], NULL))) != 0)
                    PSPAWN_ERROR(":dup2 file action unable to determine fileno");

            } else if (janet_keyeq(r.items[0], "close")) {

                if (r.len != 2)
                    PSPAWN_ERROR(":close file actions have 1 file");

                if (!janet_checkfile(r.items[1]))
                    PSPAWN_ERRORF(":close value must be a file, got %v", r.items[1]);

                if (posix_spawn_file_actions_addclose(pfile_actions, fileno(janet_unwrapfile(r.items[1], NULL))) != 0)
                    PSPAWN_ERROR(":close file action unable to determine fileno");

            } else {
                PSPAWN_ERRORF("%v is not a valid file action", r.items[0]);
            }
        }
    }

    int32_t nenviron = 0;

    if (!janet_checktype(argv[4], JANET_NIL)) {

        JanetDictView env;

        if (!janet_dictionary_view(argv[4], &env.kvs, &env.len, &env.cap))
            PSPAWN_ERRORF("env must be a dictionary, got %v", argv[4]);

        penviron = calloc((env.len + 1), sizeof(char *));
        if (!penviron)
            PSPAWN_ERROR("no memory");

        for (int32_t i = 0; i < env.cap; i++) {
            const JanetKV *kv = env.kvs + i;

            if (janet_checktype(kv->key, JANET_NIL))
                continue;

            if (!janet_checktype(kv->key, JANET_STRING))
                PSPAWN_ERROR("environ key is not a string");

            if (!janet_checktype(kv->value, JANET_STRING))
                PSPAWN_ERROR("environ value is not a string");

            const uint8_t *keys = janet_unwrap_string(kv->key);
            const uint8_t *vals = janet_unwrap_string(kv->value);
            size_t klen = janet_string_length(keys);
            size_t vlen = janet_string_length(vals);

            if (strlen((char *)keys) != klen)
                PSPAWN_ERROR("environ keys cannot have embedded nulls");

            if (strlen((char *)vals) != vlen)
                PSPAWN_ERROR("environ values cannot have embedded nulls");

            char *envitem = malloc(klen + vlen + 2);
            if (!envitem)
                PSPAWN_ERROR("no memory");
            memcpy(envitem, keys, klen);
            envitem[klen] = '=';
            memcpy(envitem + klen + 1, vals, vlen);
            envitem[klen + vlen + 1] = 0;
            penviron[nenviron++] = envitem;
        }
    }

    if (!janet_checktype(argv[5], JANET_NUMBER)) {
        PSPAWN_ERRORF("attr flags must be a number, got %v", argv[5]);
    }
    if (posix_spawnattr_setflags(pattr, janet_unwrap_number(argv[5])) != 0) {
        PSPAWN_ERROR("unable to set spawn attr flags");
    }

    if (sigemptyset(&sig_dflt_set) != 0 || sigemptyset(&sig_mask_set) != 0) {
        PSPAWN_ERROR("unable to init signal masks");
    }

    JanetView sigs;

#define SIGSETARG(S, N) \
    do { \
        if (janet_checktype(argv[N], JANET_NIL)) { \
            sigemptyset(&S); \
        } else if (janet_keyeq(argv[N], "all")) { \
            sigfillset(&S); \
        } else if (janet_indexed_view(argv[N], &sigs.items, &sigs.len)) { \
            for (int32_t i = 0; i < sigs.len; i++) { \
                if (!janet_checktype(sigs.items[i], JANET_NUMBER)) \
                    PSPAWN_ERRORF("signal must be a number, got %v", sigs.items[i]); \
                sigaddset(&S, janet_unwrap_number(sigs.items[i])); \
            } \
        } \
    } while (0)

    SIGSETARG(sig_dflt_set, 6);
    SIGSETARG(sig_mask_set, 7);
#undef SIGSETARG

    if (posix_spawnattr_setsigdefault(pattr, &sig_dflt_set) != 0) {
        PSPAWN_ERROR("unable to sig default");
    }

    if (posix_spawnattr_setsigmask(pattr, &sig_mask_set) != 0) {
        PSPAWN_ERROR("unable to set sig mask");
    }

    if (posix_spawnp(&p->pid, pcmd, pfile_actions, pattr, pargv, penviron) != 0) {
        p->pid = -1;
        PSPAWN_ERRORF("spawn failed: %v", janet_cstringv(strerror(errno)));
    }
    p->exited = 0;

done:

    if (penviron) {
        for (int32_t i = nenviron-1; i >= 0; i--) {
            free(penviron[i]);
        }
        free(penviron);
    }

    if (pargv) {
        free(pargv);
    }

    if (pfile_actions) {
        posix_spawn_file_actions_destroy(pfile_actions);
    }

    if (pattr) {
        posix_spawnattr_destroy(pattr);
    }

    if (error_msg) {
        if (want_errorf) {
            janet_panicf(error_msg, error_ctx);
        } else {
            janet_panic(error_msg);
        }
    }

    return janet_wrap_abstract(p);

#undef PSPAWN_ERRORF
#undef PSPAWN_ERROR
}

static Janet pspawn_wait(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 1);
    Process *p = (Process *)janet_getabstract(argv, 0, &process_type);

    int exit_code;

    if (process_wait(p, &exit_code, 0) != 0)
        janet_panicf("error waiting for process - %s", strerror(errno));

    return janet_wrap_integer(exit_code);
}

static Janet pspawn_signal(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 2);
    Process *p = (Process *)janet_getabstract(argv, 0, &process_type);
    int sig = janet_getinteger(argv, 1);
    if (sig == -1)
        janet_panic("invalid signal");

    int rc = process_signal(p, sig);
    if (rc < 0)
        janet_panicf("unable to signal process - %s", strerror(errno));

    return janet_wrap_nil();
}

static Janet pspawn_close(int32_t argc, Janet *argv) {
    janet_fixarity(argc, 1);
    Process *p = (Process *)janet_getabstract(argv, 0, &process_type);

    if (p->exited)
        return janet_wrap_nil();

    int rc;

    rc = process_signal(p, p->close_signal);
    if (rc < 0)
        janet_panicf("unable to signal process - %s", strerror(errno));

    rc = process_wait(p, NULL, 0);
    if (rc < 0)
        janet_panicf("unable to wait for process - %s", strerror(errno));

    return janet_wrap_nil();
}

static Janet pspawn_pipe(int32_t argc, Janet *argv) {
    (void)argv;
    janet_fixarity(argc, 0);

    int fds[2];
#ifdef __APPLE__
    if (pipe(fds) < 0)
        janet_panicf("unable to allocate pipe - %s", strerror(errno));

    if (fcntl(fds[0], F_SETFD, FD_CLOEXEC) < 0) {
        close(fds[0]);
        close(fds[1]);
        janet_panicf("unable to set pipe FD_CLOEXEC - %s", strerror(errno));
    }

    if (fcntl(fds[1], F_SETFD, FD_CLOEXEC) < 0){
        close(fds[0]);
        close(fds[1]);
        janet_panicf("unable to set pipe FD_CLOEXEC - %s", strerror(errno));
    }
#else
    if (pipe2(fds, O_CLOEXEC) < 0)
        janet_panicf("unable to allocate pipe - %s", strerror(errno));
#endif

    FILE *p1 = fdopen(fds[0], "rb");
    FILE *p2 = fdopen(fds[1], "wb");
    if (!p1 || !p2) {
        if(p1)
            fclose(p1);
        else
            close(fds[0]);

        if(p2)
            fclose(p2);
        else
            close(fds[1]);
        janet_panicf("unable to create file objects - %s", strerror(errno));
    }

    Janet *t = janet_tuple_begin(2);
    t[0] = janet_makefile(p1, JANET_FILE_READ | JANET_FILE_BINARY);
    t[1] = janet_makefile(p2, JANET_FILE_WRITE | JANET_FILE_BINARY);
    return janet_wrap_tuple(janet_tuple_end(t));
}

static const JanetReg cfuns[] = {
    {"spawn", primitive_pspawn, "(posix-spawn/spawn & args)\n\n"},
    {"signal", pspawn_signal, "(posix-spawn/signal p sig)\n\n"},
    {"close", pspawn_close, "(posix-spawn/close p)\n\n"},
    {"wait", pspawn_wait, "(posix-spawn/wait p)\n\n"},
    {"pipe", pspawn_pipe, "(posix-spawn/pipe)\n\n"},
    {NULL, NULL, NULL}
};

JANET_MODULE_ENTRY(JanetTable *env) {
    janet_cfuns(env, "posix-spawn", cfuns);
#define DEF_CONSTANT_INT(X) janet_def(env, #X, janet_wrap_integer(X), NULL)
    DEF_CONSTANT_INT(POSIX_SPAWN_SETSIGMASK);
    DEF_CONSTANT_INT(POSIX_SPAWN_SETSIGDEF);
    DEF_CONSTANT_INT(POSIX_SPAWN_RESETIDS);
#undef DEF_CONSTANT_INT
}
