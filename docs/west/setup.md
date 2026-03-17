# Setup West Workspace

This is a quick guide to get the project building using `west`.
If you follow this step-by-step, you should be up and running without issues.

---

## 1. Pick a place for your workspace

Don’t do this inside the repo itself. Go somewhere clean:

```bash
mkdir ~/west-workspace
cd ~/west-workspace
```

---

## 2. Clone the project

```bash
git clone <your-repo-url> IES2608-NordicSemiconductor
```

---

## 3. Initialize west

This tells west to use this repo as the manifest:

```bash
west init -l IES2608-NordicSemiconductor
```

---

## 4. Download dependencies

This step pulls Zephyr / SDK stuff. It might take a while.

```bash
west update
```

---

## 5. Sanity check

Just to make sure everything is wired correctly:

```bash
west topdir
```

You should see your workspace path printed.

---

## 6. Build the project

```bash
west build -b nrf9151dk/nrf9151/ns IES2608-NordicSemiconductor/app -p always
```

First build can take a bit — that’s normal.

---

## 7. Flash (optional)

If you’ve got the board connected:

```bash
west flash
```

---

## A couple of things that will save you pain

* Don’t run `west init` inside `app/` or inside the repo
* Always run `west` commands from the workspace root
* If something weird happens, try:

  ```bash
  west update
  ```

---

## If things break

**“unknown command build”**
→ you’re not inside a west workspace

**“no west workspace found”**
→ you skipped `west init`

**build errors about missing stuff**
→ run `west update` again

---

## TL;DR

```bash
mkdir workspace
cd workspace
git clone <repo>
west init -l IES2608-NordicSemiconductor
west update
west build -b nrf9151dk/nrf9151/ns IES2608-NordicSemiconductor/app
```

---

That’s it. If it doesn’t work, something is off with the environment — not the steps.
