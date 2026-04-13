import json
import sqlite3
import urllib.parse
import urllib.request


XCH_MOJOS = 1_000_000_000_000
PAIR_CONFIG = {
    "XCH/wUSDC.b": {
        "base_id": "xch",
        "quote_id": "fa4a180ac326e67ea289b869e3448256f6af05721f7cf934cb9901baa6b7a99d",
        "base_mpu": XCH_MOJOS,
        "quote_mpu": 1_000,
    },
    "XCH/BYC": {
        "base_id": "xch",
        "quote_id": "ae1536f56760e471ad85ead45f00d680ff9cca73b8cc3407be778f1c0c606eac",
        "base_mpu": XCH_MOJOS,
        "quote_mpu": 1_000,
    },
    "BYC/wUSDC.b": {
        "base_id": "ae1536f56760e471ad85ead45f00d680ff9cca73b8cc3407be778f1c0c606eac",
        "quote_id": "fa4a180ac326e67ea289b869e3448256f6af05721f7cf934cb9901baa6b7a99d",
        "base_mpu": 1_000,
        "quote_mpu": 1_000,
    },
    "XCH/DBX": {
        "base_id": "xch",
        "quote_id": "db1a9020d48d9d4ad22631b66ab4b9ebd3637ef7758ad38881348c5d24c38f20",
        "base_mpu": XCH_MOJOS,
        "quote_mpu": 1_000,
    },
}


def score_price(side: str, price: int, best_bid: int, best_ask: int) -> int:
    if price <= 0:
        return 0
    if best_bid > 0 and best_ask > best_bid:
        if side == "bid" and price >= best_ask:
            return 1
        if side == "ask" and price <= best_bid:
            return 1
        spread = float(best_ask - best_bid)
        if side == "bid":
            if price >= best_bid:
                return 10
            widths = float(best_bid - price) / spread
            return max(1, min(10, 10 - int(round(widths * 4.0))))
        if price <= best_ask:
            return 10
        widths = float(price - best_ask) / spread
        return max(1, min(10, 10 - int(round(widths * 4.0))))

    same_side_best = best_bid if side == "bid" else best_ask
    if same_side_best <= 0:
        return 0
    if (side == "bid" and price >= same_side_best) or (
        side == "ask" and price <= same_side_best
    ):
        return 9
    bps = abs(float(price - same_side_best)) / float(same_side_best) * 10000.0
    if bps <= 5.0:
        return 8
    if bps <= 10.0:
        return 7
    if bps <= 25.0:
        return 6
    if bps <= 50.0:
        return 4
    return 2


def score_queue(queue_ahead_mojos: int, our_size_mojos: int) -> int:
    if our_size_mojos <= 0:
        return 0
    if queue_ahead_mojos <= 0:
        return 10

    queue_ratio = float(queue_ahead_mojos) / float(our_size_mojos)
    if queue_ratio <= 0.25:
        return 9
    if queue_ratio <= 0.50:
        return 8
    if queue_ratio <= 1.00:
        return 7
    if queue_ratio <= 2.00:
        return 6
    if queue_ratio <= 4.00:
        return 5
    if queue_ratio <= 8.00:
        return 4
    if queue_ratio <= 12.00:
        return 3
    if queue_ratio <= 20.00:
        return 2
    return 1


def score_execution(price_score: int, queue_score: int) -> int:
    if price_score <= 0:
        return queue_score
    if queue_score <= 0:
        return price_score
    return max(1, min(10, int(round(0.7 * price_score + 0.3 * queue_score))))


def fetch_json(url: str) -> dict:
    req = urllib.request.Request(url, headers={"User-Agent": "XOPTrader/0.7.42"})
    with urllib.request.urlopen(req, timeout=15) as resp:
        return json.loads(resp.read())


def fetch_competing_offers(pair_name: str, own_offer_ids: set[str]) -> list[dict]:
    cfg = PAIR_CONFIG.get(pair_name)
    if cfg is None:
        return []

    offers = []
    for side in ("bid", "ask"):
        if side == "bid":
            offered = cfg["quote_id"]
            requested = cfg["base_id"]
            sort = "price_desc"
        else:
            offered = cfg["base_id"]
            requested = cfg["quote_id"]
            sort = "price_asc"

        params = urllib.parse.urlencode(
            {
                "offered": offered,
                "requested": requested,
                "page": 1,
                "page_size": 200,
                "sort": sort,
                "compact": "true",
                "status": 0,
            }
        )
        data = fetch_json(f"https://api.dexie.space/v1/offers?{params}")
        for offer in data.get("offers", []):
            if offer.get("id") in own_offer_ids:
                continue
            offered_assets = offer.get("offered") or []
            requested_assets = offer.get("requested") or []
            if not offered_assets or not requested_assets:
                continue

            offered_id = offered_assets[0].get("id")
            requested_id = requested_assets[0].get("id")
            matches_pair = (
                offered_id == cfg["base_id"] and requested_id == cfg["quote_id"]
            ) or (
                offered_id == cfg["quote_id"] and requested_id == cfg["base_id"]
            )
            if not matches_pair:
                continue

            offered_denom = (
                cfg["base_mpu"] if offered_id == cfg["base_id"] else cfg["quote_mpu"]
            )
            offers.append(
                {
                    "side": "ask" if offered_id == cfg["base_id"] else "bid",
                    "price": int(
                        round(
                            (
                                float(offer.get("price") or 0.0)
                                if offered_id == cfg["base_id"]
                                else (
                                    1.0 / float(offer.get("price") or 0.0)
                                    if float(offer.get("price") or 0.0) > 0.0
                                    else 0.0
                                )
                            )
                            * XCH_MOJOS
                        )
                    ),
                    "size": int(
                        round(float(offered_assets[0].get("amount") or 0.0) * offered_denom)
                    ),
                }
            )

    return offers


def compute_queue_ahead(side: str, price: int, competing_offers: list[dict]) -> int:
    queue_ahead = 0
    for offer in competing_offers:
        if offer["side"] != side:
            continue
        ahead = offer["price"] >= price if side == "bid" else offer["price"] <= price
        if ahead:
            queue_ahead += offer["size"]
    return queue_ahead


db = sqlite3.connect(r"c:\GitHub\XOPTrader\data\xop_trader.db")
db.row_factory = sqlite3.Row

pending_rows = db.execute(
    """
    SELECT id, offer_id, pair_name, side, tier, price_mojos, size_mojos,
           competitiveness_score, queue_ahead_mojos, queue_ahead_score,
           execution_quality_score, book_best_bid, book_best_ask, created_block
    FROM offer_log
    WHERE status='pending'
    ORDER BY created_block DESC, pair_name, side, tier
    """
).fetchall()

own_offer_ids = {row["offer_id"] for row in pending_rows if row["offer_id"]}
pair_books = {
    pair_name: fetch_competing_offers(pair_name, own_offer_ids)
    for pair_name in {row["pair_name"] for row in pending_rows}
}

updated = 0
for row in pending_rows:
    price_score = score_price(
        row["side"],
        row["price_mojos"],
        row["book_best_bid"] or 0,
        row["book_best_ask"] or 0,
    )
    queue_ahead = compute_queue_ahead(
        row["side"],
        row["price_mojos"],
        pair_books.get(row["pair_name"], []),
    )
    queue_score = score_queue(queue_ahead, row["size_mojos"] or 0)
    execution_score = score_execution(price_score, queue_score)
    db.execute(
        """
        UPDATE offer_log
        SET competitiveness_score=?,
            queue_ahead_mojos=?,
            queue_ahead_score=?,
            execution_quality_score=?
        WHERE id=?
        """,
        (price_score, queue_ahead, queue_score, execution_score, row["id"]),
    )
    updated += 1

db.commit()
print(f"updated={updated}")

pending = db.execute(
    """
    SELECT pair_name, side, tier, price_mojos, size_mojos,
           competitiveness_score, queue_ahead_mojos, queue_ahead_score,
           execution_quality_score, book_best_bid, book_best_ask, created_block
    FROM offer_log
    WHERE status='pending'
    ORDER BY execution_quality_score ASC, created_block DESC, pair_name, side, tier
    """
).fetchall()
for row in pending:
    print(dict(row))

db.close()
