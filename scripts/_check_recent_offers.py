import sqlite3, time, datetime as dt
db = sqlite3.connect(r"data\xop_trader.db")
db.row_factory = sqlite3.Row
cur = db.cursor()

print("=== Tables ===")
for r in cur.execute("SELECT name FROM sqlite_master WHERE type='table' ORDER BY name"):
    print(" ", r["name"])

candidates = ["offers", "offer", "active_offers", "posted_offers",
              "trade_log", "offer_events", "offer_lifecycle",
              "offer_closure_events", "engine_offers"]
print("\n=== Candidate tables present ===")
present = []
for t in candidates:
    try:
        n = cur.execute(f"SELECT COUNT(*) FROM {t}").fetchone()[0]
        present.append(t)
        print(f"  {t}: {n} rows")
    except sqlite3.Error:
        pass

for t in present:
    print(f"\n=== {t} schema ===")
    for r in cur.execute(f"PRAGMA table_info({t})"):
        print(" ", r["name"], r["type"])

# Try to dump most recent rows from each, ordered by likely timestamp col
for t in present:
    cols = [r["name"] for r in cur.execute(f"PRAGMA table_info({t})")]
    ts_col = next((c for c in ("created_at","ts","timestamp","time","posted_at","updated_at","event_ts","occurred_at") if c in cols), None)
    if not ts_col:
        ts_col = "rowid"
    print(f"\n=== {t} latest 8 rows (order by {ts_col} DESC) ===")
    try:
        for r in cur.execute(f"SELECT * FROM {t} ORDER BY {ts_col} DESC LIMIT 8"):
            d = dict(r)
            # truncate long values
            for k,v in list(d.items()):
                if isinstance(v,str) and len(v)>60:
                    d[k] = v[:60] + "..."
            print(" ", d)
    except sqlite3.Error as e:
        print(" err:", e)
