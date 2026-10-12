# Dashboard

React and Recharts live view of the telemetry. Loads history from the backend, then
stays live over its WebSocket. See the repository README for how to run the full stack.

```bash
npm install
npm run dev     # http://localhost:5173, expects the backend on http://localhost:8000
```

Set `VITE_API_URL` to point at a different backend.
