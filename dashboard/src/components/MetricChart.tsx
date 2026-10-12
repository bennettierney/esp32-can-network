import { CartesianGrid, Line, LineChart, ResponsiveContainer, Tooltip, XAxis, YAxis } from "recharts";

export interface Point {
  t: number; // epoch ms
  v: number;
}

interface Props {
  title: string;
  unit: string;
  decimals: number;
  minSpan: number; // smallest y range shown, so sensor noise does not fill the chart
  points: Point[];
  domain: [number, number]; // x range, epoch ms
}

// Round a raw step up to 1, 2 or 5 times a power of ten.
function niceStep(raw: number): number {
  const power = 10 ** Math.floor(Math.log10(raw));
  const fraction = raw / power;
  return (fraction <= 1 ? 1 : fraction <= 2 ? 2 : fraction <= 5 ? 5 : 10) * power;
}

// Y axis range and ticks on round steps, at least minSpan wide, centred on the data.
function yScale(points: Point[], minSpan: number) {
  if (points.length === 0) return { domain: [0, 1] as [number, number], ticks: [0, 1], tickDecimals: 0 };
  const values = points.map((p) => p.v);
  const lo = Math.min(...values);
  const hi = Math.max(...values);
  const span = Math.max(hi - lo, minSpan);
  const mid = (lo + hi) / 2;
  const step = niceStep(span / 4);
  const start = Math.floor((mid - span / 2) / step) * step;
  const end = Math.ceil((mid + span / 2) / step) * step;
  const ticks: number[] = [];
  for (let t = start; t <= end + step / 2; t += step) ticks.push(t);
  const tickDecimals = Math.max(0, -Math.floor(Math.log10(step)));
  return { domain: [start, end] as [number, number], ticks, tickDecimals };
}

const timeFormat = new Intl.DateTimeFormat(undefined, { hour: "2-digit", minute: "2-digit", second: "2-digit" });

// Tooltip: the value leads, the time follows; a short stroke keys the series.
function ChartTooltip({
  active,
  payload,
  unit,
  decimals,
}: {
  active?: boolean;
  payload?: { payload: Point }[];
  unit: string;
  decimals: number;
}) {
  if (!active || !payload?.length) return null;
  const p = payload[0].payload;
  return (
    <div className="tooltip">
      <div className="tooltip-row">
        <span className="tooltip-key" aria-hidden="true" />
        <strong>
          {p.v.toFixed(decimals)} {unit}
        </strong>
      </div>
      <div className="tooltip-time">{timeFormat.format(p.t)}</div>
    </div>
  );
}

/** One measure per chart: temperature, humidity and pressure never share an axis. */
export function MetricChart({ title, unit, decimals, minSpan, points, domain }: Props) {
  const y = yScale(points, minSpan);
  return (
    <section className="card chart-card" aria-label={`${title} chart`}>
      <h2 className="card-title">
        {title} <span className="card-unit">({unit})</span>
      </h2>
      <div className="chart-body">
        <ResponsiveContainer width="100%" height="100%">
          <LineChart data={points} margin={{ top: 8, right: 12, bottom: 0, left: 0 }}>
            <CartesianGrid vertical={false} stroke="var(--grid)" />
            <XAxis
              dataKey="t"
              type="number"
              domain={domain}
              scale="time"
              tickFormatter={(t: number) => timeFormat.format(t)}
              tick={{ fill: "var(--text-muted)", fontSize: 11 }}
              axisLine={{ stroke: "var(--axis)" }}
              tickLine={false}
              minTickGap={48}
            />
            <YAxis
              dataKey="v"
              domain={y.domain}
              ticks={y.ticks}
              // Ticks sit on round steps, so they need only as many decimals as the step.
              tickFormatter={(v: number) => v.toFixed(y.tickDecimals)}
              tick={{ fill: "var(--text-muted)", fontSize: 11 }}
              axisLine={false}
              tickLine={false}
              width={56}
            />
            <Tooltip
              content={<ChartTooltip unit={unit} decimals={decimals} />}
              cursor={{ stroke: "var(--text-muted)", strokeWidth: 1 }}
              isAnimationActive={false}
            />
            <Line
              dataKey="v"
              type="linear"
              stroke="var(--series-1)"
              strokeWidth={2}
              strokeLinecap="round"
              strokeLinejoin="round"
              dot={false}
              activeDot={{ r: 4, fill: "var(--series-1)", stroke: "var(--surface-1)", strokeWidth: 2 }}
              isAnimationActive={false}
            />
          </LineChart>
        </ResponsiveContainer>
      </div>
    </section>
  );
}
