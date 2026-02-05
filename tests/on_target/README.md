# Miniconda setup guide

Make sure you have `miniconda` installed:


Run the command:

```bash
curl -L -O https://github.com/conda-forge/miniforge/releases/latest/download/Miniforge3-Linux-x86_64.sh
```

then

```bash
bash Miniforge3-Linux-x86_64.sh -b -p ~/miniforge3
```

finally activate the environment (if you are using bash or zsh in your terminal) by running:

```bash
conda init "$(basename "$SHELL")"
```


# Create the Environment
Install the environment by entering the following command, in the current directory.

```bash
conda env create -f environment.yml
```

Then activate the environment:

```bash
conda activate ies2608
```