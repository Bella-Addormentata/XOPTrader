import sqlite3
db = sqlite3.connect(r"data\xop_trader.db")
db.row_factory = sqlite3.Row
cur = db.cursor()

for t in ("offer_log", "strategy_quotes"):
    print(f"=== {t} schema ===")
    cols = [r["name"] for r in cur.execute(f"PRAGMA table_info({t})")]
    for c in cols:
        print(" ", c)
    n = cur.execute(f"SELECT COUNT(*) FROM {t}").fetchone()[0]
    print(f"  rows: {n}")
    ts_col = next((c for c in ("created_at","ts","timestamp","time","posted_at","updated_at") if c in cols), "rowid")
    print(f"\n=== {t} latest 12 (order by {ts_col} DESC) ===")
    for r in cur.execute(f"SELECT * FROM {t} ORDER BY {ts_col} DESC LIMIT 12"):
        d = dict(r)
        for k,v in list(d.items()):
            if isinstance(v,str) and len(v)>50:
                d[k] = v[:50]+"..."
        print(" ", d)
    print()
