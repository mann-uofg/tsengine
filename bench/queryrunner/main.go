// queryrunner times the same TSBS-style queries against tsengine and TimescaleDB, and checks
// that both return the same answers.
//
// The query shapes and the SQL are those of the TSBS cpu-only use case (see
// cmd/tsbs_generate_queries/databases/timescaledb/devops.go in TSBS). TSBS itself cannot
// drive tsengine, which has its own HTTP API, so this program generates the query parameters
// once (hosts and time window, from a seed) and sends the equivalent request to each system.
//
// Timing covers sending the request and reading the complete response, for both systems.
// Decoding the response is not timed; a separate, untimed pass decodes both and compares.
package main

import (
	"database/sql"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"math"
	"math/rand"
	"net/http"
	"net/url"
	"os"
	"sort"
	"strings"
	"time"

	_ "github.com/lib/pq"
)

var fields = []string{
	"usage_user", "usage_system", "usage_idle", "usage_nice", "usage_iowait",
	"usage_irq", "usage_softirq", "usage_steal", "usage_guest", "usage_guest_nice",
}

type queryType struct {
	Name     string
	NumHosts int // 0 means every host
	Window   time.Duration
	Bucket   time.Duration
	Shape    string // "max", "max-all" or "mean-by-host"
	About    string
}

var queryTypes = []queryType{
	{"single-groupby-1-1-1", 1, time.Hour, time.Minute, "max",
		"max of 1 metric, 1 host, per minute, over 1 hour"},
	{"single-groupby-1-1-12", 1, 12 * time.Hour, time.Minute, "max",
		"max of 1 metric, 1 host, per minute, over 12 hours"},
	{"single-groupby-1-8-1", 8, time.Hour, time.Minute, "max",
		"max of 1 metric, 8 hosts, per minute, over 1 hour"},
	{"cpu-max-all-1", 1, 8 * time.Hour, time.Hour, "max-all",
		"max of all 10 metrics, 1 host, per hour, over 8 hours"},
	{"double-groupby-1", 0, 12 * time.Hour, time.Hour, "mean-by-host",
		"mean of 1 metric per host, all hosts, per hour, over 12 hours"},
}

// One generated query: which hosts, and the window [Start, End).
type params struct {
	Hosts []string
	Start time.Time
	End   time.Time
}

// A query result in a form both systems can be reduced to: group name -> points in time order.
type point struct {
	T int64
	V float64
}
type result map[string][]point

type system interface {
	name() string
	// run sends the query and reads the whole response without decoding it.
	run(qt queryType, p params) error
	// fetch sends the query and decodes the response.
	fetch(qt queryType, p params) (result, error)
}

// ---------------------------------------------------------------------------- tsengine

type tsengine struct {
	base   string
	client *http.Client
	// Totals reported by the engine during fetch passes.
	blocksFromStats int64
	blocksDecoded   int64
}

func (s *tsengine) name() string { return "tsengine" }

func (s *tsengine) url(qt queryType, p params) string {
	q := url.Values{}
	q.Set("measurement", "cpu")
	q.Set("start", fmt.Sprint(p.Start.UnixNano()))
	// tsengine ranges include both ends; TSBS windows exclude the end.
	q.Set("end", fmt.Sprint(p.End.UnixNano()-1))
	q.Set("bucket", fmt.Sprint(qt.Bucket.Nanoseconds()))
	for _, h := range p.Hosts {
		q.Add("tag", "hostname:"+h)
	}
	switch qt.Shape {
	case "max":
		q.Set("field", "usage_user")
		q.Set("agg", "max")
	case "max-all":
		q.Set("agg", "max")
		q.Set("group_by", "__field__")
	case "mean-by-host":
		q.Set("field", "usage_user")
		q.Set("agg", "mean")
		q.Set("group_by", "hostname")
	}
	return s.base + "/api/v1/query?" + q.Encode()
}

func (s *tsengine) get(qt queryType, p params) ([]byte, error) {
	resp, err := s.client.Get(s.url(qt, p))
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	body, err := io.ReadAll(resp.Body)
	if err != nil {
		return nil, err
	}
	if resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("status %d: %s", resp.StatusCode, body)
	}
	return body, nil
}

func (s *tsengine) run(qt queryType, p params) error {
	_, err := s.get(qt, p)
	return err
}

func (s *tsengine) fetch(qt queryType, p params) (result, error) {
	body, err := s.get(qt, p)
	if err != nil {
		return nil, err
	}
	var decoded struct {
		Results []struct {
			Name       string
			Timestamps []int64
			Values     []float64
		}
		Stats struct {
			BlocksFromStats int64 `json:"blocks_from_stats"`
			BlocksDecoded   int64 `json:"blocks_decoded"`
		}
	}
	if err := json.Unmarshal(body, &decoded); err != nil {
		return nil, err
	}
	s.blocksFromStats += decoded.Stats.BlocksFromStats
	s.blocksDecoded += decoded.Stats.BlocksDecoded
	out := result{}
	for _, r := range decoded.Results {
		for i := range r.Timestamps {
			out[r.Name] = append(out[r.Name], point{r.Timestamps[i], r.Values[i]})
		}
	}
	return out, nil
}

// ------------------------------------------------------------------------- TimescaleDB

type timescale struct {
	db *sql.DB
}

func (s *timescale) name() string { return "timescaledb" }

const pgTime = "2006-01-02 15:04:05.999999 -0700"

func hostWhere(hosts []string) string {
	quoted := make([]string, len(hosts))
	for i, h := range hosts {
		quoted[i] = "'" + h + "'"
	}
	return "tags_id IN (SELECT id FROM tags WHERE hostname IN (" + strings.Join(quoted, ",") + "))"
}

func (s *timescale) sql(qt queryType, p params) string {
	start, end := p.Start.Format(pgTime), p.End.Format(pgTime)
	seconds := int(qt.Bucket.Seconds())
	switch qt.Shape {
	case "max":
		return fmt.Sprintf(`SELECT time_bucket('%d seconds', time) AS bucket, max(usage_user)
			FROM cpu WHERE %s AND time >= '%s' AND time < '%s'
			GROUP BY bucket ORDER BY bucket ASC`, seconds, hostWhere(p.Hosts), start, end)
	case "max-all":
		selects := make([]string, len(fields))
		for i, f := range fields {
			selects[i] = "max(" + f + ")"
		}
		return fmt.Sprintf(`SELECT time_bucket('%d seconds', time) AS bucket, %s
			FROM cpu WHERE %s AND time >= '%s' AND time < '%s'
			GROUP BY bucket ORDER BY bucket ASC`,
			seconds, strings.Join(selects, ", "), hostWhere(p.Hosts), start, end)
	default: // mean-by-host
		return fmt.Sprintf(`WITH cpu_avg AS (
				SELECT time_bucket('%d seconds', time) AS bucket, tags_id,
					avg(usage_user) AS mean_usage_user
				FROM cpu WHERE time >= '%s' AND time < '%s' GROUP BY bucket, tags_id)
			SELECT bucket, tags.hostname, mean_usage_user
			FROM cpu_avg JOIN tags ON cpu_avg.tags_id = tags.id
			ORDER BY bucket, tags.hostname`, seconds, start, end)
	}
}

func (s *timescale) run(qt queryType, p params) error {
	rows, err := s.db.Query(s.sql(qt, p))
	if err != nil {
		return err
	}
	defer rows.Close()
	for rows.Next() {
	}
	return rows.Err()
}

func (s *timescale) fetch(qt queryType, p params) (result, error) {
	rows, err := s.db.Query(s.sql(qt, p))
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := result{}
	for rows.Next() {
		var bucket time.Time
		switch qt.Shape {
		case "max":
			var v float64
			if err := rows.Scan(&bucket, &v); err != nil {
				return nil, err
			}
			out[""] = append(out[""], point{bucket.UnixNano(), v})
		case "max-all":
			values := make([]float64, len(fields))
			dest := []any{&bucket}
			for i := range values {
				dest = append(dest, &values[i])
			}
			if err := rows.Scan(dest...); err != nil {
				return nil, err
			}
			for i, f := range fields {
				out[f] = append(out[f], point{bucket.UnixNano(), values[i]})
			}
		default:
			var host string
			var v float64
			if err := rows.Scan(&bucket, &host, &v); err != nil {
				return nil, err
			}
			out[host] = append(out[host], point{bucket.UnixNano(), v})
		}
	}
	return out, rows.Err()
}

// ------------------------------------------------------------------------------ driver

func generate(qt queryType, n, hosts int, dataStart, dataEnd time.Time, seed int64) []params {
	rng := rand.New(rand.NewSource(seed))
	room := int64(dataEnd.Sub(dataStart)-qt.Window) / int64(time.Second)
	out := make([]params, n)
	for i := range out {
		start := dataStart.Add(time.Duration(rng.Int63n(room+1)) * time.Second)
		p := params{Start: start, End: start.Add(qt.Window)}
		for _, h := range rng.Perm(hosts)[:qt.NumHosts] {
			p.Hosts = append(p.Hosts, fmt.Sprintf("host_%d", h))
		}
		out[i] = p
	}
	return out
}

// same returns "" if the two results agree, or a description of the first difference.
func same(a, b result) string {
	if len(a) != len(b) {
		return fmt.Sprintf("%d groups vs %d", len(a), len(b))
	}
	for group, pa := range a {
		pb, ok := b[group]
		if !ok {
			return fmt.Sprintf("group %q missing", group)
		}
		if len(pa) != len(pb) {
			return fmt.Sprintf("group %q: %d points vs %d", group, len(pa), len(pb))
		}
		for i := range pa {
			if pa[i].T != pb[i].T {
				return fmt.Sprintf("group %q point %d: time %d vs %d", group, i, pa[i].T, pb[i].T)
			}
			diff := math.Abs(pa[i].V - pb[i].V)
			if diff > 1e-9*math.Max(1, math.Abs(pa[i].V)) {
				return fmt.Sprintf("group %q point %d: value %v vs %v", group, i, pa[i].V, pb[i].V)
			}
		}
	}
	return ""
}

func percentile(sorted []float64, p float64) float64 {
	if len(sorted) == 0 {
		return 0
	}
	rank := int(math.Ceil(p/100*float64(len(sorted)))) - 1
	if rank < 0 {
		rank = 0
	}
	return sorted[rank]
}

type latency struct {
	Samples   int       `json:"samples"`
	MinMs     float64   `json:"min_ms"`
	P50Ms     float64   `json:"p50_ms"`
	P95Ms     float64   `json:"p95_ms"`
	P99Ms     float64   `json:"p99_ms"`
	MaxMs     float64   `json:"max_ms"`
	MeanMs    float64   `json:"mean_ms"`
	PassP50Ms []float64 `json:"pass_p50_ms"`
}

func timeSystem(s system, qt queryType, queries []params, warmups, passes int) (latency, error) {
	for w := 0; w < warmups; w++ {
		for _, p := range queries {
			if err := s.run(qt, p); err != nil {
				return latency{}, err
			}
		}
	}
	var all []float64
	var passP50 []float64
	for pass := 0; pass < passes; pass++ {
		one := make([]float64, 0, len(queries))
		for _, p := range queries {
			begin := time.Now()
			if err := s.run(qt, p); err != nil {
				return latency{}, err
			}
			one = append(one, float64(time.Since(begin).Nanoseconds())/1e6)
		}
		all = append(all, one...)
		sort.Float64s(one)
		passP50 = append(passP50, percentile(one, 50))
	}
	sort.Float64s(all)
	sum := 0.0
	for _, v := range all {
		sum += v
	}
	return latency{
		Samples: len(all), MinMs: all[0], P50Ms: percentile(all, 50), P95Ms: percentile(all, 95),
		P99Ms: percentile(all, 99), MaxMs: all[len(all)-1], MeanMs: sum / float64(len(all)),
		PassP50Ms: passP50,
	}, nil
}

type typeReport struct {
	Type            string             `json:"type"`
	About           string             `json:"about"`
	Queries         int                `json:"queries"`
	Verified        int                `json:"verified"`
	Mismatches      int                `json:"mismatches"`
	FirstMismatch   string             `json:"first_mismatch,omitempty"`
	ResultPoints    int                `json:"result_points_per_query"`
	BlocksFromStats int64              `json:"tsengine_blocks_from_stats"`
	BlocksDecoded   int64              `json:"tsengine_blocks_decoded"`
	Latency         map[string]latency `json:"latency"`
}

func main() {
	tsURL := flag.String("tsengine", "http://127.0.0.1:18086", "tsengine base URL; empty to skip")
	pgURL := flag.String("timescaledb", "", "PostgreSQL connection string; empty to skip")
	typesFlag := flag.String("types", "all", "comma-separated query types, or all")
	n := flag.Int("n", 1000, "distinct queries per type")
	passes := flag.Int("passes", 5, "timed passes over the queries")
	warmups := flag.Int("warmups", 1, "untimed passes before timing")
	hosts := flag.Int("hosts", 100, "number of hosts in the dataset")
	startFlag := flag.String("start", "2016-01-01T00:00:00Z", "first timestamp of the dataset")
	endFlag := flag.String("end", "2016-01-04T00:00:00Z", "end of the dataset (exclusive)")
	seed := flag.Int64("seed", 42, "seed for the query parameters")
	label := flag.String("label", "", "free text stored in the output")
	outPath := flag.String("out", "", "write the JSON report here (default: stdout)")
	flag.Parse()

	dataStart, err := time.Parse(time.RFC3339, *startFlag)
	check(err)
	dataEnd, err := time.Parse(time.RFC3339, *endFlag)
	check(err)

	var ts *tsengine
	var systems []system
	if *tsURL != "" {
		transport := &http.Transport{MaxIdleConnsPerHost: 1}
		ts = &tsengine{base: *tsURL, client: &http.Client{Transport: transport}}
		systems = append(systems, ts)
	}
	if *pgURL != "" {
		db, err := sql.Open("postgres", *pgURL)
		check(err)
		// One connection, like one TSBS query worker.
		db.SetMaxOpenConns(1)
		check(db.Ping())
		systems = append(systems, &timescale{db: db})
	}

	var reports []typeReport
	for _, qt := range queryTypes {
		if *typesFlag != "all" && !strings.Contains(","+*typesFlag+",", ","+qt.Name+",") {
			continue
		}
		queries := generate(qt, *n, *hosts, dataStart, dataEnd, *seed)
		report := typeReport{Type: qt.Name, About: qt.About, Queries: len(queries),
			Latency: map[string]latency{}}

		// Correctness first: every query must give the same answer on both systems.
		if ts != nil {
			ts.blocksFromStats, ts.blocksDecoded = 0, 0
		}
		for _, p := range queries {
			var first result
			for i, s := range systems {
				r, err := s.fetch(qt, p)
				check(err)
				if i == 0 {
					first = r
					for _, points := range r {
						report.ResultPoints += len(points)
					}
					continue
				}
				report.Verified++
				if why := same(first, r); why != "" {
					report.Mismatches++
					if report.FirstMismatch == "" {
						report.FirstMismatch = fmt.Sprintf("%v %v..%v: %s",
							p.Hosts, p.Start.Format(time.RFC3339), p.End.Format(time.RFC3339), why)
					}
				}
			}
		}
		report.ResultPoints /= len(queries)
		if ts != nil {
			report.BlocksFromStats, report.BlocksDecoded = ts.blocksFromStats, ts.blocksDecoded
		}

		for _, s := range systems {
			l, err := timeSystem(s, qt, queries, *warmups, *passes)
			check(err)
			report.Latency[s.name()] = l
			fmt.Fprintf(os.Stderr, "%-22s %-12s p50 %8.3f ms  p99 %8.3f ms  (%d samples)\n",
				qt.Name, s.name(), l.P50Ms, l.P99Ms, l.Samples)
		}
		if len(systems) > 1 {
			fmt.Fprintf(os.Stderr, "%-22s verified %d, mismatches %d %s\n",
				qt.Name, report.Verified, report.Mismatches, report.FirstMismatch)
		}
		reports = append(reports, report)
	}

	out := map[string]any{"label": *label, "queries_per_type": *n, "passes": *passes,
		"warmups": *warmups, "seed": *seed, "types": reports}
	encoded, err := json.MarshalIndent(out, "", "  ")
	check(err)
	if *outPath == "" {
		fmt.Println(string(encoded))
	} else {
		check(os.WriteFile(*outPath, append(encoded, '\n'), 0o644))
	}
}

func check(err error) {
	if err != nil {
		fmt.Fprintln(os.Stderr, "error:", err)
		os.Exit(1)
	}
}
