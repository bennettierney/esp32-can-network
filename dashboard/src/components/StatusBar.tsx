import type { NodeStatus } from "../api";

type Level = "good" | "serious" | "critical";

// Status is never color alone: every badge has an icon and a word.
const ICON: Record<Level, string> = { good: "✓", serious: "!", critical: "✕" };

function Badge({ level, label, value }: { level: Level; label: string; value: string }) {
  return (
    <div className="badge" role="status">
      <span className={`badge-icon badge-${level}`} aria-hidden="true">
        {ICON[level]}
      </span>
      <span className="badge-label">{label}</span>
      <span className="badge-value">{value}</span>
    </div>
  );
}

function formatUptime(s: number): string {
  if (s < 60) return `${s}s`;
  if (s < 3600) return `${Math.floor(s / 60)}m ${s % 60}s`;
  return `${Math.floor(s / 3600)}h ${Math.floor((s % 3600) / 60)}m`;
}

interface Props {
  status: NodeStatus | null;
  live: boolean;
  secondsSinceReading: number | null;
}

export function StatusBar({ status, live, secondsSinceReading }: Props) {
  // Data is stale if nothing arrived for 5 s. That catches a stopped gateway or node B,
  // which can't report their own failure.
  const stale = secondsSinceReading === null || secondsSinceReading > 5;

  return (
    <div className="status-bar">
      <Badge level={live ? "good" : "critical"} label="Backend feed" value={live ? "live" : "disconnected"} />
      <Badge
        level={stale ? "serious" : "good"}
        label="Data"
        value={secondsSinceReading === null ? "none yet" : stale ? `${secondsSinceReading}s old` : "fresh"}
      />
      <Badge
        level={status?.online ? "good" : "critical"}
        label="Node A"
        value={status ? (status.online ? `online, up ${formatUptime(status.uptime_s)}` : "offline") : "unknown"}
      />
      <Badge
        level={status?.sensor_ok ? "good" : "critical"}
        label="Sensor"
        value={status ? (status.sensor_ok ? "ok" : "down") : "unknown"}
      />
      <Badge
        level={status && status.can_errors > 0 ? "serious" : "good"}
        label="CAN errors"
        value={status ? status.can_errors.toLocaleString() : "unknown"}
      />
    </div>
  );
}
