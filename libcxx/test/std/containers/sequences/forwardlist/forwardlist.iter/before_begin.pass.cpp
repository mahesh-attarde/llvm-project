//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// <forward_list>

// iterator       before_begin();        // constexpr since C++26
// const_iterator before_begin() const;  // constexpr since C++26
// const_iterator cbefore_begin() const; // constexpr since C++26

#include <forward_list>
#include <cassert>
#include <iterator>

#include "test_macros.h"
#include "min_allocator.h"

TEST_CONSTEXPR_CXX26 bool test() {
  {
    typedef int T;
    typedef std::forward_list<T> C;
    C c;
    C::iterator i = c.before_begin();
    assert(std::distance(i, c.end()) == 1);
  }
  {
    typedef int T;
    typedef std::forward_list<T> C;
    const C c;
    C::const_iterator i = c.before_begin();
    assert(std::distance(i, c.end()) == 1);
  }
  {
    typedef int T;
    typedef std::forward_list<T> C;
    const C c;
    C::const_iterator i = c.cbefore_begin();
    assert(std::distance(i, c.end()) == 1);
    assert(c.cbefore_begin() == c.before_begin());
  }
  {
    typedef int T;
    typedef std::forward_list<T> C;
    const T t[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    C c(std::begin(t), std::end(t));
    C::iterator i = c.before_begin();
    assert(std::distance(i, c.end()) == 11);
    assert(std::next(c.before_begin()) == c.begin());
  }
  {
    typedef int T;
    typedef std::forward_list<T> C;
    const T t[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    const C c(std::begin(t), std::end(t));
    C::const_iterator i = c.before_begin();
    assert(std::distance(i, c.end()) == 11);
  }
#if TEST_STD_VER >= 11
  {
    typedef int T;
    typedef std::forward_list<T, min_allocator<T>> C;
    C c;
    C::iterator i = c.before_begin();
    assert(std::distance(i, c.end()) == 1);
  }
  {
    typedef int T;
    typedef std::forward_list<T, min_allocator<T>> C;
    const C c;
    C::const_iterator i = c.before_begin();
    assert(std::distance(i, c.end()) == 1);
  }
  {
    typedef int T;
    typedef std::forward_list<T, min_allocator<T>> C;
    const C c;
    C::const_iterator i = c.cbefore_begin();
    assert(std::distance(i, c.end()) == 1);
    assert(c.cbefore_begin() == c.before_begin());
  }
  {
    typedef int T;
    typedef std::forward_list<T, min_allocator<T>> C;
    const T t[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    C c(std::begin(t), std::end(t));
    C::iterator i = c.before_begin();
    assert(std::distance(i, c.end()) == 11);
    assert(std::next(c.before_begin()) == c.begin());
  }
  {
    typedef int T;
    typedef std::forward_list<T, min_allocator<T>> C;
    const T t[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    const C c(std::begin(t), std::end(t));
    C::const_iterator i = c.before_begin();
    assert(std::distance(i, c.end()) == 11);
  }
#endif

  return true;
}

int main(int, char**) {
  assert(test());
#if TEST_STD_VER >= 26
  static_assert(test());
#endif

  return 0;
}
