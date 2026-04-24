"""Quick fill / offer outcome diagnostic for the v0.7.46 'no fills' question."""
import sqlite3, sys
db = sqlite3.connect("data/xop_trader.db")
cur = db.cursor()

print("=== Last 15 trade_log rows ===")
for r in cur.execute("SELECT timestamp,pair_name,side,size_mojos,price_mojos FROM trade_log ORDER BY id DESC LIMIT 15"):
    print(r)

print("\n=== Fills last 7d / 24h / 1h ===")
for win in ("7 day","1 day","1 hour"):
    cur.execute(f"SELECT pair_name,side,COUNT(*) FROM trade_log WHERE timestamp >= datetime('now','-{win}') GROUP BY pair_name,side")
    rows = cur.fetchall()
    print(f"  -{win}: {rows or 'NONE'}")

print("\n=== offer_log status counts last 24h ===")
cur.execute("SELECT pair_name,status,COUNT(*) FROM offer_log WHERE created_at >= datetime('now','-1 day') GROUP BY pair_name,status ORDER BY pair_name,status")
for r in cur.fetchall():
    print(r)

print("\n=== Top cancel_reason last 24h ===")
cur.execute("SELECT pair_name,cancel_reason,COUNT(*) FROM offer_log WHERE created_at >= datetime('now','-1 day') AND status='cancelled' GROUP BY pair_name,cancel_reason ORDER BY 3 DESC LIMIT 20")
for r in cur.fetchall():
    print(r)

print("\n=== sanity_failures last 24h (top reasons) ===")
cur.execute("SELECT pair_name,failure_reason,COUNT(*) FROM sanity_failures WHERE created_at >= datetime('now','-1 day') GROUP BY pair_name,failure_reason ORDER BY 3 DESC LIMIT 20")
for r in cur.fetchall():
    print(r)

print("\n=== Avg competitiveness_score for offers last 24h ===")
cur.execute("SELECT pair_name,side,AVG(competitiveness_score),AVG(queue_ahead_score),COUNT(*) FROM offer_log WHERE created_at >= datetime('now','-1 day') GROUP BY pair_name,side")
for r in cur.fetchall():
    print(r)

print("\n=== Pending offers right now with age ===")
cur.execute("SELECT offer_id,pair_name,side,tier,competitiveness_score,queue_ahead_mojos,book_best_bid,book_best_ask,price_mojos,size_mojos,created_at FROM offer_log WHERE status='pending' ORDER BY created_at DESC")
for r in cur.fetchall():
    print(r)
