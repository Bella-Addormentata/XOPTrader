import sqlite3
db = sqlite3.connect('data/xop_trader.db')
cur = db.cursor()

print('=== offer_log by pair/status, last 3d ===')
for r in cur.execute("SELECT pair_name,status,COUNT(*),MAX(created_at) FROM offer_log WHERE created_at>=datetime('now','-3 day') GROUP BY pair_name,status ORDER BY pair_name,status"):
    print(r)

print('\n=== last 15 offers ===')
for r in cur.execute("SELECT created_at,pair_name,side,tier,status,competitiveness_score,cancel_reason FROM offer_log ORDER BY id DESC LIMIT 15"):
    print(r)

print('\n=== cancel_reason histogram last 3d ===')
for r in cur.execute("SELECT pair_name,cancel_reason,COUNT(*) FROM offer_log WHERE created_at>=datetime('now','-3 day') AND status='cancelled' GROUP BY pair_name,cancel_reason ORDER BY 3 DESC"):
    print(r)

print('\n=== sanity_failures by reason last 3d ===')
try:
    for r in cur.execute("SELECT pair_name,failure_reason,COUNT(*) FROM sanity_failures WHERE created_at>=datetime('now','-3 day') GROUP BY pair_name,failure_reason ORDER BY 3 DESC LIMIT 30"):
        print(r)
except Exception as e:
    print('(no sanity_failures table?)', e)

print('\n=== current pending offers ===')
for r in cur.execute("SELECT offer_id,pair_name,side,tier,competitiveness_score,created_at FROM offer_log WHERE status='pending' ORDER BY created_at DESC"):
    print(r)

print('\n=== last fills ===')
for r in cur.execute("SELECT timestamp,pair_name,side,size_mojos,price_mojos FROM trade_log ORDER BY id DESC LIMIT 5"):
    print(r)
