/* decay-rag portable spec — C11 reference port */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <dirent.h>
#include <sys/stat.h>

#define DIM 4096
#define HALF_LIFE 30.0

typedef struct {
    char id[512];
    long long ts;
    double vec[DIM];
    uint64_t *grams;
    int ngrams;
    double score;
} Chunk;

static Chunk *chunks = NULL;
static int nchunks = 0;
static int cap_chunks = 0;

static int keep_cp(unsigned cp) {
    return (cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'Z') ||
           (cp >= 'a' && cp <= 'z') || (cp >= 0x4E00 && cp <= 0x9FFF);
}

/* decode one UTF-8 code point, advance *p */
static unsigned next_cp(const unsigned char **p) {
    const unsigned char *s = *p;
    unsigned cp;
    if (s[0] < 0x80) { cp = s[0]; *p = s + 1; return cp; }
    if ((s[0] & 0xE0) == 0xC0) { cp = ((s[0] & 0x1F) << 6) | (s[1] & 0x3F); *p = s + 2; return cp; }
    if ((s[0] & 0xF0) == 0xE0) {
        cp = ((s[0] & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        *p = s + 3; return cp;
    }
    cp = ((s[0] & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
    *p = s + 4;
    return cp;
}

static uint64_t fnv1a(unsigned cp1, unsigned cp2) {
    uint64_t h = 14695981039346656037ULL;
    uint32_t pair[2];
    pair[0] = cp1; pair[1] = cp2;
    const unsigned char *b = (const unsigned char *)pair;
    for (int i = 0; i < 8; i++) { h ^= b[i]; h *= 1099511628211ULL; }
    return h;
}

/* filled: grams vector + hash set (sorted) */
static int build_grams(const char *text, double *vec, uint64_t **out_grams) {
    unsigned *cps = NULL; int ncp = 0, cap = 0;
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        unsigned cp = next_cp(&p);
        if (!keep_cp(cp)) continue;
        if (ncp == cap) { cap = cap ? cap * 2 : 64; cps = realloc(cps, cap * sizeof(unsigned)); }
        cps[ncp++] = cp;
    }
    for (int i = 0; i < DIM; i++) vec[i] = 0.0;
    int n = ncp > 1 ? ncp - 1 : 0;
    uint64_t *grams = n ? malloc((size_t)n * sizeof(uint64_t)) : NULL;
    for (int i = 0; i < n; i++) {
        uint64_t h = fnv1a(cps[i], cps[i + 1]);
        grams[i] = h;
        vec[h % DIM] += ((h >> 63) & 1) ? -1.0 : 1.0;
    }
    double norm = 0.0;
    for (int i = 0; i < DIM; i++) norm += vec[i] * vec[i];
    norm = sqrt(norm);
    if (norm > 0) for (int i = 0; i < DIM; i++) vec[i] /= norm;
    free(cps);
    /* sort for intersection */
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (grams[j] < grams[i]) { uint64_t t = grams[i]; grams[i] = grams[j]; grams[j] = t; }
    *out_grams = grams;
    return n;
}

static long long days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    long long era = (y >= 0 ? y : y - 399) / 400;
    long long yoe = y - era * 400;
    long long doy = (153LL * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

/* find YYYY-MM-DD, return epoch seconds, or -1 */
static long long parse_date(const char *s) {
    for (const char *p = s; *p; p++) {
        if (p[0] >= '0' && p[0] <= '9' && p[1] >= '0' && p[1] <= '9' &&
            p[2] >= '0' && p[2] <= '9' && p[3] >= '0' && p[3] <= '9' && p[4] == '-' &&
            p[5] >= '0' && p[5] <= '9' && p[6] >= '0' && p[6] <= '9' && p[7] == '-' &&
            p[8] >= '0' && p[8] <= '9' && p[9] >= '0' && p[9] <= '9') {
            int y = (p[0]-'0')*1000 + (p[1]-'0')*100 + (p[2]-'0')*10 + (p[3]-'0');
            int m = (p[5]-'0')*10 + (p[6]-'0');
            int d = (p[8]-'0')*10 + (p[9]-'0');
            return days_from_civil(y, m, d) * 86400;
        }
    }
    return -1;
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    size_t got = fread(buf, 1, (size_t)n, f);
    buf[got] = 0;
    fclose(f);
    return buf;
}

/* collapse CRLF, trim, return malloc'd copy */
static char *trim_copy(const char *s, size_t len) {
    size_t a = 0, b = len;
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\n' || s[a] == '\r')) a++;
    while (b > a && (s[b-1] == ' ' || s[b-1] == '\t' || s[b-1] == '\n' || s[b-1] == '\r')) b--;
    char *out = malloc(b - a + 1);
    memcpy(out, s + a, b - a);
    out[b - a] = 0;
    return out;
}

static void add_file(const char *dir, const char *name) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    struct stat st;
    if (stat(path, &st) != 0) return;
    char *raw = read_file(path);
    if (!raw) return;
    /* normalise CRLF in place */
    char *w = raw;
    for (char *r = raw; *r; r++) { if (*r == '\r' && r[1] == '\n') continue; *w++ = *r; }
    *w = 0;

    long long mts = (long long)st.st_mtime / 86400 * 86400;
    int idx = 0;
    char *p = raw;
    while (*p) {
        char *sep = strstr(p, "\n\n");
        size_t len = sep ? (size_t)(sep - p) : strlen(p);
        char *text = trim_copy(p, len);
        if (*text) {
            if (nchunks == cap_chunks) {
                cap_chunks = cap_chunks ? cap_chunks * 2 : 256;
                chunks = realloc(chunks, sizeof(Chunk) * (size_t)cap_chunks);
                if (!chunks) { fprintf(stderr, "out of memory\n"); exit(1); }
            }
            Chunk *c = &chunks[nchunks];
            snprintf(c->id, sizeof(c->id), "%s#%d", name, idx);
            long long ts = parse_date(name);
            if (ts < 0) ts = parse_date(text);
            c->ts = ts < 0 ? mts : ts;
            c->ngrams = build_grams(text, c->vec, &c->grams);
            nchunks++;
            idx++;
        }
        free(text);
        if (!sep) break;
        p = sep + 2;
    }
    free(raw);
}

static int cmp_names(const void *a, const void *b) {
    return strcmp(*(const char * const *)a, *(const char * const *)b);
}

static int cmp_chunks(const void *a, const void *b) {
    const Chunk *x = a, *y = b;
    if (x->score > y->score) return -1;
    if (x->score < y->score) return 1;
    return strcmp(x->id, y->id);
}

static int inter_count(const uint64_t *a, int na, const uint64_t *b, int nb) {
    int i = 0, j = 0, n = 0;
    while (i < na && j < nb) {
        if (a[i] == b[j]) { n++; i++; j++; }
        else if (a[i] < b[j]) i++;
        else j++;
    }
    return n;
}

int main(int argc, char **argv) {
    const char *root = argc > 1 ? argv[1] : ".";
    char qpath[1024], cdir[1024];
    snprintf(qpath, sizeof(qpath), "%s/data/queries.txt", root);
    snprintf(cdir, sizeof(cdir), "%s/data/corpus", root);

    char *qtxt = read_file(qpath);
    if (!qtxt) { fprintf(stderr, "cannot read %s\n", qpath); return 1; }
    /* parse queries: first line "now YYYY-MM-DD" */
    long long now = 0;
    char *nl = strchr(qtxt, '\n');
    if (nl) { now = parse_date(qtxt); *nl = 0; }
    char *queries[64]; int nq = 0;
    char *p = nl ? nl + 1 : qtxt + strlen(qtxt);
    while (*p && nq < 64) {
        char *e = strchr(p, '\n');
        if (e) *e = 0;
        if (*p) queries[nq++] = p;
        if (!e) break;
        p = e + 1;
    }

    DIR *d = opendir(cdir);
    if (!d) { fprintf(stderr, "cannot open %s\n", cdir); return 1; }
    char *names[1024]; int nn = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && nn < 1024) {
        size_t l = strlen(ent->d_name);
        if (l > 3 && strcmp(ent->d_name + l - 3, ".md") == 0) names[nn++] = strdup(ent->d_name);
    }
    closedir(d);
    qsort(names, (size_t)nn, sizeof(char *), cmp_names);
    for (int i = 0; i < nn; i++) { add_file(cdir, names[i]); free(names[i]); }

    for (int qi = 0; qi < nq; qi++) {
        double qvec[DIM];
        uint64_t *qg;
        int qn = build_grams(queries[qi], qvec, &qg);
        for (int i = 0; i < nchunks; i++) {
            double cos = 0.0;
            for (int k = 0; k < DIM; k++) cos += qvec[k] * chunks[i].vec[k];
            if (cos < 0) cos = 0.0;
            double lex = (double)inter_count(qg, qn, chunks[i].grams, chunks[i].ngrams) / (qn > 0 ? qn : 1);
            double sim = 0.72 * cos + 0.28 * lex;
            double age = (double)(now - chunks[i].ts) / 86400.0;
            if (age < 0) age = 0.0;
            double decay = pow(0.5, age / HALF_LIFE);
            chunks[i].score = 0.75 * sim + 0.25 * decay;
        }
        Chunk *sorted = malloc(sizeof(Chunk) * (size_t)nchunks);
        memcpy(sorted, chunks, sizeof(Chunk) * (size_t)nchunks);
        qsort(sorted, (size_t)nchunks, sizeof(Chunk), cmp_chunks);
        printf("{\"query\":\"%s\",\"top\":[", queries[qi]);
        for (int i = 0; i < 3 && i < nchunks; i++) {
            if (i) printf(",");
            printf("{\"chunk\":\"%s\",\"score\":%.6f}", sorted[i].id, sorted[i].score);
        }
        printf("]}\n");
        free(sorted);
        free(qg);
    }
    return 0;
}
