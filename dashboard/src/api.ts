import { useEffect, useState } from "react";

export const API_URL: string = import.meta.env.VITE_API_URL ?? "http://localhost:8000";

export interface Reading {
  id: number;
  node: string;
  recorded_at: string; // ISO 8601, UTC
  temperature_c: number;
  humidity_pct: number;
  pressure_pa: number;
}

export interface NodeStatus {
  node: string;
  recorded_at: string;
  online: boolean;
  sensor_ok: boolean;
  uptime_s: number;
  can_errors: number;
}

// One hour of readings at one per second, plus slack. Keeps memory bounded on a tab left open.
const MAX_READINGS = 3700;

async function getJson<T>(path: string): Promise<T> {
  const res = await fetch(API_URL + path);
  if (!res.ok) throw new Error(`${path}: HTTP ${res.status}`);
  return res.json() as Promise<T>;
}

/**
 * Loads history for the selected window, then keeps it live over the backend's WebSocket.
 * Reconnects every 2 s if the socket drops.
 */
export function useTelemetry(minutes: number) {
  const [readings, setReadings] = useState<Reading[]>([]);
  const [status, setStatus] = useState<NodeStatus | null>(null);
  const [live, setLive] = useState(false);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    let cancelled = false;
    setLoading(true);
    Promise.all([
      getJson<Reading[]>(`/api/readings?minutes=${minutes}`),
      getJson<NodeStatus[]>("/api/status"),
    ])
      .then(([r, s]) => {
        if (cancelled) return;
        setReadings(r);
        setStatus(s.find((x) => x.node === "node_a") ?? null);
        setError(null);
      })
      .catch((e: unknown) => !cancelled && setError(e instanceof Error ? e.message : String(e)))
      .finally(() => !cancelled && setLoading(false));
    return () => {
      cancelled = true;
    };
  }, [minutes]);

  useEffect(() => {
    let ws: WebSocket | null = null;
    let retry: number | undefined;
    let closed = false;

    const connect = () => {
      ws = new WebSocket(API_URL.replace(/^http/, "ws") + "/ws");
      ws.onopen = () => setLive(true);
      ws.onclose = () => {
        setLive(false);
        if (!closed) retry = window.setTimeout(connect, 2000);
      };
      ws.onmessage = (event) => {
        const msg = JSON.parse(event.data as string) as
          | { type: "reading"; data: Reading }
          | { type: "status"; data: NodeStatus };
        if (msg.type === "reading") {
          setReadings((prev) => [...prev, msg.data].slice(-MAX_READINGS));
        } else if (msg.type === "status") {
          setStatus(msg.data);
        }
      };
    };

    connect();
    return () => {
      closed = true;
      window.clearTimeout(retry);
      ws?.close();
    };
  }, []);

  return { readings, status, live, loading, error };
}
