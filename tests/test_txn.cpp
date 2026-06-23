// Tests for the lock manager: lock compatibility, blocking, strict 2PL release,
// lock upgrade, and deadlock detection with real threads.
#include <atomic>
#include <chrono>
#include <thread>

#include "minidb/exceptions.h"
#include "minidb/txn/lock_manager.h"
#include "minidb/txn/transaction.h"
#include "test_framework.h"

using namespace minidb;
using namespace std::chrono_literals;

static Resource RES(int file, int page, int slot) {
    return Resource{file, RID{page, slot}};
}

TEST(lock, shared_locks_are_compatible) {
    LockManager lm;
    Transaction t1(1), t2(2);
    Resource r = RES(0, 0, 0);
    CHECK(lm.lock_shared(&t1, r));
    CHECK(lm.lock_shared(&t2, r));  // must not block
    lm.unlock_all(&t1);
    lm.unlock_all(&t2);
}

TEST(lock, exclusive_is_exclusive_and_blocks) {
    LockManager lm;
    Transaction t1(1), t2(2);
    Resource r = RES(0, 1, 0);
    CHECK(lm.lock_exclusive(&t1, r));

    std::atomic<bool> t2_got{false};
    std::atomic<bool> t1_released{false};
    std::thread other([&]() {
        lm.lock_shared(&t2, r);  // should block until t1 releases
        // By the time we get here, t1 must have released.
        CHECK(t1_released.load());
        t2_got = true;
        lm.unlock_all(&t2);
    });

    std::this_thread::sleep_for(50ms);   // let `other` reach the blocked lock
    CHECK(!t2_got.load());               // still blocked
    t1_released = true;
    lm.unlock_all(&t1);                  // releasing wakes `other`
    other.join();
    CHECK(t2_got.load());
}

TEST(lock, upgrade_shared_to_exclusive) {
    LockManager lm;
    Transaction t1(1);
    Resource r = RES(0, 2, 0);
    CHECK(lm.lock_shared(&t1, r));
    CHECK(lm.lock_exclusive(&t1, r));  // upgrade by the sole holder
    lm.unlock_all(&t1);
}

TEST(lock, deadlock_is_detected) {
    LockManager lm;
    Transaction t1(1), t2(2);
    Resource A = RES(0, 10, 0);
    Resource B = RES(0, 20, 0);

    CHECK(lm.lock_exclusive(&t1, A));
    CHECK(lm.lock_exclusive(&t2, B));

    std::atomic<int> deadlocks{0};
    std::atomic<int> successes{0};

    // t1 now wants B (held by t2); t2 wants A (held by t1) -> cycle.
    std::thread th1([&]() {
        try {
            lm.lock_exclusive(&t1, B);
            successes++;
            lm.unlock_all(&t1);
        } catch (const DeadlockException&) {
            deadlocks++;
            lm.unlock_all(&t1);  // victim releases so the other can proceed
        }
    });
    std::thread th2([&]() {
        try {
            lm.lock_exclusive(&t2, A);
            successes++;
            lm.unlock_all(&t2);
        } catch (const DeadlockException&) {
            deadlocks++;
            lm.unlock_all(&t2);
        }
    });
    th1.join();
    th2.join();

    // Exactly one transaction is chosen as the deadlock victim; the other
    // acquires its second lock once the victim releases.
    CHECK_EQ(deadlocks.load(), 1);
    CHECK_EQ(successes.load(), 1);
}

TEST(lock, many_readers_one_writer_waits) {
    LockManager lm;
    Transaction r1(1), r2(2), r3(3), w(4);
    Resource r = RES(0, 30, 0);
    CHECK(lm.lock_shared(&r1, r));
    CHECK(lm.lock_shared(&r2, r));
    CHECK(lm.lock_shared(&r3, r));

    std::atomic<bool> writer_got{false};
    std::thread writer([&]() {
        lm.lock_exclusive(&w, r);  // blocks until all readers release
        writer_got = true;
        lm.unlock_all(&w);
    });
    std::this_thread::sleep_for(40ms);
    CHECK(!writer_got.load());
    lm.unlock_all(&r1);
    std::this_thread::sleep_for(10ms);
    CHECK(!writer_got.load());  // still two readers
    lm.unlock_all(&r2);
    lm.unlock_all(&r3);
    writer.join();
    CHECK(writer_got.load());
}
