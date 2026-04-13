import sqlite3
import subprocess
import sys
import requests
import warnings
from pathlib import Path

warnings.filterwarnings("ignore")

CERT = (
    r"C:\Users\dorkm\.chia\mainnet\config\ssl\wallet\private_wallet.crt",
    r"C:\Users\dorkm\.chia\mainnet\config\ssl\wallet\private_wallet.key",
)
BASE = "https://localhost:9256"


BACKFILL_SCRIPT = Path(__file__).with_name("backfill_competitiveness.py")


def rpc(endpoint, data=None):
    r = requests.post(
        f"{BASE}/{endpoint}",
        json=data or {},
        verify=False,
        cert=CERT,
        timeout=30,
    )
    return r.json()


subprocess.run([sys.executable, str(BACKFILL_SCRIPT)], check=True)


db = sqlite3.connect(r"c:\GitHub\XOPTrader\data\xop_trader.db")
db.row_factory = sqlite3.Row
rows = db.execute(
    """
    SELECT offer_id, pair_name, side, tier, price_mojos,
           competitiveness_score, queue_ahead_score,
           execution_quality_score
    FROM offer_log
    WHERE status='pending'
            AND (
                        competitiveness_score BETWEEN 1 AND 2
                        OR execution_quality_score BETWEEN 1 AND 3
                    )
    ORDER BY created_block DESC, id DESC
    """
).fetchall()

print(f"low_score_pending={len(rows)}")
for r in rows:
    d = dict(r)
    print(
        f"cancelling {d['pair_name']} {d['side']} t{d['tier']} price={d['price_mojos']:,} "
        f"price_score={d['competitiveness_score']} "
        f"queue_score={d['queue_ahead_score']} "
        f"execution_score={d['execution_quality_score']} "
        f"offer_id={d['offer_id'][:18]}"
    )
    res = rpc(
        "cancel_offer",
        {
            "trade_id": d["offer_id"],
            "fee": 0,
            "secure": True,
        },
    )
    print(res)

db.close()
