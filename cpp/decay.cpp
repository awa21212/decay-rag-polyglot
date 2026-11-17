// decay-rag portable spec — C++17 port
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <sys/stat.h>

namespace fs = std::filesystem;

static constexpr int DIM = 4096;
static constexpr double HALF_LIFE = 30.0;

struct Chunk {
    std::string id;
    long long ts = 0;
    std::vector<double> vec;
    std::vector<uint64_t> grams;
    double score = 0.0;
};

static bool keep_cp(unsigned cp) {
    return (cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'Z') ||
           (cp >= 'a' && cp <= 'z') || (cp >= 0x4E00 && cp <= 0x9FFF);
}

static std::vector<unsigned> codepoints(const std::string &s) {
    std::vector<unsigned> out;
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        unsigned cp; int len;
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
        else { cp = c & 0x07; len = 4; }
        for (int k = 1; k < len && i + k < s.size(); k++)
            cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
        i += len;
        if (keep_cp(cp)) out.push_back(cp);
    }
    return out;
}

static uint64_t fnv1a(unsigned cp1, unsigned cp2) {
    uint64_t h = 14695981039346656037ULL;
    uint32_t pair[2] = {cp1, cp2};
    const unsigned char *b = reinterpret_cast<const unsigned char *>(pair);
    for (int i = 0; i < 8; i++) { h ^= b[i]; h *= 1099511628211ULL; }
    return h;
}

static void build(const std::string &text, std::vector<double> &vec, std::vector<uint64_t> &grams) {
    auto cps = codepoints(text);
    vec.assign(DIM, 0.0);
    grams.clear();
    for (size_t i = 0; i + 1 < cps.size(); i++) {
        uint64_t h = fnv1a(cps[i], cps[i + 1]);
        grams.push_back(h);
        vec[h % DIM] += ((h >> 63) & 1) ? -1.0 : 1.0;
    }
    double norm = 0.0;
    for (double v : vec) norm += v * v;
    norm = std::sqrt(norm);
    if (norm > 0) for (double &v : vec) v /= norm;
    std::sort(grams.begin(), grams.end());
}

static long long days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    long long era = (y >= 0 ? y : y - 399) / 400;
    long long yoe = y - era * 400;
    long long doy = (153LL * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static long long parse_date(const std::string &s) {
    for (size_t i = 0; i + 10 <= s.size(); i++) {
        bool ok = true;
        for (int k = 0; k < 10; k++) {
            if (k == 4 || k == 7) { if (s[i + k] != '-') { ok = false; break; } }
            else if (!isdigit((unsigned char)s[i + k])) { ok = false; break; }
        }
        if (!ok) continue;
        int y = std::stoi(s.substr(i, 4));
        int m = std::stoi(s.substr(i + 5, 2));
        int d = std::stoi(s.substr(i + 8, 2));
        return days_from_civil(y, m, d) * 86400;
    }
    return -1;
}

static std::string trim(const std::string &s) {
    const char *ws = " \t\r\n";
    size_t a = s.find_first_not_of(ws);
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(ws);
    return s.substr(a, b - a + 1);
}

int main(int argc, char **argv) {
    std::string root = argc > 1 ? argv[1] : ".";
    std::ifstream qf(root + "/data/queries.txt");
    if (!qf) { std::fprintf(stderr, "cannot read queries\n"); return 1; }
    std::string line, qline;
    std::getline(qf, line);
    long long now = parse_date(line);
    std::vector<std::string> queries;
    while (std::getline(qf, line)) {
        qline = trim(line);
        if (!qline.empty()) queries.push_back(qline);
    }

    std::vector<std::string> names;
    for (const auto &e : fs::directory_iterator(root + "/data/corpus")) {
        std::string n = e.path().filename().string();
        if (n.size() > 3 && n.substr(n.size() - 3) == ".md") names.push_back(n);
    }
    std::sort(names.begin(), names.end());

    std::vector<Chunk> chunks;
    for (const auto &name : names) {
        std::ifstream in(root + "/data/corpus/" + name, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        std::string raw = ss.str();
        std::string clean;
        for (size_t i = 0; i < raw.size(); i++) {
            if (raw[i] == '\r' && i + 1 < raw.size() && raw[i + 1] == '\n') continue;
            clean.push_back(raw[i]);
        }
        struct stat st;
        long long mts = 0;
        if (::stat((root + "/data/corpus/" + name).c_str(), &st) == 0)
            mts = (long long)st.st_mtime / 86400 * 86400;

        int idx = 0;
        size_t pos = 0;
        while (pos <= clean.size()) {
            size_t sep = clean.find("\n\n", pos);
            std::string text = trim(clean.substr(pos, sep == std::string::npos ? std::string::npos : sep - pos));
            if (!text.empty()) {
                Chunk c;
                c.id = name + "#" + std::to_string(idx++);
                long long ts = parse_date(name);
                if (ts < 0) ts = parse_date(text);
                c.ts = ts < 0 ? mts : ts;
                build(text, c.vec, c.grams);
                chunks.push_back(std::move(c));
            }
            if (sep == std::string::npos) break;
            pos = sep + 2;
        }
    }

    for (const auto &q : queries) {
        std::vector<double> qvec;
        std::vector<uint64_t> qg;
        build(q, qvec, qg);
        std::vector<Chunk> scored = chunks;
        for (auto &c : scored) {
            double cos = 0.0;
            for (int k = 0; k < DIM; k++) cos += qvec[k] * c.vec[k];
            if (cos < 0) cos = 0.0;
            std::vector<uint64_t> inter;
            std::set_intersection(qg.begin(), qg.end(), c.grams.begin(), c.grams.end(),
                                  std::back_inserter(inter));
            double lex = (double)inter.size() / (qg.empty() ? 1 : qg.size());
            double sim = 0.72 * cos + 0.28 * lex;
            double age = (double)(now - c.ts) / 86400.0;
            if (age < 0) age = 0.0;
            c.score = 0.75 * sim + 0.25 * std::pow(0.5, age / HALF_LIFE);
        }
        std::sort(scored.begin(), scored.end(), [](const Chunk &a, const Chunk &b) {
            if (a.score != b.score) return a.score > b.score;
            return a.id < b.id;
        });
        std::printf("{\"query\":\"%s\",\"top\":[", q.c_str());
        for (size_t i = 0; i < scored.size() && i < 3; i++) {
            if (i) std::printf(",");
            std::printf("{\"chunk\":\"%s\",\"score\":%.6f}", scored[i].id.c_str(), scored[i].score);
        }
        std::printf("]}\n");
    }
    return 0;
}
