"""Tests for the warp job store (gui/services/warp/jobs.py).

Pure standard library -- no third-party deps -- so this runs anywhere. File-
backed stores use ``tmp_path`` so the crash-resume test can close and reopen the
same database; the rest use ``":memory:"`` for speed.
"""

from __future__ import annotations

import sqlite3

import pytest

from gui.services.warp.jobs import (
    ActiveJobExists,
    JobNotFound,
    JobStatus,
    StaleJobError,
    WarpJob,
    WarpJobStore,
    WarpJobStoreError,
)


class Clock:
    """Deterministic, strictly increasing ISO clock (one tick per call)."""

    def __init__(self) -> None:
        self.t = 0

    def __call__(self) -> str:
        self.t += 1
        return f"2026-01-01T00:00:{self.t:02d}+00:00"


@pytest.fixture
def store() -> WarpJobStore:
    s = WarpJobStore(":memory:", now=Clock())
    try:
        yield s
    finally:
        s.close()


# --------------------------------------------------------------------------- #
# Creation + basic queries.
# --------------------------------------------------------------------------- #

def test_create_sets_defaults_and_is_active(store: WarpJobStore):
    job = store.create_job("mainnet")
    assert isinstance(job, WarpJob)
    assert job.status == JobStatus.AWAITING_DEPOSIT
    assert job.network == "mainnet"
    assert job.is_open and not job.is_terminal
    assert job.retry_count == 0
    assert job.created_at == job.updated_at  # single clock tick on insert
    assert store.get_active_job().id == job.id
    assert store.get_job(job.id).id == job.id


def test_create_with_columns_and_state(store: WarpJobStore):
    job = store.create_job(
        "mainnet",
        columns={"receiver_address": "xch1abc", "amount_usdc_micros": 5_000_000},
        state={"note": "hi"},
        event_message="seeded",
    )
    assert job.receiver_address == "xch1abc"
    assert job.amount_usdc_micros == 5_000_000
    assert job.state == {"note": "hi"}
    events = store.get_events(job.id)
    assert len(events) == 1
    assert events[0].kind == "transition"
    assert events[0].message == "seeded"
    assert events[0].status == JobStatus.AWAITING_DEPOSIT


def test_create_at_recovery_status(store: WarpJobStore):
    # "Claim by tx hash" recovery starts mid-machine.
    job = store.create_job("mainnet", status=JobStatus.BRIDGE_CONFIRMED)
    assert job.status == JobStatus.BRIDGE_CONFIRMED
    assert job.is_open
    assert store.get_active_job().id == job.id


def test_create_rejects_unknown_status(store: WarpJobStore):
    with pytest.raises(WarpJobStoreError):
        store.create_job("mainnet", status="BOGUS")


def test_get_missing_job_raises(store: WarpJobStore):
    with pytest.raises(JobNotFound):
        store.get_job(999)


# --------------------------------------------------------------------------- #
# Single-open-job invariant.
# --------------------------------------------------------------------------- #

def test_second_open_job_is_rejected(store: WarpJobStore):
    store.create_job("mainnet")
    with pytest.raises(ActiveJobExists):
        store.create_job("mainnet")


def test_completing_frees_the_slot(store: WarpJobStore):
    j1 = store.create_job("mainnet")
    store.update_job(j1.id, status=JobStatus.COMPLETED)
    assert store.get_active_job() is None
    j2 = store.create_job("mainnet")  # slot free again
    assert j2.id != j1.id
    assert store.get_active_job().id == j2.id


def test_failed_job_still_holds_slot_until_cancelled(store: WarpJobStore):
    j1 = store.create_job("mainnet")
    store.update_job(j1.id, status=JobStatus.FAILED)
    # FAILED is machine-terminal but still occupies the slot.
    assert store.get_active_job().id == j1.id
    assert store.get_job(j1.id).is_terminal
    with pytest.raises(ActiveJobExists):
        store.create_job("mainnet")
    # Operator dismisses (Sweep + cancel) -> slot frees.
    store.update_job(j1.id, status=JobStatus.CANCELLED)
    assert store.get_active_job() is None
    store.create_job("mainnet")  # no raise


def test_cancelled_and_completed_jobs_still_listed(store: WarpJobStore):
    j1 = store.create_job("mainnet")
    store.update_job(j1.id, status=JobStatus.CANCELLED)
    j2 = store.create_job("mainnet")
    store.update_job(j2.id, status=JobStatus.COMPLETED)
    ids = [j.id for j in store.list_jobs()]
    assert ids == [j2.id, j1.id]  # most-recent first


# --------------------------------------------------------------------------- #
# Updates: transitions, events, state, columns.
# --------------------------------------------------------------------------- #

def test_transition_records_auto_event_and_advances_time(store: WarpJobStore):
    job = store.create_job("mainnet")
    updated = store.update_job(job.id, status=JobStatus.DEPOSIT_SEEN)
    assert updated.status == JobStatus.DEPOSIT_SEEN
    assert updated.updated_at > updated.created_at
    events = store.get_events(job.id)
    assert [e.message for e in events] == [
        "job created",
        "AWAITING_DEPOSIT -> DEPOSIT_SEEN",
    ]


def test_explicit_event_overrides_auto(store: WarpJobStore):
    job = store.create_job("mainnet")
    store.update_job(
        job.id,
        status=JobStatus.APPROVING,
        event=("info", "sent approve tx", {"hash": "0xdead"}),
    )
    ev = store.get_events(job.id)[-1]
    assert ev.kind == "info"
    assert ev.message == "sent approve tx"
    assert ev.data == {"hash": "0xdead"}


def test_column_only_update_appends_no_event(store: WarpJobStore):
    job = store.create_job("mainnet")
    n_before = len(store.get_events(job.id))
    store.update_job(job.id, columns={"bridge_tx_hash": "0xabc"})
    assert len(store.get_events(job.id)) == n_before  # no status change, no event
    assert store.get_job(job.id).bridge_tx_hash == "0xabc"


def test_state_patch_merges_shallowly(store: WarpJobStore):
    job = store.create_job("mainnet", state={"a": 1, "b": 2})
    store.update_job(job.id, state_patch={"b": 20, "c": 3})
    assert store.get_job(job.id).state == {"a": 1, "b": 20, "c": 3}


def test_unknown_column_is_rejected(store: WarpJobStore):
    job = store.create_job("mainnet")
    with pytest.raises(WarpJobStoreError):
        store.update_job(job.id, columns={"totally_made_up": 1})


def test_update_missing_job_raises(store: WarpJobStore):
    with pytest.raises(JobNotFound):
        store.update_job(12345, status=JobStatus.COMPLETED)


# --------------------------------------------------------------------------- #
# Optimistic guard.
# --------------------------------------------------------------------------- #

def test_expected_status_guard_rolls_back(store: WarpJobStore):
    job = store.create_job("mainnet")
    store.update_job(job.id, status=JobStatus.DEPOSIT_SEEN)
    n_events = len(store.get_events(job.id))
    snapshot = store.get_job(job.id)

    # A resumed step expects AWAITING_DEPOSIT but the job already advanced.
    with pytest.raises(StaleJobError):
        store.update_job(
            job.id,
            status=JobStatus.APPROVING,
            expected_status=JobStatus.AWAITING_DEPOSIT,
            columns={"bridge_tx_hash": "0xshould_not_persist"},
            event=("info", "should not persist", None),
        )

    after = store.get_job(job.id)
    assert after.status == snapshot.status == JobStatus.DEPOSIT_SEEN
    assert after.bridge_tx_hash is None
    assert after.updated_at == snapshot.updated_at
    assert len(store.get_events(job.id)) == n_events  # event rolled back


def test_expected_status_match_allows_update(store: WarpJobStore):
    job = store.create_job("mainnet")
    ok = store.update_job(
        job.id,
        status=JobStatus.DEPOSIT_SEEN,
        expected_status=JobStatus.AWAITING_DEPOSIT,
    )
    assert ok.status == JobStatus.DEPOSIT_SEEN


# --------------------------------------------------------------------------- #
# Meta.
# --------------------------------------------------------------------------- #

def test_meta_roundtrip_and_default(store: WarpJobStore):
    assert store.get_meta("schema_version") == "1"
    assert store.get_meta("portal_coin_id") is None
    assert store.get_meta("portal_coin_id", "fallback") == "fallback"
    store.set_meta("portal_coin_id", "abcd")
    assert store.get_meta("portal_coin_id") == "abcd"
    store.set_meta("portal_coin_id", "ef01")  # upsert
    assert store.get_meta("portal_coin_id") == "ef01"


# --------------------------------------------------------------------------- #
# Crash resume: close and reopen the same file.
# --------------------------------------------------------------------------- #

def test_reopen_recovers_active_job_and_state(tmp_path):
    db = tmp_path / "warp_jobs.db"
    s1 = WarpJobStore(db, now=Clock())
    job = s1.create_job(
        "mainnet",
        status=JobStatus.COLLECTING_SIGS,
        columns={"bridge_nonce": "0x2a", "post_tip_mojos": 4_985},
        state={"sigs": ["aa", "bb"], "portal_coin_id": "c0ffee"},
    )
    jid = job.id
    s1.close()  # simulate a hard kill

    s2 = WarpJobStore(db, now=Clock())
    try:
        active = s2.get_active_job()
        assert active is not None and active.id == jid
        assert active.status == JobStatus.COLLECTING_SIGS
        assert active.bridge_nonce == "0x2a"
        assert active.post_tip_mojos == 4_985
        assert active.state == {"sigs": ["aa", "bb"], "portal_coin_id": "c0ffee"}
        # Store stays usable after reopen: advance to completion.
        s2.update_job(jid, status=JobStatus.COMPLETED)
        assert s2.get_active_job() is None
    finally:
        s2.close()


def test_wal_file_is_created_on_disk(tmp_path):
    db = tmp_path / "warp_jobs.db"
    s = WarpJobStore(db, now=Clock())
    try:
        s.create_job("mainnet")
        assert db.exists()
        # WAL journal mode leaves a -wal sidecar while the connection is open.
        assert (tmp_path / "warp_jobs.db-wal").exists()
    finally:
        s.close()


# --------------------------------------------------------------------------- #
# Events + listing limits.
# --------------------------------------------------------------------------- #

def test_list_jobs_limit(tmp_path):
    db = tmp_path / "warp_jobs.db"
    s = WarpJobStore(db, now=Clock())
    try:
        for _ in range(5):
            j = s.create_job("mainnet")
            s.update_job(j.id, status=JobStatus.COMPLETED)  # free the slot
        assert s.count_jobs() == 5
        assert len(s.list_jobs(limit=3)) == 3
    finally:
        s.close()


def test_append_event_standalone_requires_job(store: WarpJobStore):
    with pytest.raises(JobNotFound):
        store.append_event(42, "info", "orphan")
    job = store.create_job("mainnet")
    store.append_event(job.id, "info", "hand-written note", {"k": "v"})
    ev = store.get_events(job.id)[-1]
    assert ev.kind == "info" and ev.data == {"k": "v"}


def test_partial_index_exists(store: WarpJobStore):
    # Guard the single-open invariant's mechanism, not just its behaviour.
    row = store._conn.execute(
        "SELECT sql FROM sqlite_master WHERE type='index' AND name='ux_warp_jobs_active'"
    ).fetchone()
    assert row is not None
    assert "WHERE active = 1" in row["sql"]
