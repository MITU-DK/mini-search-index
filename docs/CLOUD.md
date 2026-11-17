# Running the benchmarks on a cloud machine

The laptop that the first results come from has 20 logical CPUs, but they are 6 fast cores with two
threads each plus 8 slower cores, so its scaling curve flattens early. A cloud machine with 16 to 64
uniform vCPUs shows how the indexer scales when the cores are alike. This page says how to get one
and how to read what it measures.

Everything is done by one script, `tools/cloud/run_on_new_machine.sh`.

## What the script does

```sh
tools/cloud/run_on_new_machine.sh cloud-32c main --threads 1,2,4,8,16,32
```

1. checks that `g++` 14 or newer, `cmake`, `ninja`, `python3` with `venv`, `wget`, `git` and `tar`
   are installed, and that there is enough disk (4 GB for `dev`, 16 GB for `main`);
2. creates the Python environment in `.venv` (a Python 3.10, fetched with `uv` if the system Python is
   newer, because WikiExtractor 3.0.6 does not work on Python 3.11 and later);
3. downloads the Wikipedia dump files for the profile (checking their checksums), extracts the articles
   with WikiExtractor and writes the packed corpus to `data/corpus/<profile>/`, then deletes the raw files;
4. builds the `release` preset and runs the tests;
5. runs `tools/run_benchmarks.sh all` (B1 to B8) into `benchmarks/results/<machine-name>/`;
6. packs that folder into `<machine-name>-results.tar.gz` in the repository root.

Each finished step is remembered in `data/.done/`, so if the machine is stopped or the connection is
lost, run the same command again and it carries on from where it was. Delete `data/.done/` to start over.

The profiles: `dev` is 20,000 articles and takes a few minutes, which is enough to check that the
setup works. `main` is 150,000 articles (about 1.5 GB of text) and is the one to report. The thread
sweep should reach the number of vCPUs of the machine, for example `1,2,4,8,16,32` on 32 vCPUs.

## Option 1: GitHub Codespaces

A personal GitHub account includes some free Codespaces use each month: about 120 core-hours at the
time of writing, where a machine with N cores uses N core-hours per hour. A 32-core machine therefore
gives about 3.75 free hours. Check the current allowance and which machine sizes your account can
choose under *Settings, Billing and plans*; the larger sizes are not always offered.

1. On the repository page choose *Code, Codespaces, New with options*, pick the biggest machine type
   offered (the repository's `.devcontainer/devcontainer.json` installs the tools), and create it.
2. In the terminal that opens: `tools/cloud/run_on_new_machine.sh cloud-32c main --threads 1,2,4,8,16,32`
   (use the real core count in the name and the sweep).
3. When it ends, download `cloud-32c-results.tar.gz` (right-click it in the file list, *Download*).
4. **Stop or delete the codespace**, so that it does not use the allowance while idle.

The download and extraction of the dump is a large part of the time and uses few cores, so it eats the
allowance without benefit; the script has not been timed on 32 cores yet (this page will say how long
it took once it has). If the free hours are short, run `dev` first to check that everything works.

## Option 2: any paid virtual machine

A few dollars buys an hour on a machine with 32 to 64 vCPUs from any cloud provider. Pick Ubuntu
24.04, at least 32 GB of disk and 8 GB of memory (the indexer's peak was about 2 GB on the laptop, B1),
install the tools

```sh
sudo apt-get update && sudo apt-get install -y g++-14 cmake ninja-build python3-venv wget git
sudo update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-14 100
sudo update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-14 100  # googletest needs a C compiler too
```

clone the repository, and run the same script. Note the instance type (for example `c7i.8xlarge`) in the
machine name or in `benchmarks/results/<machine-name>/machine.txt`; `tools/machine_info.sh` records what
the machine reports about itself.

## Free tiers that do not help

The free trial credit of some providers caps the number of vCPUs that can run at once (the Google
Cloud free trial allows 8), which is fewer than the laptop's 20. A machine that small adds no
information. Always-free small instances are too small for the same reason.

## Putting the results into the repository

```sh
tar -xzf cloud-32c-results.tar.gz -C benchmarks/results
python3 tools/make_results.py benchmarks/results > benchmarks/RESULTS.md
.venv/bin/python tools/plot_scaling.py benchmarks/results docs/img/scaling.svg
python3 tools/make_readme.py README.md benchmarks/results
```

The chart then has one line per machine. Commit `benchmarks/results/cloud-32c/`, `benchmarks/RESULTS.md`
and `docs/img/scaling.svg`.

## How to read cloud results

- **vCPUs are hyperthreads.** On most providers one vCPU is one hardware thread, and two vCPUs share
  a core. A 32-vCPU machine has about 16 cores, so expect the speed-up to flatten at about half the
  thread count even if the program scales perfectly. This is the same effect as the laptop's
  two-threads-per-core P cores, but all the vCPUs are alike, which makes the curve cleaner.
- **Record the instance type.** The numbers mean nothing without the CPU model and the number of
  vCPUs; put them in the machine name and check `machine.txt`.
- **Run it twice.** Copy the first tarball somewhere safe, delete the `bench-...` file for this run in
  `data/.done/` (otherwise the script skips the finished benchmarks), run the script again, and compare
  the medians of the two runs. If they differ by more than 10%, say so in
  `benchmarks/NOTES.md`.
- **Noisy neighbours.** On a shared host, other customers' work can slow some runs. The
  min to max range in the tables shows it: a wide range for one thread count means a disturbed run.
- **Use a real disk.** Do not run it in a folder on `tmpfs` (often `/tmp`): a memory-backed
  filesystem cannot be evicted from the page cache, and the script refuses to start there.
- **Cold-cache numbers.** B3 and B4 evict the index file from the page cache with `posix_fadvise`.
  Some network disks hide the effect behind their own cache, so the major-fault check in those
  benchmarks may stop the run; the cold numbers cannot be trusted there.
- **Disk speed.** Cloud disks are often slower or faster than a laptop SSD. B3 and B4 cold results
  compare disks as much as software, so compare cold numbers between machines with care.

## Full English Wikipedia (B8 at full scale)

`run_on_new_machine.sh` does `dev` and `main` only. The full dump (about 7 million articles) needs a
machine with **about 100 GB of free disk** at its peak (the dump, the extracted JSON, the packed corpus,
and the run files and index of the build) and, to make it worth doing, enough memory for the budgets
tried (2, 4 and 8 GiB by default). The steps, from the repository root with the tools installed and
`.venv` made as `run_on_new_machine.sh` does it:

```sh
mkdir -p benchmarks/results/<machine-name>
tools/fetch_wikipedia.sh full                      # about 24 GB; resumes if interrupted
.venv/bin/python -m wikiextractor.WikiExtractor data/raw/enwiki-*-pages-articles.xml.bz2 \
    --json -o data/extracted/full --processes "$(nproc)" --quiet
.venv/bin/python tools/prepare_corpus.py data/extracted/full data/corpus/full \
    --stats-out benchmarks/results/<machine-name>/corpus-full.txt     # no --max-docs: all of them
rm -rf data/raw data/extracted                     # the packed corpus is all that is needed from here

tools/run_benchmarks.sh b8 --machine <machine-name> --profile full   # builds within each budget, keeps the last index
for b in b3 b4 b6 b7; do tools/run_benchmarks.sh $b --machine <machine-name> --profile full; done
```

`b8` stops at once if there is not about 1.2 times the size of the corpus free for the run files and the
index. Budgets other than the default are given with `--budgets 1G,2G`. After it, regenerate the tables
(`python3 tools/make_tables.py --machine <machine-name> --profile full`) and the results page as above.
The article count of the packed corpus is in `corpus-full.txt`; use that number, not a remembered one.
