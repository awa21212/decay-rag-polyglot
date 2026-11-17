// decay-rag portable spec — Java 17+ port (single file)
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.io.PrintStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;

public class DecayRag {
    static final int DIM = 4096;
    static final double HALF_LIFE = 30.0;

    record Chunk(String id, long ts, double[] vec, long[] grams, double score) {}

    static boolean keepCp(int cp) {
        return (cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'Z')
                || (cp >= 'a' && cp <= 'z') || (cp >= 0x4E00 && cp <= 0x9FFF);
    }

    static long fnv1a(int cp1, int cp2) {
        long h = 0xcbf29ce484222325L;
        int[] pair = {cp1, cp2};
        for (int v : pair) {
            for (int i = 0; i < 4; i++) {
                h ^= (v >>> (8 * i)) & 0xFF;
                h *= 0x100000001b3L;
            }
        }
        return h;
    }

    static double[] buildVec(String text, List<Long> outGrams) {
        List<Integer> cps = new ArrayList<>();
        for (int i = 0; i < text.length(); ) {
            int cp = text.codePointAt(i);
            i += Character.charCount(cp);
            if (keepCp(cp)) cps.add(cp);
        }
        double[] vec = new double[DIM];
        for (int i = 0; i + 1 < cps.size(); i++) {
            long h = fnv1a(cps.get(i), cps.get(i + 1));
            outGrams.add(h);
            vec[(int) Long.remainderUnsigned(h, DIM)] += (h >>> 63) == 1 ? -1.0 : 1.0;
        }
        double norm = 0;
        for (double v : vec) norm += v * v;
        norm = Math.sqrt(norm);
        if (norm > 0) for (int i = 0; i < DIM; i++) vec[i] /= norm;
        outGrams.sort(null);
        return vec;
    }

    static long daysFromCivil(int y, int m, int d) {
        y -= (m <= 2) ? 1 : 0;
        long era = (y >= 0 ? y : y - 399) / 400;
        long yoe = y - era * 400;
        long doy = (153L * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
        long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return era * 146097 + doe - 719468;
    }

    static long parseDate(String s) {
        for (int i = 0; i + 10 <= s.length(); i++) {
            String seg = s.substring(i, i + 10);
            if (!seg.matches("\\d{4}-\\d{2}-\\d{2}")) continue;
            int y = Integer.parseInt(seg.substring(0, 4));
            int m = Integer.parseInt(seg.substring(5, 7));
            int d = Integer.parseInt(seg.substring(8, 10));
            return daysFromCivil(y, m, d) * 86400;
        }
        return -1;
    }

    public static void main(String[] args) throws IOException {
        System.setOut(new PrintStream(System.out, true, StandardCharsets.UTF_8));
        String root = args.length > 0 ? args[0] : ".";
        Path qpath = Paths.get(root, "data", "queries.txt");
        List<String> lines = Files.readAllLines(qpath, StandardCharsets.UTF_8);
        long now = parseDate(lines.get(0));
        List<String> queries = new ArrayList<>();
        for (int i = 1; i < lines.size(); i++) {
            String t = lines.get(i).strip();
            if (!t.isEmpty()) queries.add(t);
        }

        Path corpus = Paths.get(root, "data", "corpus");
        List<Path> files = new ArrayList<>();
        try (DirectoryStream<Path> ds = Files.newDirectoryStream(corpus, "*.md")) {
            for (Path p : ds) files.add(p);
        }
        files.sort(Comparator.comparing(p -> p.getFileName().toString()));

        List<Chunk> chunks = new ArrayList<>();
        for (Path p : files) {
            String name = p.getFileName().toString();
            String raw = Files.readString(p, StandardCharsets.UTF_8).replace("\r\n", "\n");
            long mtime = Files.getLastModifiedTime(p).toMillis() / 1000;
            long mts = (mtime / 86400) * 86400;
            int idx = 0;
            for (String piece : raw.split("\n\n", -1)) {
                String text = piece.strip();
                if (text.isEmpty()) continue;
                long ts = parseDate(name);
                if (ts < 0) ts = parseDate(text);
                if (ts < 0) ts = mts;
                List<Long> grams = new ArrayList<>();
                double[] vec = buildVec(text, grams);
                long[] g = new long[grams.size()];
                for (int i = 0; i < g.length; i++) g[i] = grams.get(i);
                chunks.add(new Chunk(name + "#" + (idx++), ts, vec, g, 0.0));
            }
        }

        StringBuilder sb = new StringBuilder();
        for (String q : queries) {
            List<Long> qgList = new ArrayList<>();
            double[] qv = buildVec(q, qgList);
            long[] qg = new long[qgList.size()];
            for (int i = 0; i < qg.length; i++) qg[i] = qgList.get(i);

            List<Chunk> scored = new ArrayList<>();
            for (Chunk c : chunks) {
                double cos = 0;
                for (int k = 0; k < DIM; k++) cos += qv[k] * c.vec()[k];
                if (cos < 0) cos = 0;
                int i = 0, j = 0, n = 0;
                while (i < qg.length && j < c.grams().length) {
                    if (qg[i] == c.grams()[j]) { n++; i++; j++; }
                    else if (qg[i] < c.grams()[j]) i++;
                    else j++;
                }
                double lex = (double) n / Math.max(1, qg.length);
                double sim = 0.72 * cos + 0.28 * lex;
                double age = Math.max(0.0, (now - c.ts()) / 86400.0);
                double decay = Math.pow(0.5, age / HALF_LIFE);
                scored.add(new Chunk(c.id(), c.ts(), c.vec(), c.grams(), 0.75 * sim + 0.25 * decay));
            }
            scored.sort((a, b) -> {
                int cmp = Double.compare(b.score(), a.score());
                return cmp != 0 ? cmp : a.id().compareTo(b.id());
            });
            sb.setLength(0);
            sb.append("{\"query\":\"").append(q).append("\",\"top\":[");
            for (int i = 0; i < 3 && i < scored.size(); i++) {
                if (i > 0) sb.append(',');
                sb.append(String.format(Locale.ROOT, "{\"chunk\":\"%s\",\"score\":%.6f}",
                        scored.get(i).id(), scored.get(i).score()));
            }
            sb.append("]}");
            System.out.println(sb);
        }
    }
}
