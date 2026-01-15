#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "version.h"
#include "subst.h"

typedef struct {
    char **v;
    size_t n, cap;
} vecstr;
typedef struct {
    char *key;
    char *val;
} kv;
typedef struct {
    kv *v;
    size_t n, cap;
} mapkv;

static const char *const default_bin_root = "bin";
static const char *bin_root = "bin";
static const char *const default_scratch_root = "/tmp";
static const char *scratch_root = "/tmp";
static unsigned timeout_secs = 0;
static const char tikl_version[] = TIKL_VERSION;
static bool lit_compat = false;
static const char *run_shell_path = "/bin/sh";
static bool run_shell_has_pipefail = false;

static void
die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(2);
}

static void
copy_str(char *dst, size_t cap, const char *src, const char *what)
{
    size_t needed = strlen(src) + 1;
    if (needed > cap)
        die("%s too long", what);
    memcpy(dst, src, needed);
}

static void
join_with_space(char *dst, size_t cap, const char *lhs,
                const char *rhs, const char *what)
{
    size_t nl = strlen(lhs);
    size_t nr = strlen(rhs);
    size_t needed = nl + 1 + nr + 1;
    if (needed > cap)
        die("%s too long", what);
    memcpy(dst, lhs, nl);
    dst[nl] = ' ';
    memcpy(dst + nl + 1, rhs, nr + 1);
}

static void
ensure_dir(const char *path)
{
    if (!path || *path == '\0')
        return;
    if (path[0] == '.' && path[1] == '\0')
        return;
    char buf[PATH_MAX];
    if (snprintf(buf, sizeof(buf), "%s", path) >= (int)sizeof(buf)) {
        die("path too long: %s", path);
    }
    char *p = buf;
    if (*p == '/')
        p++;
    for (; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (buf[0] != '\0' && mkdir(buf, 0755) != 0 && errno != EEXIST) {
                die("mkdir %s: %s", buf, strerror(errno));
            }
            *p = '/';
        }
    }
    if (mkdir(buf, 0755) != 0 && errno != EEXIST) {
        die("mkdir %s: %s", buf, strerror(errno));
    }
}

static void *
xrealloc(void *p, size_t z)
{
    void *q = realloc(p, z);
    if (!q)
        die("OOM");
    return q;
}
static char *
xstrdup(const char *s)
{
    char *d = strdup(s);
    if (!d)
        die("OOM");
    return d;
}

static void
vecstr_push(vecstr *vv, const char *s)
{
    if (vv->n == vv->cap) {
        vv->cap = vv->cap ? vv->cap * 2 : 8;
        vv->v = xrealloc(vv->v, vv->cap * sizeof(*vv->v));
    }
    vv->v[vv->n++] = xstrdup(s);
}
static void
vecstr_free(vecstr *vv)
{
    for (size_t i = 0; i < vv->n; i++)
        free(vv->v[i]);
    free(vv->v);
}

static void
mapkv_put(mapkv *m, const char *key, const char *val)
{
    for (size_t i = 0; i < m->n; i++)
        if (strcmp(m->v[i].key, key) == 0) {
            free(m->v[i].val);
            m->v[i].val = xstrdup(val);
            return;
        }
    if (m->n == m->cap) {
        m->cap = m->cap ? m->cap * 2 : 16;
        m->v = xrealloc(m->v, m->cap * sizeof(*m->v));
    }
    m->v[m->n].key = xstrdup(key);
    m->v[m->n].val = xstrdup(val);
    m->n++;
}
static void
mapkv_free(mapkv *m)
{
    for (size_t i = 0; i < m->n; i++) {
        free(m->v[i].key);
        free(m->v[i].val);
    }
    free(m->v);
}

static const char *
lookup_mapkv_cb(void *ctx, const char *key, size_t len)
{
    mapkv *m = ctx;
    if (!m)
        return NULL;
    for (size_t i = 0; i < m->n; i++) {
        if (strlen(m->v[i].key) == len &&
            strncmp(m->v[i].key, key, len) == 0)
            return m->v[i].val;
    }
    return NULL;
}

typedef struct {
    const char *s;
    const char *S;
    const char *t;
    const char *T;
    const char *b;
    const char *B;
} builtin_substs;

static const char *
lookup_builtin_cb(void *ctx, const char *key, size_t len)
{
    builtin_substs *b = ctx;
    if (!b || len != 1)
        return NULL;
    switch (key[0]) {
        case 's':
            return b->s;
        case 'S':
            return b->S;
        case 't':
            return b->t;
        case 'T':
            return b->T;
        case 'b':
            return b->b;
        case 'B':
            return b->B;
        default:
            return NULL;
    }
}

static bool
has_feature(vecstr *features, const char *name)
{
    for (size_t i = 0; i < features->n;
         i++)
        if (strcmp(features->v[i], name) == 0)
            return true;
    return false;
}

static char *
ltrim(char *s)
{
    while (isspace((unsigned char) * s))
        s++;
    return s;
}
static void
rtrim_inplace(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (unsigned char)s[n - 1] <= ' ') {
        s[n - 1] = '\0';
        n--;
    }
}
static bool
ends_with(const char *s, const char *suf)
{
    size_t ns = strlen(s), nt = strlen(suf);
    if (nt > ns)
        return false;
    return memcmp(s + ns - nt, suf, nt) == 0;
}

static bool
build_temp_path(char *buf, size_t cap, const char *root,
                const char *leaf)
{
    const char *base = (root && *root) ? root : ".";
    size_t n = strlen(base);
    const char *sep = (n > 0 && base[n - 1] == '/') ? "" : "/";
    int written = snprintf(buf, cap, "%s%s%s", base, sep, leaf);
    if (written < 0)
        return false;
    return (size_t)written < cap;
}

static char *
make_temp_dir(void)
{
    char templ[PATH_MAX];
    const char *roots[2] = { scratch_root, default_scratch_root };

    for (size_t attempt = 0; attempt < sizeof(roots) / sizeof(roots[0]);
         attempt++) {
        const char *candidate = roots[attempt];
        if (!build_temp_path(templ, sizeof(templ), candidate, "tikl.XXXXXX"))
            continue;

        int fd = mkstemp(templ);
        if (fd < 0)
            continue;
        close(fd);
        if (unlink(templ) != 0)
            continue;
        if (mkdir(templ, 0700) != 0)
            continue;
        return xstrdup(templ);
    }
    return NULL;
}

static void
path_dirname(const char *path, char *out, size_t cap)
{
    const char *slash = strrchr(path, '/');
    if (!slash) {
        snprintf(out, cap, ".");
        return;
    }
    size_t n = (size_t)(slash - path);
    if (n >= cap)
        n = cap - 1;
    memcpy(out, path, n);
    out[n] = '\0';
}
static void
strip_ext(const char *path, char *out, size_t cap)
{
    snprintf(out, cap, "%s", path);
    char *dot = strrchr(out, '.');
    if (dot && dot > out)
        *dot = '\0';
}
static const char *
skip_dot_slash(const char *p)
{
    if (p[0] == '.' && p[1] == '/' && p[2] != '\0')
        return p + 2;
    return p;
}
static bool
relativize_to_cwd(const char *abs_path, char *out, size_t cap)
{
    if (!abs_path || abs_path[0] != '/')
        return false;
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd)))
        return false;
    size_t cwd_len = strlen(cwd);
    while (cwd_len > 1 && cwd[cwd_len - 1] == '/')
        cwd_len--;
    if (strncmp(abs_path, cwd, cwd_len) != 0)
        return false;
    const char *rest = abs_path + cwd_len;
    if (*rest == '/')
        rest++;
    if (*rest == '\0')
        return false;
    copy_str(out, cap, rest, "relative test path");
    return true;
}
static const char *
pick_test_path(const char *path, const char *path_abs,
               char *out, size_t out_cap,
               char *dir_out, size_t dir_cap)
{
    if (!relativize_to_cwd(path_abs, out, out_cap)) {
        copy_str(out, out_cap, path, "test path");
        const char *trimmed = skip_dot_slash(out);
        if (trimmed != out) {
            size_t len = strlen(trimmed);
            memmove(out, trimmed, len + 1);
        }
    }
    path_dirname(out, dir_out, dir_cap);
    return out;
}
static void
map_source_to_bin(const char *src, char *out, size_t cap)
{
    char noext[PATH_MAX];
    strip_ext(src, noext, sizeof(noext));
    const char *rel = skip_dot_slash(noext);
    const char *root = (bin_root && *bin_root) ? bin_root : default_bin_root;
    size_t nroot = strlen(root);
    bool need_slash = nroot && root[nroot - 1] != '/';
    size_t nrel = strlen(rel);
    size_t needed = nroot + (need_slash ? 1 : 0) + nrel + 1;
    if (cap == 0 || needed > cap) {
        die("output path too long: %s", src);
    }
    char *p = out;
    memcpy(p, root, nroot);
    p += nroot;
    if (need_slash)
        *p++ = '/';
    memcpy(p, rel, nrel + 1);
}

static void
parse_config(const char *path, mapkv *subs, vecstr *config_args)
{
    if (!path)
        return;
    FILE *f = fopen(path, "r");
    if (!f)
        die("cannot open config %s: %s", path, strerror(errno));
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    size_t lno = 0;
    while ((n = getline(&line, &cap, f)) != -1) {
        lno++;
        rtrim_inplace(line);
        char *s = ltrim(line);
        if (*s == '\0' || *s == '#')
            continue;
        if (*s == '-') {
            char *tok = strtok(s, " \t");
            while (tok) {
                if (strcmp(tok, "-c") == 0) {
                    tok = strtok(NULL, " \t");
                    tok = strtok(NULL, " \t");
                    continue;
                }
                vecstr_push(config_args, tok);
                tok = strtok(NULL, " \t");
            }
            continue;
        }
        char *eq = strchr(s, '=');
        if (!eq) {
            fprintf(stderr, "config %s:%lu: missing '='\n", path, (unsigned long)lno);
            continue;
        }
        char *key = s;
        char *val = eq + 1;
        char *key_end = eq - 1;
        while (key_end >= key && isspace((unsigned char) * key_end)) {
            *key_end = '\0';
            key_end--;
        }
        val = ltrim(val);
        mapkv_put(subs, key, val);
    }
    free(line);
    fclose(f);
}

static char *
apply_config_substitutions(const char *input, mapkv *subs)
{
    char *current = xstrdup(input);
    for (int pass = 0; pass < 8; pass++) {
        int status = 0;
        char *next = tikl_expand_placeholders(current, true, false,
                                              lookup_mapkv_cb, subs,
                                              "tikl", &status);
        if (status != 0) {
            free(current);
            free(next);
            die("invalid placeholder in configuration");
        }
        if (!next)
            return current;
        bool changed = strcmp(current, next) != 0;
        free(current);
        current = next;
        if (!changed)
            break;
    }
    return current;
}

static char *
perform_substitutions(const char *cmd_in, mapkv *subs,
                      const char *testpath,
                      const char *testpath_abs)
{
    char s_path[PATH_MAX];
    char s_dir[PATH_MAX];
    const char *path_for_s = pick_test_path(testpath, testpath_abs,
                                            s_path, sizeof(s_path),
                                            s_dir, sizeof(s_dir));
    char bmap[PATH_MAX];
    map_source_to_bin(path_for_s, bmap, sizeof(bmap));
    char bdir[PATH_MAX];
    path_dirname(bmap, bdir, sizeof(bdir));
    ensure_dir(bdir);

    char *tdir = make_temp_dir();

    char tfile[PATH_MAX];
    int fd = -1;
    const char *candidates[3];
    size_t c = 0;
    if (tdir)
        candidates[c++] = tdir;
    candidates[c++] = scratch_root;
    candidates[c++] = default_scratch_root;
    for (size_t i = 0; i < c; i++) {
        if (!build_temp_path(tfile, sizeof(tfile), candidates[i],
                             "out.XXXXXX"))
            continue;
        fd = mkstemp(tfile);
        if (fd >= 0)
            break;
    }
    if (fd >= 0)
        close(fd);
    if (fd < 0) {
        if (build_temp_path(tfile, sizeof(tfile), default_scratch_root,
                            "tikl-out.XXXXXX")) {
            fd = mkstemp(tfile);
            if (fd >= 0)
                close(fd);
        } else {
            snprintf(tfile, sizeof(tfile), "%s/tikl-out.XXXXXX", default_scratch_root);
        }
    }

    char *cmd = apply_config_substitutions(cmd_in, subs);

    const char *scratch_for_T = tdir ? tdir : ((scratch_root &&
                                                *scratch_root) ? scratch_root :
                                               default_scratch_root);

    builtin_substs builtins = {
        .s = path_for_s,
        .S = s_dir,
        .t = tfile,
        .T = scratch_for_T,
        .b = bmap,
        .B = bdir,
    };

    int status = 0;
    char *expanded = tikl_expand_placeholders(cmd, true, true,
                                              lookup_builtin_cb, &builtins,
                                              "tikl", &status);
    free(cmd);
    free(tdir);
    if (status != 0) {
        free(expanded);
        die("invalid placeholder expansion");
    }
    return expanded;
}

static void
push_sub_line(vecstr *lines, const char *key, const char *val)
{
    size_t nk = strlen(key);
    size_t nv = strlen(val);
    char *buf = malloc(nk + 1 + nv + 1);
    if (!buf)
        die("OOM");
    memcpy(buf, key, nk);
    buf[nk] = '=';
    memcpy(buf + nk + 1, val, nv + 1);
    vecstr_push(lines, buf);
    free(buf);
}

static char *
expand_for_check_value(const char *input, mapkv *subs,
                       const char *path_for_s,
                       const char *s_dir,
                       const char *bmap,
                       const char *bdir)
{
    char *cmd = apply_config_substitutions(input, subs);
    builtin_substs builtins = {
        .s = path_for_s,
        .S = s_dir,
        .b = bmap,
        .B = bdir,
    };
    int status = 0;
    char *out = tikl_expand_placeholders(cmd, true, true,
                                         lookup_builtin_cb, &builtins,
                                         "tikl", &status);
    free(cmd);
    if (status != 0) {
        free(out);
        die("invalid placeholder in configuration");
    }
    return out;
}

static char *
build_check_subs_blob(mapkv *subs, const char *testpath,
                      const char *testpath_abs)
{
    char s_path[PATH_MAX];
    char s_dir[PATH_MAX];
    const char *path_for_s = pick_test_path(testpath, testpath_abs,
                                            s_path, sizeof(s_path),
                                            s_dir, sizeof(s_dir));
    char bmap[PATH_MAX];
    map_source_to_bin(path_for_s, bmap, sizeof(bmap));
    char bdir[PATH_MAX];
    path_dirname(bmap, bdir, sizeof(bdir));
    ensure_dir(bdir);

    vecstr lines = {0};
    push_sub_line(&lines, "s", path_for_s);
    push_sub_line(&lines, "S", s_dir);
    push_sub_line(&lines, "b", bmap);
    push_sub_line(&lines, "B", bdir);

    for (size_t i = 0; i < subs->n; i++) {
        const char *key = subs->v[i].key;
        if (strcmp(key, "s") == 0 || strcmp(key, "S") == 0 ||
            strcmp(key, "b") == 0 || strcmp(key, "B") == 0) {
            continue;
        }
        char *expanded = expand_for_check_value(subs->v[i].val, subs,
                                                path_for_s, s_dir, bmap, bdir);
        push_sub_line(&lines, key, expanded);
        free(expanded);
    }

    if (lines.n == 0) {
        vecstr_free(&lines);
        return NULL;
    }

    size_t total = 0;
    for (size_t i = 0; i < lines.n; i++)
        total += strlen(lines.v[i]) + 1;
    char *blob = malloc(total);
    if (!blob)
        die("OOM");
    size_t off = 0;
    for (size_t i = 0; i < lines.n; i++) {
        size_t len = strlen(lines.v[i]);
        if (i > 0)
            blob[off++] = '\n';
        memcpy(blob + off, lines.v[i], len);
        off += len;
    }
    blob[off] = '\0';
    vecstr_free(&lines);
    return blob;
}

static int
run_shell(const char *cmd, bool verbose, bool *timed_out)
{
#ifdef TIKL_FUZZ
    (void)cmd;
    (void)verbose;
    if (timed_out)
        *timed_out = false;
    return 0;
#else
    if (timed_out)
        *timed_out = false;
    const char *script = cmd;
    char *wrapped = NULL;
    if (!lit_compat && run_shell_has_pipefail) {
        const char *pipefail_prelude = "set -o pipefail 2>/dev/null || :; ";
        size_t want = strlen(pipefail_prelude) + strlen(cmd) + 1;
        wrapped = malloc(want);
        if (!wrapped)
            return 127;
        snprintf(wrapped, want, "%s%s", pipefail_prelude, cmd);
        script = wrapped;
    }
    if (verbose) {
        fputs("    $ ", stderr);
        fputs(cmd, stderr);
        fputc('\n', stderr);
    }
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        free(wrapped);
        return 127;
    }
    if (pid == 0) {
        execl(run_shell_path, run_shell_path, "-c", script, (char*)0);
        _exit(127);
    }
    int st = 0;

    if (timeout_secs == 0) {
        while (waitpid(pid, &st, 0) < 0) {
            if (errno == EINTR)
                continue;
            perror("waitpid");
            free(wrapped);
            return 127;
        }
    } else {
        unsigned remaining = timeout_secs;
        for (;;) {
            pid_t r = waitpid(pid, &st, WNOHANG);
            if (r == pid)
                break;
            if (r < 0) {
                if (errno == EINTR)
                    continue;
                perror("waitpid");
                free(wrapped);
                return 127;
            }
            if (remaining == 0) {
                (void)kill(pid, SIGKILL);
                while (waitpid(pid, &st, 0) < 0) {
                    if (errno == EINTR)
                        continue;
                    break;
                }
                if (timed_out)
                    *timed_out = true;
                free(wrapped);
                return 124;
            }
            sleep(1);
            if (remaining > 0)
                remaining--;
        }
    }
    if (WIFEXITED(st))
        st = WEXITSTATUS(st);
    else if (WIFSIGNALED(st))
        st = 128 + WTERMSIG(st);
    else
        st = 127;
    free(wrapped);
    return st;
#endif
}

static bool
probe_pipefail(const char *shell_path)
{
#ifdef TIKL_FUZZ
    (void)shell_path;
    return true;
#else
    pid_t pid = fork();
    if (pid < 0)
        return false;
    if (pid == 0) {
        execl(shell_path, shell_path, "-c", "set -o pipefail 2>/dev/null", (char *)0);
        _exit(127);
    }
    int st = 0;
    while (waitpid(pid, &st, 0) < 0) {
        if (errno == EINTR)
            continue;
        return false;
    }
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
#endif
}

static void
init_run_shell(void)
{
    run_shell_path = "/bin/sh";
    run_shell_has_pipefail = false;
    const char *forced_shell = getenv("TIKL_SHELL");
    if (forced_shell && *forced_shell) {
        run_shell_path = forced_shell;
        if (!lit_compat)
            run_shell_has_pipefail = probe_pipefail(forced_shell);
        return;
    }
    if (lit_compat)
        return;
    if (probe_pipefail("/bin/sh")) {
        run_shell_has_pipefail = true;
        return;
    }
    if (access("/bin/bash", X_OK) == 0 && probe_pipefail("/bin/bash")) {
        run_shell_path = "/bin/bash";
        run_shell_has_pipefail = true;
        return;
    }
}

static bool
parse_comment_run(const char *line, char *out, size_t cap)
{
    const char *p = line;
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p == '/' && p[1] == '/')
        p += 2;
    else if (*p == '#')
        p++;
    else if (*p == ';')
        p++;
    else
        return false;
    while (*p == ' ' || *p == '\t')
        p++;
    if (strncmp(p, "RUN:", 4) != 0)
        return false;
    p += 4;
    while (*p == ' ' || *p == '\t')
        p++;
    snprintf(out, cap, "%s", p);
    return true;
}

static void
parse_requires(const char *line, vecstr *reqs)
{
    const char *p = strstr(line, "REQUIRES:");
    if (!p)
        return;
    p += 9;
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s", p);
    char *tok = strtok(buf, ", ");
    while (tok) {
        if (*tok)
            vecstr_push(reqs, tok);
        tok = strtok(NULL, ", ");
    }
}
static void
parse_unsupported(const char *line, vecstr *uns)
{
    const char *p = strstr(line, "UNSUPPORTED:");
    if (!p)
        return;
    p += 12;
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s", p);
    char *tok = strtok(buf, ", ");
    while (tok) {
        if (*tok)
            vecstr_push(uns, tok);
        tok = strtok(NULL, ", ");
    }
}

static void
parse_xfail(const char *line, bool *xfail, char **reason)
{
    const char *p = strstr(line, "XFAIL:");
    if (!p)
        return;
    p += strlen("XFAIL:");
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s", p);
    char *trim = ltrim(buf);
    rtrim_inplace(trim);
    free(*reason);
    *reason = (*trim) ? xstrdup(trim) : NULL;
    *xfail = true;
}

static int
parse_allow_retries(const char *line, unsigned *value)
{
    const char *p = strstr(line, "ALLOW_RETRIES:");
    if (!p)
        return -1;
    p += strlen("ALLOW_RETRIES:");
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s", p);
    char *trim = ltrim(buf);
    rtrim_inplace(trim);
    if (*trim == '\0')
        return 0;
    errno = 0;
    char *end = NULL;
    unsigned long v = strtoul(trim, &end, 10);
    if (errno || end == trim)
        return 0;
    while (end && *end) {
        if (!isspace((unsigned char) * end))
            return 0;
        end++;
    }
    if (v > UINT_MAX)
        return 0;
    *value = (unsigned)v;
    return 1;
}

static int
run_test_file(const char *path, mapkv *cfgsubs, vecstr *features,
              bool verbose, bool quiet)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "open %s: %s\n", path, strerror(errno));
        return 2;
    }

    char testpath_abs_buf[PATH_MAX];
    if (!realpath(path, testpath_abs_buf)) {
        fprintf(stderr, "realpath %s: %s\n", path, strerror(errno));
        fclose(f);
        return 2;
    }
    const char *testpath_abs = testpath_abs_buf;
    if (lit_compat) {
        unsetenv("TIKL_CHECK_SUBSTS");
    } else {
        char *check_env = build_check_subs_blob(cfgsubs, path, testpath_abs);
        if (check_env) {
            if (setenv("TIKL_CHECK_SUBSTS", check_env, 1) != 0) {
                fprintf(stderr, "setenv TIKL_CHECK_SUBSTS: %s\n", strerror(errno));
                free(check_env);
                fclose(f);
                unsetenv("TIKL_CHECK_SUBSTS");
                unsetenv("TIKL_LIT_COMPAT");
                return 2;
            }
            free(check_env);
        } else {
            unsetenv("TIKL_CHECK_SUBSTS");
        }
    }
    if (lit_compat) {
        if (setenv("TIKL_LIT_COMPAT", "1", 1) != 0) {
            fprintf(stderr, "setenv TIKL_LIT_COMPAT: %s\n", strerror(errno));
            fclose(f);
            unsetenv("TIKL_CHECK_SUBSTS");
            unsetenv("TIKL_LIT_COMPAT");
            return 2;
        }
    } else {
        unsetenv("TIKL_LIT_COMPAT");
    }

    vecstr runs = {0};
    vecstr reqs = {0};
    vecstr uns  = {0};
    bool xfail = false;
    char *xfail_reason = NULL;
    unsigned allow_retries = 0;
    bool have_allow_retries = false;

    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    char pending[8192] = "";
    bool have_pending = false;
    while ((n = getline(&line, &cap, f)) != -1) {
        rtrim_inplace(line);
        parse_requires(line, &reqs);
        parse_unsupported(line, &uns);
        parse_xfail(line, &xfail, &xfail_reason);
        unsigned retries_val = 0;
        int retries_parse = parse_allow_retries(line, &retries_val);
        if (retries_parse == 1) {
            allow_retries = retries_val;
            have_allow_retries = true;
        } else if (retries_parse == 0) {
            fprintf(stderr, "%s: invalid ALLOW_RETRIES directive\n", path);
        }

        char cmd[8192];
        if (parse_comment_run(line, cmd, sizeof(cmd))) {
            if (ends_with(cmd, "\\")) {
                size_t len = strlen(cmd);
                if (len > 0)
                    cmd[len - 1] = '\0';
                copy_str(pending, sizeof(pending), cmd, "continued command");
                have_pending = true;
                continue;
            } else {
                if (have_pending) {
                    char joined[8192];
                    join_with_space(joined, sizeof(joined), pending, cmd, "joined command");
                    vecstr_push(&runs, joined);
                    have_pending = false;
                    pending[0] = '\0';
                } else {
                    vecstr_push(&runs, cmd);
                }
            }
        } else if (have_pending) {
            char cont[8192];
            const char *p = line;
            join_with_space(cont, sizeof(cont), pending, p, "continued command");
            if (ends_with(cont, "\\")) {
                cont[strlen(cont) -1] = '\0';
                copy_str(pending, sizeof(pending), cont, "continued command");
            } else {
                vecstr_push(&runs, cont);
                have_pending = false;
                pending[0] = '\0';
            }
        }
    }
    free(line);
    fclose(f);
    if (have_pending) {
        vecstr_push(&runs, pending);
    }

    if (!quiet)
        fprintf(stderr, "[ RUN ] %s\n", path);

    for (size_t i = 0; i < reqs.n; i++) {
        if (!has_feature(features, reqs.v[i])) {
            if (!quiet)
                fprintf(stderr, "[ SKIP] %s (missing feature: %s)\n", path,
                        reqs.v[i]);
            vecstr_free(&runs);
            vecstr_free(&reqs);
            vecstr_free(&uns);
            unsetenv("TIKL_CHECK_SUBSTS");
            unsetenv("TIKL_LIT_COMPAT");
            return 0;
        }
    }
    for (size_t i = 0; i < uns.n; i++) {
        if (has_feature(features, uns.v[i])) {
            if (!quiet)
                fprintf(stderr, "[ SKIP] %s (unsupported on feature: %s)\n", path,
                        uns.v[i]);
            vecstr_free(&runs);
            vecstr_free(&reqs);
            vecstr_free(&uns);
            unsetenv("TIKL_CHECK_SUBSTS");
            unsetenv("TIKL_LIT_COMPAT");
            return 0;
        }
    }

    if (runs.n == 0) {
        if (!quiet) {
            if (xfail) {
                const char *sep = (xfail_reason && *xfail_reason) ? "; " : "";
                const char *msg = (xfail_reason && *xfail_reason) ? xfail_reason : "";
                fprintf(stderr, "[XFAIL] %s (no RUN directives%s%s)\n", path, sep, msg);
            } else {
                fprintf(stderr, "[FAIL] %s (no RUN directives)\n", path);
            }
        }
        vecstr_free(&runs);
        vecstr_free(&reqs);
        vecstr_free(&uns);
        free(xfail_reason);
        unsetenv("TIKL_CHECK_SUBSTS");
        unsetenv("TIKL_LIT_COMPAT");
        return xfail ? 0 : 1;
    }

    int rc = 0;
    bool xfail_hit = false;
    for (size_t i = 0; i < runs.n; i++) {
        char *cmd = perform_substitutions(runs.v[i], cfgsubs, path, testpath_abs);
        unsigned attempts = have_allow_retries ? (allow_retries + 1) : 1;
        if (attempts == 0)
            attempts = 1;
        bool success = false;
        int ec = 0;
        bool timed_out = false;
        unsigned used_attempts = 0;
        for (unsigned attempt = 0; attempt < attempts; attempt++) {
            bool this_timeout = false;
            ec = run_shell(cmd, verbose, &this_timeout);
            used_attempts = attempt + 1;
            timed_out = this_timeout;
            if (ec == 0) {
                success = true;
                break;
            }
            if (attempt + 1 < attempts && !quiet) {
                if (this_timeout) {
                    fprintf(stderr, "[RETRY] %s (step %zu timed out, retry %u/%u)\n",
                            path, i + 1, attempt + 2, attempts);
                } else {
                    fprintf(stderr, "[RETRY] %s (step %zu exit %d, retry %u/%u)\n",
                            path, i + 1, ec, attempt + 2, attempts);
                }
            }
        }
        if (!success) {
            if (!quiet) {
                if (xfail) {
                    const char *sep = (xfail_reason && *xfail_reason) ? "; " : "";
                    const char *msg = (xfail_reason && *xfail_reason) ? xfail_reason : "";
                    if (timed_out) {
                        fprintf(stderr, "[XFAIL] %s (step %zu timed out%s%s)\n", path,
                                i + 1, sep, msg);
                    } else {
                        fprintf(stderr, "[XFAIL] %s (step %zu exit %d%s%s)\n", path, i + 1,
                                ec, sep, msg);
                    }
                } else {
                    const char *attempt_note = (used_attempts > 1) ? " after retries" : "";
                    if (timed_out) {
                        fprintf(stderr, "[TIME] %s (step %zu exceeded %u s%s)\n", path,
                                i + 1, timeout_secs, attempt_note);
                    } else {
                        fprintf(stderr, "[FAIL] %s (step %zu exit %d%s)\n", path, i + 1,
                                ec, attempt_note);
                    }
                }
            }
            free(cmd);
            if (xfail) {
                xfail_hit = true;
                rc = 0;
            } else {
                rc = ec ? ec : 1;
            }
            break;
        }
        free(cmd);
    }
    if (rc == 0) {
        if (xfail) {
            if (!xfail_hit) {
                if (!quiet) {
                    const char *sep = (xfail_reason && *xfail_reason) ? ": " : "";
                    const char *msg = (xfail_reason && *xfail_reason) ? xfail_reason : "";
                    fprintf(stderr, "[XPASS] %s%s%s\n", path, sep, msg);
                }
                rc = 1;
            }
        } else {
            if (!quiet)
                fprintf(stderr, "[  OK ] %s\n", path);
        }
    }

    vecstr_free(&runs);
    vecstr_free(&reqs);
    vecstr_free(&uns);
    free(xfail_reason);
    unsetenv("TIKL_CHECK_SUBSTS");
    unsetenv("TIKL_LIT_COMPAT");
    return rc;
}

static void
usage(const char *arg0)
{
    fprintf(stderr,
            "Usage: %s [-v|-q] [-c config] [-D feature]... [-t seconds] "
            "[-T scratch] [-b binroot] [-j jobs] [-L] FILE...\n"
            "  -v           verbose shell commands\n"
            "  -q           quiet (only pass/fail)\n"
            "  -c FILE      substitution config (lines: key = value)\n"
            "  -D feature   enable feature for REQUIRES/UNSUPPORTED\n"
            "  -t SECONDS   timeout for each RUN command (0 disables)\n"
            "  -T DIR       scratch directory root for %%t/%%T (default /tmp)\n"
            "  -b DIR       base directory used when expanding %%b/%%B (default bin)\n"
            "  -j JOBS      run up to JOBS workers in parallel\n"
            "  -L           force lit-compatible behaviour (disable tikl extras)\n"
            "  -V           print tikl version and exit\n", arg0);
}

int
main(int argc, char **argv)
{
    bool verbose = false, quiet = false;
    vecstr features = {0};
    vecstr config_args = {0};
    const char *cfgpath = NULL;
    unsigned jobs = 1;

    /* pre-scan argv for -c to pick config before parsing options */
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "-c") == 0) {
            cfgpath = argv[i + 1];
            break;
        }
    }

    int opt;
    /* load placeholders default */
    mapkv subs = {0};
    mapkv_put(&subs, "check", "tikl-check %s");
    vecstr_push(&features, "check");

    if (cfgpath)
        parse_config(cfgpath, &subs, &config_args);

    /* merge config args before user args (excluding -c) */
    vecstr merged = {0};
    vecstr_push(&merged, argv[0]);
    for (size_t i = 0; i < config_args.n; i++)
        vecstr_push(&merged, config_args.v[i]);
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0) {
            i++;
            continue;
        }
        vecstr_push(&merged, argv[i]);
    }

    char **pargv = merged.v;
    int parc = (int)merged.n;

    optind = 1;
    while ((opt = getopt(parc, pargv, "vqc:D:t:T:b:j:VL")) != -1) {
        switch (opt) {
            case 'v':
                verbose = true;
                break;
            case 'q':
                quiet = true;
                break;
            case 'c':
                cfgpath = optarg;
                break;
            case 'D':
                vecstr_push(&features, optarg);
                break;
            case 't': {
                    errno = 0;
                    char *end = NULL;
                    unsigned long v = strtoul(optarg, &end, 10);
                    if (errno || !end || *end != '\0')
                        die("invalid timeout: %s", optarg);
                    if (v > UINT_MAX)
                        die("timeout too large: %s", optarg);
                    timeout_secs = (unsigned)v;
                    break;
                }
            case 'T':
                scratch_root = (optarg && *optarg) ? optarg : default_scratch_root;
                break;
            case 'b':
                bin_root = (optarg && *optarg) ? optarg : default_bin_root;
                break;
            case 'j': {
                    errno = 0;
                    char *end = NULL;
                    unsigned long v = strtoul(optarg, &end, 10);
                    if (errno || !end || *end != '\0' || v == 0)
                        die("invalid jobs: %s", optarg);
                    if (v > UINT_MAX)
                        die("jobs too large: %s", optarg);
                    jobs = (unsigned)v;
                    break;
                }
            case 'V':
                printf("tikl %s\n", tikl_version);
                vecstr_free(&features);
                vecstr_free(&config_args);
                vecstr_free(&merged);
                mapkv_free(&subs);
                return 0;
            case 'L':
                lit_compat = true;
                break;
            default:
                usage(pargv[0]);
                vecstr_free(&features);
                vecstr_free(&config_args);
                vecstr_free(&merged);
                mapkv_free(&subs);
                return 2;
        }
    }
    if (quiet && verbose)
        quiet = false;

    if (optind >= parc) {
        usage(pargv[0]);
        vecstr_free(&config_args);
        vecstr_free(&merged);
        mapkv_free(&subs);
        return 2;
    }

    init_run_shell();

    int overall_rc = 0;
    int nfiles = parc - optind;
    if (jobs <= 1 || nfiles <= 1) {
        for (int i = optind; i < parc; i++) {
            int rc = run_test_file(pargv[i], &subs, &features, verbose, quiet);
            if (rc != 0) {
                overall_rc = rc;
                break;
            }
        }
    } else {
        int next = optind;
        unsigned active = 0;
        bool fail_seen = false;
        while ((next < parc && !fail_seen) || active > 0) {
            while (active < jobs && next < parc && !fail_seen) {
                pid_t pid = fork();
                if (pid == 0) {
                    char *worker_scratch = make_temp_dir();
                    if (!worker_scratch)
                        _exit(127);
                    scratch_root = worker_scratch;
                    int rc = run_test_file(pargv[next], &subs, &features,
                                           verbose, quiet);
                    free(worker_scratch);
                    _exit(rc);
                } else if (pid > 0) {
                    active++;
                    next++;
                } else {
                    perror("fork");
                    overall_rc = 127;
                    break;
                }
            }
            if (active == 0)
                break;
            int st = 0;
            pid_t w = wait(&st);
            if (w < 0) {
                if (errno == EINTR)
                    continue;
                perror("wait");
                overall_rc = 127;
                break;
            }
            active--;
            int rc = 1;
            if (WIFEXITED(st))
                rc = WEXITSTATUS(st);
            if (rc != 0) {
                overall_rc = rc;
                fail_seen = true;
            }
        }
    }

    mapkv_free(&subs);
    vecstr_free(&features);
    vecstr_free(&config_args);
    vecstr_free(&merged);
    return overall_rc;
}
