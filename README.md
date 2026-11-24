# DawaBox

Smart pill dispenser for elders — dispenses the right dose at the right time, and tells the family when a dose is missed.

Built by Team Orbit Crew (M. Junaid Javed, Irtiza, Farhan, Ali), Department of
Electrical Engineering, COMSATS University Islamabad. Supervisor: Dr. Amir Rasheed.
Submitted to EEECI 2026 — TechForge.

## What it is

Three medicine towers hold six sealed doses each (18 doses total). At the
scheduled time a servo-driven pushrod kicks exactly one rigid printed pouch
(28 x 20 x 5 mm) into a front tray, a buzzer sounds, and an infrared sensor
checks whether the patient collects the dose by hand within 10 minutes. If
not, the event is logged as a missed dose. A Blynk companion app lets family
members set dose times, view live inventory per tower, trigger a test
dispense, and receive missed-dose alerts.

## Hardware

- ESP32 (30-pin) — controller
- DS3231 real-time clock (SDA D25, SCL D27)
- 3x SG90 servos (D13, D12, D14) driving the pushrods
- Buzzer (D26), IR hand sensor (D34, LOW = hand)
- 5 V / 3.3 V breadboard rails; no LCD fitted

## Firmware

`pill_dispenser_3pill.ino` is the current 3-pill firmware (Arduino IDE, ESP32
board support, Blynk library). Set the RTC once with `set_clock.ino`. The
Blynk auth token lives only in the phone app's local storage, never in the
code.

## Status

Prototype. Subsystems verified: all three servos move, buzzer and RTC
ticking, Wi-Fi and Blynk connected. End-to-end validation (full dispense
test, pouch printing, dose-time saving) is in progress. Prototype — for
academic demonstration only, not for sale. Commercial sale needs DRAP
registration first.
