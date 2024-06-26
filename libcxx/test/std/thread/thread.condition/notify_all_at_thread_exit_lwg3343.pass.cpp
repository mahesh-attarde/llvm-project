//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// UNSUPPORTED: no-threads

// notify_all_at_thread_exit(...) requires move semantics to transfer the unique_lock.
// UNSUPPORTED: c++03

// The fix of LWG3343 is done in the dylib. That means Apple backdeployment
// targets remain broken. Due to the nature of the test, testing on a broken
// system does not guarantee that the test fails, so the test can't use XFAIL.
// UNSUPPORTED: stdlib=system && target={{.+}}-apple-macosx10.{{.+}}
// UNSUPPORTED: stdlib=system && target={{.+}}-apple-macosx11.{{.+}}

// This is a regression test for LWG3343.
//
// <condition_variable>
//
// void notify_all_at_thread_exit(condition_variable& cond, unique_lock<mutex> lk);

#include "make_test_thread.h"
#include "test_macros.h"

#include <condition_variable>
#include <cassert>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>

int condition_variable_lock_skipped_counter = 0;

TEST_DIAGNOSTIC_PUSH
// MSVC warning C4583: 'X::cv_': destructor is not implicitly called
TEST_MSVC_DIAGNOSTIC_IGNORED(4583)

union X {
    X() : cv_() {}
    ~X() {}
    std::condition_variable cv_;
    unsigned char bytes_[sizeof(std::condition_variable)];
};

TEST_DIAGNOSTIC_POP

void test()
{
    constexpr int N = 3;

    X x;
    std::mutex m;
    int threads_active = N;

    for (int i = 0; i < N; ++i) {
        std::thread t = support::make_test_thread([&] {
            // Emulate work being done.
            std::this_thread::sleep_for(std::chrono::milliseconds(1));

            // Signal thread completion.
            std::unique_lock<std::mutex> lk(m);
            --threads_active;
            std::notify_all_at_thread_exit(x.cv_, std::move(lk));
        });
        t.detach();
    }

    // Wait until all threads complete, i.e. until they've all
    // decremented `threads_active` and then unlocked `m` at thread exit.
    // It is possible that this `wait` may spuriously wake up,
    // but it won't be able to continue until the last thread
    // unlocks `m`.
    {
        std::unique_lock<std::mutex> lk(m);
        // Due to OS scheduling the workers might have terminated when this
        // code is reached. In that case the wait will not sleep and the call
        // to notify_all_at_thread_exit has no effect; the condition variable
        // will not be used here.
        //
        // Keep track of how often that happens, if too often the test needs
        // to be improved.
        if(threads_active == 0)
            ++condition_variable_lock_skipped_counter;
        x.cv_.wait(lk, [&]() { return threads_active == 0; });
    }

    // Destroy the condition_variable and shred the bytes.
    // Simulate reusing the memory for something else.
    x.cv_.~condition_variable();
    for (unsigned char& c : x.bytes_) {
        c = 0xcd;
    }

    DoNotOptimize(x.bytes_);

    // Check that the bytes still have the same value we just wrote to them.
    // If any thread wrongly unlocked `m` before calling cv.notify_all(), and
    // cv.notify_all() writes to the memory of the cv, then we have a chance
    // to detect the problem here.
    int sum = 0;
    for (unsigned char c : x.bytes_) {
       sum += c;
    }
    DoNotOptimize(sum);
    assert(sum == (0xcd * sizeof(std::condition_variable)));
}

int main(int, char**)
{
    for (int i = 0; i < 1000; ++i) {
        test();
    }

    // The threshold is arbitrary, it just makes sure the notification is
    // tested a reasonable number of times.
    assert(condition_variable_lock_skipped_counter < 250);

    return 0;
}
