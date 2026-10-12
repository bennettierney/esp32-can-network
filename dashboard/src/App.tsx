import { useEffect, useMemo, useState } from "react";
import { useTelemetry, type Reading } from "./api";
import { MetricChart, type Point } from "./components/MetricChart";
import { StatusBar } from "./components/StatusBar";

const RANGES = [5, 15, 60] as const;
type Range = (typeof RANGES)[number];

interface Metric {
  key: keyof Pick<Reading, "temperature_c" | "humidity_pct" | "pressure_pa">;
  title: string;
  unit: string;
  decimals: number;
  minSpan: number; // smallest y range on the chart
}

const METRICS: Metric[] = [
  { key: "temperature_c", title: "Temperature", unit: "°C", decimals: 2, minSpan: 0.5 },
  { key: "humidity_pct", title: "Humidity", unit: "%", decimals: 2, minSpan: 1 },
  { key: "pressure_pa", title: "Pressure", unit: "Pa", decimals: 0, minSpan: 50 },
];

function StatTile({ label, value, detail }: { label: string; value: string; detail: string }) {
  return (
    <div className="card stat">
      <div className="stat-label">{label}</div>
      <div className="stat-value">{value}</div>
      <div className="stat-detail">{detail}</div>
    </div>
  );
}

const timeFormat = new Intl.DateTimeFormat(undefined, { hour: "2-digit", minute: "2-digit", second: "2-digit" });

export default function App() {
  const [minutes, setMinutes] = useState<Range>(5);
  const [showTable, setShowTable] = useState(false);
  const [now, setNow] = useState(() => Date.now());
  const { readings, status, live, loading, error } = useTelemetry(minutes);

  // Tick once a second so the time window slides and "seconds since" stays current.
  useEffect(() => {
    const id = window.setInterval(() => setNow(Date.now()), 1000);
    return () => window.clearInterval(id);
  }, []);

  const start = now - minutes * 60_000;
  const inWindow = useMemo(
    () => readings.filter((r) => Date.parse(r.recorded_at) >= start),
    [readings, start],
  );
  const latest = readings.length ? readings[readings.length - 1] : null;
  const secondsSinceReading = latest ? Math.max(0, Math.round((now - Date.parse(latest.recorded_at)) / 1000)) : null;

  const series = (key: Metric["key"]): Point[] => inWindow.map((r) => ({ t: Date.parse(r.recorded_at), v: r[key] }));

  return (
    <main className="page">
      <header className="page-header">
        <div>
          <h1>Environment telemetry</h1>
          <p className="subtitle">BME280 on node A, over CAN to node B, over BLE to the gateway</p>
        </div>
      </header>

      <StatusBar status={status} live={live} secondsSinceReading={secondsSinceReading} />

      {error && <p className="notice">Can't reach the backend: {error}</p>}

      <div className="filters" role="group" aria-label="Time range">
        {RANGES.map((m) => (
          <button key={m} type="button" aria-pressed={m === minutes} onClick={() => setMinutes(m)}>
            Last {m} min
          </button>
        ))}
        <button type="button" className="table-toggle" aria-pressed={showTable} onClick={() => setShowTable((s) => !s)}>
          {showTable ? "Hide table" : "Show table"}
        </button>
      </div>

      <div className={`content ${loading ? "refetching" : ""}`}>
        <div className="stats">
          {METRICS.map((m) => {
            const values = inWindow.map((r) => r[m.key]);
            return (
              <StatTile
                key={m.key}
                label={m.title}
                value={latest ? `${latest[m.key].toFixed(m.decimals)} ${m.unit}` : "–"}
                detail={
                  values.length
                    ? `min ${Math.min(...values).toFixed(m.decimals)}, max ${Math.max(...values).toFixed(m.decimals)} in ${minutes} min`
                    : "no data"
                }
              />
            );
          })}
        </div>

        {inWindow.length === 0 && !loading ? (
          <p className="notice">No readings in the last {minutes} minutes. Is the gateway running?</p>
        ) : (
          <div className="charts">
            {METRICS.map((m) => (
              <MetricChart
                key={m.key}
                title={m.title}
                unit={m.unit}
                decimals={m.decimals}
                minSpan={m.minSpan}
                points={series(m.key)}
                domain={[start, now]}
              />
            ))}
          </div>
        )}

        {showTable && (
          <section className="card">
            <h2 className="card-title">Latest readings</h2>
            <table className="readings">
              <thead>
                <tr>
                  <th scope="col">Time</th>
                  <th scope="col">Temperature (°C)</th>
                  <th scope="col">Humidity (%)</th>
                  <th scope="col">Pressure (Pa)</th>
                </tr>
              </thead>
              <tbody>
                {inWindow
                  .slice(-30)
                  .reverse()
                  .map((r) => (
                    <tr key={r.id}>
                      <td>{timeFormat.format(Date.parse(r.recorded_at))}</td>
                      <td>{r.temperature_c.toFixed(2)}</td>
                      <td>{r.humidity_pct.toFixed(2)}</td>
                      <td>{r.pressure_pa}</td>
                    </tr>
                  ))}
              </tbody>
            </table>
          </section>
        )}
      </div>
    </main>
  );
}
