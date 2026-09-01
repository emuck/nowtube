# Nowtube Look Studio

Look Studio is a local design-review page. It previews the curated clock
collection, six-panel layout, brightness, and approved sunny icon before a
firmware build or device flash. It does not change a connected Nowtube.

## Start it

Python 3 is the only requirement—there are no packages to install. From the
repository root, run the one command for your operating system:

```bash
# macOS or Linux
python3 tools/look-studio/serve.py

# Windows PowerShell or Command Prompt
py tools\look-studio\serve.py
```

The command opens your default browser automatically at:

```
http://127.0.0.1:8765/tools/look-studio/
```

Leave that terminal window open while reviewing. Press `Ctrl+C` in it when
you are finished. After a reboot, run the same command again—there is no
installation, service, or project build step to restore.

## If the default port is busy

```bash
# macOS or Linux
python3 tools/look-studio/serve.py --port 8877

# Windows
py tools\look-studio\serve.py --port 8877
```

Use the URL printed by the command.

## Viewing from another device

Look Studio binds to this computer only by default. To deliberately make it
available on your local network, use `--host 0.0.0.0` and open the printed
port with this computer's LAN IP. Do this only on a trusted home network.

```bash
python3 tools/look-studio/serve.py --host 0.0.0.0 --no-browser
```

It intentionally uses Google Fonts' browser delivery for review convenience;
the firmware remains self-contained and uses compiled LVGL font assets. Treat
this as a direction-setting tool. Confirm the final selected look on a real
Nowtube before release.
