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

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

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
static const char tinl_version[] = "0.1.0";

static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(2);
}
static void *xrealloc(void *p, size_t z) {
    void *q = realloc(p, z);
    if(!q) die("OOM");
    return q;
}
static char *xstrdup(const char *s) {
    char *d = strdup(s);
    if(!d) die("OOM");
    return d;
}

static void vecstr_push(vecstr *vv, const char *s)
{
    if(vv->n == vv->cap) {
        vv->cap = vv->cap ? vv->cap * 2 : 8;
        vv->v = xrealloc(vv->v, vv->cap * sizeof(*vv->v));
    }
    vv->v[vv->n++] = xstrdup(s);
}
static void vecstr_free(vecstr *vv) {
    for(size_t i = 0; i < vv->n; i++) free(vv->v[i]);
    free(vv->v);
}

static void mapkv_put(mapkv *m, const char *key, const char *val)
{
    for(size_t i = 0; i < m->n; i++) if(strcmp(m->v[i].key, key) == 0) {
            free(m->v[i].val);
            m->v[i].val = xstrdup(val);
            return;
        }
    if(m->n == m->cap) {
        m->cap = m->cap ? m->cap * 2 : 16;
        m->v = xrealloc(m->v, m->cap * sizeof(*m->v));
    }
    m->v[m->n].key = xstrdup(key);
    m->v[m->n].val = xstrdup(val);
    m->n++;
}
static void mapkv_free(mapkv *m)
{
    for(size_t i = 0; i < m->n; i++) {
        free(m->v[i].key);
        free(m->v[i].val);
    }
    free(m->v);
}

static bool has_feature(vecstr *features, const char *name)
{
    for(size_t i = 0; i < features->n; i++) if(strcmp(features->v[i], name) == 0) return true;
    return false;
}

static char *ltrim(char *s) {
    while(isspace((unsigned char) * s)) s++;
    return s;
}
static void rtrim_inplace(char *s)
{
    size_t n = strlen(s);
    while(n > 0 && (unsigned char)s[n - 1] <= ' ') {
        s[n - 1] = '\0';
        n--;
    }
}
static bool ends_with(const char *s, const char *suf)
{
    size_t ns = strlen(s), nt = strlen(suf);
    if(nt > ns) return false;
    return memcmp(s + ns - nt, suf, nt) == 0;
}

static bool build_temp_path(char *buf, size_t cap, const char *root, const char *leaf)
{
    const char *base = (root && *root) ? root : ".";
    size_t n = strlen(base);
    const char *sep = (n > 0 && base[n - 1] == '/') ? "" : "/";
    int written = snprintf(buf, cap, "%s%s%s", base, sep, leaf);
    if(written < 0) return false;
    return (size_t)written < cap;
}

static char *make_temp_dir(void)
{
    char templ[PATH_MAX];
    const char *roots[2] = { scratch_root, default_scratch_root };

    for(size_t attempt = 0; attempt < sizeof(roots) / sizeof(roots[0]); attempt++) {
        const char *candidate = roots[attempt];
        if(!build_temp_path(templ, sizeof templ, candidate, "tinl.XXXXXX")) continue;

        int fd = mkstemp(templ);
        if(fd < 0) continue;
        close(fd);
        if(unlink(templ) != 0) continue;
        if(mkdir(templ, 0700) != 0) continue;
        return xstrdup(templ);
    }
    return NULL;
}

static void path_dirname(const char *path, char *out, size_t cap)
{
    const char *slash = strrchr(path, '/');
    if(!slash) {
        snprintf(out, cap, ".");
        return;
    }
    size_t n = (size_t)(slash - path);
    if(n >= cap) n = cap - 1;
    memcpy(out, path, n);
    out[n] = '\0';
}
static void strip_ext(const char *path, char *out, size_t cap)
{
    snprintf(out, cap, "%s", path);
    char *dot = strrchr(out, '.');
    if(dot && dot > out) *dot = '\0';
}
static const char *skip_dot_slash(const char *p)
{
    if(p[0] == '.' && p[1] == '/' && p[2] != '\0') return p + 2;
    return p;
}
static void map_source_to_bin(const char *src, char *out, size_t cap)
{
    char noext[PATH_MAX];
    strip_ext(src, noext, sizeof noext);
    const char *rel = skip_dot_slash(noext);
    const char *root = (bin_root && *bin_root) ? bin_root : default_bin_root;
    size_t nroot = strlen(root);
    bool need_slash = nroot && root[nroot - 1] != '/';
    size_t nrel = strlen(rel);
    size_t needed = nroot + (need_slash ? 1 : 0) + nrel + 1;
    if(cap == 0 || needed > cap) {
        die("output path too long: %s", src);
    }
    char *p = out;
    memcpy(p, root, nroot);
    p += nroot;
    if(need_slash) *p++ = '/';
    memcpy(p, rel, nrel + 1);
}

static void parse_config(const char *path, mapkv *subs)
{
    if(!path) return;
    FILE *f = fopen(path, "r");
    if(!f) die("cannot open config %s: %s", path, strerror(errno));
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    size_t lno = 0;
    while((n = getline(&line, &cap, f)) != -1) {
        lno++;
        rtrim_inplace(line);
        char *s = ltrim(line);
        if(*s == '\0' || *s == '#') continue;
        char *eq = strchr(s, '=');
        if(!eq) {
            fprintf(stderr, "config %s:%lu: missing '='\n", path, (unsigned long)lno);
            continue;
        }
        char *key = s;
        char *val = eq + 1;
        char *key_end = eq - 1;
        while(key_end >= key && isspace((unsigned char) * key_end)) {
            *key_end = '\0';
            key_end--;
        }
        val = ltrim(val);
        mapkv_put(subs, key, val);
    }
    free(line);
    fclose(f);
}

static char *subst_once(const char *in, const char *sym, const char *val)
{
    char needle[128];
    snprintf(needle, sizeof needle, "%%%s", sym);
    size_t kne = strlen(needle);
    size_t nin = strlen(in), nval = strlen(val);
    size_t cap = nin + 1 + (nval + 1) * 8;
    char *out = malloc(cap);
    if(!out) die("OOM");
    const char *p = in;
    char *q = out;
    while(*p) {
        if(strncmp(p, needle, kne) == 0) {
            size_t need = (size_t)(q - out) + nval + 1;
            if(need > cap) {
                cap = need * 2;
                size_t off = (size_t)(q - out);
                out = realloc(out, cap);
                if(!out) die("OOM");
                q = out + off;
            }
            memcpy(q, val, nval);
            q += nval;
            p += kne;
        } else {
            if((size_t)(q - out) + 2 > cap) {
                cap *= 2;
                size_t off = (size_t)(q - out);
                out = realloc(out, cap);
                if(!out) die("OOM");
                q = out + off;
            }
            *q++ = *p++;
        }
    }
    *q = '\0';
    return out;
}

static char *perform_substitutions(const char *cmd_in, mapkv *subs, const char *testpath)
{
    char s_dir[PATH_MAX];
    path_dirname(testpath, s_dir, sizeof s_dir);
    char bmap[PATH_MAX];
    map_source_to_bin(testpath, bmap, sizeof bmap);
    char bdir[PATH_MAX];
    path_dirname(bmap, bdir, sizeof bdir);

    char *tdir = make_temp_dir();

    char tfile[PATH_MAX];
    int fd = -1;
    const char *candidates[3];
    size_t c = 0;
    if(tdir) candidates[c++] = tdir;
    candidates[c++] = scratch_root;
    candidates[c++] = default_scratch_root;
    for(size_t i = 0; i < c; i++) {
        if(!build_temp_path(tfile, sizeof tfile, candidates[i], "out.XXXXXX")) continue;
        fd = mkstemp(tfile);
        if(fd >= 0) break;
    }
    if(fd >= 0) close(fd);
    if(fd < 0) {
        if(build_temp_path(tfile, sizeof tfile, default_scratch_root, "tinl-out.XXXXXX")) {
            fd = mkstemp(tfile);
            if(fd >= 0) close(fd);
        } else {
            snprintf(tfile, sizeof tfile, "%s/tinl-out.XXXXXX", default_scratch_root);
        }
    }

    char *cmd = xstrdup(cmd_in);

    for(int pass = 0; pass < 8; pass++) {
        bool changed = false;
        for(size_t i = 0; i < subs->n; i++) {
            char *next = subst_once(cmd, subs->v[i].key, subs->v[i].val);
            if(strcmp(next, cmd) != 0) changed = true;
            free(cmd);
            cmd = next;
        }
        if(!changed) break;
    }

    char *x;
    const char *scratch_for_T = tdir ? tdir : ((scratch_root && *scratch_root) ? scratch_root : default_scratch_root);

    x = subst_once(cmd, "s", testpath);
    free(cmd);
    cmd = x;
    x = subst_once(cmd, "S", s_dir);
    free(cmd);
    cmd = x;
    x = subst_once(cmd, "t", tfile);
    free(cmd);
    cmd = x;
    x = subst_once(cmd, "T", scratch_for_T);
    free(cmd);
    cmd = x;
    x = subst_once(cmd, "b", bmap);
    free(cmd);
    cmd = x;
    x = subst_once(cmd, "B", bdir);
    free(cmd);
    cmd = x;

    free(tdir);
    return cmd;
}

static int run_shell(const char *cmd, bool verbose, bool *timed_out)
{
    if(timed_out) *timed_out = false;
    if(verbose) {
        fputs("    $ ", stderr);
        fputs(cmd, stderr);
        fputc('\n', stderr);
    }
    pid_t pid = fork();
    if(pid < 0) {
        perror("fork");
        return 127;
    }
    if(pid == 0) {
        execl("/bin/sh", "sh", "-c", cmd, (char*)0);
        _exit(127);
    }
    int st = 0;

    if(timeout_secs == 0) {
        while(waitpid(pid, &st, 0) < 0) {
            if(errno == EINTR) continue;
            perror("waitpid");
            return 127;
        }
    } else {
        unsigned remaining = timeout_secs;
        for(;;) {
            pid_t r = waitpid(pid, &st, WNOHANG);
            if(r == pid) break;
            if(r < 0) {
                if(errno == EINTR) continue;
                perror("waitpid");
                return 127;
            }
            if(remaining == 0) {
                (void)kill(pid, SIGKILL);
                while(waitpid(pid, &st, 0) < 0) {
                    if(errno == EINTR) continue;
                    break;
                }
                if(timed_out) *timed_out = true;
                return 124;
            }
            sleep(1);
            if(remaining > 0) remaining--;
        }
    }
    if(WIFEXITED(st)) return WEXITSTATUS(st);
    if(WIFSIGNALED(st)) return 128 + WTERMSIG(st);
    return 127;
}

static bool parse_comment_run(const char *line, char *out, size_t cap)
{
    const char *p = line;
    while(*p == ' ' || *p == '\t') p++;
    if(*p == '/' && p[1] == '/') p += 2;
    else if(*p == '#') p++;
    else if(*p == ';') p++;
    else return false;
    while(*p == ' ' || *p == '\t') p++;
    if(strncmp(p, "RUN:", 4) != 0) return false;
    p += 4;
    while(*p == ' ' || *p == '\t') p++;
    snprintf(out, cap, "%s", p);
    return true;
}

static void parse_requires(const char *line, vecstr *reqs)
{
    const char *p = strstr(line, "REQUIRES:");
    if(!p) return;
    p += 9;
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", p);
    char *tok = strtok(buf, ", ");
    while(tok) {
        if(*tok) vecstr_push(reqs, tok);
        tok = strtok(NULL, ", ");
    }
}
static void parse_unsupported(const char *line, vecstr *uns)
{
    const char *p = strstr(line, "UNSUPPORTED:");
    if(!p) return;
    p += 12;
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", p);
    char *tok = strtok(buf, ", ");
    while(tok) {
        if(*tok) vecstr_push(uns, tok);
        tok = strtok(NULL, ", ");
    }
}

static int run_test_file(const char *path, mapkv *cfgsubs, vecstr *features, bool verbose, bool quiet)
{
    FILE *f = fopen(path, "r");
    if(!f) {
        fprintf(stderr, "open %s: %s\n", path, strerror(errno));
        return 2;
    }

    vecstr runs = {0};
    vecstr reqs = {0};
    vecstr uns  = {0};

    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    char pending[8192] = "";
    bool have_pending = false;
    while((n = getline(&line, &cap, f)) != -1) {
        rtrim_inplace(line);
        parse_requires(line, &reqs);
        parse_unsupported(line, &uns);

        char cmd[8192];
        if(parse_comment_run(line, cmd, sizeof cmd)) {
            if(ends_with(cmd, "\\")) {
                size_t len = strlen(cmd);
                if(len > 0) cmd[len - 1] = '\0';
                snprintf(pending, sizeof pending, "%s", cmd);
                have_pending = true;
                continue;
            } else {
                if(have_pending) {
                    char joined[8192];
                    snprintf(joined, sizeof joined, "%s %s", pending, cmd);
                    vecstr_push(&runs, joined);
                    have_pending = false;
                    pending[0] = '\0';
                } else {
                    vecstr_push(&runs, cmd);
                }
            }
        } else if(have_pending) {
            char cont[8192];
            const char *p = line;
            snprintf(cont, sizeof cont, "%s %s", pending, p);
            if(ends_with(cont, "\\")) {
                cont[strlen(cont) -1] = '\0';
                snprintf(pending, sizeof pending, "%s", cont);
            } else {
                vecstr_push(&runs, cont);
                have_pending = false;
                pending[0] = '\0';
            }
        }
    }
    free(line);
    fclose(f);
    if(have_pending) {
        vecstr_push(&runs, pending);
    }

    if(!quiet) fprintf(stderr, "[ RUN ] %s\n", path);

    for(size_t i = 0; i < reqs.n; i++) {
        if(!has_feature(features, reqs.v[i])) {
            if(!quiet) fprintf(stderr, "[ SKIP] %s (missing feature: %s)\n", path, reqs.v[i]);
            vecstr_free(&runs);
            vecstr_free(&reqs);
            vecstr_free(&uns);
            return 0;
        }
    }
    for(size_t i = 0; i < uns.n; i++) {
        if(has_feature(features, uns.v[i])) {
            if(!quiet) fprintf(stderr, "[ SKIP] %s (unsupported on feature: %s)\n", path, uns.v[i]);
            vecstr_free(&runs);
            vecstr_free(&reqs);
            vecstr_free(&uns);
            return 0;
        }
    }

    int rc = 0;
    for(size_t i = 0; i < runs.n; i++) {
        char *cmd = perform_substitutions(runs.v[i], cfgsubs, path);
        bool timed_out = false;
        int ec = run_shell(cmd, verbose, &timed_out);
        if(ec != 0) {
            if(!quiet) {
                if(timed_out) {
                    fprintf(stderr, "[TIME] %s (step %zu exceeded %u s)\n", path, i + 1, timeout_secs);
                } else {
                    fprintf(stderr, "[FAIL] %s (step %zu exit %d)\n", path, i + 1, ec);
                }
            }
            free(cmd);
            rc = ec ? ec : 1;
            break;
        }
        free(cmd);
    }
    if(rc == 0 && !quiet) fprintf(stderr, "[  OK ] %s\n", path);

    vecstr_free(&runs);
    vecstr_free(&reqs);
    vecstr_free(&uns);
    return rc;
}

static void usage(const char *arg0)
{
    fprintf(stderr,
            "Usage: %s [-v|-q] [-c config] [-D feature]... [-t seconds] "
            "[-T scratch] [-b binroot] FILE...\n"
            "  -v           verbose shell commands\n"
            "  -q           quiet (only pass/fail)\n"
            "  -c FILE      substitution config (lines: key = value)\n"
            "  -D feature   enable feature for REQUIRES/UNSUPPORTED\n"
            "  -t SECONDS   timeout for each RUN command (0 disables)\n"
            "  -T DIR       scratch directory root for %%t/%%T (default /tmp)\n"
            "  -b DIR       base directory used when expanding %%b/%%B (default bin)\n"
            "  -V           print tinl version and exit\n", arg0);
}

int main(int argc, char **argv)
{
    bool verbose = false, quiet = false;
    const char *cfgpath = NULL;
    vecstr features = {0};

    int opt;
    while((opt = getopt(argc, argv, "vqc:D:t:T:b:V")) != -1) {
        switch(opt) {
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
            if(errno || !end || *end != '\0') die("invalid timeout: %s", optarg);
            if(v > UINT_MAX) die("timeout too large: %s", optarg);
            timeout_secs = (unsigned)v;
            break;
        }
        case 'T':
            scratch_root = (optarg && *optarg) ? optarg : default_scratch_root;
            break;
        case 'b':
            bin_root = (optarg && *optarg) ? optarg : default_bin_root;
            break;
        case 'V':
            printf("tinl %s\n", tinl_version);
            vecstr_free(&features);
            return 0;
        default:
            usage(argv[0]);
            return 2;
        }
    }
    if(quiet && verbose) quiet = false;

    if(optind >= argc) {
        usage(argv[0]);
        return 2;
    }

    mapkv subs = {0};
    mapkv_put(&subs, "check", "tinl-check %s");
    vecstr_push(&features, "check");
    if(cfgpath) parse_config(cfgpath, &subs);

    int overall_rc = 0;
    for(int i = optind; i < argc; i++) {
        int rc = run_test_file(argv[i], &subs, &features, verbose, quiet);
        if(rc != 0) overall_rc = rc;
    }

    mapkv_free(&subs);
    vecstr_free(&features);
    return overall_rc;
}
