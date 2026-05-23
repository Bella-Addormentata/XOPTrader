import sqlite3, collections
c = sqlite3.connect('data/xop_trader.db')
cur = c.cursor()

print("=== 7d FILLS by pair/side ===")
for r in cur.execute("SELECT pair_name, side, COUNT(*) FROM trade_log WHERE created_at > datetime('now','-7 days') GROUP BY pair_name, side ORDER BY 3 DESC"):
    print(r)

print("\n=== 7d OFFER STATUS by pair ===")
for r in cur.execute("SELECT pair_name, status, COUNT(*) FROM offer_log WHERE created_at > datetime('now','-7 days') GROUP BY pair_name, status ORDER BY pair_name, 3 DESC"):
    print(r)

print("\n=== 7d CANCEL REASON top ===")
for r in cur.execute("SELECT pair_name, COALESCE(cancel_reason,'?'), COUNT(*) FROM offer_log WHERE status='cancelled' AND created_at > datetime('now','-7 days') GROUP BY pair_name, cancel_reason ORDER BY 3 DESC LIMIT 30"):
    print(r)

print("\n=== 7d AVG competitiveness / queue_ahead / exec_quality by pair/side/status ===")
for r in cur.execute("SELECT pair_name, side, status, ROUND(AVG(competitiveness_score),2), ROUND(AVG(queue_ahead_score),2), ROUND(AVG(execution_quality_score),2), COUNT(*) FROM offer_log WHERE created_at > datetime('now','-7 days') GROUP BY pair_name, side, status ORDER BY pair_name, side, status"):
    print(r)

print("\n=== Fill rate vs competitiveness bucket (last 7d) ===")
for r in cur.execute("""
    SELECT pair_name,
           CASE WHEN competitiveness_score >= 8 THEN 'A:8-10'
                WHEN competitiveness_score >= 5 THEN 'B:5-7'
                WHEN competitiveness_score >= 2 THEN 'C:2-4'
                ELSE 'D:0-1' END AS bucket,
           SUM(CASE WHEN status='filled' THEN 1 ELSE 0 END) AS fills,
           SUM(CASE WHEN status='cancelled' THEN 1 ELSE 0 END) AS cancels,
           COUNT(*) AS total
    FROM offer_log WHERE created_at > datetime('now','-7 days')
    GROUP BY pair_name, bucket ORDER BY pair_name, bucket
"""):
    print(r)
