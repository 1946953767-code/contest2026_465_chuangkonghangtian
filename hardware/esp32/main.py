# ESP32 offline flight computer boot entry.
# The USB console is only for diagnostics; telemetry runs without WiFi or PC.
import gc
import time

print("offline telemetry boot")
gc.collect()
time.sleep_ms(300)
try:
    import central_tx
    central_tx.main()
except Exception as e:
    print("central_tx stopped:", e)
    raise
