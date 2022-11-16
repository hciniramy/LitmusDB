![LitmusDB](https://litmusdb.com/assets/img/litmusdb.png)


# LitmusDB Prototype Build
[[:globe_with_meridians: Website]](https://litmusdb.com/) [[:memo: Paper]](https://dl.acm.org/doi/10.1145/3514221.3517851) [[:book: Extended Version]](https://yxia.me/assets/thesis.pdf) [[:tv: Video]](https://youtu.be/gMKxLbDYhTU)

## Important: Read Before You Use This Codebase

**You should never ever use it in production as it is a prototype build. The code is provided as it is with no warranty or support.**

This implementation is a proof-of-concept prototype build for performance and speedup estimating purposes. As discussed in the paper, there are three limitations:

1. We include the key generation in the critical path while it is unnecessary. This factor results in performance underestimation. We are working on porting the code to rust and replace the verifiable computation implementation with [Plonk](https://eprint.iacr.org/2019/953.pdf), a zkp protocol with universal setup. 
2. ~~We use a **small** RSA group to avoid the engineering effort of multi-scalar operations in the constraint system. In theory, this should not affect the speedup of the techniques we presented in the paper.For how to extend our implementation to support large groups, [xJsnark](https://github.com/akosba/xjsnark) and [Bellman-Bignat](https://github.com/alex-ozdemir/bellman-bignat) would be helpful. We are working on porting the codebase to rust and integrating it with Bellman-Bignat for large RSA groups (>=2048 bits).~~ We implemented an extension for multi-scalar operations and added a 1024-bit RSA group to Litmus. It is at `multi-scalar` branch.
3. We assume a division-intractibility hash function exists [1]. This brings two effects: (a) the non-existence proof of key lookups might fail occasionally because the large integers could have common divisors. We commented one constraint at line 185 of file `litmus-gadgets.hpp`. This does not affect the performance much since the prover workload depends on the number of constraints and variables. You may uncomment the constraint back but then you need to remove the assertion *verified==true* at line 2377 of file `litmus-prover.cpp`. (b) This skips the Pocklington verification. It results in performance overestimation (by a constant factor).

Please do not hesitate fo submit an issue or contact us directly if you found any bugs.

[1] Ozdemir, Alex, Riad Wahby, Barry Whitehat, and Dan Boneh. "Scaling verifiable computation using efficient set accumulators." In 29th USENIX Security Symposium (USENIX Security 20), pp. 2075-2092. 2020.

*The `pequin` folder is the Pequin ToolChain project from https://github.com/pepper-project/pequin*

## How to Install

*Note: You need to have gcc > 5.5 and glibc < 2.28 to finish the following setup process.* Thanks to Changhao Zhu for pointing this out.

1. Navigate into pequin/ and follow the instruction in GETTING_STARTED.md and INSTALLING.md. Make sure you can successfully prove the examples shipped with Pequin.
2. Navigate into dbx1000/ and run install_deps.sh
3. Run `export LD_LIBRARY_PATH=/usr/lib/jvm/adoptopenjdk-14-hotspot-amd64/lib/server/`
4. Run `python3 tools/compile.py`. As an alternative use the following command to compile a certain variant: `python3 tools/compile.py serial YCSB NO_WAIT LOG_DATA RSA_AD false`
5. Create the `logs` and `results` folder.

You should now see a number of executables whose names start with `rundb`. They represent the baselines.

## Usage with docker
1. Run the `build_docker.sh` script (this will create the pequin base docker image if required and the litmusdb docker image)
2. Navigate into `dbx1000_logging` and create the tmpfs filesystem: `cd ../dbx1000_logging && mkdir logs && sudo mount -t tmpfs -o rw,size=30G tmpfs ./logs`
3. create a new docker container using the following command: `docker run --privileged -h litmus_dev -it -v $HOME/LitmusDB/dbx1000_logging/logs/:/root/LitmusDB/dbx1000_logging/logs litmusdb bash`

(`privileged` mode is required since the script flushes OS caches and for numactl, `-h litmus_dev` is required since the `runExpr.py` script selects the OS cache flush method based on the hostname) 

## How to Use

Run `rundb_XXX -h` for options.

## How to Reproduce the Evaluation

Before you run the evaluation, make sure you create a `tmpfs` filesystem larger than 20GB and mount it to the folder `logs`.

`python3 scripts/runExpr.py litmus`

`python3 scripts/runExpr.py litmusBatchSize`

`python3 scripts/runExpr.py litmusProverThreadsRAMDISK`

To collect the stat, add `COLLECT_DATA=x` to the above lines.

`COLLECT_DATA=1`: Throughput

`COLLECT_DATA=2`: Latency

`COLLECT_DATA=3`: Communication Cost

`COLLECT_DATA=4`: Time Breakdown

`COLLECT_DATA=5`: Throughput for No-Verification baselines (removing all the overhead)

# Next Steps

1. Port the code into Rust
2. Integrate with Bellman-Bignat and Plonk
3. Full TPC-C support
