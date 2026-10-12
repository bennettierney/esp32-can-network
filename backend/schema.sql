-- Applied by the backend at startup. Every statement is safe to run more than once.

-- One row per reading node A took, as forwarded by node B and the gateway.
CREATE TABLE IF NOT EXISTS readings (
    id             BIGSERIAL PRIMARY KEY,
    node           TEXT NOT NULL,
    recorded_at    TIMESTAMPTZ NOT NULL,              -- when the gateway received it
    received_at    TIMESTAMPTZ NOT NULL DEFAULT now(), -- when the backend stored it
    temperature_c  DOUBLE PRECISION NOT NULL,
    humidity_pct   DOUBLE PRECISION NOT NULL,
    pressure_pa    INTEGER NOT NULL
);

-- The dashboard's main query is "latest readings for a node", so index for it.
CREATE INDEX IF NOT EXISTS readings_node_recorded_at ON readings (node, recorded_at DESC);

-- Latest health report per node, overwritten on every status message.
CREATE TABLE IF NOT EXISTS node_status (
    node         TEXT PRIMARY KEY,
    recorded_at  TIMESTAMPTZ NOT NULL,
    online       BOOLEAN NOT NULL,
    sensor_ok    BOOLEAN NOT NULL,
    uptime_s     BIGINT NOT NULL,
    can_errors   BIGINT NOT NULL
);
