# FUSS: Fuzzing on a Shoe String

Inspired by this [FUSS research thesis](https://drive.google.com/file/d/0B3VzYomhg7UUM1lwbUJ5ZExIRWc/view?usp=sharing&resourcekey=0-X9UXm156sao3rs2rXB79_Q), the following LLVM plugin implements an instrumentation pruner for guided fuzzers. Instead of coarsely disabling sanitizers at a function level, which introduces massive security blind spots, this pass operates at the basic block level, safely balancing detection capabilities with execution throughput.

## The Sanitizer Tax Problem
Guided fuzzers like LibFuzzer rely heavily on Address Sanitizer and Sanitizer Coverage to track execution states and catch memory corruptions. However, when parsing highly structural binary formats (such as pictures in libjpeg), the target application frequently encounters massive bottleneck loops(especially in decompressing algorithms).

Injected instrumentation inside these loops executes millions of times per second, burning massive CPU cycles on redundant shadow-memory checks and static coverage updates that have already saturated. This sanitizer tax severely bottlenecks the fuzzer's exec/s.

## The Proposed Solution: Micro-Pruning via LTO

First the Profile Cost Analysis where the pass utilizes LLVM’s ` BlockFrequencyInfo ` and `TargetTransformInfo ` to calculate the precise relative dynamic cost of every single sanitizer check in the application.  
Then once a hot instrumentation block is identified, the pass uses a program slicer to trace backward through basic blocks, capturing the entire chain of inline sanitization math (shadow map calculations, pointer arithmetic, bit-shifts, and comparisons).

Finally the pass safely overrides the conditional branch feeding into the sanitizer crash block, making the safety check unconditionally safe. LLVM's optimization pipelines can then cleanly sweep away the orphaned arithmetic.
The users can dynamically configure the performance/security tradeoff  ensuring only the top percentile of bottlenecking instructions are optimized away while the rest of the application remains fully protected.  
The project was tested using `-fsanitize=address,fuzzer-no-link -fsanitize-address-use-after-scope`

---
## Usage

The project is using [Google fuzzer test suite](https://github.com/google/fuzzer-test-suite.git) and tested using libjpeg-turgo. The following commands are to be ran for a sucessive compilation:

1. ` clang++ -shared -fPIC $(llvm-config --cxxflags) removeAsanFromFunc.cpp -o RemoveAsanFromFunc.so $(llvm-config --ldflags) `  
1. ` cd fuzzer-test-suite `  
1. ` ./libjpeg-turbo-07-2017/build.sh -p` (This will create a profile executable)  
1. `./libjpeg-turbo-07-2017-fsanitize_fuzzer`  
1. `llvm-profdata merge freetype_original.profdata ./profiles/*.profraw` (Merging the data)  
1. `./libjpeg-turbo-07-2017/build.sh` (This will finally create the final optimized build)

---
## Results

On a linux machine that has an AMD Ryzen 9 5900X (24) @ 5.36 GHz the following results were concluded for a run with seed 1234, cutoff 100% and 1000000 (One Milion) fuzzer tests:

| Metric | Fuzzer No Asan | Fuzzer Asan | Fuzzer Asan Optimized |
| :--- | :--- | :--- | :--- |
| Time | 597.25 mils | 62.36 s | 19.73 s|

While complettly removing asan instructions gives a version 33 times faster than the optimized one, the latter is also 3 times faster, confirming the thesis. It should also be noted that for a finer grained approach, instead of cutting instructions from the hottest blocks, the cutting is done from the hottest instructions.

---

The cutoff factor is not fixed and it should be experimented with for best performance/security:

| Cutoff Factor | 100% | 90% | 75% | 50% |
| :--- | :--- | :--- | :--- | :--- | :--- 
| Instructions pruned | 7589 | 47 | 19 | 6
| Time | 19.73 s| 56.08 s | 56.82 s | 58.24 s

## Conclusions

Is it worth it? Maybe. It all depends on the type of program you are testing. It needs to be instrumentation heavy and should be compute bound. This project will have little to no impact for projects that are memory bound or allocation bound such as Freetype, whose memory allocations are hijacked by Asan.

---
## Notes and Comments
* 90% of the time was spent fighting this build system with a bunch of autogen and $hell scripts
* This was the first time I interacted with LLVM's IR and I am sure someone who is passioned/more knowledgeable can do a better job. 
* Do I think this has inputs that will break it? Absolutlly. I am not accustomed to all the edge cases that can appear  

