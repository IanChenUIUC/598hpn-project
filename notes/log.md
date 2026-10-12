## October 12th, 2026

Created two short-term datasets

- inputs: read-only (except for when its populated)
- results: during the experiment, where all the logs/results go

Repo organization

- single monorepo with all experiments
- different scripts for running each experiment (`profile.py` gets an input parameter)
- Apptainer image for (small-scale) local testing of scripts

Scripts

- populates the input dataset with `friendster` (downloaded from SNAP)
- ligra, galois, and grape scripts

## October 3rd, 2026

Read the CloudLab documentation.

First goal is to get familiar with the platforms and libraries.
Some initial measurements on [Galois/Gluon](https://github.com/IntelligentSoftwareSystems/Galois) seems to be a good way to start, e.g. implement connectivity and MST and measure

- time to completion
- communication throughput
- communication latency
- memory usage

Maybe try different network topologies.

Compare between Ligra and D-Ligra (latter using distributed).
Compare between LCI and MPI.

Datasets: webbase seems reasonable.
