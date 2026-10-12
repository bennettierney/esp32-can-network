import { CartesianGrid, Line, LineChart, ResponsiveContainer, Tooltip, XAxis, YAxis } from "recharts";

export interface Point {
  t: number; // epoch ms
  v: number;
}

interface Props {
  title: string;
  unit: string;
  decimals: number;
  points: Point[];
  domain: [number, number]; // x range, epoch ms
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
export function MetricChart({ title, unit, decimals, points, domain }: Props) {
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
              domain={["auto", "auto"]}
              tickFormatter={(v: number) => v.toFixed(decimals === 0 ? 0 : 1)}
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
