"""Calculate current P&L from trade_log database.

Shows P&L in mojos, XCH, and US Dollars.

USD conversion:
  - The trading pair XCH/wUSDC.b uses wUSDC (bridged USDC) as the quote asset.
  - Chia CAT tokens use 1000 mojos per token, so 1000 wUSDC mojos = $1 USD.
  - Realized PnL (spread) is denominated in wUSDC mojos → divide by 1000 for USD.
  - Blockchain fees are denominated in XCH mojos → divide by 10^12 for XCH,
    then multiply by the XCH/USD price to get USD.
"""
import sqlite3

DB = r"c:\GitHub\XOPTrader\data\xop_trader.db"
K = 1_000_000_000_000  # kMojosPerXch  (mojos per 1 XCH)
CAT_MOJOS_PER_TOKEN = 1_000  # CAT standard: 1000 mojos per token

conn = sqlite3.connect(DB)
c = conn.cursor()

# Get all fills in chronological order (with stored PnL values from backfill)
c.execute(
    "SELECT timestamp, side, price_mojos, size_mojos, "
    "       cost_basis_mojos, realized_pnl_mojos, fee_mojos "
    "FROM trade_log ORDER BY timestamp, id"
)
fills = c.fetchall()

total_realized = sum(r[5] for r in fills)  # from stored realized_pnl_mojos
total_bought = sum(r[3] for r in fills if r[1] == "bid")
total_sold = sum(r[3] for r in fills if r[1] == "ask")
inventory = total_bought - total_sold
fills_with_no_basis = sum(1 for r in fills if r[1] == "ask" and r[4] == 0)

print()
print("=" * 62)
print("  XOPTrader P&L REPORT (Weighted-Average Cost Basis)")
print("=" * 62)
print(f"Period:       {fills[0][0]}  to  {fills[-1][0]}")
print(f"Total fills:  {len(fills)}  (pair: XCH/wUSDC)")
print(f"  Buys:       {sum(1 for r in fills if r[1]=='bid')}")
print(f"  Sells:      {sum(1 for r in fills if r[1]=='ask')}")
print()

# XCH/USD price derived from average trade price (wUSDC mojos / CAT_MOJOS_PER_TOKEN)
avg_buy = sum(r[2] for r in fills if r[1] == "bid") / max(1, sum(1 for r in fills if r[1] == "bid"))
avg_sell = sum(r[2] for r in fills if r[1] == "ask") / max(1, sum(1 for r in fills if r[1] == "ask"))
avg_trade_price = sum(r[2] for r in fills) / len(fills)
xch_usd = avg_trade_price / CAT_MOJOS_PER_TOKEN  # e.g. 2385 mojos / 1000 = $2.385

# Get latest trade price for current XCH/USD estimate
latest_price = fills[-1][2] / CAT_MOJOS_PER_TOKEN

print("--- Prices ---")
print(f"  Avg buy price:   {avg_buy / CAT_MOJOS_PER_TOKEN:>10.3f} USD  ({avg_buy:,.0f} wUSDC mojos)")
print(f"  Avg sell price:  {avg_sell / CAT_MOJOS_PER_TOKEN:>10.3f} USD  ({avg_sell:,.0f} wUSDC mojos)")
spread_pct = (avg_sell - avg_buy) / avg_buy * 100 if avg_buy else 0
print(f"  Avg spread:      {spread_pct:>9.2f}%")
print(f"  Latest XCH/USD:  {latest_price:>10.3f} USD")
print()

print("--- Volume ---")
print(f"  Total bought:    {total_bought/K:>12.6f} XCH  (${total_bought/K * xch_usd:>10.2f})")
print(f"  Total sold:      {total_sold/K:>12.6f} XCH  (${total_sold/K * xch_usd:>10.2f})")
print(f"  Net inventory:   {inventory/K:>12.6f} XCH  (${inventory/K * latest_price:>10.2f})")
if inventory < 0:
    print(f"    (negative = sold more than bought; pre-existing inventory)")
print()

print("--- Realized Spread P&L ---")
realized_usd = total_realized / CAT_MOJOS_PER_TOKEN
print(f"  {total_realized:>15,.0f} wUSDC mojos")
print(f"  ${realized_usd:>14.2f} USD")
if fills_with_no_basis:
    print(f"  ({fills_with_no_basis} sells had no cost basis — pre-existing inventory)")
print()

# Simple proceeds-vs-cost cross-check
c.execute(
    "SELECT "
    "  SUM(CASE WHEN side='ask' THEN CAST(price_mojos AS REAL)*size_mojos ELSE 0 END),"
    "  SUM(CASE WHEN side='bid' THEN CAST(price_mojos AS REAL)*size_mojos ELSE 0 END)"
    " FROM trade_log"
)
proceeds, cost = c.fetchone()
net_quote = (proceeds - cost) / K
print("--- Cross-Check: Total Proceeds vs Total Cost ---")
print(f"  Total sell proceeds: {proceeds/K:>12,.0f} wUSDC mojos  (${proceeds/K/CAT_MOJOS_PER_TOKEN:>8.2f})")
print(f"  Total buy cost:      {cost/K:>12,.0f} wUSDC mojos  (${cost/K/CAT_MOJOS_PER_TOKEN:>8.2f})")
print(f"  Net difference:      {net_quote:>12,.0f} wUSDC mojos  (${net_quote/CAT_MOJOS_PER_TOKEN:>8.2f})")
print()

# Fees (in XCH mojos — blockchain fees are paid in XCH)
print("--- Fees (blockchain, paid in XCH) ---")
c.execute("SELECT SUM(fee_mojos) FROM trade_log")
total_fees = c.fetchone()[0] or 0
fees_xch = total_fees / K
fees_usd = fees_xch * latest_price
print(f"  Total fees:      {total_fees:>15,} XCH mojos")
print(f"                   {fees_xch:>15.6f} XCH")
print(f"                   ${fees_usd:>14.4f} USD")
print()

print("=" * 62)
print(f"  REALIZED SPREAD P&L:    ${realized_usd:>+10.2f} USD")
print(f"  BLOCKCHAIN FEES:        ${-fees_usd:>+10.4f} USD")
print(f"  ─────────────────────────────────────")
net_usd = realized_usd - fees_usd
print(f"  NET P&L:                ${net_usd:>+10.2f} USD")
print(f"                          {total_realized - total_fees:>+,.0f} mojos")
print("=" * 62)

conn.close()
