# FUSS: Fuzzing on a Shoe String

Inspired by this [FUSS research thesis](https://drive.google.com/file/d/0B3VzYomhg7UUM1lwbUJ5ZExIRWc/view?usp=sharing&resourcekey=0-X9UXm156sao3rs2rXB79_Q), the following LLVM plugin implements an instrumentation pruner for guided fuzzers. Instead of coarsely disabling sanitizers at a function level, which introduces massive security blind spots, this pass operates at the basic block level, safely balancing detection capabilities with execution throughput.

## The Sanitizer Tax Problem
Guided fuzzers like LibFuzzer rely heavily on Address Sanitizer and Sanitizer Coverage to track execution states and catch memory corruptions. However, when parsing highly structural binary formats (such as pictures in libjpeg), the target application frequently encounters massive bottleneck loops(especially in decompressing algorithms).

Injected instrumentation inside these loops executes millions of times per second, burning massive CPU cycles on redundant shadow-memory checks and static coverage updates that have already saturated. This sanitizer tax severely bottlenecks the fuzzer's exec/s.

## The Proposed Solution: Micro-Pruning via LTO

First the Profile Cost Analysis where the pass utilizes LLVM’s ``` BlockFrequencyInfo ``` and ``` TargetTransformInfo ``` to calculate the precise relative dynamic cost of every single sanitizer check in the application.  
Then once a hot instrumentation block is identified, the pass uses a program slicer to trace backward through basic blocks, capturing the entire chain of inline sanitization math (shadow map calculations, pointer arithmetic, bit-shifts, and comparisons).

Finally the pass safely overrides the conditional branch feeding into the sanitizer crash block, making the safety check unconditionally safe. LLVM's optimization pipelines can then cleanly sweep away the orphaned arithmetic.
The users can dynamically configure the performance/security tradeoff  ensuring only the top percentile of bottlenecking instructions are optimized away while the rest of the application remains fully protected.


# Sanitzers
#  -fsanitize=address,fuzzer-no-link -fsanitize-address-use-after-scope


# clang++ -shared -fPIC $(llvm-config --cxxflags) removeAsanFromFunc.cpp -o RemoveAsanFromFunc.so $(llvm-config --ldflags)



STEPS:

1.

NORMAL + CLANG

2.

./freetype2-2017-fsanitize_fuzzer seeds/ -runs=1000000

llvm-profdata merge -output=/home/sas/Coding/Fuss/fuzzer-test-suite/freetype_original.profdata ./profiles/*.profraw

3. 
CEL CU PLUG

<!-- FUSS: Pruned 24315 ASan instructions across 22621 blocks. -->





