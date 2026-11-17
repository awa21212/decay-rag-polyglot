// decay-rag portable spec — Go 1.21+ port
package main

import (
	"bufio"
	"fmt"
	"math"
	"os"
	"path/filepath"
	"sort"
	"strings"
)

const (
	dim      = 4096
	halfLife = 30.0
)

type chunk struct {
	id    string
	ts    int64
	vec   []float64
	grams []uint64
}

func keepCp(cp rune) bool {
	return (cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'Z') ||
		(cp >= 'a' && cp <= 'z') || (cp >= 0x4E00 && cp <= 0x9FFF)
}

func fnv1a(a, b rune) uint64 {
	h := uint64(14695981039346656037)
	pair := []uint32{uint32(a), uint32(b)}
	for _, v := range pair {
		for i := 0; i < 4; i++ {
			h ^= uint64(byte(v >> (8 * i)))
			h *= 1099511628211
		}
	}
	return h
}

func build(text string) ([]float64, []uint64) {
	vec := make([]float64, dim)
	var grams []uint64
	prev := rune(-1)
	for _, r := range text {
		if !keepCp(r) {
			continue
		}
		if prev >= 0 {
			h := fnv1a(prev, r)
			grams = append(grams, h)
			if (h>>63)&1 == 1 {
				vec[h%dim] -= 1
			} else {
				vec[h%dim] += 1
			}
		}
		prev = r
	}
	var norm float64
	for _, v := range vec {
		norm += v * v
	}
	norm = math.Sqrt(norm)
	if norm > 0 {
		for i := range vec {
			vec[i] /= norm
		}
	}
	sort.Slice(grams, func(i, j int) bool { return grams[i] < grams[j] })
	return vec, grams
}

func daysFromCivil(y, m, d int) int64 {
	if m <= 2 {
		y--
	}
	era := y / 400
	if y < 0 && y%400 != 0 {
		era--
	}
	yoe := int64(y - era*400)
	doy := int64((153*(m+func() int {
		if m > 2 {
			return -3
		}
		return 9
	}())+2)/5 + d - 1)
	doe := yoe*365 + yoe/4 - yoe/100 + doy
	return int64(era)*146097 + doe - 719468
}

func parseDate(s string) int64 {
	b := []byte(s)
	for i := 0; i+10 <= len(b); i++ {
		ok := true
		for k := 0; k < 10; k++ {
			if k == 4 || k == 7 {
				if b[i+k] != '-' {
					ok = false
					break
				}
			} else if b[i+k] < '0' || b[i+k] > '9' {
				ok = false
				break
			}
		}
		if !ok {
			continue
		}
		y := int(b[i]-'0')*1000 + int(b[i+1]-'0')*100 + int(b[i+2]-'0')*10 + int(b[i+3]-'0')
		m := int(b[i+5]-'0')*10 + int(b[i+6]-'0')
		d := int(b[i+8]-'0')*10 + int(b[i+9]-'0')
		return daysFromCivil(y, m, d) * 86400
	}
	return -1
}

func main() {
	root := "."
	if len(os.Args) > 1 {
		root = os.Args[1]
	}
	qf, err := os.Open(filepath.Join(root, "data", "queries.txt"))
	if err != nil {
		fmt.Fprintln(os.Stderr, "cannot read queries")
		os.Exit(1)
	}
	var now int64
	var queries []string
	sc := bufio.NewScanner(qf)
	first := true
	for sc.Scan() {
		line := strings.TrimSpace(sc.Text())
		if first {
			now = parseDate(line)
			first = false
			continue
		}
		if line != "" {
			queries = append(queries, line)
		}
	}
	qf.Close()

	files, _ := filepath.Glob(filepath.Join(root, "data", "corpus", "*.md"))
	sort.Strings(files)

	var chunks []chunk
	for _, path := range files {
		raw, err := os.ReadFile(path)
		if err != nil {
			continue
		}
		text := strings.ReplaceAll(string(raw), "\r\n", "\n")
		st, _ := os.Stat(path)
		mts := st.ModTime().Unix()
		mts = mts / 86400 * 86400
		name := filepath.Base(path)
		idx := 0
		pos := 0
		for {
			sep := strings.Index(text[pos:], "\n\n")
			var piece string
			if sep < 0 {
				piece = text[pos:]
			} else {
				piece = text[pos : pos+sep]
			}
			piece = strings.Trim(piece, " \t\r\n")
			if piece != "" {
				ts := parseDate(name)
				if ts < 0 {
					ts = parseDate(piece)
				}
				if ts < 0 {
					ts = mts
				}
				vec, grams := build(piece)
				chunks = append(chunks, chunk{
					id:    fmt.Sprintf("%s#%d", name, idx),
					ts:    ts,
					vec:   vec,
					grams: grams,
				})
				idx++
			}
			if sep < 0 {
				break
			}
			pos += sep + 2
		}
	}

	for _, q := range queries {
		qvec, qg := build(q)
		type scored struct {
			id string
			s  float64
		}
		var out []scored
		for _, c := range chunks {
			var cos float64
			for k := 0; k < dim; k++ {
				cos += qvec[k] * c.vec[k]
			}
			if cos < 0 {
				cos = 0
			}
			i, j, n := 0, 0, 0
			for i < len(qg) && j < len(c.grams) {
				if qg[i] == c.grams[j] {
					n++
					i++
					j++
				} else if qg[i] < c.grams[j] {
					i++
				} else {
					j++
				}
			}
			denom := len(qg)
			if denom == 0 {
				denom = 1
			}
			lex := float64(n) / float64(denom)
			sim := 0.72*cos + 0.28*lex
			age := float64(now-c.ts) / 86400.0
			if age < 0 {
				age = 0
			}
			out = append(out, scored{c.id, 0.75*sim + 0.25*math.Pow(0.5, age/halfLife)})
		}
		sort.Slice(out, func(a, b int) bool {
			if out[a].s != out[b].s {
				return out[a].s > out[b].s
			}
			return out[a].id < out[b].id
		})
		n := 3
		if len(out) < n {
			n = len(out)
		}
		parts := make([]string, 0, n)
		for i := 0; i < n; i++ {
			parts = append(parts, fmt.Sprintf(`{"chunk":"%s","score":%.6f}`, out[i].id, out[i].s))
		}
		fmt.Printf("{\"query\":\"%s\",\"top\":[%s]}\n", q, strings.Join(parts, ","))
	}
}
