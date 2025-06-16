# The LLVM Compiler Infrastructure

# LLVM Project Directory Overview

This document describes the purpose and contents of each directory in the LLVM source tree.



## bolt/

BOLT: Binary Optimization and Layout Tool. Tools and libraries for post-link binary optimization.

## clang/

Clang C/C++/Objective-C compiler frontend.

- [clang/](clang/)


## clang-tools-extra/

Additional tools built on top of Clang, such as clang-tidy and clangd.

- [clang-tools-extra/](clang-tools-extra/)


## cmake/

CMake build system modules and configuration files.

- [cmake/](cmake/)


## compiler-rt/

Runtime libraries for compiler-generated code, such as sanitizers and profiling.

- [compiler-rt/](compiler-rt/)


## cross-project-tests/

Test suites that span multiple LLVM subprojects.


## libc/

LLVM's implementation of the C standard library.


## libclc/

OpenCL C library implementation.


## libcxx/

LLVM's implementation of the C++ standard library.


## libcxxabi/

C++ ABI support library for libc++.


## libunwind/

A library to determine the call-chain of a program.


## lld/

LLVM's linker.


## lldb/

LLVM's debugger.


---

## llvm/

The core of the LLVM project. This directory contains the main libraries, tools, and utilities that form the backbone of the LLVM infrastructure. Key components include:

- **lib/**: Core libraries for IR (Intermediate Representation), code generation, optimization passes, target backends, and more.
- **include/**: Public header files for LLVM APIs.
- **tools/**: Command-line tools such as `llvm-as`, `llvm-dis`, `llvm-link`, and the main `opt` optimizer.
- **utils/**: Utility scripts and helper programs for development and testing.
- **docs/**: Documentation for LLVM internals, APIs, and usage.
- **test/**: Regression and unit tests for LLVM components.
- **examples/**: Example programs demonstrating LLVM API usage.

The `llvm/` directory is the foundation for all other LLVM subprojects, providing the infrastructure for compiler frontends, optimizers, code generators, and analysis tools.

---

## mlir/

MLIR (Multi-Level Intermediate Representation) is a subproject of LLVM designed to provide a flexible infrastructure for building reusable and extensible compiler components. MLIR enables the representation and transformation of code at multiple abstraction levels, making it easier to target diverse hardware and optimize across domains.

Key components include:

- **include/mlir/**: Public headers for MLIR APIs, dialect definitions, and core infrastructure.
- **lib/**: Implementation of MLIR core, dialects, passes, and transformations.
- **tools/**: MLIR-specific tools such as `mlir-opt` (optimizer), `mlir-translate` (IR translation), and others.
- **docs/**: Documentation for MLIR concepts, dialects, and developer guides.
- **test/**: Test suites for MLIR dialects, passes, and tools.
- **examples/**: Example projects and code demonstrating MLIR usage and extensibility.

MLIR is used both within LLVM and by external projects to build domain-specific compilers, optimize machine learning models, and enable advanced code transformations.

---

## llvm-libgcc/

LLVM's implementation of libgcc runtime routines.


## offload/

Support for offloading computation to accelerators.


## openmp/

OpenMP runtime and tools.


## polly/

Polyhedral optimization framework for LLVM.


## pstl/

Parallel STL implementation.


## runtimes/

Meta-project for building multiple runtime libraries together.


## third-party/

Third-party dependencies and libraries.


## utils/

Utility scripts and tools for development and testing.

