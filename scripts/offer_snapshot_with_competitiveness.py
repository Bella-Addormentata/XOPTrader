import sqlite3

db = sqlite3.connect(r"c:\GitHub\XOPTrader\data\xop_trader.db")
db.row_factory = sqlite3.Row

rows = db.execute(
    """
    SELECT id, pair_name, side, tier, price_mojos, size_mojos,
           competitiveness_score, queue_ahead_mojos, queue_ahead_score,
           execution_quality_score, book_best_bid, book_best_ask,
           created_block, status, cancel_reason
    FROM offer_log
    WHERE status='pending'
    ORDER BY execution_quality_score ASC, created_block DESC, pair_name, side, tier
    """
).fetchall()

print("=== CURRENT PENDING OFFERS WITH EXECUTION QUALITY ===")
print(f"pending_count={len(rows)}")
for r in rows:
    d = dict(r)
    print(
        f"block={d['created_block']} {d['pair_name']} {d['side']} t{d['tier']} "
        f"price={d['price_mojos']:,} price_score={d['competitiveness_score']}/10 "
        f"queue_ahead={d['queue_ahead_mojos']:,} queue_score={d['queue_ahead_score']}/10 "
        f"execution_score={d['execution_quality_score']}/10 "
        f"bbo={d['book_best_bid']:,}/{d['book_best_ask']:,}"
    )

db.close()
