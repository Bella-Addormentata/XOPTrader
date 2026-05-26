import sqlite3
db = sqlite3.connect(r'C:\GitHub\XOPTrader\data\xop_trader.db')
db.row_factory = sqlite3.Row

print("=== trade_log SCHEMA ===")
for r in db.execute("PRAGMA table_info(trade_log)"):
    print(dict(r))

print("\n=== TRADES SINCE 2026-05-26 13:00 ===")
for r in db.execute("""
    SELECT id, timestamp, pair_name, side, price_mojos, size_mojos
    FROM trade_log
    WHERE timestamp >= '2026-05-26T13'
    ORDER BY id DESC
    LIMIT 30
"""):
    print(dict(r))

print("\n=== ALL TRADES TODAY COUNT BY PAIR ===")
for r in db.execute("""
    SELECT pair_name, side, COUNT(*) as n
    FROM trade_log
    WHERE timestamp >= '2026-05-26'
    GROUP BY pair_name, side
"""):
    print(dict(r))
