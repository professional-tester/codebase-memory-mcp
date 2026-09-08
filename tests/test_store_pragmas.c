/*
 * test_store_pragmas.c — Tests for SQLite pragma resolution.
 *
 * Validates that the CBM_SQLITE_MMAP_SIZE env var controls the mmap_size
 * pragma applied to on-disk stores. Default behavior (env unset) must
 * remain 64 MB. Setting the env to 0 disables memory-mapped I/O so
 * concurrent processes that truncate the DB file under a sibling's live
 * mapping return SQLITE_IOERR instead of crashing the process with SIGBUS.
 */
#include "../src/foundation/compat.h"
#include "test_framework.h"
#include <store/store.h>
#include <sqlite3.h>
#include <stdio.h>
#include "test_helpers.h"
#include <store/store.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void clear_mmap_env(void) {
    cbm_unsetenv("CBM_SQLITE_MMAP_SIZE");
}

TEST(mmap_size_default_when_unset) {
    clear_mmap_env();
    ASSERT_EQ(cbm_store_resolve_mmap_size(), 67108864LL);
    PASS();
}

TEST(mmap_size_zero_disables_mmap) {
    cbm_setenv("CBM_SQLITE_MMAP_SIZE", "0", 1);
    ASSERT_EQ(cbm_store_resolve_mmap_size(), 0LL);
    clear_mmap_env();
    PASS();
}

TEST(mmap_size_explicit_value) {
    cbm_setenv("CBM_SQLITE_MMAP_SIZE", "1048576", 1);
    ASSERT_EQ(cbm_store_resolve_mmap_size(), 1048576LL);
    clear_mmap_env();
    PASS();
}

TEST(mmap_size_negative_clamped_to_zero) {
    cbm_setenv("CBM_SQLITE_MMAP_SIZE", "-1", 1);
    ASSERT_EQ(cbm_store_resolve_mmap_size(), 0LL);
    clear_mmap_env();
    PASS();
}

TEST(mmap_size_garbage_falls_back_to_default) {
    cbm_setenv("CBM_SQLITE_MMAP_SIZE", "not-a-number", 1);
    ASSERT_EQ(cbm_store_resolve_mmap_size(), 67108864LL);
    clear_mmap_env();
    PASS();
}

TEST(mmap_size_partial_garbage_falls_back_to_default) {
    cbm_setenv("CBM_SQLITE_MMAP_SIZE", "123abc", 1);
    ASSERT_EQ(cbm_store_resolve_mmap_size(), 67108864LL);
    clear_mmap_env();
    PASS();
}

/* Integration smoke: opening a file-backed store with mmap_size=0 must
 * succeed. Proves the resolver is wired through configure_pragmas(). */
TEST(store_open_with_mmap_disabled) {
    cbm_setenv("CBM_SQLITE_MMAP_SIZE", "0", 1);
    char tmp_path[256];
    snprintf(tmp_path, sizeof(tmp_path), "%s/cbm_test_pragmas_%d.db", cbm_tmpdir(), (int)getpid());
    unlink(tmp_path);

    cbm_store_t *s = cbm_store_open_path(tmp_path);
    ASSERT(s != NULL);
    cbm_store_close(s);

    unlink(tmp_path);
    /* WAL/SHM siblings created by the open */
    char tmp_wal[300];
    char tmp_shm[300];
    snprintf(tmp_wal, sizeof(tmp_wal), "%s-wal", tmp_path);
    snprintf(tmp_shm, sizeof(tmp_shm), "%s-shm", tmp_path);
    unlink(tmp_wal);
    unlink(tmp_shm);

    clear_mmap_env();
    PASS();
}

/* ── Integrity verdict (upstream #1206/#1037) ───────────────────────
 *
 * resolve_store()'s quarantine decision must quarantine ONLY confirmed
 * corruption. A transient SQLITE_BUSY/LOCKED from a concurrent writer is not
 * damage, and a torn node/edge btree with an intact projects table IS damage
 * even though the shallow check passes. */

/* Build a per-project db in rollback-journal (DELETE) mode with one sane
 * project row — models a db written by a non-WAL peer or shared filesystem. */
static int verdict_make_delete_journal_db(const char *path) {
    sqlite3 *db = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK) {
        return -1;
    }
    char *err = NULL;
    int rc = sqlite3_exec(db,
                          "PRAGMA journal_mode = DELETE;"
                          "CREATE TABLE projects ("
                          "  name TEXT PRIMARY KEY,"
                          "  indexed_at TEXT NOT NULL,"
                          "  root_path TEXT NOT NULL"
                          ");"
                          "INSERT INTO projects(name, indexed_at, root_path)"
                          " VALUES('verdict-proj', '2026-01-01T00:00:00Z', '/tmp/verdict-proj');",
                          NULL, NULL, &err);
    sqlite3_free(err);
    sqlite3_close(db);
    return rc == SQLITE_OK ? 0 : -1;
}

TEST(integrity_verdict_healthy_ok) {
    char dir[300];
    char *tmp = th_mktempdir("cbm_verdict_ok");
    ASSERT_NOT_NULL(tmp);
    snprintf(dir, sizeof(dir), "%s", tmp);
    char db_path[300];
    snprintf(db_path, sizeof(db_path), "%s/verdict-proj.db", dir);

    cbm_store_t *s = cbm_store_open_path(db_path);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(cbm_store_upsert_project(s, "verdict-proj", "/tmp/verdict-proj"), CBM_STORE_OK);
    /* Shallow bool and tri-state verdict agree on a healthy store. */
    ASSERT_TRUE(cbm_store_check_integrity(s));
    ASSERT_EQ(cbm_store_check_integrity_verdict(s), CBM_INTEGRITY_OK);
    cbm_store_close(s);
    th_cleanup(dir);
    PASS();
}

TEST(integrity_verdict_transient_under_exclusive_lock) {
    char dir[300];
    char *tmp = th_mktempdir("cbm_verdict_busy");
    ASSERT_NOT_NULL(tmp);
    snprintf(dir, sizeof(dir), "%s", tmp);
    char db_path[300];
    snprintf(db_path, sizeof(db_path), "%s/verdict-proj.db", dir);
    ASSERT_EQ(verdict_make_delete_journal_db(db_path), 0);

    /* Open the store connection BEFORE the lock is taken — a locked open would
     * hit the immutable-URI fallback, which ignores locks entirely. The store
     * connection opens fine — the file is NOT damaged. */
    cbm_store_t *s = cbm_store_open_path_query(db_path);
    ASSERT_NOT_NULL(s);
    /* Reset the store's busy_timeout (10s) to a tiny value so the subsequent
     * prepare fails fast with SQLITE_BUSY instead of blocking for the full
     * 10s and timing out the test harness. The verdict path is a synchronous
     * single prepare — we don't need the production retry policy here. */
    {
        sqlite3 *store_db = cbm_store_get_db(s);
        ASSERT_EQ(sqlite3_busy_timeout(store_db, 50), SQLITE_OK);
    }
    /* Bump the schema cookie so the store connection must reload its schema on
     * the next prepare — models another instance's first-run DDL (#1206). */
    sqlite3 *ddl = NULL;
    ASSERT_EQ(sqlite3_open(db_path, &ddl), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(ddl, "CREATE TABLE schema_bump(x INTEGER);", NULL, NULL, NULL),
              SQLITE_OK);
    sqlite3_close(ddl);
    /* A second connection holds the writer lock across the resolve. */
    sqlite3 *locker = NULL;
    ASSERT_EQ(sqlite3_open(db_path, &locker), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(locker, "BEGIN EXCLUSIVE;", NULL, NULL, NULL), SQLITE_OK);
    /* NEW: the verdict classifies the locked-but-healthy DB as transient →
     * do not quarantine. The plain bool check's return under lock is
     * undefined (it ignores step-level BUSY), so only the verdict is
     * asserted here. */
    ASSERT_EQ(cbm_store_check_integrity_verdict(s), CBM_INTEGRITY_TRANSIENT);

    cbm_store_close(s);
    ASSERT_EQ(sqlite3_exec(locker, "COMMIT;", NULL, NULL, NULL), SQLITE_OK);
    sqlite3_close(locker);
    unlink(db_path);
    th_cleanup(dir);
    PASS();
}

TEST(integrity_verdict_torn_btree_corrupt_while_shallow_passes) {
    enum { CORRUPT_NODES = 2000, ZERO_PAGES = 40 };
    char dir[300];
    char *tmp = th_mktempdir("cbm_verdict_torn");
    ASSERT_NOT_NULL(tmp);
    snprintf(dir, sizeof(dir), "%s", tmp);
    char db_path[300];
    snprintf(db_path, sizeof(db_path), "%s/verdict-torn.db", dir);

    cbm_store_t *s = cbm_store_open_path(db_path);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(cbm_store_upsert_project(s, "verdict-torn", "/tmp/verdict-torn"), CBM_STORE_OK);
    for (int i = 0; i < CORRUPT_NODES; i++) {
        char name[64];
        char qn[256];
        snprintf(name, sizeof(name), "torn_probe_fn_%04d", i);
        snprintf(qn, sizeof(qn),
                 "torn.some.rather.long.module.path.to.fill.table.pages.%s_padding_padding", name);
        cbm_node_t n = {.project = "verdict-torn",
                        .label = "Function",
                        .name = name,
                        .qualified_name = qn,
                        .file_path = "src/torn_probe.py",
                        .start_line = i + 1,
                        .end_line = i + 2};
        ASSERT_TRUE(cbm_store_upsert_node(s, &n) > 0);
    }
    cbm_store_close(s);
    /* Clean close checkpoints the WAL; clear any siblings so the file is the
     * single source of truth for the corruption below. */
    char sibling[310];
    snprintf(sibling, sizeof(sibling), "%s-wal", db_path);
    unlink(sibling);
    snprintf(sibling, sizeof(sibling), "%s-shm", db_path);
    unlink(sibling);

    /* Zero a band of mid-file pages (the #1037 shape): the projects table is
     * tiny and stays intact, node/edge btrees get torn. */
    FILE *f = fopen(db_path, "rb+");
    ASSERT_NOT_NULL(f);
    (void)fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    enum { PAGE = 4096 };
    long page_count = fsize / PAGE;
    ASSERT_TRUE(page_count > ZERO_PAGES + 8);
    char zero[PAGE];
    memset(zero, 0, sizeof(zero));
    (void)fseek(f, (page_count / 4) * (long)PAGE, SEEK_SET);
    for (int i = 0; i < ZERO_PAGES; i++) {
        ASSERT_EQ(fwrite(zero, 1, PAGE, f), (size_t)PAGE);
    }
    (void)fclose(f);

    cbm_store_t *s2 = cbm_store_open_path_query(db_path);
    ASSERT_NOT_NULL(s2);
    /* The hole in the old check: shallow sanity passes on the torn DB… */
    ASSERT_TRUE(cbm_store_check_integrity(s2));
    /* …while the verdict runs quick_check and correctly reports CORRUPT. */
    ASSERT_EQ(cbm_store_check_integrity_verdict(s2), CBM_INTEGRITY_CORRUPT);
    cbm_store_close(s2);
    unlink(db_path);
    th_cleanup(dir);
    PASS();
}

SUITE(store_pragmas) {
    RUN_TEST(mmap_size_default_when_unset);
    RUN_TEST(mmap_size_zero_disables_mmap);
    RUN_TEST(mmap_size_explicit_value);
    RUN_TEST(mmap_size_negative_clamped_to_zero);
    RUN_TEST(mmap_size_garbage_falls_back_to_default);
    RUN_TEST(mmap_size_partial_garbage_falls_back_to_default);
    RUN_TEST(store_open_with_mmap_disabled);
    /* Integrity verdict (upstream #1206/#1037) */
    RUN_TEST(integrity_verdict_healthy_ok);
    RUN_TEST(integrity_verdict_transient_under_exclusive_lock);
    RUN_TEST(integrity_verdict_torn_btree_corrupt_while_shallow_passes);
}
