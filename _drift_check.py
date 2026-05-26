import sqlite3, os
os.chdir(r'C:\GitHub\XOPTrader')
db = sqlite3.connect('data/xop_trader.db')
db.row_factory = sqlite3.Row

print("=== LATEST SNAPSHOT PER PAIR ===")
for r in db.execute("""
    SELECT pair_name, block_height, inventory_ratio, mid_price_mojos
    FROM snapshots
    WHERE id IN (SELECT MAX(id) FROM snapshots GROUP BY pair_name)
    ORDER BY pair_name
"""):
    print(dict(r))

print("\n=== LATEST POSITIONS ===")
try:
    for r in db.execute("""
        SELECT asset_id, quantity_mojos, cost_basis_mojos, last_update_block
        FROM positions ORDER BY asset_id
    """):
        print(dict(r))
except sqlite3.OperationalError as e:
    print('positions table:', e)

print("\n=== TRADES SINCE 2026-05-26 ===")
for r in db.execute("""
    SELECT id, timestamp, pair_name, side, price_mojos, size_mojos
    FROM trades
    WHERE timestamp >= '2026-05-26'
    ORDER BY id DESC LIMIT 20
"""):
    print(dict(r))
