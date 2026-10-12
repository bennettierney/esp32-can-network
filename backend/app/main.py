"""Telemetry backend.

Stores readings and node status sent by the gateway, serves history to the dashboard,
and pushes new data to connected dashboards over a WebSocket.

Run:
    docker compose up -d                      # Postgres, from the repo root
    .venv/bin/uvicorn app.main:app --reload   # from backend/
"""

import os
from contextlib import asynccontextmanager
from datetime import datetime
from pathlib import Path

from fastapi import FastAPI, Query, WebSocket, WebSocketDisconnect
from fastapi.middleware.cors import CORSMiddleware
from psycopg.rows import dict_row
from psycopg_pool import AsyncConnectionPool
from pydantic import BaseModel, Field

DATABASE_URL = os.environ.get(
    "DATABASE_URL", "postgresql://telemetry:telemetry@localhost:5433/telemetry"
)
SCHEMA_PATH = Path(__file__).resolve().parent.parent / "schema.sql"


# ---------------------------------------------------------------------------
# Request and response models. Ranges match the BME280's rated limits, the same
# check node A applies before packing a CAN frame.
# ---------------------------------------------------------------------------

class ReadingIn(BaseModel):
    node: str = "node_a"
    recorded_at: datetime
    temperature_c: float = Field(ge=-40, le=85)
    humidity_pct: float = Field(ge=0, le=100)
    pressure_pa: int = Field(ge=30000, le=110000)


class ReadingOut(ReadingIn):
    id: int


class StatusIn(BaseModel):
    node: str = "node_a"
    recorded_at: datetime
    online: bool
    sensor_ok: bool
    uptime_s: int = Field(ge=0)
    can_errors: int = Field(ge=0)


# ---------------------------------------------------------------------------
# Live updates: every connected dashboard gets each new reading and status.
# ---------------------------------------------------------------------------

class Hub:
    def __init__(self) -> None:
        self.clients: set[WebSocket] = set()

    async def broadcast(self, message: dict) -> None:
        dead = []
        for ws in self.clients:
            try:
                await ws.send_json(message)
            except Exception:  # client went away mid send
                dead.append(ws)
        for ws in dead:
            self.clients.discard(ws)


hub = Hub()
pool = AsyncConnectionPool(DATABASE_URL, open=False, kwargs={"row_factory": dict_row})


@asynccontextmanager
async def lifespan(app: FastAPI):
    await pool.open()
    async with pool.connection() as conn:
        await conn.execute(SCHEMA_PATH.read_text())
    yield
    await pool.close()


app = FastAPI(title="ESP32 CAN telemetry", lifespan=lifespan)

# The dashboard's dev server runs on a different port, so the browser needs CORS.
app.add_middleware(
    CORSMiddleware,
    allow_origins=["http://localhost:5173"],
    allow_methods=["*"],
    allow_headers=["*"],
)


# ---------------------------------------------------------------------------
# Endpoints
# ---------------------------------------------------------------------------

@app.get("/api/health")
async def health() -> dict:
    async with pool.connection() as conn:
        await conn.execute("SELECT 1")
    return {"ok": True}


@app.post("/api/readings", status_code=201, response_model=ReadingOut)
async def add_reading(reading: ReadingIn) -> ReadingOut:
    async with pool.connection() as conn:
        row = await (await conn.execute(
            """
            INSERT INTO readings (node, recorded_at, temperature_c, humidity_pct, pressure_pa)
            VALUES (%(node)s, %(recorded_at)s, %(temperature_c)s, %(humidity_pct)s, %(pressure_pa)s)
            RETURNING id
            """,
            reading.model_dump(),
        )).fetchone()
    out = ReadingOut(id=row["id"], **reading.model_dump())
    await hub.broadcast({"type": "reading", "data": out.model_dump(mode="json")})
    return out


@app.get("/api/readings", response_model=list[ReadingOut])
async def list_readings(
    node: str = "node_a",
    minutes: int = Query(10, ge=1, le=24 * 60),
    limit: int = Query(5000, ge=1, le=50000),
) -> list[dict]:
    """Readings from the last `minutes`, oldest first, ready to chart."""
    async with pool.connection() as conn:
        rows = await (await conn.execute(
            """
            SELECT id, node, recorded_at, temperature_c, humidity_pct, pressure_pa
            FROM (
                SELECT * FROM readings
                WHERE node = %s AND recorded_at > now() - make_interval(mins => %s)
                ORDER BY recorded_at DESC
                LIMIT %s
            ) latest
            ORDER BY recorded_at
            """,
            (node, minutes, limit),
        )).fetchall()
    return rows


@app.post("/api/status", status_code=204)
async def set_status(status: StatusIn) -> None:
    async with pool.connection() as conn:
        await conn.execute(
            """
            INSERT INTO node_status (node, recorded_at, online, sensor_ok, uptime_s, can_errors)
            VALUES (%(node)s, %(recorded_at)s, %(online)s, %(sensor_ok)s, %(uptime_s)s, %(can_errors)s)
            ON CONFLICT (node) DO UPDATE SET
                recorded_at = EXCLUDED.recorded_at,
                online = EXCLUDED.online,
                sensor_ok = EXCLUDED.sensor_ok,
                uptime_s = EXCLUDED.uptime_s,
                can_errors = EXCLUDED.can_errors
            """,
            status.model_dump(),
        )
    await hub.broadcast({"type": "status", "data": status.model_dump(mode="json")})


@app.get("/api/status", response_model=list[StatusIn])
async def get_status() -> list[dict]:
    async with pool.connection() as conn:
        return await (await conn.execute(
            "SELECT node, recorded_at, online, sensor_ok, uptime_s, can_errors FROM node_status ORDER BY node"
        )).fetchall()


@app.websocket("/ws")
async def live(ws: WebSocket) -> None:
    await ws.accept()
    hub.clients.add(ws)
    try:
        while True:
            await ws.receive_text()  # keep the connection open; clients do not send anything
    except WebSocketDisconnect:
        pass
    finally:
        hub.clients.discard(ws)
