import sqlite3
db = sqlite3.connect(r'C:\GitHub\XOPTrader\data\xop_trader.db')
db.row_factory = sqlite3.Row
print("=== TABLES ===")
for r in db.execute("SELECT name FROM sqlite_master WHERE type='table' ORDER BY name"):
    print(' -', r['name'])
