"""
Token Counter hub — minimal reference.

Receives absolute counts from stations over HTTP and broadcasts them to any
connected dashboards over a WebSocket. This is a stripped-down reference: no admin
routes, no reset logic, no branded dashboard assets — just the core data path.

Run:
    pip install fastapi uvicorn
    uvicorn main:app --host 0.0.0.0 --port 8000
"""

from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from fastapi.responses import HTMLResponse
from pydantic import BaseModel
import asyncio
import json

app = FastAPI()

# Latest absolute counts, keyed by station id.
state: dict[str, dict[str, int]] = {}

# Connected dashboard websockets.
clients: set[WebSocket] = set()


class Update(BaseModel):
    station_id: int
    black: int
    blue: int


async def broadcast() -> None:
    payload = json.dumps(state)
    dead = []
    for ws in clients:
        try:
            await ws.send_text(payload)
        except Exception:
            dead.append(ws)
    for ws in dead:
        clients.discard(ws)


@app.post("/update")
async def update(u: Update):
    # Absolute counts — just overwrite. A missed report self-corrects next time.
    state[str(u.station_id)] = {"black": u.black, "blue": u.blue}
    await broadcast()
    return {"ok": True}


@app.get("/session")
async def session(station_id: int):
    # Stations call this on boot to restore their last known counts. If the hub
    # has nothing for this station yet, it returns zeros and the station falls
    # back to its own SD file.
    return state.get(str(station_id), {"black": 0, "blue": 0})


@app.websocket("/ws")
async def ws_endpoint(ws: WebSocket):
    await ws.accept()
    clients.add(ws)
    await ws.send_text(json.dumps(state))  # send current state on connect
    try:
        while True:
            await ws.receive_text()  # keep the connection open
    except WebSocketDisconnect:
        clients.discard(ws)


@app.get("/", response_class=HTMLResponse)
async def dashboard():
    # Minimal live dashboard. Replace with your own styling/layout.
    return """
<!doctype html>
<html>
<head><meta charset="utf-8"><title>Token Counter — Live</title>
<style>
  body { font-family: system-ui, sans-serif; margin: 2rem; }
  h1 { font-weight: 500; }
  .grid { display: grid; grid-template-columns: repeat(4, 1fr); gap: 1rem; }
  .card { border: 1px solid #ddd; border-radius: 12px; padding: 1rem; }
  .name { font-size: 0.9rem; color: #666; }
  .counts { display: flex; gap: 1.5rem; margin-top: 0.5rem; }
  .n { font-size: 2rem; font-weight: 500; }
  .lbl { font-size: 0.7rem; color: #999; text-transform: uppercase; }
</style></head>
<body>
  <h1>Token Counter — Live</h1>
  <div class="grid" id="grid"></div>
<script>
  const grid = document.getElementById('grid');
  const ws = new WebSocket(`ws://${location.host}/ws`);
  ws.onmessage = (e) => {
    const state = JSON.parse(e.data);
    grid.innerHTML = '';
    for (const [station, c] of Object.entries(state)) {
      const el = document.createElement('div');
      el.className = 'card';
      el.innerHTML = `<div class="name">${station}</div>
        <div class="counts">
          <div><div class="n">${c.black}</div><div class="lbl">Black</div></div>
          <div><div class="n">${c.blue}</div><div class="lbl">Blue</div></div>
        </div>`;
      grid.appendChild(el);
    }
  };
</script>
</body>
</html>
"""
