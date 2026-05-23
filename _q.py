import sqlite3
c=sqlite3.connect('data/xop_trader.db')
cur=c.cursor()
cur.execute("SELECT name FROM sqlite_master WHERE type='table'")
print('TABLES:', cur.fetchall())
cur.execute("SELECT sql FROM sqlite_master WHERE type='table' AND name='offer_log'")
print('OFFER_LOG SCHEMA:'); [print(r[0]) for r in cur.fetchall()]
cur.execute("SELECT sql FROM sqlite_master WHERE type='table' AND name IN ('trades','trade_log')")
print('TRADES SCHEMAS:'); [print(r[0]) for r in cur.fetchall()]
